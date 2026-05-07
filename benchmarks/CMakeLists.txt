find_package(benchmark CONFIG REQUIRED)

add_executable(buffer_manager_benchmarks benchmark_infra.cc)
target_link_libraries(
  buffer_manager_benchmarks
  PRIVATE buffer_manager::buffer_manager benchmark::benchmark_main
          buffer_manager_options)
