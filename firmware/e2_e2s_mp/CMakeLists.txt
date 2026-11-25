# miaopaper_e2_e2s — Miaopaper-compatible reader for Paperang E2 / E2S (ESP-IDF v6.1.1)
#
# 本工程已集成独立底层驱动与字库组件（位于本目录 components/ 下），
# 且仍为 app-only 镜像：严禁直接执行 idf.py flash 覆写引导或分区。
cmake_minimum_required(VERSION 3.16)

# Miaopaper uses a 1.0 s long-press; the shared input component defaults to
# 800 ms. Keep the frozen default overridable.
add_compile_definitions(READER_KEY_LONG_MS=1000)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(miaopaper_e2_e2s)
