# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(snapclient)

# this is needed by udp_logger component. We don't want autostart.
IF (CONFIG_ESP32_UDP_LOGGER_ENABLED)
idf_build_set_property(COMPILE_OPTIONS "-DESP32_UDP_LOGGER_NO_AUTOSTART" APPEND)
endif()