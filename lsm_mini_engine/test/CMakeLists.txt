# Generated (ChatGPT 5.2) from : prompts/cmake_gen

include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For MSVC runtime consistency (safe on other platforms too)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Discover all *_test.cpp files under tests/
file(GLOB_RECURSE ALL_TESTS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cxx"
)

foreach(test_src ${ALL_TESTS})
  # Determine module name from path: tests/<module>/foo_test.cpp
  file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}" "${test_src}")
  get_filename_component(module_dir "${rel}" DIRECTORY)
  string(REPLACE "/" "_" module_id "${module_dir}")

  get_filename_component(test_name "${test_src}" NAME_WE)
  set(exe "test_${module_id}_${test_name}")

  add_executable(${exe} "${test_src}")
  target_compile_features(${exe} PRIVATE cxx_std_17)
  target_link_libraries(${exe} PRIVATE GTest::gtest_main)

  # Link to module library if present: mod_<module>
  # We only support the first path segment as module name.
  # E.g. tests/foo/bar_test.cpp -> module is "foo"
  string(REGEX MATCH "^[^/]+" module "${module_dir}")
  if(module)
    set(mod_target "mod_${module}")
    if(TARGET ${mod_target})
      target_link_libraries(${exe} PRIVATE ${mod_target})
    else()
      message(WARNING "No target ${mod_target} for test ${exe} (path ${rel}).")
    endif()
  endif()

  # Include paths (tests can include from src/)
  target_include_directories(${exe} PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )

  include(GoogleTest)
  gtest_discover_tests(${exe})
endforeach()

