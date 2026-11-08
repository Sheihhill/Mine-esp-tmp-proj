# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 固件版本号：发新版本时改这里（会显示在网页"设备状态"栏，也用于区分 OTA 前后固件）
set(PROJECT_VER "0.4")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(softap_sta)
