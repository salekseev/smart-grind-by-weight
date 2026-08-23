#pragma once

#include <esp_ota_ops.h>

#include <cstddef>
#include <cstdint>

//==============================================================================
// WEB OTA IMAGE WRITER
//==============================================================================
// Streams a firmware image into the inactive app partition and marks it bootable
// once it validates. Used by both the browser upload and the GitHub release
// download; the BLE path writes a delta patch instead and goes through
// components/delta.

class OtaWriter {
public:
    /**
     * Open the inactive partition for writing.
     *
     * @param expected_size Image size when known, or 0 to size it from the
     *        partition. Sizes larger than the partition are rejected.
     */
    bool begin(size_t expected_size);

    /** Append image bytes. Writes must arrive in order. */
    bool write(const uint8_t* data, size_t length);

    /** Validate the image and set it as the boot partition. */
    bool end();

    /** Discard a partially written image. */
    void abort();

    bool is_active() const { return handle_ != 0; }

    /** Human-readable reason the last operation failed. */
    const char* error() const { return error_; }

private:
    bool fail(const char* reason);

    esp_ota_handle_t handle_ = 0;
    const esp_partition_t* partition_ = nullptr;
    size_t written_ = 0;
    const char* error_ = "";
};
