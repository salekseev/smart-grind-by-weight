# The firmware sources form the project's main ESP-IDF component. Third-party
# dependencies are declared in idf_component.yml; delta/detools live under
# components/ and are picked up automatically.
file(GLOB_RECURSE app_sources
     "${CMAKE_CURRENT_LIST_DIR}/*.c"
     "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    REQUIRES
        # Espressif components
        app_update
        driver
        esp_driver_usb_serial_jtag
        esp_event
        esp_http_client
        esp_http_server
        esp_lcd
        esp_netif
        esp_partition
        esp_timer
        esp_wifi
        esp-tls
        esptool_py
        lwip
        mbedtls
        nvs_flash
        spi_flash
        vfs
        # Managed / local components
        delta
        esp_lcd_co5300
        esp_websocket_client
        improv
        littlefs
        lvgl
        mdns
        esp-nimble-cpp
)

# The generated LVGL font sources in src/font pick their lvgl.h include style
# from this macro.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
