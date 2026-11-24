# Magmora: portable DSP library, tests, developer tools and bench. No plugin targets (docs/BUILD.md).
#
# Nobody needs to edit this file or any CMakeLists.txt to add code. Sources are picked up from:
#   dsp/src/<package>/**/*.cpp   object library magmora_<package>, all of them linked into magmora_dsp
#   dsp/tests/**/test_<name>.cpp executable test_<name>, ctest test "<name>"
#   dsp/tools/<tool>.cpp         executable <tool>
#   bench/CMakeLists.txt         owned by the bench
# A package that needs its own flags puts them in dsp/src/<package>/package.cmake.
cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Oldest supported macOS" FORCE)
endif()

# The product version has one definition: plugin/config.h
file(STRINGS plugin/config.h _version_line REGEX "^#define PLUG_VERSION_STR ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" MAGMORA_VERSION "${_version_line}")
if(NOT MAGMORA_VERSION)
  message(FATAL_ERROR "PLUG_VERSION_STR not found in plugin/config.h")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS plugin/config.h)

project(Magmora VERSION ${MAGMORA_VERSION} LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not supported: pass -B <build dir>")
endif()

get_property(_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)              # read by scripts/license_audit.py
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # same CRT as the plug-in

option(BUILD_TESTING "Build the tests" ON)
option(MAGMORA_BUILD_TOOLS "Build magmora-render, magmora-probe, magmora-bench" ON)
option(MAGMORA_BUILD_BENCH "Build the kernel benchmark" ON)

include(cmake/MagmoraWarnings.cmake)

add_subdirectory(dsp)
if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(dsp/tests)
endif()
if(MAGMORA_BUILD_TOOLS)
  add_subdirectory(dsp/tools)
  if(BUILD_TESTING)
    get_property(_tools GLOBAL PROPERTY MAGMORA_TOOLS)
    foreach(tool IN LISTS _tools)
      add_test(NAME tool.${tool} COMMAND ${tool} --version)
    endforeach()
  endif()
endif()
if(MAGMORA_BUILD_BENCH AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt")
  add_subdirectory(bench)
endif()
