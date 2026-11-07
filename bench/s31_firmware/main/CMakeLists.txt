idf_component_register(
    SRCS "main.c"
         "../../../components/esp_lv_eaf_player/src/eaf4.c"
         "../../../components/esp_lv_eaf_player/src/esp_eaf_dec.c"
    INCLUDE_DIRS "."
                 "../../../components/esp_lv_eaf_player/include"
                 "../../../components/esp_lv_eaf_player/src"
    REQUIRES esp_partition esp_timer esp_psram esp_driver_ppa esp_driver_jpeg esp_mm esp_rom espressif__zlib espressif__esp_new_jpeg
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG=1
    CONFIG_ESP_LV_EAF_ENABLE_HW_JPEG=1
)
