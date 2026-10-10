"""Keep firmware updates from erasing the whole inactive partition up front."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]


class OtaWriterTest(unittest.TestCase):
    def test_partition_is_erased_as_the_image_arrives(self):
        # A browser upload runs on the HTTP server's only task; erasing the
        # image's extent before the first write stalls every request for seconds.
        source = (ROOT / "src/network/ota_writer.cpp").read_text()
        code = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
using esp_err_t = int;
using esp_ota_handle_t = uint32_t;
constexpr esp_err_t ESP_OK = 0;
#define OTA_SIZE_UNKNOWN 0xffffffff
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
struct esp_partition_t { size_t size; const char* label; } inactive{0x300000, "ota_1"};
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &inactive; }
size_t begun_with = 0;
esp_err_t esp_ota_begin(const esp_partition_t* partition, size_t image_size, esp_ota_handle_t* out) {
    assert(partition == &inactive);
    begun_with = image_size; *out = 1; return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t) { return ESP_OK; }
const char* esp_err_to_name(esp_err_t) { return "error"; }
class OtaWriter {
public:
    bool begin(size_t expected_size);
private:
    bool fail(const char* reason);
    esp_ota_handle_t handle_ = 0;
    const esp_partition_t* partition_ = nullptr;
    size_t written_ = 0;
    const char* error_ = "";
};
''' + function(source, "bool OtaWriter::fail(") + function(source, "bool OtaWriter::begin(") + r'''
int main() {
    for (size_t size : {size_t{0}, size_t{2455776}}) {
        OtaWriter writer; begun_with = 0;
        assert(writer.begin(size));
        assert(begun_with == OTA_WITH_SEQUENTIAL_WRITES);
    }
    OtaWriter oversized;
    assert(!oversized.begin(inactive.size + 1));
}
'''
        with tempfile.TemporaryDirectory() as folder:
            cpp, binary = Path(folder) / "writer.cpp", Path(folder) / "writer"
            cpp.write_text(code)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
