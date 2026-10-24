idf_component_register(SRCS "main.c" "eth.c" "web.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_sdmmc sdmmc esp_driver_usb_serial_jtag esp_eth esp_netif lwip nvs_flash esp_event esp_timer esp_hosted esp_serial_slave_link app_update
             esp_http_server esp_partition vfs littlefs)

# LittleFS image of ../www (staged there by tools/make_www.py) -> build/www.bin,
# flashed to the "www" partition by "idf.py flash" or uploaded with "P4 WWW".
# This is littlefs_create_partition_image(www ../www FLASH_IN_PROJECT) of the
# joltwallet/littlefs component (same venv, same tool, same options) plus
# "--compact": the files then sit at the start of the image instead of at a
# pseudo-random position, so the 0xFF tail of the full-size (6 MB) image can
# be skipped by the "P4 WWW" uploader.
set(www_dir "${CMAKE_CURRENT_SOURCE_DIR}/../www")
set(www_image "${CMAKE_BINARY_DIR}/www.bin")
partition_table_get_partition_info(www_size "--partition-name www" "size")
if(NOT www_size)
    message(FATAL_ERROR "no 'www' partition in the partition table")
endif()
idf_build_get_property(python PYTHON)
if(CMAKE_HOST_WIN32)
    set(www_python "${littlefs_py_venv}/Scripts/python.exe")
else()
    set(www_python "${littlefs_py_venv}/bin/python")
endif()
add_custom_command(OUTPUT ${littlefs_py_venv}
    COMMAND ${python} -m venv ${littlefs_py_venv} && ${www_python} -m pip install -r ${littlefs_py_requirements}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS ${littlefs_py_requirements})
add_custom_target(littlefs_www_bin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${www_dir}
    COMMAND ${www_python} -m littlefs create ${www_dir} ${www_image} -v --compact
            --fs-size=${www_size} --name-max=${CONFIG_LITTLEFS_OBJ_NAME_LEN} --block-size=4096
    DEPENDS ${littlefs_py_venv})
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${www_image})
esp_partition_register_target(www "${www_image}" DEPENDS littlefs_www_bin FLASH_IN_PROJECT)
