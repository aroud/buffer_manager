#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "buffer_manager/buffer_manager.h"

namespace buffer_manager {
namespace {

class TempDiskFile final {
 public:
  explicit TempDiskFile(std::string name) : path_(MakePath(std::move(name))) {}

  ~TempDiskFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  TempDiskFile(const TempDiskFile&) = delete;
  TempDiskFile& operator=(const TempDiskFile&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  static std::filesystem::path MakePath(std::string name) {
    static std::atomic<std::uint64_t> counter{0};

    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    const auto pid = ::getpid();

    return std::filesystem::temp_directory_path() /
           ("buffer_manager_async_test_" + std::move(name) + "_" +
            std::to_string(pid) + "_" + std::to_string(now) + "_" +
            std::to_string(id) + ".db");
  }

  std::filesystem::path path_;
};

[[nodiscard]] BufferManagerOptions Options(const std::filesystem::path& path,
                                           std::size_t frame_count,
                                           PageId max_page_count,
                                           std::size_t worker_count = 4) {
  return {
      .disk =
          {
              .path = path,
              .max_page_count = max_page_count,
              .truncate_existing = true,
              .preallocate = true,
              .io_mode = IoMode::kBuffered,
          },
      .frame_count = frame_count,
      .async_worker_count = worker_count,
  };
}

void FillPage(Page& page, std::byte value) {
  page.data.fill(value);
}

void FillPageWithPageId(PageId page_id, Page& page) {
  for (std::size_t i = 0; i < page.data.size(); ++i) {
    const auto value = static_cast<unsigned char>((page_id + i) % 251);
    page.data[i] = std::byte{value};
  }
}

void ExpectPagesEqual(const Page& expected, const Page& actual) {
  ASSERT_EQ(expected.data.size(), actual.data.size());

  for (std::size_t i = 0; i < expected.data.size(); ++i) {
    EXPECT_EQ(std::to_integer<unsigned int>(expected.data[i]),
              std::to_integer<unsigned int>(actual.data[i]))
        << "byte offset " << i;
  }
}

TEST(BufferManagerAsyncTest, NewPageAsyncReturnsPinnedGuard) {
  TempDiskFile file("new_page_async");

  BufferManager buffer_manager(Options(file.path(), 2, 4));

  std::future<PageGuard> future = buffer_manager.NewPageAsync();
  PageGuard guard = future.get();

  ASSERT_TRUE(guard.valid());
  EXPECT_EQ(guard.page_id(), 0U);

  for (std::byte byte : guard.page().data) {
    EXPECT_EQ(byte, std::byte{0});
  }
}

TEST(BufferManagerAsyncTest, MultipleNewPageAsyncCallsAllocateUniquePageIds) {
  TempDiskFile file("new_page_async_unique");

  constexpr std::size_t kPageCount = 16;

  BufferManager buffer_manager(
      Options(file.path(), kPageCount, kPageCount, /*worker_count=*/4));

  std::vector<std::future<PageGuard>> futures;
  futures.reserve(kPageCount);

  for (std::size_t i = 0; i < kPageCount; ++i) {
    futures.push_back(buffer_manager.NewPageAsync());
  }

  std::vector<PageGuard> guards;
  std::unordered_set<PageId> page_ids;

  guards.reserve(kPageCount);

  for (auto& future : futures) {
    PageGuard guard = future.get();

    ASSERT_TRUE(guard.valid());
    EXPECT_TRUE(page_ids.insert(guard.page_id()).second);

    guards.push_back(std::move(guard));
  }

  EXPECT_EQ(page_ids.size(), kPageCount);
}

TEST(BufferManagerAsyncTest, FetchPageAsyncReturnsResidentPage) {
  TempDiskFile file("fetch_resident_async");

  BufferManager buffer_manager(Options(file.path(), 2, 4));

  PageGuard original = buffer_manager.NewPage();
  const PageId page_id = original.page_id();

  Page expected;
  FillPageWithPageId(page_id, expected);

  original.page() = expected;
  original.MarkDirty();
  original.Drop();

  std::future<PageGuard> future = buffer_manager.FetchPageAsync(page_id);
  PageGuard fetched = future.get();

  ASSERT_TRUE(fetched.valid());
  EXPECT_EQ(fetched.page_id(), page_id);
  ExpectPagesEqual(expected, fetched.page());
}

TEST(BufferManagerAsyncTest, FetchPageAsyncPropagatesExceptions) {
  TempDiskFile file("fetch_unused_async");

  BufferManager buffer_manager(Options(file.path(), 2, 4));

  std::future<PageGuard> future = buffer_manager.FetchPageAsync(0);

  EXPECT_THROW(
      {
        PageGuard guard = future.get();
        (void)guard;
      },
      std::logic_error);
}

TEST(BufferManagerAsyncTest, MultipleAsyncFetchesShareNonResidentLoad) {
  TempDiskFile file("fetch_same_nonresident_async");

  constexpr std::size_t kFetchCount = 8;

  BufferManager buffer_manager(Options(file.path(), 1, 3, /*worker_count=*/4));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  Page expected;
  FillPageWithPageId(first_page_id, expected);

  first.page() = expected;
  first.MarkDirty();
  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  std::vector<std::future<PageGuard>> futures;
  futures.reserve(kFetchCount);

  for (std::size_t i = 0; i < kFetchCount; ++i) {
    futures.push_back(buffer_manager.FetchPageAsync(first_page_id));
  }

  std::vector<PageGuard> guards;
  guards.reserve(kFetchCount);

  for (auto& future : futures) {
    PageGuard guard = future.get();

    ASSERT_TRUE(guard.valid());
    EXPECT_EQ(guard.page_id(), first_page_id);
    ExpectPagesEqual(expected, guard.page());

    guards.push_back(std::move(guard));
  }
}

TEST(BufferManagerAsyncTest, FlushPageAsyncPersistsPage) {
  TempDiskFile file("flush_page_async");

  BufferManager buffer_manager(Options(file.path(), 1, 3));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  Page expected;
  FillPageWithPageId(first_page_id, expected);

  first.page() = expected;
  first.MarkDirty();

  std::future<void> flush = buffer_manager.FlushPageAsync(first_page_id);
  flush.get();

  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  PageGuard fetched = buffer_manager.FetchPage(first_page_id);
  ExpectPagesEqual(expected, fetched.page());
}

TEST(BufferManagerAsyncTest, FlushAllPagesAsyncPersistsPages) {
  TempDiskFile file("flush_all_async");

  constexpr std::size_t kPageCount = 4;

  BufferManager buffer_manager(
      Options(file.path(), kPageCount, 2 * kPageCount));

  std::vector<PageId> page_ids;
  std::vector<Page> expected_pages(kPageCount);

  page_ids.reserve(kPageCount);

  for (std::size_t i = 0; i < kPageCount; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    const PageId page_id = guard.page_id();

    FillPageWithPageId(page_id, expected_pages[i]);

    guard.page() = expected_pages[i];
    guard.MarkDirty();

    page_ids.push_back(page_id);
  }

  std::future<void> flush_all = buffer_manager.FlushAllPagesAsync();
  flush_all.get();

  for (std::size_t i = 0; i < kPageCount; ++i) {
    PageGuard guard = buffer_manager.FetchPage(page_ids[i]);
    ExpectPagesEqual(expected_pages[i], guard.page());
  }
}

TEST(BufferManagerAsyncTest, DeletePageAsyncDeletesUnpinnedPage) {
  TempDiskFile file("delete_async");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();
  const PageId page_id = guard.page_id();

  guard.Drop();

  std::future<void> deletion = buffer_manager.DeletePageAsync(page_id);
  deletion.get();

  EXPECT_THROW(
      {
        PageGuard fetched = buffer_manager.FetchPage(page_id);
        (void)fetched;
      },
      std::logic_error);
}

TEST(BufferManagerAsyncTest, DeletePageAsyncPropagatesPinnedPageError) {
  TempDiskFile file("delete_pinned_async");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();
  const PageId page_id = guard.page_id();

  std::future<void> deletion = buffer_manager.DeletePageAsync(page_id);

  EXPECT_THROW(deletion.get(), std::logic_error);
}

TEST(BufferManagerAsyncTest, SyncAsyncCompletesAfterFlush) {
  TempDiskFile file("sync_async");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();
  const PageId page_id = guard.page_id();

  FillPage(guard.page(), std::byte{42});
  guard.MarkDirty();

  buffer_manager.FlushPageAsync(page_id).get();
  buffer_manager.SyncAsync().get();

  SUCCEED();
}

}  // namespace
}  // namespace buffer_manager