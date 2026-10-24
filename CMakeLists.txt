cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4bridge)

# esp_hosted starts itself from a constructor and then owns the SDMMC host.
# Drop that file: the bridge decides at boot whether the C6 speaks ESPsoup
# (ESSL) or ESP-Hosted and calls esp_hosted_init() itself.
idf_component_get_property(hosted_dir espressif__esp_hosted COMPONENT_DIR)
set_source_files_properties("${hosted_dir}/host/port/esp/freertos/src/port_esp_hosted_host_init.c"
    DIRECTORY "${hosted_dir}" PROPERTIES HEADER_FILE_ONLY TRUE)
