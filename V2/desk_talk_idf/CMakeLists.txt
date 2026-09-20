# 2026-09-19: Create the ESP-IDF project entry and lock the migrated firmware to ESP32-S3.
cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(desk_talk_idf)
