#include "ota_writer.h"

#include <esp_err.h>

#include "../config/logging.h"

bool OtaWriter::fail(const char* reason) {
    error_ = reason;
    if (handle_ != 0) {
        esp_ota_abort(handle_);
        handle_ = 0;
    }
    return false;
}

bool OtaWriter::begin(size_t expected_size) {
    if (handle_ != 0) return fail("An update is already in progress");

    partition_ = esp_ota_get_next_update_partition(nullptr);
    if (!partition_) return fail("No inactive firmware partition");
    if (expected_size > partition_->size) return fail("Firmware image is too large");

    written_ = 0;
    error_ = "";
    const size_t size = expected_size > 0 ? expected_size : OTA_SIZE_UNKNOWN;
    const esp_err_t err = esp_ota_begin(partition_, size, &handle_);
    if (err != ESP_OK) {
        handle_ = 0;
        return fail(esp_err_to_name(err));
    }
    return true;
}

bool OtaWriter::write(const uint8_t* data, size_t length) {
    if (handle_ == 0) return fail("No update in progress");
    if (length == 0) return true;
    if (written_ + length > partition_->size) return fail("Firmware image is too large");

    const esp_err_t err = esp_ota_write(handle_, data, length);
    if (err != ESP_OK) return fail(esp_err_to_name(err));
    written_ += length;
    return true;
}

bool OtaWriter::end() {
    if (handle_ == 0) return fail("No update in progress");

    const esp_ota_handle_t handle = handle_;
    handle_ = 0;
    const esp_err_t validated = esp_ota_end(handle);
    if (validated != ESP_OK) {
        error_ = validated == ESP_ERR_OTA_VALIDATE_FAILED ? "Image failed validation"
                                                          : esp_err_to_name(validated);
        return false;
    }

    const esp_err_t activated = esp_ota_set_boot_partition(partition_);
    if (activated != ESP_OK) {
        error_ = esp_err_to_name(activated);
        return false;
    }
    LOG_BLE("[WEB OTA] Wrote %u bytes to %s\n", static_cast<unsigned>(written_),
            partition_->label);
    return true;
}

void OtaWriter::abort() {
    if (handle_ == 0) return;
    esp_ota_abort(handle_);
    handle_ = 0;
}
