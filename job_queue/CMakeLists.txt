cmake_minimum_required(VERSION 3.20)
project(job_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- Library sources under src/ ---
file(GLOB_RECURSE LSM_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/*.h"
)

add_library(job_queue STATIC ${LSM_SOURCES})
target_include_directories(job_queue PUBLIC "${CMAKE_SOURCE_DIR}/src")

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

add_executable(test_job_queue test/test_job_queue.cpp)
target_link_libraries(test_job_queue PRIVATE gtest_main)
target_include_directories(test_job_queue PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME test_job_queue COMMAND test_job_queue)

include(GoogleTest)
gtest_discover_tests(test_job_queue)

