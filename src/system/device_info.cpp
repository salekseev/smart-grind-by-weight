#include "device_info.h"
#include <cstdint>

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <soc/rtc.h>

namespace device_info {

size_t free_heap_bytes() {
    return esp_get_free_heap_size();
}

size_t total_heap_bytes() {
    return heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
}

size_t free_internal_heap_bytes() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t largest_free_internal_block_bytes() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t free_psram_bytes() {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t flash_chip_size_bytes() {
    uint32_t size = 0;
    if (esp_flash_get_size(nullptr, &size) != ESP_OK) return 0;
    return size;
}

uint64_t efuse_mac() {
    uint8_t mac[6] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
    uint64_t value = 0;
    for (uint8_t byte : mac) {
        value = (value << 8) | byte;
    }
    return value;
}

uint32_t cpu_frequency_mhz() {
    rtc_cpu_freq_config_t config = {};
    rtc_clk_cpu_freq_get_config(&config);
    return config.freq_mhz;
}

bool set_cpu_frequency_mhz(uint32_t mhz) {
    if (mhz == cpu_frequency_mhz()) return true;

    rtc_cpu_freq_config_t config = {};
    if (!rtc_clk_cpu_freq_mhz_to_config(mhz, &config)) return false;

    // The fast variant keeps the ROM's microsecond delay calibration in step,
    // which the HX711 bit-banging driver depends on.
    rtc_clk_cpu_freq_set_config_fast(&config);
    return cpu_frequency_mhz() == mhz;
}

}  // namespace device_info
