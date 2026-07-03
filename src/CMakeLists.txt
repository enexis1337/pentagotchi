# ============================================================
# src/CMakeLists.txt — регистрация основного компонента приложения
#
# INCLUDE_DIRS "." добавляет src/ для внутренних #include "config.h" и т.п.
# Заголовки из ../include/ нужно прописать отдельно ниже.
# ============================================================

file(GLOB_RECURSE app_sources ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
list(FILTER app_sources EXCLUDE REGEX "lib_ssd1680(_old)?/")

idf_component_register(
    SRCS ${app_sources} 
         "eink_regions.cpp"
    INCLUDE_DIRS
        "."
        "../include"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        fatfs
        sdmmc
        driver
        arduinojson
        u8g2
        gxepd2
)
