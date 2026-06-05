#include <benchmark/benchmark.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "buffer_manager/buffer_manager.h"

namespace buffer_manager {
namespace {

constexpr std::int64_t kResidentPageCount = 1024;

constexpr std::int64_t kSmallFrameCount = 1024;
constexpr std::int64_t kSmallPageCount = 1024;

constexpr std::int64_t kScanFrameCount = 1024;
constexpr std::int64_t kScanPageCount = 16 * 1024;

constexpr std::int64_t kFlushPageCount = 1024;

constexpr std::size_t kRandomRequestCount = 1 << 16;

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
           ("buffer_manager_bench_" + std::move(name) + "_" +
            std::to_string(pid) + "_" + std::to_string(now) + "_" +
            std::to_string(id) + ".db");
  }

  std::filesystem::path path_;
};

[[nodiscard]] BufferManagerOptions MakeOptions(
    const std::filesystem::path& path, std::size_t frame_count,
    PageId max_page_count, IoMode io_mode) {
  return {
      .disk =
          {
              .path = path,
              .max_page_count = max_page_count,
              .truncate_existing = true,
              .preallocate = true,
              .io_mode = io_mode,
          },
      .frame_count = frame_count,
  };
}

void FillPageWithPageId(PageId page_id, Page& page) {
  for (std::size_t i = 0; i < page.data.size(); ++i) {
    const auto value = static_cast<unsigned char>((page_id + i) % 251);
    page.data[i] = std::byte{value};
  }
}

void TouchPage(Page& page, PageId page_id, std::size_t iteration) {
  const auto value = static_cast<unsigned char>((page_id + iteration) % 251);
  page.data[0] = std::byte{value};
}

std::vector<PageId> MakeRandomPageIds(std::size_t page_count,
                                      std::size_t request_count) {
  std::mt19937_64 rng{123456789};
  std::uniform_int_distribution<PageId> distribution{
      0, static_cast<PageId>(page_count - 1)};

  std::vector<PageId> page_ids;
  page_ids.reserve(request_count);

  for (std::size_t i = 0; i < request_count; ++i) {
    page_ids.push_back(distribution(rng));
  }

  return page_ids;
}

std::vector<PageId> CreateDirtyPages(BufferManager& buffer_manager,
                                     std::size_t page_count) {
  std::vector<PageId> page_ids;
  page_ids.reserve(page_count);

  for (std::size_t i = 0; i < page_count; ++i) {
    PageGuard guard = buffer_manager.NewPage();
    const PageId page_id = guard.page_id();

    FillPageWithPageId(page_id, guard.page());
    guard.MarkDirty();

    page_ids.push_back(page_id);
  }

  return page_ids;
}

void ReportPageOperations(benchmark::State& state,
                          std::int64_t page_operations) {
  state.SetBytesProcessed(page_operations *
                          static_cast<std::int64_t>(kPageSize));
  state.counters["pages/s"] = benchmark::Counter(
      static_cast<double>(page_operations), benchmark::Counter::kIsRate);
}

void BM_BufferManagerFetchResident(benchmark::State& state) {
  const auto page_count = static_cast<std::size_t>(state.range(0));

  state.PauseTiming();

  TempDiskFile file("bm_fetch_resident");
  BufferManager buffer_manager(MakeOptions(file.path(), page_count,
                                           static_cast<PageId>(page_count),
                                           IoMode::kBuffered));

  const std::vector<PageId> page_ids =
      CreateDirtyPages(buffer_manager, page_count);

  std::size_t page_index = 0;

  state.ResumeTiming();

  for (auto _ : state) {
    PageGuard guard = buffer_manager.FetchPage(page_ids[page_index]);
    benchmark::DoNotOptimize(guard.page().data.data());

    ++page_index;
    if (page_index == page_ids.size()) {
      page_index = 0;
    }
  }

  ReportPageOperations(state, state.iterations());
}

void BM_BufferManagerNewPageNoEviction(benchmark::State& state) {
  const auto page_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();

    TempDiskFile file("bm_new_no_eviction");
    BufferManager buffer_manager(MakeOptions(file.path(), page_count,
                                             static_cast<PageId>(page_count),
                                             IoMode::kBuffered));

    std::vector<PageGuard> guards;
    guards.reserve(page_count);

    state.ResumeTiming();

    for (std::size_t i = 0; i < page_count; ++i) {
      PageGuard guard = buffer_manager.NewPage();
      benchmark::DoNotOptimize(guard.page_id());
      guards.push_back(std::move(guard));
    }

    state.PauseTiming();
  }

  state.ResumeTiming();

  const auto operations =
      state.iterations() * static_cast<std::int64_t>(page_count);
  ReportPageOperations(state, operations);
}

void BM_BufferManagerNewPageWithEviction(benchmark::State& state) {
  const auto frame_count = static_cast<std::size_t>(state.range(0));
  const auto page_count = static_cast<std::size_t>(state.range(1));

  for (auto _ : state) {
    state.PauseTiming();

    TempDiskFile file("bm_new_with_eviction");
    BufferManager buffer_manager(MakeOptions(file.path(), frame_count,
                                             static_cast<PageId>(page_count),
                                             IoMode::kBuffered));

    state.ResumeTiming();

    for (std::size_t i = 0; i < page_count; ++i) {
      PageGuard guard = buffer_manager.NewPage();
      FillPageWithPageId(guard.page_id(), guard.page());
      guard.MarkDirty();
      benchmark::DoNotOptimize(guard.page().data.data());
    }

    state.PauseTiming();
  }

  state.ResumeTiming();

  const auto operations =
      state.iterations() * static_cast<std::int64_t>(page_count);
  ReportPageOperations(state, operations);
}

void BenchmarkSequentialScan(benchmark::State& state, IoMode io_mode,
                             std::string_view name) {
  const auto frame_count = static_cast<std::size_t>(state.range(0));
  const auto page_count = static_cast<std::size_t>(state.range(1));

  state.PauseTiming();

  TempDiskFile file(std::string{name});
  BufferManager buffer_manager(MakeOptions(
      file.path(), frame_count, static_cast<PageId>(page_count), io_mode));

  const std::vector<PageId> page_ids =
      CreateDirtyPages(buffer_manager, page_count);

  std::size_t page_index = 0;

  state.ResumeTiming();

  for (auto _ : state) {
    PageGuard guard = buffer_manager.FetchPage(page_ids[page_index]);
    benchmark::DoNotOptimize(guard.page().data.data());

    ++page_index;
    if (page_index == page_ids.size()) {
      page_index = 0;
    }
  }

  ReportPageOperations(state, state.iterations());
}

void BenchmarkRandomRead(benchmark::State& state, IoMode io_mode,
                         std::string_view name) {
  const auto frame_count = static_cast<std::size_t>(state.range(0));
  const auto page_count = static_cast<std::size_t>(state.range(1));

  state.PauseTiming();

  TempDiskFile file(std::string{name});
  BufferManager buffer_manager(MakeOptions(
      file.path(), frame_count, static_cast<PageId>(page_count), io_mode));

  CreateDirtyPages(buffer_manager, page_count);

  const std::vector<PageId> page_ids =
      MakeRandomPageIds(page_count, kRandomRequestCount);

  std::size_t request_index = 0;

  state.ResumeTiming();

  for (auto _ : state) {
    PageGuard guard = buffer_manager.FetchPage(page_ids[request_index]);
    benchmark::DoNotOptimize(guard.page().data.data());

    ++request_index;
    if (request_index == page_ids.size()) {
      request_index = 0;
    }
  }

  ReportPageOperations(state, state.iterations());
}

void BenchmarkRandomWriteDirty(benchmark::State& state, IoMode io_mode,
                               std::string_view name) {
  const auto frame_count = static_cast<std::size_t>(state.range(0));
  const auto page_count = static_cast<std::size_t>(state.range(1));

  state.PauseTiming();

  TempDiskFile file(std::string{name});
  BufferManager buffer_manager(MakeOptions(
      file.path(), frame_count, static_cast<PageId>(page_count), io_mode));

  CreateDirtyPages(buffer_manager, page_count);

  const std::vector<PageId> page_ids =
      MakeRandomPageIds(page_count, kRandomRequestCount);

  std::size_t request_index = 0;

  state.ResumeTiming();

  for (auto _ : state) {
    const PageId page_id = page_ids[request_index];

    PageGuard guard = buffer_manager.FetchPage(page_id);
    TouchPage(guard.page(), page_id,
              static_cast<std::size_t>(state.iterations()));
    guard.MarkDirty();

    benchmark::DoNotOptimize(guard.page().data.data());
    benchmark::ClobberMemory();

    ++request_index;
    if (request_index == page_ids.size()) {
      request_index = 0;
    }
  }

  ReportPageOperations(state, state.iterations());
}

void BenchmarkFlushAllDirty(benchmark::State& state, IoMode io_mode, bool sync,
                            std::string_view name) {
  const auto page_count = static_cast<std::size_t>(state.range(0));

  for (auto _ : state) {
    state.PauseTiming();

    TempDiskFile file(std::string{name});
    BufferManager buffer_manager(MakeOptions(
        file.path(), page_count, static_cast<PageId>(page_count), io_mode));

    CreateDirtyPages(buffer_manager, page_count);

    state.ResumeTiming();

    buffer_manager.FlushAllPages();

    if (sync) {
      buffer_manager.Sync();
    }

    state.PauseTiming();
  }

  state.ResumeTiming();

  const auto operations =
      state.iterations() * static_cast<std::int64_t>(page_count);
  ReportPageOperations(state, operations);
}

void BM_BufferManagerSequentialScanBuffered(benchmark::State& state) {
  BenchmarkSequentialScan(state, IoMode::kBuffered, "bm_seq_scan_buffered");
}

void BM_BufferManagerSequentialScanDirect(benchmark::State& state) {
  BenchmarkSequentialScan(state, IoMode::kDirect, "bm_seq_scan_direct");
}

void BM_BufferManagerRandomReadBuffered(benchmark::State& state) {
  BenchmarkRandomRead(state, IoMode::kBuffered, "bm_random_read_buffered");
}

void BM_BufferManagerRandomReadDirect(benchmark::State& state) {
  BenchmarkRandomRead(state, IoMode::kDirect, "bm_random_read_direct");
}

void BM_BufferManagerRandomWriteDirtyBuffered(benchmark::State& state) {
  BenchmarkRandomWriteDirty(state, IoMode::kBuffered,
                            "bm_random_write_dirty_buffered");
}

void BM_BufferManagerRandomWriteDirtyDirect(benchmark::State& state) {
  BenchmarkRandomWriteDirty(state, IoMode::kDirect,
                            "bm_random_write_dirty_direct");
}

void BM_BufferManagerFlushAllDirtyBuffered(benchmark::State& state) {
  BenchmarkFlushAllDirty(state, IoMode::kBuffered, /*sync=*/false,
                         "bm_flush_all_dirty_buffered");
}

void BM_BufferManagerFlushAllDirtyDirect(benchmark::State& state) {
  BenchmarkFlushAllDirty(state, IoMode::kDirect, /*sync=*/false,
                         "bm_flush_all_dirty_direct");
}

void BM_BufferManagerFlushAllDirtyAndSyncBuffered(benchmark::State& state) {
  BenchmarkFlushAllDirty(state, IoMode::kBuffered, /*sync=*/true,
                         "bm_flush_all_dirty_sync_buffered");
}

void BM_BufferManagerFlushAllDirtyAndSyncDirect(benchmark::State& state) {
  BenchmarkFlushAllDirty(state, IoMode::kDirect, /*sync=*/true,
                         "bm_flush_all_dirty_sync_direct");
}

BENCHMARK(BM_BufferManagerFetchResident)
    ->Arg(kResidentPageCount)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_BufferManagerNewPageNoEviction)
    ->Arg(kSmallPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerNewPageWithEviction)
    ->Args({kSmallFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerSequentialScanBuffered)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerSequentialScanDirect)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerRandomReadBuffered)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerRandomReadDirect)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerRandomWriteDirtyBuffered)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerRandomWriteDirtyDirect)
    ->Args({kScanFrameCount, kScanPageCount})
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerFlushAllDirtyBuffered)
    ->Arg(kFlushPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerFlushAllDirtyDirect)
    ->Arg(kFlushPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerFlushAllDirtyAndSyncBuffered)
    ->Arg(kFlushPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_BufferManagerFlushAllDirtyAndSyncDirect)
    ->Arg(kFlushPageCount)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace buffer_manager