#pragma once

#include <stddef.h>
#include <stdint.h>

//==============================================================================
// SYSTEM AND CHIP INFORMATION
//==============================================================================
// Thin, self-documenting accessors over the ESP-IDF heap, efuse, flash and clock
// APIs. The diagnostics screen, BLE system-info service and the HTTP status
// endpoint all report these values.

namespace device_info {

/** Free bytes across every heap region (internal + PSRAM). */
size_t free_heap_bytes();

/** Total bytes across every heap region (internal + PSRAM). */
size_t total_heap_bytes();

/** Free bytes in internal (DMA-capable) RAM only. */
size_t free_internal_heap_bytes();

/** Largest single allocatable block of internal RAM. */
size_t largest_free_internal_block_bytes();

/** Free bytes of external PSRAM, or 0 when PSRAM is unavailable. */
size_t free_psram_bytes();

/** Size of the attached SPI flash chip in bytes. */
size_t flash_chip_size_bytes();

/** Factory-programmed 48-bit MAC address, used as the stable device identifier. */
uint64_t efuse_mac();

/** Current CPU frequency in MHz. */
uint32_t cpu_frequency_mhz();

/**
 * Switch the CPU frequency, mirroring the reduced-clock window the BLE OTA
 * handler uses to keep flash writes stable.
 *
 * @return true when the requested frequency was applied.
 */
bool set_cpu_frequency_mhz(uint32_t mhz);

}  // namespace device_info
