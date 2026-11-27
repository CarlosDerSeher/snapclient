# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Firmware version (esp_app_get_description()->version, logged at boot): git
# describe in a git checkout, e.g. v0.0.4-12-g1a2b3c4-dirty; the hash is the
# commit on GitHub. Without .git (a source archive), version.txt, which git
# archive fills in (export-subst in .gitattributes). Set here because IDF would
# otherwise prefer version.txt over git describe.
git_describe(PROJECT_VER "${CMAKE_CURRENT_LIST_DIR}")
if(NOT PROJECT_VER OR PROJECT_VER MATCHES "NOTFOUND")
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_CURRENT_LIST_DIR}/version.txt")
  if(NOT PROJECT_VER OR PROJECT_VER MATCHES "Format:")
    set(PROJECT_VER "unknown")
  endif()
endif()

# lwIP: acknowledge as soon as the application has read a full segment
# (default: a quarter of the receive window, otherwise the delayed ACK timer,
# CONFIG_LWIP_TCP_TMR_INTERVAL). Under packet loss the server's congestion
# window shrinks to a few segments; it then waits for our ACKs, and a delayed
# ACK (up to 250 ms) outlasts its retransmit timeout, so it resends data we
# already have and shrinks the window further. Applies to all components;
# lwIP's opt.h only sets the default when it is not defined yet.
idf_build_set_property(COMPILE_OPTIONS "-DTCP_WND_UPDATE_THRESHOLD=TCP_MSS" APPEND)

project(snapclient)

# this is needed by udp_logger component. We don't want autostart.
if(CONFIG_ESP32_UDP_LOGGER_ENABLED)
  idf_build_set_property(COMPILE_OPTIONS "-DESP32_UDP_LOGGER_NO_AUTOSTART" APPEND)
endif()