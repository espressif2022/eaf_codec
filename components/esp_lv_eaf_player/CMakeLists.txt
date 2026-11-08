set(PRIV_REQUIRES_LIST esp_mm esp_timer espressif__zlib)
if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND PRIV_REQUIRES_LIST esp_driver_jpeg)
endif()

set(EAF_SRCS
    "src/core/esp_eaf_player.c"
    "src/core/esp_eaf_decoder.c"
    "src/container/eaf/esp_eaf_container.c"
    "src/container/eaf4/esp_eaf4_container.c"
    "src/codec/esp_eaf_codec_registry.c"
    "src/codec/esp_eaf_codec_bounded.c"
    "src/codec/common/esp_eaf_codec_support.c"
    "src/codec/eaf/compat.c"
    "src/codec/eaf/lossless.c"
    "src/codec/eaf/jpeg_delta.c"
    "src/codec/eaf/video.c"
    "src/codec/eaf4/lossless.c"
)

# LVGL is an optional adapter, not a core dependency. Applications using
# lv_eaf.h add lvgl/lvgl themselves; other GUIs only build esp_eaf_player.h.
idf_build_get_property(build_components BUILD_COMPONENTS)
set(lvgl_name "")
if(lvgl IN_LIST build_components)
    set(lvgl_name lvgl)
elseif(lvgl__lvgl IN_LIST build_components)
    set(lvgl_name lvgl__lvgl)
endif()
if(NOT "${lvgl_name}" STREQUAL "")
    list(APPEND EAF_SRCS "src/adapter/lvgl/lv_eaf.c")
endif()

idf_component_register(
    SRCS ${EAF_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "src/codec"
        "src/core"
        "src/adapter/lvgl"
    PRIV_REQUIRES ${PRIV_REQUIRES_LIST}
)

set(extra_libs "")
if(NOT "${lvgl_name}" STREQUAL "")
    idf_component_get_property(lvgl_ver ${lvgl_name} COMPONENT_VERSION)
    message(STATUS "esp_lv_eaf_player: optional LVGL adapter enabled (${lvgl_ver})")
    list(APPEND extra_libs idf::${lvgl_name})
    idf_component_get_property(lvgl_dir ${lvgl_name} COMPONENT_DIR)
    if(EXISTS "${lvgl_dir}")
        target_include_directories(${COMPONENT_LIB} PRIVATE "${lvgl_dir}")
    endif()
else()
    message(STATUS "esp_lv_eaf_player: core-only build (no LVGL component found)")
endif()

if(CONFIG_ESP_LV_EAF_ENABLE_SW_JPEG)
    if("esp_new_jpeg" IN_LIST build_components)
        list(APPEND extra_libs idf::esp_new_jpeg)
    endif()
    if("espressif__esp_new_jpeg" IN_LIST build_components)
        list(APPEND extra_libs idf::espressif__esp_new_jpeg)
    endif()
else()
    message(STATUS "esp_lv_eaf_player: software JPEG decoder disabled")
endif()

if("espressif__esp_h264" IN_LIST build_components)
    list(APPEND extra_libs idf::espressif__esp_h264)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESP_LV_EAF_ENABLE_H264=1)
    message(STATUS "esp_lv_eaf_player: H.264 decoder enabled")
endif()

if(IDF_TARGET STREQUAL "esp32p4" OR IDF_TARGET STREQUAL "esp32s31")
    list(APPEND extra_libs idf::esp_driver_jpeg)
endif()

if(extra_libs)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${extra_libs})
endif()

include(package_manager)
cu_pkg_define_version(${CMAKE_CURRENT_LIST_DIR})
