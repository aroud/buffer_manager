# cmake/ClangTools.cmake

if(BUFFER_MANAGER_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy)

  if(NOT CLANG_TIDY_EXE)
    message(
      FATAL_ERROR
        "BUFFER_MANAGER_ENABLE_CLANG_TIDY=ON, but clang-tidy was not found. "
        "Install clang-tidy or configure with BUFFER_MANAGER_ENABLE_CLANG_TIDY=OFF.")
  endif()

  set(CMAKE_CXX_CLANG_TIDY
      "${CLANG_TIDY_EXE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
endif()

function(buffer_manager_add_format_targets)
  find_program(
    CLANG_FORMAT_EXE
    NAMES clang-format
    HINTS /opt/homebrew/bin /usr/local/bin)

  if(NOT CLANG_FORMAT_EXE)
    message(WARNING "clang-format was not found; format targets are disabled.")
    return()
  endif()

  file(
    GLOB_RECURSE BUFFER_MANAGER_FORMAT_FILES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${PROJECT_SOURCE_DIR}/include/*.h"
    "${PROJECT_SOURCE_DIR}/include/*.hpp"
    "${PROJECT_SOURCE_DIR}/include/*.hh"
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/src/*.hpp"
    "${PROJECT_SOURCE_DIR}/src/*.hh"
    "${PROJECT_SOURCE_DIR}/src/*.cc"
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    "${PROJECT_SOURCE_DIR}/tests/*.hh"
    "${PROJECT_SOURCE_DIR}/tests/*.cc"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.h"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.hpp"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.hh"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.cc"
    "${PROJECT_SOURCE_DIR}/benchmarks/*.cpp")

  if(NOT BUFFER_MANAGER_FORMAT_FILES)
    message(WARNING "No C++ source files found for clang-format targets.")
    return()
  endif()

  add_custom_target(
    format
    COMMAND "${CLANG_FORMAT_EXE}" -i ${BUFFER_MANAGER_FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting C++ sources with clang-format"
    COMMAND_EXPAND_LISTS
    VERBATIM)

  add_custom_target(
    format-check
    COMMAND "${CLANG_FORMAT_EXE}" --dry-run --Werror
            ${BUFFER_MANAGER_FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking C++ formatting with clang-format"
    COMMAND_EXPAND_LISTS
    VERBATIM)
endfunction()