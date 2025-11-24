set(COMPONENT_REQUIRES)
set(COMPONENT_PRIV_REQUIRES esp-dsp lightsnapcast dsp_processor_settings)

list(APPEND COMPONENT_ADD_INCLUDEDIRS ./include)
set(COMPONENT_SRCS ./dsp_processor.c)
register_component()
