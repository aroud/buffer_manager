#include <benchmark/benchmark.h>

static void BM_ToolingSmoke(benchmark::State& state) {
  for (auto _ : state) {
    benchmark::DoNotOptimize(0);
  }
}
BENCHMARK(BM_ToolingSmoke);
