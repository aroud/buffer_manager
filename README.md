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


## DiskManager

`DiskManager` provides fixed-size page I/O for the buffer manager. Pages are 4 KiB and are stored in a single backing file.

File layout:

```text
[4 KiB header][page 0][page 1][page 2]...
```

Page offsets are computed directly:

```text
offset = header_size + page_id * page_size
```

No persistent page directory is currently needed.

### I/O modes

The implementation supports two I/O modes:

| Mode                | Purpose                                                                                        |
| ------------------- | ---------------------------------------------------------------------------------------------- |
| `IoMode::kDirect`   | DBMS-style I/O. Uses Linux `O_DIRECT` or macOS no-cache flags to reduce OS page-cache effects. |
| `IoMode::kBuffered` | Normal OS-buffered I/O. Useful for comparison, debugging, and fallback.                        |

`Sync()` is explicit. Normal page writes do not force durability after every write.

### Large benchmark results

Large benchmarks use a 4 GiB file with 4 KiB pages.

| Benchmark                        | Mode     |    Pattern | Real time | Throughput |
| -------------------------------- | -------- | ---------: | --------: | ---------: |
| Large write                      | Direct   | Sequential |   11.1 µs |  403 MiB/s |
| Large write                      | Buffered | Sequential |   2.16 µs | 2.19 GiB/s |
| Large write                      | Direct   |     Random |    113 µs | 47.8 MiB/s |
| Large write                      | Buffered |     Random |    108 µs | 47.7 MiB/s |
| Large read after cache pollution | Direct   | Sequential |   17.6 µs |  849 MiB/s |
| Large read after cache pollution | Buffered | Sequential |   1.18 µs | 6.28 GiB/s |
| Large read after cache pollution | Direct   |     Random |   69.4 µs |  685 MiB/s |
| Large read after cache pollution | Buffered |     Random |   41.7 µs | 1.17 GiB/s |

### Observations

Buffered I/O is faster for sequential workloads because the operating system can use the page cache and read-ahead. Direct I/O is slower, but it better isolates the buffer manager from OS caching effects.

For large random writes, direct and buffered modes converge to roughly the same throughput, around 48 MiB/s. In this case the storage access pattern dominates, and the OS cache provides little benefit.

`Sync()` is much more expensive than unsynced page writes, so durability barriers are kept explicit and outside the normal write path.
