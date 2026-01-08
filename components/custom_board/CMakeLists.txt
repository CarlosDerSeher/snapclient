# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES)
set(COMPONENT_PRIV_REQUIRES audio_hal esp_peripherals)

if(CONFIG_AUDIO_BOARD_CUSTOM)
  message(STATUS "Current board name is " CONFIG_AUDIO_BOARD_CUSTOM)
  set(COMPONENT_ADD_INCLUDEDIRS ./generic_board/include)
  set(COMPONENT_SRCS
	./generic_board/board.c
	./generic_board/board_pins_config.c
	)

  if (CONFIG_DAC_ADAU1961)
    message(STATUS "Selected DAC is " CONFIG_DAC_ADAU1961)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./adau1961/include)
	list(APPEND COMPONENT_SRCS ./adau1961/adau1961.c)
  endif()

  if(CONFIG_DAC_PCM51XX)
	message(STATUS "Selected DAC is " CONFIG_DAC_PCM51XX)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./pcm51xx/include)
	list(APPEND COMPONENT_SRCS ./pcm51xx/pcm51xx.c)
  endif()

  if(CONFIG_DAC_PCM5102A)
	message(STATUS "Selected DAC is " CONFIG_DAC_PCM5102A)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./pcm5102a/include)
	list(APPEND COMPONENT_SRCS ./pcm5102a/pcm5102a.c)
  endif()

  if(CONFIG_DAC_MA120X0)
	message(STATUS "Selected DAC is " CONFIG_DAC_MA120X0)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./ma120x0/include)
	list(APPEND COMPONENT_SRCS ./ma120x0/MerusAudio.c)
  endif()

  if(CONFIG_DAC_MA120)
	message(STATUS "Selected DAC is " CONFIG_DAC_MA120)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./ma120/include)
	list(APPEND COMPONENT_SRCS ./ma120/ma120.c)
  endif()

  if(CONFIG_DAC_MAX98357)
	message(STATUS "Selected DAC is " CONFIG_DAC_MAX98357)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./max98357/include)
	list(APPEND COMPONENT_SRCS ./max98357/max98357.c)
  endif()

  if(CONFIG_DAC_TAS5805M)
	message(STATUS "Selected DAC is " CONFIG_DAC_TAS5805M)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./tas5805m/include)
	list(APPEND COMPONENT_SRCS ./tas5805m/tas5805m.c)
  endif()

  if(CONFIG_DAC_PT8211)
	message(STATUS "Selected DAC is " CONFIG_DAC_PT8211)
	list(APPEND COMPONENT_ADD_INCLUDEDIRS ./pt8211/include)
	list(APPEND COMPONENT_SRCS ./pt8211/pt8211.c)
  endif()
endif()

register_component()

IF (IDF_VER MATCHES "v4.")
idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})

ELSEIF (IDF_VER MATCHES "v3.")
set_property(TARGET idf_component_audio_board APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES  $<TARGET_PROPERTY:${COMPONENT_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)

ENDIF (IDF_VER MATCHES "v4.")
