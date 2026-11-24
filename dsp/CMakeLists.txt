# One object library per directory in dsp/src, so `cmake --build <dir> --target magmora_solver` works while
# another package is broken. Adding files or directories needs no edit here.
set(_src "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB_RECURSE _all CONFIGURE_DEPENDS "${_src}/*.cpp")

set(_packages "")
foreach(file IN LISTS _all)
  file(RELATIVE_PATH rel "${_src}" "${file}")
  if(NOT rel MATCHES "^([^/]+)/")
    message(FATAL_ERROR "dsp/src/${rel}: sources live in a package directory, dsp/src/<package>/")
  endif()
  set(pkg "${CMAKE_MATCH_1}")
  list(APPEND _packages "${pkg}")
  list(APPEND _sources_${pkg} "${file}")
endforeach()
list(REMOVE_DUPLICATES _packages)

add_library(magmora_dsp STATIC)
add_library(magmora::dsp ALIAS magmora_dsp)
target_include_directories(magmora_dsp PUBLIC include)
target_compile_features(magmora_dsp PUBLIC cxx_std_17)

foreach(pkg IN LISTS _packages)
  add_library(magmora_${pkg} OBJECT ${_sources_${pkg}})
  target_include_directories(magmora_${pkg} PUBLIC include PRIVATE src)
  target_compile_definitions(magmora_${pkg} PRIVATE MAGMORA_VERSION="${MAGMORA_VERSION}")
  magmora_warnings(magmora_${pkg})
  target_sources(magmora_dsp PRIVATE $<TARGET_OBJECTS:magmora_${pkg}>)
endforeach()

# Optional per-package additions, included after the targets exist.
file(GLOB _extras CONFIGURE_DEPENDS "${_src}/*/package.cmake")
foreach(extra IN LISTS _extras)
  include("${extra}")
endforeach()
