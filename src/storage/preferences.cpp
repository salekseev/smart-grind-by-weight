#include "preferences.h"

#include <esp_err.h>

#include <cstring>
#include <vector>

Preferences::~Preferences() {
    end();
}

bool Preferences::begin(const char* name, bool read_only) {
    if (open_ || !name) return false;

    const esp_err_t err =
        nvs_open(name, read_only ? NVS_READONLY : NVS_READWRITE, &handle_);
    if (err != ESP_OK) return false;

    open_ = true;
    read_only_ = read_only;
    return true;
}

void Preferences::end() {
    if (!open_) return;
    nvs_close(handle_);
    handle_ = 0;
    open_ = false;
}

bool Preferences::commit() {
    return nvs_commit(handle_) == ESP_OK;
}

bool Preferences::clear() {
    if (!open_ || read_only_) return false;
    return nvs_erase_all(handle_) == ESP_OK && commit();
}

bool Preferences::remove(const char* key) {
    if (!open_ || read_only_ || !key) return false;
    const esp_err_t err = nvs_erase_key(handle_, key);
    // Erasing a key that was never written is not a failure for callers that
    // just want it gone.
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    return err == ESP_OK && commit();
}

bool Preferences::isKey(const char* key) {
    if (!open_ || !key) return false;
    size_t length = 0;
    // A type-agnostic probe: ask for the entry as a blob and as a string, then
    // fall back to the fixed-width getters. nvs_find_key reports the stored
    // type directly and covers every case in one call.
    nvs_type_t type = NVS_TYPE_ANY;
    if (nvs_find_key(handle_, key, &type) == ESP_OK) return true;
    return nvs_get_blob(handle_, key, nullptr, &length) == ESP_OK;
}

size_t Preferences::putChar(const char* key, int8_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_i8(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putUChar(const char* key, uint8_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_u8(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putShort(const char* key, int16_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_i16(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putUShort(const char* key, uint16_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_u16(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putInt(const char* key, int32_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_i32(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putUInt(const char* key, uint32_t value) {
    if (!open_ || read_only_ || !key) return 0;
    if (nvs_set_u32(handle_, key, value) != ESP_OK || !commit()) return 0;
    return sizeof(value);
}

size_t Preferences::putBool(const char* key, bool value) {
    return putUChar(key, value ? 1 : 0);
}

size_t Preferences::putFloat(const char* key, float value) {
    return putBytes(key, &value, sizeof(value));
}

size_t Preferences::putDouble(const char* key, double value) {
    return putBytes(key, &value, sizeof(value));
}

size_t Preferences::putString(const char* key, const char* value) {
    if (!open_ || read_only_ || !key || !value) return 0;
    if (nvs_set_str(handle_, key, value) != ESP_OK || !commit()) return 0;
    return strlen(value);
}

size_t Preferences::putString(const char* key, const std::string& value) {
    return putString(key, value.c_str());
}

size_t Preferences::putBytes(const char* key, const void* value, size_t length) {
    if (!open_ || read_only_ || !key || !value || length == 0) return 0;
    if (nvs_set_blob(handle_, key, value, length) != ESP_OK || !commit()) return 0;
    return length;
}

int8_t Preferences::getChar(const char* key, int8_t default_value) {
    if (!open_ || !key) return default_value;
    int8_t value = default_value;
    return nvs_get_i8(handle_, key, &value) == ESP_OK ? value : default_value;
}

uint8_t Preferences::getUChar(const char* key, uint8_t default_value) {
    if (!open_ || !key) return default_value;
    uint8_t value = default_value;
    return nvs_get_u8(handle_, key, &value) == ESP_OK ? value : default_value;
}

int16_t Preferences::getShort(const char* key, int16_t default_value) {
    if (!open_ || !key) return default_value;
    int16_t value = default_value;
    return nvs_get_i16(handle_, key, &value) == ESP_OK ? value : default_value;
}

uint16_t Preferences::getUShort(const char* key, uint16_t default_value) {
    if (!open_ || !key) return default_value;
    uint16_t value = default_value;
    return nvs_get_u16(handle_, key, &value) == ESP_OK ? value : default_value;
}

int32_t Preferences::getInt(const char* key, int32_t default_value) {
    if (!open_ || !key) return default_value;
    int32_t value = default_value;
    return nvs_get_i32(handle_, key, &value) == ESP_OK ? value : default_value;
}

uint32_t Preferences::getUInt(const char* key, uint32_t default_value) {
    if (!open_ || !key) return default_value;
    uint32_t value = default_value;
    return nvs_get_u32(handle_, key, &value) == ESP_OK ? value : default_value;
}

bool Preferences::getBool(const char* key, bool default_value) {
    return getUChar(key, default_value ? 1 : 0) == 1;
}

float Preferences::getFloat(const char* key, float default_value) {
    float value = default_value;
    getBytes(key, &value, sizeof(value));
    return value;
}

double Preferences::getDouble(const char* key, double default_value) {
    double value = default_value;
    getBytes(key, &value, sizeof(value));
    return value;
}

std::string Preferences::getString(const char* key, const char* default_value) {
    if (!open_ || !key) return default_value ? default_value : "";

    size_t length = 0;
    if (nvs_get_str(handle_, key, nullptr, &length) != ESP_OK || length == 0) {
        return default_value ? default_value : "";
    }

    std::vector<char> buffer(length);
    if (nvs_get_str(handle_, key, buffer.data(), &length) != ESP_OK) {
        return default_value ? default_value : "";
    }
    // NVS reports the length including the terminator.
    return std::string(buffer.data());
}

std::string Preferences::getString(const char* key, const std::string& default_value) {
    return getString(key, default_value.c_str());
}

size_t Preferences::getBytes(const char* key, void* buffer, size_t max_length) {
    const size_t length = getBytesLength(key);
    if (length == 0 || !buffer || max_length == 0) return length;
    // Refuse partial reads: a truncated blob would silently corrupt the
    // statistics snapshot and the calibration record.
    if (length > max_length) return 0;

    size_t read_length = length;
    if (nvs_get_blob(handle_, key, buffer, &read_length) != ESP_OK) return 0;
    return read_length;
}

size_t Preferences::getBytesLength(const char* key) {
    if (!open_ || !key) return 0;
    size_t length = 0;
    if (nvs_get_blob(handle_, key, nullptr, &length) != ESP_OK) return 0;
    return length;
}
