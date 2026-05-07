cmake_minimum_required(VERSION 3.24)

project(
  buffer_manager
  VERSION 0.1.0
  DESCRIPTION "Buffer manager for database implementation experiments"
  LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

option(BUFFER_MANAGER_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(BUFFER_MANAGER_ENABLE_CLANG_TIDY "Run clang-tidy during builds" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(ClangTools)

add_library(
  buffer_manager STATIC
  src/buffer_manager.cc
  src/page/frame_allocator.cc
  src/page/page.cc
  src/page/page_table.cc
  src/replacement/clock_replacer.cc
  src/replacement/replacer.cc
  src/storage/disk_manager.cc
  src/types.cc)
add_library(buffer_manager::buffer_manager ALIAS buffer_manager)

target_include_directories(
  buffer_manager
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:include>"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(buffer_manager PUBLIC cxx_std_20)
target_link_libraries(buffer_manager PRIVATE buffer_manager_options)

include(CTest)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(BUFFER_MANAGER_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

buffer_manager_add_format_targets()
