idf_component_register(SRCS "main.cpp" "ui.cpp" "settings.cpp" "speaker.cpp"
    PRIV_REQUIRES hw1_stt nvs_flash fatfs sdmmc esp_driver_sdmmc esp_driver_i2s esp_driver_gpio
                  esp_driver_spi esp_lcd esp_timer)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
