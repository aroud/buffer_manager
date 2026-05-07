find_package(GTest CONFIG REQUIRED)

add_executable(buffer_manager_tests testing_infra_test.cc)

target_include_directories(
  buffer_manager_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(
  buffer_manager_tests
  PRIVATE
    buffer_manager::buffer_manager
    GTest::gtest_main
    buffer_manager_options)

include(GoogleTest)
gtest_discover_tests(buffer_manager_tests DISCOVERY_MODE PRE_TEST)