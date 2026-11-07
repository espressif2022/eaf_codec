# Root ESP-IDF component registration so `eaf_codec` can be used directly
# as an ESP-IDF component or via `EXTRA_COMPONENT_DIRS`.
set(COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/components/esp_lv_eaf_player")

set(PRIV_REQUIRES_LIST esp_mm esp_timer espressif__zlib)
if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND PRIV_REQUIRES_LIST esp_driver_jpeg)
endif()

idf_component_register(
    SRC_DIRS "${COMPONENT_DIR}/src"
    INCLUDE_DIRS "${COMPONENT_DIR}/include"
    PRIV_INCLUDE_DIRS "${COMPONENT_DIR}/src"
    PRIV_REQUIRES ${PRIV_REQUIRES_LIST}
)

idf_build_get_property(build_components BUILD_COMPONENTS)
if(lvgl IN_LIST build_components)
    set(lvgl_name lvgl)
elseif(lvgl__lvgl IN_LIST build_components)
    set(lvgl_name lvgl__lvgl)
endif()

set(extra_libs "")
if(DEFINED lvgl_name)
    list(APPEND extra_libs idf::${lvgl_name})
    idf_component_get_property(lvgl_dir ${lvgl_name} COMPONENT_DIR)
    if(EXISTS "${lvgl_dir}")
        target_include_directories(${COMPONENT_LIB} PRIVATE "${lvgl_dir}")
    endif()
endif()

if(CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG)
    if("esp_new_jpeg" IN_LIST build_components)
        list(APPEND extra_libs idf::esp_new_jpeg)
    elseif("espressif__esp_new_jpeg" IN_LIST build_components)
        list(APPEND extra_libs idf::espressif__esp_new_jpeg)
    endif()
endif()

if("espressif__esp_h264" IN_LIST build_components)
    list(APPEND extra_libs idf::espressif__esp_h264)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_LV_EAF_ENABLE_H264=1)
endif()

idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32p4" OR target STREQUAL "esp32s31")
    list(APPEND extra_libs idf::esp_driver_jpeg)
endif()

if(extra_libs)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${extra_libs})
endif()
