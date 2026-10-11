"""Run the production GitHub release download against a model of ESP-IDF's HTTP client.

DeviceWebServer::perform_github_ota is compiled unchanged against host fakes of
esp_http_client, the inactive-partition writer and a fake millisecond clock.
The fake esp_http_client_read follows esp_http_client.c and transport_ssl.c in
ESP-IDF 6.1: each call keeps reading until the caller's buffer is full or the
body is complete, waiting up to the client's timeout_ms for every TLS read. A
wait that times out returns the bytes gathered so far, or -ESP_ERR_HTTP_EAGAIN
when there are none. A peer close (FIN) returns the bytes gathered so far, or
ESP_FAIL when there are none and the body is incomplete. The call returns 0
only once the whole body has arrived. A scripted number of redirect responses
can precede the image; as in esp_http_client.c, the caller drains each one and
calls esp_http_client_set_redirection before opening again. Time passes only
while a read is blocked so every elapsed-time assertion is exact.
"""
from pathlib import Path
from types import SimpleNamespace
import re
import subprocess
import tempfile
import unittest
from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]
PARTITION_SIZE = 0x200000
SOURCE = (ROOT / "src/network/device_web_server.cpp").read_text()
# The per-read wait that the production download configures on its client.
READ_TIMEOUT_MS = int(re.search(r"OTA_READ_TIMEOUT_MS = (\d+);", SOURCE).group(1))
MAX_REDIRECTS = int(re.search(r"HTTP_MAX_REDIRECTS = (\d+);", SOURCE).group(1))

HARNESS = r'''
#include "config/constants.h"  // production LOG_BLE and HW_RELEASE_FIRMWARE_SUFFIX

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "harness: %s\n", message);
    std::exit(2);
}

// Production LOG_BLE expands to diagnostic_log_printf; keep its output for failures.
void diagnostic_log_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

// ---- Fake clock: only blocked reads advance it ----
constexpr uint32_t FAKE_RUN_LIMIT_MS = 10U * 60U * 1000U;
uint32_t clock_ms = 0;
uint32_t run_started_ms = 0;
uint32_t elapsed_ms() { return clock_ms - run_started_ms; }
void advance_clock(uint32_t ms) {
    clock_ms += ms;
    check(elapsed_ms() <= FAKE_RUN_LIMIT_MS, "download still running after 10 minutes");
}


// ---- esp_err.h ----
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_HTTP_BASE 0x7000
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)

// ---- system/device_info.h and the grinder ----
namespace device_info {
size_t free_internal_heap_bytes() { return 96U * 1024U; }
size_t largest_free_internal_block_bytes() { return 48U * 1024U; }
}  // namespace device_info

struct Grinder {
    int stops = 0;
    void stop() { ++stops; }
};
struct HardwareManager {
    Grinder grinder;
    Grinder* get_grinder() { return &grinder; }
};

// ---- esp_ota_ops.h ----
struct esp_partition_t {
    uint32_t size;
};
const esp_partition_t inactive_partition{PARTITION_SIZE};
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) {
    return &inactive_partition;
}

// ---- network/ota_writer.h: records how the inactive partition is used ----
struct OtaWriter {
    bool opened = false;
    bool image_validates = true;
    bool flash_write_fails = false;
    int begin_calls = 0;
    int end_calls = 0;
    int abort_calls = 0;
    int writes_while_closed = 0;
    long begin_ms = -1;
    size_t begin_size = 0;
    std::vector<size_t> write_sizes;
    std::vector<uint8_t> written;

    bool begin(size_t expected_size) {
        ++begin_calls;
        begin_ms = static_cast<long>(elapsed_ms());
        begin_size = expected_size;
        written.clear();
        opened = true;
        return true;
    }
    bool write(const uint8_t* data, size_t length) {
        if (!opened) {
            ++writes_while_closed;
            return false;
        }
        if (flash_write_fails) {
            opened = false;  // OtaWriter::fail aborts the OTA handle
            return false;
        }
        write_sizes.push_back(length);
        written.insert(written.end(), data, data + length);
        return true;
    }
    bool end() {
        ++end_calls;
        const bool was_open = opened;
        opened = false;
        return was_open && image_validates && written.size() == begin_size &&
               !written.empty() && written[0] == 0xE9;
    }
    void abort() {
        ++abort_calls;
        opened = false;
    }
    const char* error() const { return "fake writer error"; }
};
OtaWriter web_firmware_update;

// ---- esp_http_client.h / esp_crt_bundle.h over a scripted TLS peer ----
struct ServerEvent {
    uint32_t at_ms;  // relative to the start of the download
    bool close;      // peer FIN; every later read sees it too
    std::vector<uint8_t> bytes;
};

struct esp_http_client {
    int status = 200;
    int64_t content_length = 0;
    int redirects_left = 0;  // redirect responses still to come before the image
    bool redirect_drained = false;
    int opens = 0;
    int redirections = 0;
    std::vector<ServerEvent> events;
    size_t event_index = 0;
    size_t event_offset = 0;
    int timeout_ms = 0;
    int buffer_size_rx = 0;
    int64_t data_process = 0;
    bool headers_fetched = false;
    bool closed = false;
    int reads = 0;
    int closes = 0;
    int cleanups = 0;
    long first_body_byte_ms = -1;
} http;
typedef esp_http_client* esp_http_client_handle_t;

struct esp_http_client_config_t {
    const char* url;
    int timeout_ms;
    esp_err_t (*crt_bundle_attach)(void* conf);
    const char* user_agent;
    bool disable_auto_redirect;
    int max_redirection_count;
    int buffer_size;
};

esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
    check(config->crt_bundle_attach != nullptr, "TLS certificate bundle not attached");
    http.timeout_ms = config->timeout_ms > 0 ? config->timeout_ms : 5000;  // DEFAULT_TIMEOUT_MS
    http.buffer_size_rx = config->buffer_size > 0 ? config->buffer_size : 512;
    return &http;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char*, const char*) {
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t, int) {
    check(!http.headers_fetched, "opened again without following the redirect");
    ++http.opens;
    return ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) {
    http.headers_fetched = true;
    if (http.redirects_left > 0) return 0;  // a redirect with an empty body
    return http.content_length > 0 ? http.content_length : 0;  // 0 means chunked/unknown
}
int esp_http_client_get_status_code(esp_http_client_handle_t) {
    return http.redirects_left > 0 ? 302 : http.status;
}
esp_err_t esp_http_client_flush_response(esp_http_client_handle_t, int* len) {
    check(http.headers_fetched && http.redirects_left > 0, "flushed a response that was not a redirect");
    http.redirect_drained = true;
    if (len) *len = 0;
    return ESP_OK;
}
// Points the client at the Location header; the next open sends the new request.
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t) {
    check(http.redirect_drained, "redirected before draining the redirect response");
    --http.redirects_left;
    ++http.redirections;
    http.redirect_drained = false;
    http.headers_fetched = false;
    return ESP_OK;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t) {
    ++http.closes;
    http.closed = true;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t) {
    ++http.cleanups;
    return ESP_OK;
}

// esp_transport_read on the TLS transport: poll up to timeout_ms, then return
// decrypted bytes, 0 (ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) or -1 (CLOSED_BY_FIN).
int transport_read(uint8_t* out, int wanted, int timeout_ms) {
    const uint32_t now = elapsed_ms();
    if (http.event_index == http.events.size() ||
        http.events[http.event_index].at_ms > now + static_cast<uint32_t>(timeout_ms)) {
        advance_clock(static_cast<uint32_t>(timeout_ms));
        return 0;
    }
    const ServerEvent& event = http.events[http.event_index];
    if (event.at_ms > now) advance_clock(event.at_ms - now);
    if (event.close) return -1;
    const size_t count =
        std::min(static_cast<size_t>(wanted), event.bytes.size() - http.event_offset);
    std::copy(event.bytes.data() + http.event_offset,
              event.bytes.data() + http.event_offset + count, out);
    http.event_offset += count;
    if (http.event_offset == event.bytes.size()) {
        ++http.event_index;
        http.event_offset = 0;
    }
    if (http.first_body_byte_ms < 0) http.first_body_byte_ms = static_cast<long>(elapsed_ms());
    return static_cast<int>(count);
}

// esp_http_client_read for a Content-Length body (esp_http_client.c).
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len) {
    check(client == &http && http.headers_fetched && !http.closed && http.redirects_left == 0,
          "body read outside an open response");
    check(++http.reads < 100000, "download kept reading without progress");
    int ridx = 0;
    while (len - ridx > 0) {
        if (http.data_process >= http.content_length) break;  // no data remains
        const int wanted = static_cast<int>(std::min<int64_t>(
            {len - ridx, http.buffer_size_rx, http.content_length - http.data_process}));
        const int rlen =
            transport_read(reinterpret_cast<uint8_t*>(buffer) + ridx, wanted, http.timeout_ms);
        if (rlen == 0) return ridx > 0 ? ridx : -ESP_ERR_HTTP_EAGAIN;
        if (rlen < 0) {
            const bool complete = http.data_process >= http.content_length;
            return ridx == 0 && !complete ? ESP_FAIL : ridx;
        }
        ridx += rlen;
        http.data_process += rlen;
    }
    return ridx;
}

// ---- DeviceWebServer members perform_github_ota touches ----
class DeviceWebServer {
public:
    void perform_github_ota(const std::string& tag);
    void finish_ota(bool success);

    HardwareManager* hardware_manager_ = nullptr;
    std::atomic<size_t> ota_received_{0};
    std::atomic<size_t> ota_total_{0};
    std::vector<int> finish_results;
    bool writer_open_at_finish = false;
};

void DeviceWebServer::finish_ota(bool success) {
    finish_results.push_back(success ? 1 : 0);
    writer_open_at_finish = web_firmware_update.opened;
}

@PRODUCTION_CONSTANTS@

@PRODUCTION_OPEN_RESPONSE@

@PRODUCTION_DOWNLOAD@

std::vector<uint8_t> from_hex(const std::string& hex) {
    check(hex.size() % 2 == 0, "odd-length hex payload");
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

template <typename T>
std::string join(const std::vector<T>& values) {
    std::string text;
    for (const T& value : values) text += (text.empty() ? "" : ",") + std::to_string(value);
    return text;
}

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream fields(line);
        std::string key;
        fields >> key;
        if (key == "status") {
            fields >> http.status;
        } else if (key == "length") {
            fields >> http.content_length;
        } else if (key == "redirects") {
            fields >> http.redirects_left;
        } else if (key == "invalid_image") {
            web_firmware_update.image_validates = false;
        } else if (key == "flash_write_fails") {
            web_firmware_update.flash_write_fails = true;
        } else if (key == "data" || key == "close") {
            ServerEvent event{0, key == "close", {}};
            fields >> event.at_ms;
            if (!event.close) {
                std::string hex;
                fields >> hex;
                event.bytes = from_hex(hex);
                check(!event.bytes.empty(), "data event without bytes");
            }
            check(http.events.empty() || http.events.back().at_ms <= event.at_ms,
                  "server events out of order");
            http.events.push_back(event);
        } else {
            check(key.empty(), "unknown scenario line");
        }
        check(!fields.fail(), "malformed scenario line");
    }

    HardwareManager hardware;
    DeviceWebServer server;
    server.hardware_manager_ = &hardware;
    run_started_ms = clock_ms;
    server.perform_github_ota("v1.5.9");

    static const char digits[] = "0123456789abcdef";
    std::string written;
    for (const uint8_t byte : web_firmware_update.written) {
        written += digits[byte >> 4];
        written += digits[byte & 0x0F];
    }
    std::cout << "finish=" << join(server.finish_results) << "\n"
              << "elapsed_ms=" << elapsed_ms() << "\n"
              << "grinder_stops=" << hardware.grinder.stops << "\n"
              << "http_reads=" << http.reads << "\n"
              << "http_opens=" << http.opens << "\n"
              << "http_redirections=" << http.redirections << "\n"
              << "http_closes=" << http.closes << "\n"
              << "http_cleanups=" << http.cleanups << "\n"
              << "first_body_byte_ms=" << http.first_body_byte_ms << "\n"
              << "begin_calls=" << web_firmware_update.begin_calls << "\n"
              << "begin_size=" << web_firmware_update.begin_size << "\n"
              << "begin_ms=" << web_firmware_update.begin_ms << "\n"
              << "end_calls=" << web_firmware_update.end_calls << "\n"
              << "abort_calls=" << web_firmware_update.abort_calls << "\n"
              << "writes_while_closed=" << web_firmware_update.writes_while_closed << "\n"
              << "writer_open_at_finish=" << server.writer_open_at_finish << "\n"
              << "received=" << server.ota_received_.load() << "\n"
              << "write_sizes=" << join(web_firmware_update.write_sizes) << "\n"
              << "written=" << written << "\n";
}
'''


def constants_used_by(source, code):
    """The production file-scope constexpr definitions that `code` refers to."""
    declarations = re.findall(r"^constexpr\b[^;]*;", source, re.M)
    names = [re.search(r"(\w+)\s*(?:\[\])?\s*=", declaration).group(1)
             for declaration in declarations]
    return [declaration for declaration, name in zip(declarations, names)
            if re.search(rf"\b{name}\b", code)]


def image(size):
    """A stand-in application image: the 0xE9 magic byte, then position-dependent bytes."""
    return bytes([0xE9]) + bytes((i * 31 + 7) & 0xFF for i in range(1, size))


def data(at_ms, payload):
    return f"data {at_ms} {payload.hex()}"


def close(at_ms):
    return f"close {at_ms}"


class OtaStreamTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        opener = function(SOURCE, "bool open_response(")
        download = function(SOURCE, "void DeviceWebServer::perform_github_ota(const std::string& tag)")
        constants = "\n".join(constants_used_by(SOURCE, opener + download))
        harness = HARNESS.replace("@PRODUCTION_CONSTANTS@", constants)
        harness = harness.replace("@PRODUCTION_OPEN_RESPONSE@", opener)
        harness = harness.replace("@PRODUCTION_DOWNLOAD@", download)
        cls._tmp = tempfile.TemporaryDirectory()
        cpp, cls._binary = Path(cls._tmp.name) / "download.cpp", Path(cls._tmp.name) / "download"
        cpp.write_text(harness)
        compiled = subprocess.run(
            ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-DPARTITION_SIZE={PARTITION_SIZE}U",
             "-I", str(ROOT / "src"), str(cpp), "-o", str(cls._binary)],
            capture_output=True, text=True)
        if compiled.returncode:
            cls._tmp.cleanup()
            raise AssertionError("download harness failed to compile:\n" + compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def download(self, *events, status=200, length=None, image_validates=True,
                 flash_write_fails=False, redirects=0):
        """Run perform_github_ota against a server sending `events` after the headers."""
        if length is None:
            length = sum(len(bytes.fromhex(e.split()[2])) for e in events if e.startswith("data"))
        script = [f"status {status}", f"length {length}", f"redirects {redirects}", *events]
        if not image_validates:
            script.append("invalid_image")
        if flash_write_fails:
            script.append("flash_write_fails")
        run = subprocess.run([str(self._binary)], input="\n".join(script) + "\n",
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        fields = dict(line.split("=", 1) for line in run.stdout.splitlines())
        result = SimpleNamespace(
            finish=[value == "1" for value in fields.pop("finish").split(",") if value],
            write_sizes=[int(value) for value in fields.pop("write_sizes").split(",") if value],
            written=bytes.fromhex(fields.pop("written")),
            log=run.stderr,
            **{key: int(value) for key, value in fields.items()})
        self.assertEqual(len(result.finish), 1, "finish_ota must be reported exactly once")
        self.assertEqual(result.writes_while_closed, 0, "wrote to a partition that was not open")
        self.assertEqual((result.http_closes, result.http_cleanups), (1, 1),
                         "the HTTPS connection must be closed and freed exactly once")
        return result

    def assert_installed(self, result, payload):
        self.assertEqual(result.finish, [True], result.log)
        self.assertEqual((result.begin_calls, result.begin_size), (1, len(payload)))
        self.assertGreaterEqual(result.begin_ms, result.first_body_byte_ms,
                                "inactive partition opened before the first image byte")
        self.assertTrue(result.written == payload,
                        f"wrote {len(result.written)} bytes that differ from the "
                        f"{len(payload)}-byte image")
        self.assertEqual((result.end_calls, result.abort_calls), (1, 0))
        self.assertEqual(result.received, len(payload))

    def assert_rejected_unopened(self, result):
        self.assertEqual(result.finish, [False], result.log)
        self.assertEqual(result.begin_calls, 0, "inactive partition opened for a rejected download")
        self.assertEqual(result.written, b"")
        self.assertEqual(result.end_calls, 0)

    def assert_partial_image_discarded(self, result):
        self.assertEqual(result.finish, [False], result.log)
        self.assertEqual(result.begin_calls, 1)
        self.assertEqual(result.end_calls, 0, "an incomplete image must not be finalised")
        self.assertGreaterEqual(result.abort_calls, 1)
        self.assertFalse(result.writer_open_at_finish,
                         "failure reported while the partial image was still open")

    def test_body_delayed_after_headers_opens_partition_on_first_byte(self):
        payload = image(5000)
        result = self.download(data(40, payload))
        self.assert_installed(result, payload)
        self.assertEqual(result.first_body_byte_ms, 40)
        self.assertEqual(result.begin_ms, 40)
        self.assertEqual(result.elapsed_ms, 40)
        self.assertEqual(result.grinder_stops, 1)

    def test_slow_body_within_read_timeout_is_accepted(self):
        payload = image(5000)
        result = self.download(data(READ_TIMEOUT_MS - 2, payload))
        self.assert_installed(result, payload)
        self.assertEqual(result.elapsed_ms, READ_TIMEOUT_MS - 2)

    def test_non_image_body_is_rejected_before_opening_partition(self):
        page = (b"<!DOCTYPE html><html><body>Not Found</body></html>" * 100)[:5000]
        result = self.download(data(40, page))
        self.assert_rejected_unopened(result)
        self.assertEqual(result.elapsed_ms, 40)

    def test_missing_body_fails_after_read_timeout(self):
        result = self.download(length=5000)
        self.assert_rejected_unopened(result)
        self.assertEqual(result.elapsed_ms, READ_TIMEOUT_MS)

    def test_disconnect_before_body_fails_without_waiting(self):
        result = self.download(close(20), length=5000)
        self.assert_rejected_unopened(result)
        self.assertEqual(result.elapsed_ms, 20)

    def test_disconnect_mid_image_discards_partial_image(self):
        payload = image(5000)
        result = self.download(data(40, payload[:3000]), close(60), length=len(payload))
        self.assert_partial_image_discarded(result)
        self.assertEqual(result.written, payload[:3000])
        self.assertEqual(result.elapsed_ms, 60)

    def test_stall_mid_image_fails_after_read_timeout(self):
        payload = image(5000)
        result = self.download(data(40, payload[:1]), length=len(payload))
        self.assert_partial_image_discarded(result)
        # The first wait returns the byte it gathered; the next one times out empty.
        self.assertEqual(result.elapsed_ms, 40 + 2 * READ_TIMEOUT_MS)

    def test_image_failing_validation_is_not_reported_as_success(self):
        payload = image(5000)
        result = self.download(data(40, payload), image_validates=False)
        self.assertEqual(result.finish, [False])
        self.assertEqual(result.end_calls, 1)
        self.assertEqual(result.written, payload)

    def test_flash_write_failure_stops_download(self):
        payload = image(9000)
        result = self.download(data(40, payload), flash_write_fails=True)
        self.assertEqual(result.finish, [False])
        self.assertEqual((result.begin_calls, result.end_calls), (1, 0))
        self.assertEqual(result.http_reads, 1, "kept downloading after a flash write failed")

    def test_image_split_across_reads_is_written_in_order(self):
        payload = image(9000)
        # A lone magic byte, a pause longer than one TLS read wait, then small
        # records of varying size that the client gathers into full buffers.
        events, offset, at_ms = [data(40, payload[:1])], 1, 40 + READ_TIMEOUT_MS + 5
        for size in [1, 2, 700, 13, 1500, 4096, 333] * 4:
            if offset >= len(payload):
                break
            events.append(data(at_ms, payload[offset:offset + size]))
            offset, at_ms = min(offset + size, len(payload)), at_ms + 5
        self.assertEqual(offset, len(payload))
        result = self.download(*events)
        self.assert_installed(result, payload)
        self.assertEqual(result.write_sizes[0], 1)
        self.assertEqual(result.begin_ms, 40 + READ_TIMEOUT_MS)
        self.assertGreater(len(result.write_sizes), 2)
        self.assertEqual(result.elapsed_ms, at_ms - 5)

    def test_redirected_release_is_followed_to_the_image(self):
        payload = image(5000)
        result = self.download(data(40, payload), redirects=MAX_REDIRECTS)
        self.assert_installed(result, payload)
        self.assertEqual((result.http_opens, result.http_redirections),
                         (MAX_REDIRECTS + 1, MAX_REDIRECTS))

    def test_redirect_loop_is_abandoned_before_reading_body(self):
        result = self.download(data(0, image(5000)), redirects=MAX_REDIRECTS + 1)
        self.assert_rejected_unopened(result)
        self.assertEqual((result.http_opens, result.http_reads), (MAX_REDIRECTS + 1, 0))

    def test_bad_status_or_size_is_rejected_before_reading_body(self):
        payload = image(5000)
        for name, status, length in [("not found", 404, len(payload)),
                                     ("server error", 500, len(payload)),
                                     ("larger than partition", 200, PARTITION_SIZE + 1),
                                     ("unknown length", 200, 0)]:
            with self.subTest(name):
                result = self.download(data(0, payload), status=status, length=length)
                self.assert_rejected_unopened(result)
                self.assertEqual(result.http_reads, 0)
                self.assertEqual(result.elapsed_ms, 0)


if __name__ == "__main__":
    unittest.main()
