cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL ships as a managed component and locates lv_conf.h through
# LV_CONF_INCLUDE_SIMPLE, so the project's include/ directory has to be on the
# search path of every component - not just the firmware sources.
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_LIST_DIR}/include" APPEND)

project(smart-grind-by-weight)
