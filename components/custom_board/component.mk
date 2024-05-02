#
# "main" pseudo-component makefile.
#
# (Uses default behaviour of compiling all source files in directory, adding 'include' to include path.)

ifdef CONFIG_AUDIO_BOARD_CUSTOM
COMPONENT_ADD_INCLUDEDIRS += ./generic_board/include
COMPONENT_SRCDIRS += ./generic_board

ifdef CONFIG_DAC_PCM51XX
COMPONENT_ADD_INCLUDEDIRS += ./pcm51xx/include
COMPONENT_SRCDIRS += ./pcm51xx
endif

ifdef CONFIG_DAC_PCM5102A
COMPONENT_ADD_INCLUDEDIRS += ./pcm5102a/include
COMPONENT_SRCDIRS += ./pcm5102a
endif

ifdef CONFIG_DAC_MA120X0
COMPONENT_ADD_INCLUDEDIRS += ./ma120x0/include
COMPONENT_SRCDIRS += ./ma120x0
endif

ifdef CONFIG_DAC_MAX98357
COMPONENT_ADD_INCLUDEDIRS += ./max98357/include
COMPONENT_SRCDIRS += ./max98357
endif

ifdef CONFIG_DAC_TAS5805M
COMPONENT_ADD_INCLUDEDIRS += ./tas5805m/include
COMPONENT_SRCDIRS += ./tas5805m
endif

ifdef CONFIG_DAC_PT8211
COMPONENT_ADD_INCLUDEDIRS += ./pt8211/include
COMPONENT_SRCDIRS += ./pt8211
endif

endif
