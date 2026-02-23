# Generated (ChatGPT 5.2) from : prompts/cmake_gen

# Discover "modules" under src/<module>/*.cpp (excluding components)
file(GLOB MODULE_DIRS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*")

foreach(dir ${MODULE_DIRS})

  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${dir}")
    file(GLOB_RECURSE MOD_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cc"
      "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.cxx"
      "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/*.hpp"
    )

    # Create a library only if there is at least one .cpp/.cc/.cxx
    set(MOD_IMPL_SOURCES "")
    foreach(f ${MOD_SOURCES})
      if(f MATCHES ".*\\.(cpp|cc|cxx)$")
        list(APPEND MOD_IMPL_SOURCES "${f}")
      endif()
    endforeach()

    if(MOD_IMPL_SOURCES)
      set(target "mod_${dir}")
      add_library(${target} STATIC ${MOD_IMPL_SOURCES})

      target_include_directories(${target}
        PUBLIC
          "${CMAKE_CURRENT_SOURCE_DIR}"
          "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
      )
      target_compile_features(${target} PUBLIC cxx_std_17)

    endif()
  endif()
endforeach()
