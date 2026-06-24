# ============================================================
# Корневой CMakeLists.txt — обязателен для ESP-IDF build system
#
# Эти три строки в этом порядке обязательны для ЛЮБОГО ESP-IDF проекта.
# ============================================================
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pwny_s3)
