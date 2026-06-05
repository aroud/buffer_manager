#include "storage/disk_manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
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

    return std::filesystem::temp_directory_path() /
           ("buffer_manager_" + std::move(name) + "_" + std::to_string(now) +
            "_" + std::to_string(id) + ".db");
  }

  std::filesystem::path path_;
};

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
  for (std::size_t i = 0; i < kPageSize; ++i) {
    EXPECT_EQ(expected.data[i], actual.data[i]) << "mismatch at byte " << i;
  }
}

bool IsZeroPage(const Page& page) {
  return std::ranges::all_of(
      page.data, [](std::byte value) { return value == std::byte{0}; });
}

void ReadRawPageAt(const std::filesystem::path& path, std::uint64_t offset,
                   Page* page) {
  ASSERT_NE(page, nullptr);

  std::ifstream input(path, std::ios::binary);
  ASSERT_TRUE(input.is_open());

  input.seekg(static_cast<std::streamoff>(offset));
  ASSERT_TRUE(input.good());

  input.read(reinterpret_cast<char*>(page->data.data()),
             static_cast<std::streamsize>(page->data.size()));
  ASSERT_EQ(input.gcount(), static_cast<std::streamsize>(page->data.size()));
}

DiskManagerOptions DirectOptions(const std::filesystem::path& path,
                                 PageId max_page_count) {
  return {
      .path = path,
      .max_page_count = max_page_count,
      .truncate_existing = true,
      .io_mode = IoMode::kDirect,
  };
}

TEST(DiskManagerTest, PageIsAlignedForDirectIo) {
  Page page;
  const auto address = reinterpret_cast<std::uintptr_t>(page.data.data());

  EXPECT_EQ(address % kPageSize, 0);
  EXPECT_EQ(sizeof(Page), kPageSize);
  EXPECT_EQ(alignof(Page), kPageSize);
}

TEST(DiskManagerTest, CreatesFileAndStoresPagesAfterHeader) {
  TempDiskFile file("layout");

  DiskManager disk(DirectOptions(file.path(), 8));

  Page first_page;
  Page second_page;
  FillPage(first_page, std::byte{11});
  FillPage(second_page, std::byte{22});

  disk.WritePage(0, first_page);
  disk.WritePage(1, second_page);
  disk.Sync();

  Page raw_first_page;
  Page raw_second_page;
  ReadRawPageAt(file.path(), kPageSize, &raw_first_page);
  ReadRawPageAt(file.path(), 2 * kPageSize, &raw_second_page);

  EXPECT_TRUE(std::filesystem::exists(file.path()));
  ExpectPagesEqual(first_page, raw_first_page);
  ExpectPagesEqual(second_page, raw_second_page);
}

TEST(DiskManagerTest, WriteThenReadPage) {
  TempDiskFile file("write_read");
  DiskManager disk(DirectOptions(file.path(), 16));

  Page written;
  Page read;

  FillPage(written, std::byte{42});

  disk.WritePage(3, written);
  disk.ReadPage(3, &read);

  ExpectPagesEqual(written, read);
}

TEST(DiskManagerTest, ReopenPreservesPageData) {
  TempDiskFile file("reopen");

  Page expected;
  FillPageWithPageId(5, expected);

  {
    DiskManager disk(DirectOptions(file.path(), 16));
    disk.WritePage(5, expected);
    disk.Sync();
  }

  {
    DiskManager disk({
        .path = file.path(),
        .max_page_count = 16,
        .io_mode = IoMode::kDirect,
    });

    Page actual;
    disk.ReadPage(5, &actual);

    ExpectPagesEqual(expected, actual);
  }
}

TEST(DiskManagerTest, ReadUnwrittenPageReturnsZeroPage) {
  TempDiskFile file("zero_read");
  DiskManager disk(DirectOptions(file.path(), 16));

  Page page;
  FillPage(page, std::byte{123});

  disk.ReadPage(9, &page);

  EXPECT_TRUE(IsZeroPage(page));
}

TEST(DiskManagerTest, RejectsOutOfRangePageIds) {
  TempDiskFile file("out_of_range");
  DiskManager disk(DirectOptions(file.path(), 2));

  Page page;

  EXPECT_NO_THROW(disk.ReadPage(0, &page));
  EXPECT_NO_THROW(disk.WritePage(1, page));

  EXPECT_THROW(disk.ReadPage(2, &page), std::out_of_range);
  EXPECT_THROW(disk.WritePage(2, page), std::out_of_range);
}

TEST(DiskManagerTest, RejectsNullReadPagePointer) {
  TempDiskFile file("null_read");
  DiskManager disk(DirectOptions(file.path(), 4));

  EXPECT_THROW(disk.ReadPage(0, nullptr), std::invalid_argument);
}

TEST(DiskManagerTest, TruncateExistingCreatesFreshFile) {
  TempDiskFile file("truncate");

  {
    DiskManager disk(DirectOptions(file.path(), 8));

    Page page;
    FillPage(page, std::byte{88});
    disk.WritePage(1, page);
    disk.Sync();
  }

  {
    DiskManager disk(DirectOptions(file.path(), 8));

    Page page;
    disk.ReadPage(1, &page);

    EXPECT_TRUE(IsZeroPage(page));
  }
}

TEST(DiskManagerTest, RejectsExistingFileWithDifferentMaxPageCount) {
  TempDiskFile file("wrong_max_pages");

  {
    DiskManager disk(DirectOptions(file.path(), 8));
  }

  EXPECT_THROW(DiskManager({
                   .path = file.path(),
                   .max_page_count = 16,
               }),
               std::runtime_error);
}

TEST(DiskManagerTest, RejectsCorruptedHeader) {
  TempDiskFile file("corrupt_header");

  {
    std::ofstream out(file.path(), std::ios::binary);
    ASSERT_TRUE(out.is_open());
    out << "not a buffer manager database";
  }

  EXPECT_THROW(DiskManager({
                   .path = file.path(),
                   .max_page_count = 8,
               }),
               std::runtime_error);
}

TEST(DiskManagerTest, PreallocateCreatesExpectedFileSize) {
  TempDiskFile file("preallocate");

  constexpr PageId kMaxPages = 5;

  DiskManager disk({
      .path = file.path(),
      .max_page_count = kMaxPages,
      .truncate_existing = true,
      .preallocate = true,
      .io_mode = IoMode::kDirect,
  });

  const auto expected_size =
      static_cast<std::uintmax_t>(kPageSize * (kMaxPages + 1));

  EXPECT_EQ(std::filesystem::file_size(file.path()), expected_size);
}

TEST(DiskManagerTest, DefaultBufferedModeAlsoWorks) {
  TempDiskFile file("default_buffered");

  DiskManager disk({
      .path = file.path(),
      .max_page_count = 8,
      .truncate_existing = true,
  });

  Page written;
  Page read;

  FillPage(written, std::byte{17});

  disk.WritePage(2, written);
  disk.ReadPage(2, &read);

  ExpectPagesEqual(written, read);
}

TEST(DiskManagerTest, ConcurrentWritesToDifferentPagesAreReadable) {
  TempDiskFile file("concurrent_writes");

  constexpr PageId kPageCount = 128;
  constexpr std::size_t kThreadCount = 8;

  DiskManager disk(DirectOptions(file.path(), kPageCount));

  std::mutex error_mutex;
  std::vector<std::exception_ptr> errors;
  errors.reserve(kThreadCount);

  {
    std::vector<std::jthread> threads;
    threads.reserve(kThreadCount);

    for (std::size_t thread_id = 0; thread_id < kThreadCount; ++thread_id) {
      threads.emplace_back([&, thread_id] {
        try {
          for (auto page_id = static_cast<PageId>(thread_id);
               page_id < kPageCount; page_id += kThreadCount) {
            Page page;
            FillPageWithPageId(page_id, page);
            disk.WritePage(page_id, page);
          }
        } catch (...) {
          std::scoped_lock lock(error_mutex);
          errors.push_back(std::current_exception());
        }
      });
    }
  }

  ASSERT_TRUE(errors.empty());

  for (PageId page_id = 0; page_id < kPageCount; ++page_id) {
    Page expected;
    Page actual;

    FillPageWithPageId(page_id, expected);
    disk.ReadPage(page_id, &actual);

    ExpectPagesEqual(expected, actual);
  }
}

}  // namespace
}  // namespace buffer_manager