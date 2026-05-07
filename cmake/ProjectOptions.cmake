add_library(buffer_manager_options INTERFACE)

target_compile_options(
  buffer_manager_options
  INTERFACE
    "$<$<CXX_COMPILER_ID:AppleClang,Clang>:-Wall;-Wextra;-Wpedantic;-Wshadow>")
