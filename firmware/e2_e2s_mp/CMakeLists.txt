# miaopaper_e2_e2s — Miaopaper-compatible reader for Paperang E2 / E2S (ESP-IDF v6.1.1)
#
# 本工程已集成独立底层驱动与字库组件（位于本目录 components/ 下），
# 且仍为 app-only 镜像：严禁直接执行 idf.py flash 覆写引导或分区。
cmake_minimum_required(VERSION 3.16)

# 统一固件版本号单一真源（自动写入 esp_app_desc_t.version，与关于页/BLE OTA/E2SFlash 保持一致）。
set(PROJECT_VER "1.2.1")

# Miaopaper uses a 1.0 s long-press; the shared input component defaults to
# 800 ms. Keep the frozen default overridable.
add_compile_definitions(READER_KEY_LONG_MS=1000)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(miaopaper_e2_e2s)
