cmake_minimum_required(VERSION 3.15)

# Build as an ESP-IDF component
   idf_component_register(
      SRCS "improvWifi.cpp" "improv_wrapper.cpp" "wifi_provisioning.c"
      PRIV_INCLUDE_DIRS "Improv-WiFi-Library/src" "priv_include"
      INCLUDE_DIRS "include"
      PRIV_REQUIRES driver esp_wifi network_interface
      #SRCS "Improv-WiFi-Library/src/ImprovWiFiLibrary.cpp" "wifi_provisioning.c"
      #INCLUDE_DIRS "Improv-WiFi-Library/src" "include"
   )
