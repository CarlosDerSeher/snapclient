# 3.1 is OK for most parts. However:
# 3.3 is needed in src/libFLAC
# 3.5 is needed in src/libFLAC/ia32
# 3.9 is needed in 'doc' because of doxygen_add_docs()
cmake_minimum_required(VERSION 3.5)

file(GLOB srcs "flac/src/libFLAC/*.c")
list(FILTER srcs EXCLUDE REGEX ".*ogg.*\\.c$")
list(FILTER srcs EXCLUDE REGEX "windows_unicode_filenames\\.c$")

# libFLAC's input buffer (bitreader) is 8 KB. It only has to hold a few hundred bits at a time and
# is refilled through the read callback, so 1 KB is enough and saves 7 KB of heap. The constant is
# not configurable, so compile a patched copy of bitreader.c from the build directory instead of
# changing the submodule.
set(bitreader_src "${CMAKE_CURRENT_LIST_DIR}/flac/src/libFLAC/bitreader.c")
set(bitreader_patched "${CMAKE_CURRENT_BINARY_DIR}/bitreader.c")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  file(READ "${bitreader_src}" content)
  string(REPLACE "65536u / FLAC__BITS_PER_WORD" "8192u / FLAC__BITS_PER_WORD" patched "${content}")
  if(patched STREQUAL content)
    message(FATAL_ERROR "flac: bitreader capacity not found in ${bitreader_src}, update the patch")
  endif()
  # write via a temp file so an unchanged copy keeps its timestamp and is not rebuilt
  file(WRITE "${bitreader_patched}.tmp" "${patched}")
  configure_file("${bitreader_patched}.tmp" "${bitreader_patched}" COPYONLY)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bitreader_src}")
endif()
list(FILTER srcs EXCLUDE REGEX "/bitreader\\.c$")
list(APPEND srcs "${bitreader_patched}")

#project(FLAC VERSION 1.3.3) # HOMEPAGE_URL "https://www.xiph.org/flac/")
set(VERSION "1.3.3")
set(PACKAGE_VERSION "1.3.3")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS .
                       "flac/include"
                       PRIV_INCLUDE_DIRS "flac/src/libFLAC/include/"
		      )

#set_source_files_properties(opus/silk/quant_LTP_gains.c PROPERTIES COMPILE_FLAGS -Wno-maybe-uninitialized)
#target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_CONFIG_H")

set_source_files_properties(flac/src/libFLAC/bitwriter.c PROPERTIES COMPILE_FLAGS -Wno-format)
set_source_files_properties(${bitreader_patched} PROPERTIES COMPILE_FLAGS -Wno-format)
set_source_files_properties(flac/src/libFLAC/stream_decoder.c PROPERTIES COMPILE_FLAGS -Wno-incompatible-pointer-types)
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_CONFIG_H")
