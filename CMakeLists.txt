# Copyright (c) 2024 Cesanta Software Limited
# Mail: support@cesanta.com
#
# SPDX-License-Identifier: GPL2.0 or commercial

cmake_minimum_required(VERSION 3.20.0)
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/esp32s3.overlay)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp_wifi)

file(GLOB mongoose_files ${CMAKE_CURRENT_SOURCE_DIR}/mongoose/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c ${mongoose_files})

target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/inc" "${CMAKE_CURRENT_SOURCE_DIR}/mongoose")
