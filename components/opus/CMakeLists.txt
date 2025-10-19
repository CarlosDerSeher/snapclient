file(GLOB srcs "opus/src/*.c")
file(GLOB silk "opus/silk/*.c")
file(GLOB celt "opus/celt/*.c")
file(GLOB fixed "opus/slik/fixed/*.c")
file(GLOB float "opus/slik/float/*.c")


idf_component_register(SRCS "${srcs}" "${silk}" "${celt}" "${fixed}" "${float}"
                       INCLUDE_DIRS .
					                "opus/include"
									"opus/silk"
									"opus/silk/fixed"
									"opus/silk/float"
									"opus/celt"
									)

set_source_files_properties(opus/silk/quant_LTP_gains.c PROPERTIES COMPILE_FLAGS -Wno-maybe-uninitialized)
set_source_files_properties(opus/silk/NLSF2A.c PROPERTIES COMPILE_FLAGS -Wno-maybe-uninitialized)
set_source_files_properties(opus/celt/celt_lpc.c PROPERTIES COMPILE_FLAGS -Wno-maybe-uninitialized)
set_source_files_properties(opus/silk/enc_API.c PROPERTIES COMPILE_FLAGS  -Wno-stringop-overflow)
target_compile_definitions(${COMPONENT_TARGET} PRIVATE "-DHAVE_CONFIG_H")
