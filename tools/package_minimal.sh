#!/usr/bin/env bash
set -euo pipefail

PACKAGE_NAME="buffer_manager_submission"
DIST_DIR="dist"
PACKAGE_DIR="${DIST_DIR}/${PACKAGE_NAME}"
ZIP_FILE="${PACKAGE_NAME}.zip"

rm -rf "${DIST_DIR}" "${ZIP_FILE}"

mkdir -p "${PACKAGE_DIR}/include"
mkdir -p "${PACKAGE_DIR}/src/page"
mkdir -p "${PACKAGE_DIR}/src/replacement"
mkdir -p "${PACKAGE_DIR}/src/storage"
mkdir -p "${PACKAGE_DIR}/benchmarks"

cp -R include/buffer_manager "${PACKAGE_DIR}/include/"

cp src/buffer_manager.cc "${PACKAGE_DIR}/src/"
cp src/types.cc "${PACKAGE_DIR}/src/"

cp src/page/frame_allocator.cc "${PACKAGE_DIR}/src/page/"
cp src/page/frame_allocator.h "${PACKAGE_DIR}/src/page/"
cp src/page/page.cc "${PACKAGE_DIR}/src/page/"
cp src/page/page_table.cc "${PACKAGE_DIR}/src/page/"
cp src/page/page_table.h "${PACKAGE_DIR}/src/page/"

cp src/replacement/clock_replacer.cc "${PACKAGE_DIR}/src/replacement/"
cp src/replacement/clock_replacer.h "${PACKAGE_DIR}/src/replacement/"
cp src/replacement/replacer.cc "${PACKAGE_DIR}/src/replacement/"
cp src/replacement/replacer.h "${PACKAGE_DIR}/src/replacement/"

cp src/storage/disk_manager.cc "${PACKAGE_DIR}/src/storage/"
cp src/storage/disk_manager.h "${PACKAGE_DIR}/src/storage/"

cp benchmarks/main_benchmark.cc "${PACKAGE_DIR}/benchmarks/"
cp benchmarks/benchmark_distribution.h "${PACKAGE_DIR}/benchmarks/"
cp benchmarks/buffer_manager_adapter.h "${PACKAGE_DIR}/benchmarks/"

cat > "${PACKAGE_DIR}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)

project(
  buffer_manager_submission
  VERSION 0.1.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

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

target_include_directories(
  buffer_manager
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(
  buffer_manager
  PRIVATE Threads::Threads)

target_compile_options(
  buffer_manager
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow)

add_executable(
  main_benchmark
  benchmarks/main_benchmark.cc)

target_include_directories(
  main_benchmark
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks"
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(
  main_benchmark
  PRIVATE
    buffer_manager
    Threads::Threads)

target_compile_options(
  main_benchmark
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow)
EOF

cat > "${PACKAGE_DIR}/run.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" -j

rm -f "${SCRIPT_DIR}/bfr_mngr_file"
"${BUILD_DIR}/main_benchmark"
rm -f "${SCRIPT_DIR}/bfr_mngr_file"
EOF

chmod +x "${PACKAGE_DIR}/run.sh"

(
  cd "${DIST_DIR}"
  zip -qr "../${ZIP_FILE}" "${PACKAGE_NAME}"
)

echo "Created ${ZIP_FILE}"
echo "Run with:"
echo "  unzip -q ${ZIP_FILE} && ./${PACKAGE_NAME}/run.sh"