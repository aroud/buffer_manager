find_package(benchmark CONFIG REQUIRED)

add_executable(buffer_manager_benchmarks
  benchmark_infra.cc
  disk_manager_benchmark.cc
  frame_allocator_benchmark.cc)

target_include_directories(
  buffer_manager_benchmarks
  PRIVATE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(
  buffer_manager_benchmarks
  PRIVATE buffer_manager::buffer_manager benchmark::benchmark_main
          buffer_manager_options)
