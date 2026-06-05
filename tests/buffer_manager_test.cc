#include "buffer_manager/buffer_manager.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

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
           ("buffer_manager_test_" + std::move(name) + "_" +
            std::to_string(pid) + "_" + std::to_string(now) + "_" +
            std::to_string(id) + ".db");
  }

  std::filesystem::path path_;
};

[[nodiscard]] BufferManagerOptions Options(const std::filesystem::path& path,
                                           std::size_t frame_count,
                                           PageId max_page_count) {
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

class NeverVictimReplacer final : public Replacer {
 public:
  void RecordAccess(FrameId /*frame_id*/) override {}

  void SetEvictable(FrameId /*frame_id*/, bool /*evictable*/) override {}

  [[nodiscard]] std::optional<FrameId> Victim() override {
    return std::nullopt;
  }

  void Remove(FrameId /*frame_id*/) override {}

  [[nodiscard]] std::size_t evictable_count() const noexcept override {
    return 0;
  }
};

void RethrowThreadErrors(const std::vector<std::exception_ptr>& errors) {
  if (!errors.empty()) {
    std::rethrow_exception(errors.front());
  }
}

TEST(BufferManagerTest, ConstructorRejectsZeroFrames) {
  TempDiskFile file("zero_frames");

  EXPECT_THROW(BufferManager(Options(file.path(), 0, 8)),
               std::invalid_argument);
}

TEST(BufferManagerTest, ConstructorRejectsZeroMaxPages) {
  TempDiskFile file("zero_max_pages");

  EXPECT_THROW(BufferManager(Options(file.path(), 8, 0)),
               std::invalid_argument);
}

TEST(BufferManagerTest, ReportsFrameCountAndMaxPageCount) {
  TempDiskFile file("reports_counts");

  BufferManager buffer_manager(Options(file.path(), 4, 16));

  EXPECT_EQ(buffer_manager.frame_count(), 4U);
  EXPECT_EQ(buffer_manager.max_page_count(), 16U);
}

TEST(BufferManagerTest, UsesCustomReplacer) {
  TempDiskFile file("custom_replacer");

  BufferManager buffer_manager(Options(file.path(), 1, 2),
                               std::make_unique<NeverVictimReplacer>());

  PageGuard first = buffer_manager.NewPage();
  first.Drop();

  EXPECT_THROW(
      {
        PageGuard second = buffer_manager.NewPage();
        (void)second;
      },
      std::runtime_error);
}

TEST(PageGuardTest, DefaultGuardIsInvalid) {
  PageGuard guard;

  EXPECT_FALSE(guard.valid());
  EXPECT_THROW(static_cast<void>(guard.page_id()), std::logic_error);
  EXPECT_THROW(static_cast<void>(guard.page()), std::logic_error);
  EXPECT_THROW(guard.MarkDirty(), std::logic_error);

  EXPECT_NO_THROW(guard.Drop());
}

TEST(PageGuardTest, MoveConstructorTransfersOwnership) {
  TempDiskFile file("move_constructor");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard original = buffer_manager.NewPage();
  const PageId page_id = original.page_id();

  PageGuard moved(std::move(original));

  EXPECT_FALSE(original.valid());
  ASSERT_TRUE(moved.valid());
  EXPECT_EQ(moved.page_id(), page_id);
}

TEST(PageGuardTest, MoveAssignmentDropsPreviousGuard) {
  TempDiskFile file("move_assignment");

  BufferManager buffer_manager(Options(file.path(), 2, 3));

  PageGuard first = buffer_manager.NewPage();
  PageGuard second = buffer_manager.NewPage();
  const PageId second_page_id = second.page_id();

  first = std::move(second);

  EXPECT_TRUE(first.valid());
  EXPECT_FALSE(second.valid());
  EXPECT_EQ(first.page_id(), second_page_id);

  EXPECT_NO_THROW({
    PageGuard third = buffer_manager.NewPage();
    (void)third;
  });
}

TEST(PageGuardTest, DropInvalidatesGuard) {
  TempDiskFile file("drop_invalidates");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();

  ASSERT_TRUE(guard.valid());

  guard.Drop();

  EXPECT_FALSE(guard.valid());
  EXPECT_THROW(static_cast<void>(guard.page_id()), std::logic_error);
  EXPECT_THROW(static_cast<void>(guard.page()), std::logic_error);
}

TEST(PageGuardTest, DestructorDropsGuard) {
  TempDiskFile file("destructor_drops");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  {
    PageGuard guard = buffer_manager.NewPage();
    ASSERT_TRUE(guard.valid());
  }

  EXPECT_NO_THROW({
    PageGuard guard = buffer_manager.NewPage();
    (void)guard;
  });
}

TEST(BufferManagerTest, NewPageReturnsValidZeroedPage) {
  TempDiskFile file("new_page_zeroed");

  BufferManager buffer_manager(Options(file.path(), 2, 4));

  PageGuard guard = buffer_manager.NewPage();

  ASSERT_TRUE(guard.valid());
  EXPECT_EQ(guard.page_id(), 0U);

  for (std::byte byte : guard.page().data) {
    EXPECT_EQ(byte, std::byte{0});
  }
}

TEST(BufferManagerTest, NewPageAssignsIncreasingPageIds) {
  TempDiskFile file("increasing_page_ids");

  BufferManager buffer_manager(Options(file.path(), 4, 4));

  PageGuard first = buffer_manager.NewPage();
  PageGuard second = buffer_manager.NewPage();
  PageGuard third = buffer_manager.NewPage();

  EXPECT_EQ(first.page_id(), 0U);
  EXPECT_EQ(second.page_id(), 1U);
  EXPECT_EQ(third.page_id(), 2U);
}

TEST(BufferManagerTest, NewPageThrowsWhenMaxPageCountReached) {
  TempDiskFile file("max_page_count");

  BufferManager buffer_manager(Options(file.path(), 1, 1));

  PageGuard guard = buffer_manager.NewPage();

  EXPECT_THROW(
      {
        PageGuard extra = buffer_manager.NewPage();
        (void)extra;
      },
      std::runtime_error);
}

TEST(BufferManagerTest, NewPageRollsBackPageIdIfNoFrameAvailable) {
  TempDiskFile file("rollback_page_id");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();

  EXPECT_THROW(
      {
        PageGuard second = buffer_manager.NewPage();
        (void)second;
      },
      std::runtime_error);

  first.Drop();

  PageGuard second = buffer_manager.NewPage();

  EXPECT_EQ(second.page_id(), 1U);
}

TEST(BufferManagerTest, FetchUnusedPageThrows) {
  TempDiskFile file("fetch_unused");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  EXPECT_THROW(
      {
        PageGuard guard = buffer_manager.FetchPage(0);
        (void)guard;
      },
      std::logic_error);
}

TEST(BufferManagerTest, FetchOutOfRangePageThrows) {
  TempDiskFile file("fetch_out_of_range");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  EXPECT_THROW(
      {
        PageGuard guard = buffer_manager.FetchPage(2);
        (void)guard;
      },
      std::out_of_range);
}

TEST(BufferManagerTest, FetchResidentPageReturnsSameContents) {
  TempDiskFile file("fetch_resident");

  BufferManager buffer_manager(Options(file.path(), 2, 4));

  PageGuard first = buffer_manager.NewPage();
  const PageId page_id = first.page_id();

  Page expected;
  FillPageWithPageId(page_id, expected);

  first.page() = expected;
  first.MarkDirty();

  PageGuard second = buffer_manager.FetchPage(page_id);

  ExpectPagesEqual(expected, second.page());
}

TEST(BufferManagerTest, MultipleGuardsPinSameResidentPage) {
  TempDiskFile file("multiple_guards_pin");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();
  PageGuard second = buffer_manager.FetchPage(first.page_id());

  first.Drop();

  EXPECT_THROW(
      {
        PageGuard other = buffer_manager.NewPage();
        (void)other;
      },
      std::runtime_error);

  second.Drop();

  EXPECT_NO_THROW({
    PageGuard other = buffer_manager.NewPage();
    (void)other;
  });
}

TEST(BufferManagerTest, GuardDropMakesPageEvictable) {
  TempDiskFile file("drop_makes_evictable");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();

  first.Drop();

  EXPECT_NO_THROW({
    PageGuard second = buffer_manager.NewPage();
    (void)second;
  });
}

TEST(BufferManagerTest, DoesNotEvictPinnedPage) {
  TempDiskFile file("does_not_evict_pinned");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard pinned = buffer_manager.NewPage();

  EXPECT_THROW(
      {
        PageGuard other = buffer_manager.NewPage();
        (void)other;
      },
      std::runtime_error);
}

TEST(BufferManagerTest, ThrowsWhenAllFramesPinned) {
  TempDiskFile file("all_frames_pinned");

  BufferManager buffer_manager(Options(file.path(), 2, 3));

  PageGuard first = buffer_manager.NewPage();
  PageGuard second = buffer_manager.NewPage();

  EXPECT_THROW(
      {
        PageGuard third = buffer_manager.NewPage();
        (void)third;
      },
      std::runtime_error);
}

TEST(BufferManagerTest, DirtyPageIsWrittenBeforeEviction) {
  TempDiskFile file("dirty_eviction");

  BufferManager buffer_manager(Options(file.path(), 1, 3));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  Page expected;
  FillPageWithPageId(first_page_id, expected);

  first.page() = expected;
  first.MarkDirty();
  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  PageGuard fetched = buffer_manager.FetchPage(first_page_id);

  ExpectPagesEqual(expected, fetched.page());
}

TEST(BufferManagerTest, FlushPagePersistsPageAcrossEviction) {
  TempDiskFile file("flush_page");

  BufferManager buffer_manager(Options(file.path(), 1, 3));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  Page expected;
  FillPageWithPageId(first_page_id, expected);

  first.page() = expected;
  first.MarkDirty();

  buffer_manager.FlushPage(first_page_id);

  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  PageGuard fetched = buffer_manager.FetchPage(first_page_id);

  ExpectPagesEqual(expected, fetched.page());
}

TEST(BufferManagerTest, FlushCleanPageIsNoOp) {
  TempDiskFile file("flush_clean");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();
  const PageId page_id = guard.page_id();

  buffer_manager.FlushPage(page_id);

  EXPECT_NO_THROW(buffer_manager.FlushPage(page_id));
}

TEST(BufferManagerTest, FlushNonResidentPageIsNoOp) {
  TempDiskFile file("flush_nonresident");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  EXPECT_NO_THROW(buffer_manager.FlushPage(first_page_id));
}

TEST(BufferManagerTest, FlushUnusedPageThrows) {
  TempDiskFile file("flush_unused");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  EXPECT_THROW(buffer_manager.FlushPage(0), std::logic_error);
}

TEST(BufferManagerTest, FlushAllPagesPersistsMultiplePages) {
  TempDiskFile file("flush_all");

  constexpr std::size_t kFrameCount = 3;
  constexpr PageId kMaxPageCount = 6;

  BufferManager buffer_manager(
      Options(file.path(), kFrameCount, kMaxPageCount));

  std::vector<PageId> page_ids;
  std::vector<Page> expected_pages;

  page_ids.reserve(kFrameCount);
  expected_pages.resize(kFrameCount);

  for (std::size_t i = 0; i < kFrameCount; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    const PageId page_id = guard.page_id();

    FillPageWithPageId(page_id, expected_pages[i]);

    guard.page() = expected_pages[i];
    guard.MarkDirty();

    page_ids.push_back(page_id);
  }

  buffer_manager.FlushAllPages();

  for (std::size_t i = 0; i < kFrameCount; ++i) {
    PageGuard extra = buffer_manager.NewPage();
    extra.Drop();
  }

  for (std::size_t i = 0; i < page_ids.size(); ++i) {
    PageGuard fetched = buffer_manager.FetchPage(page_ids[i]);
    ExpectPagesEqual(expected_pages[i], fetched.page());
  }
}

TEST(BufferManagerTest, DeleteUnusedPageIsNoOp) {
  TempDiskFile file("delete_unused");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  EXPECT_NO_THROW(buffer_manager.DeletePage(0));
}

TEST(BufferManagerTest, DeletePinnedPageThrows) {
  TempDiskFile file("delete_pinned");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard guard = buffer_manager.NewPage();

  EXPECT_THROW(buffer_manager.DeletePage(guard.page_id()), std::logic_error);
}

TEST(BufferManagerTest, DeleteResidentUnpinnedPageFreesFrameAndPageId) {
  TempDiskFile file("delete_resident_unpinned");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  first.Drop();

  buffer_manager.DeletePage(first_page_id);

  EXPECT_THROW(
      {
        PageGuard fetched = buffer_manager.FetchPage(first_page_id);
        (void)fetched;
      },
      std::logic_error);

  PageGuard reused = buffer_manager.NewPage();

  EXPECT_EQ(reused.page_id(), first_page_id);
}

TEST(BufferManagerTest, DeleteNonResidentPageFreesPageId) {
  TempDiskFile file("delete_nonresident");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();
  first.Drop();

  PageGuard second = buffer_manager.NewPage();
  second.Drop();

  buffer_manager.DeletePage(first_page_id);

  PageGuard reused = buffer_manager.NewPage();

  EXPECT_EQ(reused.page_id(), first_page_id);
}

TEST(BufferManagerTest, ReusedFrameForNewPageIsZeroed) {
  TempDiskFile file("reused_frame_zeroed");

  BufferManager buffer_manager(Options(file.path(), 1, 2));

  PageGuard first = buffer_manager.NewPage();
  const PageId first_page_id = first.page_id();

  FillPage(first.page(), std::byte{0xAA});
  first.MarkDirty();
  first.Drop();

  buffer_manager.DeletePage(first_page_id);

  PageGuard reused = buffer_manager.NewPage();

  for (std::byte byte : reused.page().data) {
    EXPECT_EQ(byte, std::byte{0});
  }
}

TEST(BufferManagerConcurrencyTest, ConcurrentNewPageAllocatesUniquePageIds) {
  TempDiskFile file("concurrent_new_page");

  constexpr std::size_t kThreadCount = 8;

  BufferManager buffer_manager(
      Options(file.path(), kThreadCount, kThreadCount));

  std::mutex mutex;
  std::vector<PageId> page_ids;
  std::vector<std::exception_ptr> errors;
  std::vector<std::thread> threads;

  page_ids.reserve(kThreadCount);
  errors.reserve(kThreadCount);
  threads.reserve(kThreadCount);

  for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&] {
      try {
        PageGuard guard = buffer_manager.NewPage();

        {
          std::scoped_lock lock(mutex);
          page_ids.push_back(guard.page_id());
        }
      } catch (...) {
        std::scoped_lock lock(mutex);
        errors.push_back(std::current_exception());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  RethrowThreadErrors(errors);

  std::unordered_set<PageId> unique_ids(page_ids.begin(), page_ids.end());

  EXPECT_EQ(page_ids.size(), kThreadCount);
  EXPECT_EQ(unique_ids.size(), kThreadCount);
}

TEST(BufferManagerConcurrencyTest, ConcurrentFetchSameResidentPage) {
  TempDiskFile file("concurrent_fetch_same");

  constexpr std::size_t kThreadCount = 8;
  constexpr std::size_t kIterations = 100;

  BufferManager buffer_manager(Options(file.path(), 1, 1));

  PageGuard initial = buffer_manager.NewPage();
  const PageId page_id = initial.page_id();

  Page expected;
  FillPageWithPageId(page_id, expected);

  initial.page() = expected;
  initial.MarkDirty();
  initial.Drop();

  std::mutex error_mutex;
  std::vector<std::exception_ptr> errors;
  std::vector<std::thread> threads;

  errors.reserve(kThreadCount);
  threads.reserve(kThreadCount);

  for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&] {
      try {
        for (std::size_t i = 0; i < kIterations; ++i) {
          PageGuard guard = buffer_manager.FetchPage(page_id);
          ExpectPagesEqual(expected, guard.page());
        }
      } catch (...) {
        std::scoped_lock lock(error_mutex);
        errors.push_back(std::current_exception());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  RethrowThreadErrors(errors);
}

TEST(BufferManagerConcurrencyTest, ConcurrentFetchDifferentResidentPages) {
  TempDiskFile file("concurrent_fetch_different");

  constexpr std::size_t kThreadCount = 8;
  constexpr std::size_t kIterations = 100;

  BufferManager buffer_manager(
      Options(file.path(), kThreadCount, kThreadCount));

  std::vector<PageId> page_ids;
  std::vector<Page> expected_pages(kThreadCount);

  page_ids.reserve(kThreadCount);

  for (std::size_t i = 0; i < kThreadCount; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    const PageId page_id = guard.page_id();

    FillPageWithPageId(page_id, expected_pages[i]);

    guard.page() = expected_pages[i];
    guard.MarkDirty();

    page_ids.push_back(page_id);
  }

  std::mutex error_mutex;
  std::vector<std::exception_ptr> errors;
  std::vector<std::thread> threads;

  errors.reserve(kThreadCount);
  threads.reserve(kThreadCount);

  for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      try {
        for (std::size_t i = 0; i < kIterations; ++i) {
          PageGuard guard = buffer_manager.FetchPage(page_ids[thread_id]);
          ExpectPagesEqual(expected_pages[thread_id], guard.page());
        }
      } catch (...) {
        std::scoped_lock lock(error_mutex);
        errors.push_back(std::current_exception());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  RethrowThreadErrors(errors);
}

TEST(BufferManagerConcurrencyTest, ConcurrentDirtyWritesDifferentPages) {
  TempDiskFile file("concurrent_dirty_writes");

  constexpr std::size_t kThreadCount = 8;

  BufferManager buffer_manager(
      Options(file.path(), kThreadCount, kThreadCount));

  std::vector<PageId> page_ids;
  std::vector<Page> expected_pages(kThreadCount);

  page_ids.reserve(kThreadCount);

  for (std::size_t i = 0; i < kThreadCount; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    page_ids.push_back(guard.page_id());
  }

  std::mutex error_mutex;
  std::vector<std::exception_ptr> errors;
  std::vector<std::thread> threads;

  errors.reserve(kThreadCount);
  threads.reserve(kThreadCount);

  for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      try {
        PageGuard guard = buffer_manager.FetchPage(page_ids[thread_id]);

        FillPage(guard.page(), static_cast<std::byte>(thread_id + 1));
        guard.MarkDirty();

        FillPage(expected_pages[thread_id],
                 static_cast<std::byte>(thread_id + 1));
      } catch (...) {
        std::scoped_lock lock(error_mutex);
        errors.push_back(std::current_exception());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  RethrowThreadErrors(errors);

  buffer_manager.FlushAllPages();

  for (std::size_t i = 0; i < kThreadCount; ++i) {
    PageGuard guard = buffer_manager.FetchPage(page_ids[i]);
    ExpectPagesEqual(expected_pages[i], guard.page());
  }
}

TEST(BufferManagerConcurrencyTest, ConcurrentFetchWithEviction) {
  TempDiskFile file("concurrent_fetch_eviction");

  constexpr std::size_t kFrameCount = 4;
  constexpr std::size_t kPageCount = 16;
  constexpr std::size_t kThreadCount = 4;

  BufferManager buffer_manager(Options(file.path(), kFrameCount, kPageCount));

  std::vector<PageId> page_ids;
  std::vector<Page> expected_pages(kPageCount);

  page_ids.reserve(kPageCount);

  for (std::size_t i = 0; i < kPageCount; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    const PageId page_id = guard.page_id();

    FillPageWithPageId(page_id, expected_pages[i]);

    guard.page() = expected_pages[i];
    guard.MarkDirty();
    guard.Drop();

    page_ids.push_back(page_id);
  }

  std::mutex error_mutex;
  std::vector<std::exception_ptr> errors;
  std::vector<std::thread> threads;

  errors.reserve(kThreadCount);
  threads.reserve(kThreadCount);

  for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
    threads.emplace_back([&, thread_id] {
      try {
        for (std::size_t index = thread_id; index < kPageCount;
             index += kThreadCount) {
          PageGuard guard = buffer_manager.FetchPage(page_ids[index]);
          ExpectPagesEqual(expected_pages[index], guard.page());
        }
      } catch (...) {
        std::scoped_lock lock(error_mutex);
        errors.push_back(std::current_exception());
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  RethrowThreadErrors(errors);
}

}  // namespace
}  // namespace buffer_manager