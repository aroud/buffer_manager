# Buffer Manager
Implementation of a buffer manager managing a cache of pages in memory including reading/flushing to disk and page replacement by FIFO and LRU algorithms.

## Requirements

`clang++`, CMake, Ninja, GoogleTest, and `clang-format`.
Google Benchmark is required only for benchmarks.

On macOS with Homebrew:
```sh
brew install cmake ninja clang-format googletest google-benchmark
```

## Build

The checked-in presets use Ninja.

```sh
cmake --preset debug
cmake --build --preset debug
```

## Tests

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

## Benchmarks

```sh
cmake --preset benchmarks
cmake --build --preset benchmarks
./build/benchmarks/benchmarks/buffer_manager_benchmarks
```

## Formatting

```sh
cmake --preset debug
cmake --build --preset format
cmake --build --preset format-check
```
