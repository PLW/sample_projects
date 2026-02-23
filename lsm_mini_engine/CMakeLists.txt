cmake_minimum_required(VERSION 3.20)
project(lsm_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- Library sources under src/ ---
file(GLOB_RECURSE LSM_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/iter/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/wal/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/memtable/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/sstable/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/version/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/compaction/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/db/*.cpp"
)

add_library(lsm_engine STATIC ${LSM_SOURCES})
target_include_directories(lsm_engine PUBLIC "${CMAKE_SOURCE_DIR}/src")

# --- Tests ---
include(CTest)
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(lsm_engine_test test/lsm_engine_test.cpp)
target_link_libraries(lsm_engine_test PRIVATE lsm_engine GTest::gtest_main)
target_include_directories(lsm_engine_test PRIVATE "${CMAKE_SOURCE_DIR}/src")

include(GoogleTest)
gtest_discover_tests(lsm_engine_test)
