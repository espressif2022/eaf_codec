cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "/home/xuxin/esp_work/customer/LSM_DEMO_V1.3/managed_components/espressif__zlib"
    "/home/xuxin/esp_work/customer/LSM_DEMO_V1.3/managed_components/espressif__esp_new_jpeg"
    "/home/xuxin/esp_work/customer/LSM_DEMO_V1.3/managed_components/espressif__cmake_utilities"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(s31_eaf_bench)
