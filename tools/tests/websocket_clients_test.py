"""Run the production WebSocket client list against esp_http_server fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/network/device_api.cpp").read_text()

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using esp_err_t = int;
using httpd_handle_t = void*;
#define LOG_BLE(...) std::printf(__VA_ARGS__)
uint32_t now_ms = 0;
uint32_t millis() { return now_ms; }

// What each descriptor currently is on the server, as httpd_ws_get_fd_info reports it.
enum httpd_ws_client_info_t { HTTPD_WS_CLIENT_INVALID, HTTPD_WS_CLIENT_HTTP, HTTPD_WS_CLIENT_WEBSOCKET };
std::map<int, httpd_ws_client_info_t> sessions;
httpd_ws_client_info_t httpd_ws_get_fd_info(httpd_handle_t, int fd) {
    const auto found = sessions.find(fd);
    return found == sessions.end() ? HTTPD_WS_CLIENT_INVALID : found->second;
}
std::vector<int> closed;
esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd) { closed.push_back(fd); return 0; }

class DeviceApi {
public:
    static constexpr size_t MAX_CLIENTS = 4;
    static constexpr uint32_t PUBLISH_INTERVAL_MS = 100;
    static constexpr uint8_t MAX_CONSECUTIVE_BACKPRESSURE_SKIPS = 50;
    static constexpr int NO_CLIENT = -1;
    bool initialized_ = true;
    httpd_handle_t server_ = this;
    std::atomic<bool> settings_cache_dirty_{false};
    uint32_t last_publish_ms_ = 0;
    std::atomic<int> client_fds_[MAX_CLIENTS]{};
    std::atomic<uint8_t> backpressure_skips_[MAX_CLIENTS]{};
    std::vector<int> sent_to;

    DeviceApi() { for (auto& slot : client_fds_) slot.store(NO_CLIENT); }
    void refresh_settings_cache() {}
    std::string build_state_message() { return "{}"; }
    // Records the frame; production send_text writes it to the socket.
    bool send_text(int fd, const std::string&) {
        assert(httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET);
        sent_to.push_back(fd);
        return true;
    }
    void update();
    void add_client(int client_fd);
    void remove_client(int client_fd);
    size_t clients() const {
        size_t count = 0;
        for (const auto& slot : client_fds_) count += slot.load() != NO_CLIENT;
        return count;
    }
};
''' + "\n".join(function(SOURCE, signature) for signature in (
    "void DeviceApi::update()",
    "void DeviceApi::add_client(int client_fd)",
    "void DeviceApi::remove_client(int client_fd)",
)) + r'''

void publish(DeviceApi& api) {
    api.sent_to.clear();
    now_ms += DeviceApi::PUBLISH_INTERVAL_MS;
    api.update();
}

int main() {
    DeviceApi api;
    sessions[54] = HTTPD_WS_CLIENT_WEBSOCKET;
    sessions[55] = HTTPD_WS_CLIENT_WEBSOCKET;
    api.add_client(54);
    api.add_client(55);
    publish(api);
    assert((api.sent_to == std::vector<int>{54, 55}));

    // The server purges client 54 without a CLOSE frame and a plain HTTP
    // request receives the same descriptor.
    sessions[54] = HTTPD_WS_CLIENT_HTTP;
    publish(api);
    assert((api.sent_to == std::vector<int>{55}) && api.clients() == 1);
    publish(api);
    assert((api.sent_to == std::vector<int>{55}));

    // A dropped peer whose descriptor is not reused yet is forgotten too.
    sessions.erase(55);
    publish(api);
    assert(api.sent_to.empty() && api.clients() == 0 && closed.empty());

    // A WebSocket that reuses a descriptor still listed occupies one slot.
    sessions[56] = HTTPD_WS_CLIENT_WEBSOCKET;
    api.add_client(56);
    api.add_client(56);
    publish(api);
    assert((api.sent_to == std::vector<int>{56}) && api.clients() == 1);
}
'''


class WebSocketClientsTest(unittest.TestCase):
    def test_closed_sessions_stop_receiving_state_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            cpp, binary = Path(folder) / "clients.cpp", Path(folder) / "clients"
            cpp.write_text(HARNESS)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-format",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
