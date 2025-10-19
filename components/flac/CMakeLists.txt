# 3.1 is OK for most parts. However:
# 3.3 is needed in src/libFLAC
# 3.5 is needed in src/libFLAC/ia32
# 3.9 is needed in 'doc' because of doxygen_add_docs()
cmake_minimum_required(VERSION 3.5)

file(GLOB srcs "flac/src/libFLAC/*.c")
list(FILTER srcs EXCLUDE REGEX ".*ogg.*\\.c$")
list(FILTER srcs EXCLUDE REGEX "windows_unicode_filenames\\.c$")

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

set_source_files_properties(flac/src/libFLAC/bitwriter.c PROPERTIES COMPILE_FLAGS -Wno-error=format)
set_source_files_properties(flac/src/libFLAC/bitreader.c PROPERTIES COMPILE_FLAGS -Wno-error=format)
set_source_files_properties(flac/src/libFLAC/stream_decoder.c PROPERTIES COMPILE_FLAGS -Wno-incompatible-pointer-types)
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_CONFIG_H")
