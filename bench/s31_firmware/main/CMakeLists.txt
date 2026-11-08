set(BENCH_REQUIRES esp_partition esp_timer esp_psram esp_mm esp_rom espressif__zlib espressif__esp_new_jpeg)
idf_build_get_property(bench_target IDF_TARGET)
if(bench_target STREQUAL "esp32s3")
    list(APPEND BENCH_REQUIRES esp_lcd espressif__esp32_s3_lcd_ev_board)
endif()
if(bench_target STREQUAL "esp32p4" OR bench_target STREQUAL "esp32s31")
    list(APPEND BENCH_REQUIRES esp_driver_ppa esp_driver_jpeg)
endif()

idf_component_register(
    SRCS "main.c"
         "../../../components/esp_lv_eaf_player/src/core/esp_eaf_decoder.c"
         "../../../components/esp_lv_eaf_player/src/container/eaf/esp_eaf_container.c"
         "../../../components/esp_lv_eaf_player/src/container/eaf4/esp_eaf4_container.c"
         "../../../components/esp_lv_eaf_player/src/codec/esp_eaf_codec_registry.c"
         "../../../components/esp_lv_eaf_player/src/codec/esp_eaf_codec_bounded.c"
         "../../../components/esp_lv_eaf_player/src/codec/common/esp_eaf_codec_support.c"
         "../../../components/esp_lv_eaf_player/src/codec/eaf/compat.c"
         "../../../components/esp_lv_eaf_player/src/codec/eaf/lossless.c"
         "../../../components/esp_lv_eaf_player/src/codec/eaf/jpeg_delta.c"
         "../../../components/esp_lv_eaf_player/src/codec/eaf/video.c"
         "../../../components/esp_lv_eaf_player/src/codec/eaf4/lossless.c"
    INCLUDE_DIRS "."
                 "../../../components/esp_lv_eaf_player/include"
                 "../../../components/esp_lv_eaf_player/src/codec"
                 "../../../components/esp_lv_eaf_player/src/core"
    REQUIRES ${BENCH_REQUIRES}
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG=1
)
