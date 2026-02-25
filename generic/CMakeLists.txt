cmake_minimum_required(VERSION 3.20)
project(distidx_minimal_grpc CXX)
project(XXX_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/generated/dist_index.pb.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/generated/dist_index.grpc.pb.cc
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- Library sources under src/ ---
file(GLOB_RECURSE XXX_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/A/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/B/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/C/*.cpp"
)

add_library(XXX_engine STATIC ${LSM_SOURCES})
target_include_directories(XXX_engine PUBLIC "${CMAKE_SOURCE_DIR}/src")

# --- grpc ---
add_library(distidx_proto ${PROTO_SRC})
target_include_directories(distidx_proto PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/generated
)
target_link_libraries(distidx_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

add_library(distidx_common
  src/common/grpc_util.cpp
  src/common/shard_map.cpp
  src/common/tokenize.cpp
)
target_include_directories(distidx_common PUBLIC src)
target_link_libraries(distidx_common PUBLIC distidx_proto)

add_executable(metadata_server src/metadata/metadata_server.cpp)
target_link_libraries(metadata_server PRIVATE distidx_common)

add_executable(index_server src/index/index_server.cpp)
target_link_libraries(index_server PRIVATE distidx_common)

add_executable(eval_server src/eval/eval_server.cpp)
target_link_libraries(eval_server PRIVATE distidx_common)

add_executable(eval_client src/client/eval_client.cpp)
target_link_libraries(eval_client PRIVATE distidx_common)

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

add_executable(XXX_engine_test test/XXX_engine_test.cpp)
target_link_libraries(XXX_engine_test PRIVATE XXX_engine GTest::gtest_main)
target_include_directories(XXX_engine_test PRIVATE "${CMAKE_SOURCE_DIR}/src")

include(GoogleTest)
gtest_discover_tests(XXX_engine_test)

