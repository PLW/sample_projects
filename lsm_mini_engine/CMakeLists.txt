# Generated (ChatGPT 5.2) from : prompts/cmake_gen

cmake_minimum_required(VERSION 3.20)

project(vectordb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Good defaults
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_TESTS "Build unit tests" ON)

add_subdirectory(src)

if(ENABLE_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

