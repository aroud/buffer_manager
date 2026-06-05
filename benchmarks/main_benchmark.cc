#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "benchmark_distribution.h"
#include "buffer_manager/page.h"
#include "buffer_manager/types.h"
#include "buffer_manager_adapter.h"

namespace buffer_manager::main_benchmark {
namespace {

struct Scenario final {
  std::uint64_t memory_capacity = 0;
  std::uint64_t disk_capacity = 0;
  double theta = 0.0;
  std::uint64_t request_count = 0;
};

enum class BenchKind : uint8_t {
  kSingleThreadedSync,
  kMultiThreadedSync,
  kMultiThreadedAsync,
};

struct BenchType final {
  BenchKind kind = BenchKind::kSingleThreadedSync;
  std::size_t thread_count = 1;

  [[nodiscard]] static BenchType SingleThreadedSync() {
    return {
        .kind = BenchKind::kSingleThreadedSync,
        .thread_count = 1,
    };
  }

  [[nodiscard]] static BenchType MultiThreadedSync(std::size_t thread_count) {
    return {
        .kind = BenchKind::kMultiThreadedSync,
        .thread_count = thread_count,
    };
  }

  [[nodiscard]] static BenchType MultiThreadedAsync(std::size_t thread_count) {
    return {
        .kind = BenchKind::kMultiThreadedAsync,
        .thread_count = thread_count,
    };
  }
};

[[nodiscard]] const char* BenchTypeName(BenchType type) {
  switch (type.kind) {
    case BenchKind::kSingleThreadedSync:
      return "SingleThreadedSync";
    case BenchKind::kMultiThreadedSync:
      return "MultiThreadedSync";
    case BenchKind::kMultiThreadedAsync:
      return "MultiThreadedAsync";
  }

  return "Unknown";
}

[[nodiscard]] bool IsPowerOfTwo(std::uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

[[nodiscard]] std::uint64_t NowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void WriteFirstU64(Page& page, std::uint64_t value) {
  std::memcpy(page.data.data(), &value, sizeof(value));
}

[[nodiscard]] std::uint64_t ReadFirstU64(const Page& page) {
  std::uint64_t value = 0;
  std::memcpy(&value, page.data.data(), sizeof(value));
  return value;
}

[[nodiscard]] bool PageOk(bool verify, const Page& page, PageId pfn,
                          unsigned verify_shift) {
  if (!verify) {
    return true;
  }

  return ReadFirstU64(page) ==
         (static_cast<std::uint64_t>(pfn) << verify_shift);
}

void InitPFNs(BufferManagerAdapter& buffer_manager, std::uint64_t pfn_count,
              bool init_page, unsigned verify_shift) {
  for (std::uint64_t i = 0; i < pfn_count; ++i) {
    auto pinned_page = buffer_manager.AllocPageFrame(/*thread_id=*/0);

    if (pinned_page.pfn != i) {
      throw std::runtime_error("allocated PFN is not sequential");
    }

    if (init_page) {
      WriteFirstU64(pinned_page.page(), i << verify_shift);
      pinned_page.guard.MarkDirty();
    }

    BufferManagerAdapter::DecrementPinCount(pinned_page);
  }
}

void RunBench(BufferManagerAdapter& buffer_manager,
              const std::vector<std::uint64_t>& pfn_requests, std::size_t begin,
              std::size_t end, bool verify, unsigned verify_shift,
              std::size_t thread_id, std::atomic_bool& start_signal,
              std::atomic_bool& bench_success) {
  while (!start_signal.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  for (std::size_t i = begin; i < end; ++i) {
    const auto pfn = static_cast<PageId>(pfn_requests[i]);

    try {
      auto pinned_page = buffer_manager.PFNToPage(pfn, thread_id);

      if (!PageOk(verify, pinned_page.page(), pfn, verify_shift)) {
        bench_success.store(false, std::memory_order_release);
        return;
      }

      BufferManagerAdapter::DecrementPinCount(pinned_page);
    } catch (...) {
      bench_success.store(false, std::memory_order_release);
      return;
    }
  }
}

void RetryInflightRequests(
    std::size_t thread_id, std::atomic_bool& bench_success,
    std::vector<std::optional<BufferManagerAdapter::AsyncRequest>>&
        in_flight_requests,
    std::size_t& in_flight_count, bool verify, unsigned verify_shift) {
  for (auto& candidate : in_flight_requests) {
    if (!candidate.has_value()) {
      continue;
    }

    try {
      std::optional<BufferManagerAdapter::PinnedPage> completed =
          BufferManagerAdapter::TryComplete(*candidate);

      if (!completed.has_value()) {
        continue;
      }

      if (!PageOk(verify, completed->page(), completed->pfn, verify_shift)) {
        bench_success.store(false, std::memory_order_release);
        return;
      }

      BufferManagerAdapter::DecrementPinCount(*completed);
      candidate.reset();
      --in_flight_count;
    } catch (...) {
      bench_success.store(false, std::memory_order_release);
      return;
    }
  }

  (void)thread_id;
}

void RunAsyncBench(BufferManagerAdapter& buffer_manager,
                   const std::vector<std::uint64_t>& pfn_requests,
                   std::size_t begin, std::size_t end, bool verify,
                   unsigned verify_shift, std::size_t thread_id,
                   std::atomic_bool& start_signal,
                   std::atomic_bool& bench_success, std::size_t max_in_flight) {
  std::vector<std::optional<BufferManagerAdapter::AsyncRequest>>
      in_flight_requests(max_in_flight);

  std::size_t in_flight_count = 0;

  while (!start_signal.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  for (std::size_t i = begin; i < end; ++i) {
    while (in_flight_count == max_in_flight) {
      RetryInflightRequests(thread_id, bench_success, in_flight_requests,
                            in_flight_count, verify, verify_shift);

      if (!bench_success.load(std::memory_order_acquire)) {
        return;
      }

      std::this_thread::yield();
    }

    const auto pfn = static_cast<PageId>(pfn_requests[i]);

    try {
      auto request = buffer_manager.PFNToPageAsync(pfn, thread_id);

      bool inserted = false;
      for (auto& slot : in_flight_requests) {
        if (!slot.has_value()) {
          slot.emplace(std::move(request));
          inserted = true;
          ++in_flight_count;
          break;
        }
      }

      if (!inserted) {
        bench_success.store(false, std::memory_order_release);
        return;
      }

      RetryInflightRequests(thread_id, bench_success, in_flight_requests,
                            in_flight_count, verify, verify_shift);
    } catch (...) {
      bench_success.store(false, std::memory_order_release);
      return;
    }
  }

  while (in_flight_count != 0) {
    RetryInflightRequests(thread_id, bench_success, in_flight_requests,
                          in_flight_count, verify, verify_shift);

    if (!bench_success.load(std::memory_order_acquire)) {
      return;
    }

    std::this_thread::yield();
  }
}

[[nodiscard]] std::uint64_t RunSTBench(
    BufferManagerAdapter& buffer_manager,
    const std::vector<std::uint64_t>& pfn_requests, bool verify,
    unsigned verify_shift) {
  std::atomic_bool start_signal{true};
  std::atomic_bool bench_success{true};

  const std::uint64_t start_ns = NowNs();

  RunBench(buffer_manager, pfn_requests, 0, pfn_requests.size(), verify,
           verify_shift, /*thread_id=*/0, start_signal, bench_success);

  const std::uint64_t end_ns = NowNs();

  if (!bench_success.load(std::memory_order_acquire)) {
    throw std::runtime_error("benchmark failed");
  }

  return end_ns - start_ns;
}

[[nodiscard]] std::uint64_t RunMTBench(
    BufferManagerAdapter& buffer_manager,
    const std::vector<std::uint64_t>& pfn_requests, bool verify,
    unsigned verify_shift, std::size_t thread_count) {
  std::atomic_bool start_signal{false};
  std::atomic_bool bench_success{true};

  const std::size_t requests_per_thread = pfn_requests.size() / thread_count;
  std::vector<std::thread> workers;
  workers.reserve(thread_count - 1);

  for (std::size_t thread_id = 1; thread_id < thread_count; ++thread_id) {
    const std::size_t begin = requests_per_thread * thread_id;
    const std::size_t end = begin + requests_per_thread;

    workers.emplace_back([&, begin, end, thread_id] {
      RunBench(buffer_manager, pfn_requests, begin, end, verify, verify_shift,
               thread_id, start_signal, bench_success);
    });
  }

  const std::uint64_t start_ns = NowNs();

  start_signal.store(true, std::memory_order_release);

  RunBench(buffer_manager, pfn_requests,
           requests_per_thread * (thread_count - 1), pfn_requests.size(),
           verify, verify_shift, /*thread_id=*/0, start_signal, bench_success);

  for (std::thread& worker : workers) {
    worker.join();
  }

  const std::uint64_t end_ns = NowNs();

  if (!bench_success.load(std::memory_order_acquire)) {
    throw std::runtime_error("benchmark failed");
  }

  return end_ns - start_ns;
}

[[nodiscard]] std::uint64_t RunMTAsyncBench(
    BufferManagerAdapter& buffer_manager,
    const std::vector<std::uint64_t>& pfn_requests, bool verify,
    unsigned verify_shift, std::size_t thread_count,
    std::size_t max_in_flight) {
  std::atomic_bool start_signal{false};
  std::atomic_bool bench_success{true};

  const std::size_t requests_per_thread = pfn_requests.size() / thread_count;
  std::vector<std::thread> workers;
  workers.reserve(thread_count - 1);

  for (std::size_t thread_id = 1; thread_id < thread_count; ++thread_id) {
    const std::size_t begin = requests_per_thread * thread_id;
    const std::size_t end = begin + requests_per_thread;

    workers.emplace_back([&, begin, end, thread_id] {
      RunAsyncBench(buffer_manager, pfn_requests, begin, end, verify,
                    verify_shift, thread_id, start_signal, bench_success,
                    max_in_flight);
    });
  }

  const std::uint64_t start_ns = NowNs();

  start_signal.store(true, std::memory_order_release);

  RunAsyncBench(buffer_manager, pfn_requests,
                requests_per_thread * (thread_count - 1), pfn_requests.size(),
                verify, verify_shift, /*thread_id=*/0, start_signal,
                bench_success, max_in_flight);

  for (std::thread& worker : workers) {
    worker.join();
  }

  const std::uint64_t end_ns = NowNs();

  if (!bench_success.load(std::memory_order_acquire)) {
    throw std::runtime_error("benchmark failed");
  }

  return end_ns - start_ns;
}

[[nodiscard]] double RunBenchmark(std::uint64_t memory_capacity,
                                  std::uint64_t disk_capacity,
                                  std::uint64_t page_size, BenchType bench_type,
                                  bool verify, double theta,
                                  std::uint64_t total_request_count,
                                  const std::filesystem::path& file_path) {
  if (bench_type.thread_count == 0) {
    throw std::invalid_argument("thread_count must be greater than zero");
  }

  if (disk_capacity < memory_capacity) {
    throw std::invalid_argument("disk_capacity must be >= memory_capacity");
  }

  if (page_size > memory_capacity) {
    throw std::invalid_argument("page_size must be <= memory_capacity");
  }

  if (!IsPowerOfTwo(page_size)) {
    throw std::invalid_argument("page_size must be a power of two");
  }

  if ((disk_capacity % page_size) != 0 || (memory_capacity % page_size) != 0) {
    throw std::invalid_argument("capacities must be multiples of page_size");
  }

  if (page_size != kPageSize) {
    throw std::invalid_argument(
        "this C++ implementation currently supports only kPageSize pages");
  }

  const std::uint64_t pfn_count = disk_capacity / page_size;
  const std::size_t frame_count =
      static_cast<std::size_t>(memory_capacity / page_size);

  const std::size_t thread_count = bench_type.thread_count;
  constexpr std::size_t kMaxInFlight = 128;

  std::cout << "\nRunning Benchmark with Parameters:\n\n"
            << "\tType: " << BenchTypeName(bench_type) << "\n"
            << "\tThread Count: " << thread_count << "\n"
            << "\tMemory Capacity: " << memory_capacity << " bytes\n"
            << "\tDisk Capacity: " << disk_capacity << " bytes\n"
            << "\tPage Size: " << page_size << " bytes\n"
            << "\tTheta: " << theta << "\n"
            << "\tRequest Count: " << total_request_count << "\n\n";

  std::cout << "Setting up benchmark..." << std::flush;

  const std::uint64_t seed = NowNs();
  std::mt19937_64 rng(seed);

  std::vector<std::uint64_t> pfn_requests =
      GenScrambledZipfSequence(0, pfn_count, theta, rng, total_request_count);

  const unsigned verify_shift = static_cast<unsigned>(rng() % 63);

  BufferManagerAdapter buffer_manager(
      file_path, frame_count, static_cast<PageId>(pfn_count), thread_count);

  InitPFNs(buffer_manager, pfn_count, verify, verify_shift);

  std::cout << " Done\n\nBeginning benchmark..." << std::flush;

  std::uint64_t runtime_ns = 0;

  switch (bench_type.kind) {
    case BenchKind::kSingleThreadedSync:
      runtime_ns =
          RunSTBench(buffer_manager, pfn_requests, verify, verify_shift);
      break;

    case BenchKind::kMultiThreadedSync:
      runtime_ns = RunMTBench(buffer_manager, pfn_requests, verify,
                              verify_shift, thread_count);
      break;

    case BenchKind::kMultiThreadedAsync:
      runtime_ns = RunMTAsyncBench(buffer_manager, pfn_requests, verify,
                                   verify_shift, thread_count, kMaxInFlight);
      break;
  }

  const double runtime_s = static_cast<double>(runtime_ns) / 1'000'000'000.0;
  const double ops = static_cast<double>(total_request_count) / runtime_s;

  std::cout << " Done\n"
            << "Took: " << runtime_ns << " ns\n"
            << "MOPS: " << (ops / 1'000'000.0) << "\n";

  return ops;
}

}  // namespace
}  // namespace buffer_manager::main_benchmark

int main() {
  namespace bm = buffer_manager::main_benchmark;

  try {
    const std::filesystem::path file_path = "bfr_mngr_file";

    constexpr std::uint64_t kMemCapacity = 1ULL << 30;
    constexpr std::size_t kThreadCount = 16;
    constexpr bool kVerify = true;
    constexpr std::uint64_t kTotalRequestCount = 1ULL << 24;

    constexpr std::uint64_t kBenchmarkPageSize = 1ULL << 12;
    constexpr double kThetaHigh = 2.0;
    constexpr double kThetaLow = 0.5;

    constexpr std::uint64_t kLowMemCapacity = kMemCapacity >> 3;

    constexpr std::uint64_t kDiskCapacityHigh = kLowMemCapacity << 3;
    constexpr std::uint64_t kDiskCapacityLow = kMemCapacity << 1;
    constexpr std::uint64_t kDiskCapacityNoIo = kMemCapacity;

    constexpr std::uint64_t kRequestCountNoIo = kTotalRequestCount;
    constexpr std::uint64_t kRequestCountLowIo = kTotalRequestCount >> 3;
    constexpr std::uint64_t kRequestCountHighIo = kTotalRequestCount >> 5;

    const bm::Scenario scenarios[] = {
        {
            .memory_capacity = kMemCapacity,
            .disk_capacity = kDiskCapacityNoIo,
            .theta = kThetaHigh,
            .request_count = kRequestCountNoIo,
        },
        {
            .memory_capacity = kMemCapacity,
            .disk_capacity = kDiskCapacityNoIo,
            .theta = kThetaLow,
            .request_count = kRequestCountNoIo,
        },
        {
            .memory_capacity = kMemCapacity,
            .disk_capacity = kDiskCapacityLow,
            .theta = kThetaHigh,
            .request_count = kRequestCountLowIo,
        },
        {
            .memory_capacity = kMemCapacity,
            .disk_capacity = kDiskCapacityLow,
            .theta = kThetaLow,
            .request_count = kRequestCountLowIo,
        },
        {
            .memory_capacity = kLowMemCapacity,
            .disk_capacity = kDiskCapacityHigh,
            .theta = kThetaHigh,
            .request_count = kRequestCountHighIo,
        },
        {
            .memory_capacity = kLowMemCapacity,
            .disk_capacity = kDiskCapacityHigh,
            .theta = kThetaLow,
            .request_count = kRequestCountHighIo,
        },
    };

    const bm::BenchType bench_types[] = {
        bm::BenchType::SingleThreadedSync(),
        bm::BenchType::MultiThreadedSync(kThreadCount),
        bm::BenchType::MultiThreadedAsync(kThreadCount),
    };

    for (const bm::Scenario& scenario : scenarios) {
      for (const bm::BenchType& bench_type : bench_types) {
        (void)bm::RunBenchmark(scenario.memory_capacity, scenario.disk_capacity,
                               kBenchmarkPageSize, bench_type, kVerify,
                               scenario.theta, scenario.request_count,
                               file_path);
      }
    }

    std::error_code error;
    std::filesystem::remove(file_path, error);

    return 0;
  } catch (const std::exception& error) {
    std::cerr << "main benchmark failed: " << error.what() << "\n";
    return 1;
  }
}
