#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "page/frame_allocator.h"

namespace buffer_manager {
namespace {

constexpr std::int64_t kSmallFrameCount = 1024;
constexpr std::int64_t kLargeFrameCount = 16 * kSmallFrameCount;

[[nodiscard]] std::size_t FrameCountFromState(const benchmark::State& state) {
  return static_cast<std::size_t>(state.range(0));
}

void ReportFrameThroughput(benchmark::State& state, std::int64_t operations) {
  state.counters["frames/s"] = benchmark::Counter(
      static_cast<double>(operations), benchmark::Counter::kIsRate);
}

void BM_FrameAllocatorAllocateAndFreeOne(benchmark::State& state) {
  const std::size_t frame_count = FrameCountFromState(state);
  FrameAllocator allocator(frame_count);

  for (auto _ : state) {
    const std::optional<FrameId> frame_id = allocator.AllocateFrame();
    benchmark::DoNotOptimize(frame_id);

    allocator.FreeFrame(*frame_id);
  }

  ReportFrameThroughput(state, state.iterations());
}

void BM_FrameAllocatorAllocateAllAndFreeAll(benchmark::State& state) {
  const std::size_t frame_count = FrameCountFromState(state);

  FrameAllocator allocator(frame_count);
  std::vector<FrameId> frames;
  frames.reserve(frame_count);

  for (auto _ : state) {
    frames.clear();

    for (std::size_t i = 0; i < frame_count; ++i) {
      const std::optional<FrameId> frame_id = allocator.AllocateFrame();
      frames.push_back(*frame_id);
    }

    benchmark::DoNotOptimize(frames.data());

    for (FrameId frame_id : frames) {
      allocator.FreeFrame(frame_id);
    }
  }

  const auto operations = static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(frame_count) * 2;

  ReportFrameThroughput(state, operations);
}

void BM_FrameAllocatorAllocateUntilFull(benchmark::State& state) {
  const std::size_t frame_count = FrameCountFromState(state);

  for (auto _ : state) {
    state.PauseTiming();
    FrameAllocator allocator(frame_count);
    std::vector<FrameId> frames;
    frames.reserve(frame_count);
    state.ResumeTiming();

    for (std::size_t i = 0; i < frame_count; ++i) {
      const std::optional<FrameId> frame_id = allocator.AllocateFrame();
      frames.push_back(*frame_id);
    }

    benchmark::DoNotOptimize(frames.data());
  }

  const auto operations = static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(frame_count);

  ReportFrameThroughput(state, operations);
}

BENCHMARK(BM_FrameAllocatorAllocateAndFreeOne)
    ->Arg(kSmallFrameCount)
    ->Arg(kLargeFrameCount)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_FrameAllocatorAllocateAllAndFreeAll)
    ->Arg(kSmallFrameCount)
    ->Arg(kLargeFrameCount)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_FrameAllocatorAllocateUntilFull)
    ->Arg(kSmallFrameCount)
    ->Arg(kLargeFrameCount)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace buffer_manager