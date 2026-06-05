find_package(benchmark CONFIG REQUIRED)

add_executable(
  buffer_manager_benchmarks
  buffer_manager_benchmark.cc
  buffer_manager_async_benchmark.cc
  benchmark_infra.cc
  disk_manager_benchmark.cc
  frame_allocator_benchmark.cc)

target_include_directories(
  buffer_manager_benchmarks
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(
  buffer_manager_benchmarks
  PRIVATE
    buffer_manager::buffer_manager
    benchmark::benchmark_main
    buffer_manager_options)

add_executable(
  main_benchmark
  main_benchmark.cc)

target_include_directories(
  main_benchmark
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/benchmarks")

target_link_libraries(
  main_benchmark
  PRIVATE
    buffer_manager::buffer_manager
    buffer_manager_options)