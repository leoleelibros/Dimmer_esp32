idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES spiffs nvs_flash esp_wifi esp_event driver esp_http_server)
spiffs_create_partition_image(Datos "${CMAKE_CURRENT_SOURCE_DIR}/../InternalDisk" FLASH_IN_PROJECT)