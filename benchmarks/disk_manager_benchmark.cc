#include <benchmark/benchmark.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "storage/disk_manager.h"

namespace buffer_manager {
namespace {

constexpr std::int64_t kSmallBenchmarkPageCount = 1024;
constexpr std::int64_t kLargeBenchmarkPageCount = 16 * kSmallBenchmarkPageCount;

// 1,048,576 pages * 4 KiB = 4 GiB.
constexpr std::int64_t kHugeBenchmarkPageCount = std::int64_t{1024} * 1024;
constexpr std::int64_t kHugeBenchmarkIterations = 1024;

constexpr std::size_t kRandomRequestCount = std::size_t{1} << 16;

// Used by the regular AfterCachePollution benchmarks.
// 64K pages * 4 KiB = 256 MiB.
constexpr std::size_t kCachePollutionPageCount = std::size_t{64} * 1024;

// Used by 4 GiB read benchmarks to reduce the chance that buffered reads
// only measure pages left hot by the prewrite phase.
constexpr std::size_t kHugeCachePollutionPageCount =
    static_cast<std::size_t>(kHugeBenchmarkPageCount);

class TempDiskFile final {
 public:
  explicit TempDiskFile(std::string_view name) : path_(MakePath(name)) {}

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
  static std::filesystem::path MakePath(std::string_view name) {
    static std::atomic<std::uint64_t> counter{0};

    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto id = counter.fetch_add(1, std::memory_order_relaxed);
    const auto pid = ::getpid();

    std::string filename = "buffer_manager_bench_";
    filename.append(name);
    filename += "_";
    filename += std::to_string(pid);
    filename += "_";
    filename += std::to_string(now);
    filename += "_";
    filename += std::to_string(id);
    filename += ".db";

    return std::filesystem::temp_directory_path() / filename;
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

[[nodiscard]] std::size_t PageCountFromState(const benchmark::State& state) {
  const std::int64_t page_count = state.range(0);

  if (page_count <= 0) {
    throw std::invalid_argument("benchmark page count must be positive");
  }

  return static_cast<std::size_t>(page_count);
}

std::vector<PageId> MakeRandomPageIds(std::size_t page_count,
                                      std::size_t request_count) {
  if (page_count == 0) {
    throw std::invalid_argument("page_count must be positive");
  }

  const auto max_page_id = static_cast<PageId>(page_count - 1);

  std::mt19937_64 rng{123456789};
  std::uniform_int_distribution<PageId> distribution{0, max_page_id};

  std::vector<PageId> page_ids;
  page_ids.reserve(request_count);

  for (std::size_t i = 0; i < request_count; ++i) {
    page_ids.push_back(distribution(rng));
  }

  return page_ids;
}

DiskManagerOptions MakeOptions(const std::filesystem::path& path,
                               std::size_t page_count, IoMode io_mode,
                               bool truncate_existing, bool preallocate) {
  return {
      .path = path,
      .max_page_count = static_cast<PageId>(page_count),
      .truncate_existing = truncate_existing,
      .preallocate = preallocate,
      .io_mode = io_mode,
  };
}

void PrewritePages(DiskManager& disk, std::size_t page_count) {
  Page page;

  for (std::size_t page_number = 0; page_number < page_count; ++page_number) {
    const auto page_id = static_cast<PageId>(page_number);
    FillPageWithPageId(page_id, page);
    disk.WritePage(page_id, page);
  }

  disk.Sync();
}

void PolluteOsCache(std::size_t page_count) {
  TempDiskFile file("cache_pollution");

  DiskManager disk(
      MakeOptions(file.path(), page_count, IoMode::kBuffered, true, true));

  Page page;
  FillPage(page, std::byte{0x5A});

  for (std::size_t page_number = 0; page_number < page_count; ++page_number) {
    disk.WritePage(static_cast<PageId>(page_number), page);
  }

  disk.Sync();

  for (std::size_t page_number = 0; page_number < page_count; ++page_number) {
    disk.ReadPage(static_cast<PageId>(page_number), &page);
    benchmark::DoNotOptimize(page.data.data());
  }

  benchmark::ClobberMemory();
}

void PolluteOsCache() {
  PolluteOsCache(kCachePollutionPageCount);
}

void PolluteOsCacheLarge() {
  PolluteOsCache(kHugeCachePollutionPageCount);
}

void ReportPageThroughput(benchmark::State& state) {
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(kPageSize));
  state.counters["pages/s"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
}

void BenchmarkSequentialWrite(benchmark::State& state, IoMode io_mode,
                              std::string_view name) {
  const std::size_t page_count = PageCountFromState(state);
  const auto page_id_limit = static_cast<PageId>(page_count);

  std::optional<TempDiskFile> file;
  std::optional<DiskManager> disk;
  Page page;
  FillPage(page, std::byte{7});
  PageId page_id = 0;
  bool initialized = false;

  for (auto _ : state) {
    if (!initialized) {
      state.PauseTiming();
      file.emplace(name);
      disk.emplace(MakeOptions(file->path(), page_count, io_mode, true, true));
      initialized = true;
      state.ResumeTiming();
    }

    disk->WritePage(page_id, page);

    ++page_id;
    if (page_id == page_id_limit) {
      page_id = 0;
    }
  }

  ReportPageThroughput(state);
}

void BenchmarkRandomWrite(benchmark::State& state, IoMode io_mode,
                          std::string_view name) {
  const std::size_t page_count = PageCountFromState(state);

  std::optional<TempDiskFile> file;
  std::optional<DiskManager> disk;
  std::vector<PageId> page_ids;
  Page page;
  FillPage(page, std::byte{11});
  std::size_t request_index = 0;
  bool initialized = false;

  for (auto _ : state) {
    if (!initialized) {
      state.PauseTiming();
      page_ids = MakeRandomPageIds(page_count, kRandomRequestCount);
      file.emplace(name);
      disk.emplace(MakeOptions(file->path(), page_count, io_mode, true, true));
      initialized = true;
      state.ResumeTiming();
    }

    disk->WritePage(page_ids[request_index], page);

    ++request_index;
    if (request_index == page_ids.size()) {
      request_index = 0;
    }
  }

  ReportPageThroughput(state);
}

void BenchmarkSequentialReadReopen(benchmark::State& state, IoMode io_mode,
                                   std::string_view name, bool pollute_cache,
                                   bool large_pollution) {
  const std::size_t page_count = PageCountFromState(state);
  const auto page_id_limit = static_cast<PageId>(page_count);

  std::optional<TempDiskFile> file;
  std::optional<DiskManager> reader;
  Page page;
  PageId page_id = 0;
  bool initialized = false;

  for (auto _ : state) {
    if (!initialized) {
      state.PauseTiming();
      file.emplace(name);

      {
        DiskManager writer(
            MakeOptions(file->path(), page_count, io_mode, true, true));
        PrewritePages(writer, page_count);
      }

      if (pollute_cache) {
        if (large_pollution) {
          PolluteOsCacheLarge();
        } else {
          PolluteOsCache();
        }
      }

      reader.emplace(
          MakeOptions(file->path(), page_count, io_mode, false, false));

      initialized = true;
      state.ResumeTiming();
    }

    reader->ReadPage(page_id, &page);
    benchmark::DoNotOptimize(page.data.data());
    benchmark::ClobberMemory();

    ++page_id;
    if (page_id == page_id_limit) {
      page_id = 0;
    }
  }

  ReportPageThroughput(state);
}

void BenchmarkSequentialReadReopen(benchmark::State& state, IoMode io_mode,
                                   std::string_view name, bool pollute_cache) {
  BenchmarkSequentialReadReopen(state, io_mode, name, pollute_cache, false);
}

void BenchmarkRandomReadReopen(benchmark::State& state, IoMode io_mode,
                               std::string_view name, bool pollute_cache,
                               bool large_pollution) {
  const std::size_t page_count = PageCountFromState(state);

  std::optional<TempDiskFile> file;
  std::optional<DiskManager> reader;
  std::vector<PageId> page_ids;
  Page page;
  std::size_t request_index = 0;
  bool initialized = false;

  for (auto _ : state) {
    if (!initialized) {
      state.PauseTiming();
      page_ids = MakeRandomPageIds(page_count, kRandomRequestCount);
      file.emplace(name);

      {
        DiskManager writer(
            MakeOptions(file->path(), page_count, io_mode, true, true));
        PrewritePages(writer, page_count);
      }

      if (pollute_cache) {
        if (large_pollution) {
          PolluteOsCacheLarge();
        } else {
          PolluteOsCache();
        }
      }

      reader.emplace(
          MakeOptions(file->path(), page_count, io_mode, false, false));

      initialized = true;
      state.ResumeTiming();
    }

    reader->ReadPage(page_ids[request_index], &page);
    benchmark::DoNotOptimize(page.data.data());
    benchmark::ClobberMemory();

    ++request_index;
    if (request_index == page_ids.size()) {
      request_index = 0;
    }
  }

  ReportPageThroughput(state);
}

void BenchmarkRandomReadReopen(benchmark::State& state, IoMode io_mode,
                               std::string_view name, bool pollute_cache) {
  BenchmarkRandomReadReopen(state, io_mode, name, pollute_cache, false);
}

void BenchmarkWriteThenSync(benchmark::State& state, IoMode io_mode,
                            std::string_view name) {
  const std::size_t page_count = PageCountFromState(state);
  const auto page_id_limit = static_cast<PageId>(page_count);

  std::optional<TempDiskFile> file;
  std::optional<DiskManager> disk;
  Page page;
  FillPage(page, std::byte{33});
  PageId page_id = 0;
  bool initialized = false;

  for (auto _ : state) {
    if (!initialized) {
      state.PauseTiming();
      file.emplace(name);
      disk.emplace(MakeOptions(file->path(), page_count, io_mode, true, true));
      initialized = true;
      state.ResumeTiming();
    }

    disk->WritePage(page_id, page);
    disk->Sync();

    ++page_id;
    if (page_id == page_id_limit) {
      page_id = 0;
    }
  }

  ReportPageThroughput(state);
}

void BM_DiskManagerSequentialWriteDirect(benchmark::State& state) {
  BenchmarkSequentialWrite(state, IoMode::kDirect, "seq_write_direct");
}

void BM_DiskManagerSequentialWriteBuffered(benchmark::State& state) {
  BenchmarkSequentialWrite(state, IoMode::kBuffered, "seq_write_buffered");
}

void BM_DiskManagerRandomWriteDirect(benchmark::State& state) {
  BenchmarkRandomWrite(state, IoMode::kDirect, "random_write_direct");
}

void BM_DiskManagerRandomWriteBuffered(benchmark::State& state) {
  BenchmarkRandomWrite(state, IoMode::kBuffered, "random_write_buffered");
}

void BM_DiskManagerSequentialReadDirectReopen(benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kDirect,
                                "seq_read_direct_reopen", false);
}

void BM_DiskManagerSequentialReadBufferedReopen(benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kBuffered,
                                "seq_read_buffered_reopen", false);
}

void BM_DiskManagerRandomReadDirectReopen(benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kDirect, "random_read_direct_reopen",
                            false);
}

void BM_DiskManagerRandomReadBufferedReopen(benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kBuffered,
                            "random_read_buffered_reopen", false);
}

void BM_DiskManagerSequentialReadDirectAfterCachePollution(
    benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kDirect,
                                "seq_read_direct_polluted", true);
}

void BM_DiskManagerSequentialReadBufferedAfterCachePollution(
    benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kBuffered,
                                "seq_read_buffered_polluted", true);
}

void BM_DiskManagerRandomReadDirectAfterCachePollution(
    benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kDirect,
                            "random_read_direct_polluted", true);
}

void BM_DiskManagerRandomReadBufferedAfterCachePollution(
    benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kBuffered,
                            "random_read_buffered_polluted", true);
}

void BM_DiskManagerWriteThenSyncDirect(benchmark::State& state) {
  BenchmarkWriteThenSync(state, IoMode::kDirect, "write_sync_direct");
}

void BM_DiskManagerWriteThenSyncBuffered(benchmark::State& state) {
  BenchmarkWriteThenSync(state, IoMode::kBuffered, "write_sync_buffered");
}

void BM_DiskManagerLargeSequentialWriteDirect(benchmark::State& state) {
  BenchmarkSequentialWrite(state, IoMode::kDirect, "large_seq_write_direct");
}

void BM_DiskManagerLargeSequentialWriteBuffered(benchmark::State& state) {
  BenchmarkSequentialWrite(state, IoMode::kBuffered,
                           "large_seq_write_buffered");
}

void BM_DiskManagerLargeRandomWriteDirect(benchmark::State& state) {
  BenchmarkRandomWrite(state, IoMode::kDirect, "large_random_write_direct");
}

void BM_DiskManagerLargeRandomWriteBuffered(benchmark::State& state) {
  BenchmarkRandomWrite(state, IoMode::kBuffered, "large_random_write_buffered");
}

void BM_DiskManagerLargeSequentialReadDirectAfterCachePollution(
    benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kDirect,
                                "large_seq_read_direct_polluted", true, true);
}

void BM_DiskManagerLargeSequentialReadBufferedAfterCachePollution(
    benchmark::State& state) {
  BenchmarkSequentialReadReopen(state, IoMode::kBuffered,
                                "large_seq_read_buffered_polluted", true, true);
}

void BM_DiskManagerLargeRandomReadDirectAfterCachePollution(
    benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kDirect,
                            "large_random_read_direct_polluted", true, true);
}

void BM_DiskManagerLargeRandomReadBufferedAfterCachePollution(
    benchmark::State& state) {
  BenchmarkRandomReadReopen(state, IoMode::kBuffered,
                            "large_random_read_buffered_polluted", true, true);
}

BENCHMARK(BM_DiskManagerSequentialWriteDirect)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerSequentialWriteBuffered)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomWriteDirect)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomWriteBuffered)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerSequentialReadDirectReopen)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerSequentialReadBufferedReopen)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomReadDirectReopen)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomReadBufferedReopen)
    ->Arg(kSmallBenchmarkPageCount)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

// The cache-pollution variants are intentionally only run for the larger
// regular workload to keep default benchmark runtime reasonable.
BENCHMARK(BM_DiskManagerSequentialReadDirectAfterCachePollution)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerSequentialReadBufferedAfterCachePollution)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomReadDirectAfterCachePollution)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerRandomReadBufferedAfterCachePollution)
    ->Arg(kLargeBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerWriteThenSyncDirect)
    ->Arg(kSmallBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerWriteThenSyncBuffered)
    ->Arg(kSmallBenchmarkPageCount)
    ->Unit(benchmark::kMicrosecond);

// Manual-only 4 GiB benchmarks. Run with:
//   --benchmark_filter=^BM_DiskManagerLarge
BENCHMARK(BM_DiskManagerLargeSequentialWriteDirect)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeSequentialWriteBuffered)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeRandomWriteDirect)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeRandomWriteBuffered)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeSequentialReadDirectAfterCachePollution)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeSequentialReadBufferedAfterCachePollution)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeRandomReadDirectAfterCachePollution)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_DiskManagerLargeRandomReadBufferedAfterCachePollution)
    ->Arg(kHugeBenchmarkPageCount)
    ->Iterations(kHugeBenchmarkIterations)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace buffer_manager
