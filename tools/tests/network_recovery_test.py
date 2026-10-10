"""Run the production Wi-Fi state machine and driver setup against ESP-IDF fakes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/network/network_manager.cpp").read_text()

HARNESS = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
#define LOG_BLE(...) std::printf(__VA_ARGS__)
const char* esp_err_to_name(esp_err_t) { return "error"; }
uint32_t now_ms = 1000;
uint32_t millis() { return now_ms; }

// ---- esp_netif / esp_wifi / esp_event ----
struct esp_netif_t {} station, access_point;
int station_creates = 0, access_point_creates = 0;
esp_netif_t* esp_netif_create_default_wifi_sta() { ++station_creates; return &station; }
esp_netif_t* esp_netif_create_default_wifi_ap() { ++access_point_creates; return &access_point; }
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
int driver_inits = 0, driver_init_failures = 0;
esp_err_t esp_wifi_init(const wifi_init_config_t*) {
    ++driver_inits;
    return driver_init_failures-- > 0 ? ESP_FAIL : ESP_OK;
}
enum wifi_storage_t { WIFI_STORAGE_RAM };
esp_err_t esp_wifi_set_storage(wifi_storage_t) { return ESP_OK; }
int disconnects = 0;
esp_err_t esp_wifi_disconnect() { ++disconnects; return ESP_OK; }
using esp_event_base_t = const char*;
using esp_event_handler_t = void (*)(void*, esp_event_base_t, int32_t, void*);
using esp_event_handler_instance_t = void*;
const char* WIFI_EVENT = "WIFI_EVENT";
const char* IP_EVENT = "IP_EVENT";
constexpr int32_t ESP_EVENT_ANY_ID = -1, IP_EVENT_STA_GOT_IP = 0;
int wifi_registrations = 0, ip_registrations = 0, ip_registration_failures = 0;
int instance_storage = 0;
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t, esp_event_handler_t,
                                              void*, esp_event_handler_instance_t* instance) {
    if (base == IP_EVENT) {
        ++ip_registrations;
        if (ip_registration_failures-- > 0) return ESP_FAIL;
    } else {
        ++wifi_registrations;
    }
    *instance = &instance_storage;
    return ESP_OK;
}

// ---- SmartGrindNetworkManager members the production code touches ----
enum class NetworkState : uint8_t {
    WIFI_DISABLED, WIFI_NO_CREDENTIALS, WIFI_CONNECTING, WIFI_CONNECTED,
    WIFI_RETRY_WAIT, WIFI_SETUP_REQUIRED, WIFI_SETUP_AP,
};
class SmartGrindNetworkManager {
public:
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
    static constexpr uint32_t RETRY_DELAY_MS = 30000;
    std::atomic<NetworkState> state_{NetworkState::WIFI_DISABLED};
    std::atomic<bool> initialized_{true};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> station_got_ip_{false};
    bool ever_connected_ = false;
    uint32_t state_changed_at_ms_ = 0;
    esp_netif_t* station_netif_ = nullptr;
    esp_netif_t* access_point_netif_ = nullptr;
    bool wifi_driver_initialized_ = false;
    esp_event_handler_instance_t wifi_event_instance_ = nullptr;
    esp_event_handler_instance_t ip_event_instance_ = nullptr;
    int connection_attempts = 0, mdns_stops = 0;
    bool connection_starts = true;

    NetworkState state() const { return state_.load(); }
    void set_state(NetworkState state) { state_.store(state); state_changed_at_ms_ = millis(); }
    void stop_mdns() { ++mdns_stops; }
    void handle_connected() { ever_connected_ = true; set_state(NetworkState::WIFI_CONNECTED); }
    // Stand-in for the production begin_connection: it ends in CONNECTING once
    // the driver accepts the attempt, and returns early otherwise.
    void begin_connection() {
        ++connection_attempts;
        if (connection_starts) set_state(NetworkState::WIFI_CONNECTING);
    }
    bool ensure_wifi_initialized();
    void update();
    static void wifi_event_handler(void*, esp_event_base_t, int32_t, void*) {}
    static void ip_event_handler(void*, esp_event_base_t, int32_t, void*) {}
};
''' + function(SOURCE, "bool SmartGrindNetworkManager::ensure_wifi_initialized()") \
    + function(SOURCE, "void SmartGrindNetworkManager::update()") + r'''

void connected(SmartGrindNetworkManager& wifi) {
    wifi.station_got_ip_ = true; wifi.update();
    assert(wifi.state() == NetworkState::WIFI_CONNECTED);
}

int main() {
    // A dropped connection reconnects on the next service-loop pass instead of
    // waiting out the retry delay.
    SmartGrindNetworkManager wifi;
    connected(wifi);
    wifi.station_got_ip_ = false; wifi.update();
    assert(wifi.connection_attempts == 1 && wifi.mdns_stops == 1);
    assert(wifi.state() == NetworkState::WIFI_CONNECTING);
    connected(wifi);

    // An attempt that cannot start waits out the retry delay.
    wifi.connection_starts = false;
    wifi.station_got_ip_ = false; wifi.update();
    assert(wifi.connection_attempts == 2 && wifi.state() == NetworkState::WIFI_RETRY_WAIT);
    now_ms += SmartGrindNetworkManager::RETRY_DELAY_MS - 1; wifi.update();
    assert(wifi.connection_attempts == 2);
    now_ms += 1; wifi.connection_starts = true; wifi.update();
    assert(wifi.connection_attempts == 3 && wifi.state() == NetworkState::WIFI_CONNECTING);

    // A reconnection that times out backs off rather than retrying at once.
    now_ms += SmartGrindNetworkManager::CONNECT_TIMEOUT_MS; wifi.update();
    assert(disconnects == 1 && wifi.state() == NetworkState::WIFI_RETRY_WAIT);
    assert(wifi.connection_attempts == 3);

    // Driver setup retries the step that failed and repeats none that worked.
    SmartGrindNetworkManager fresh;
    driver_init_failures = 1; ip_registration_failures = 1;
    assert(!fresh.ensure_wifi_initialized());  // driver init fails
    assert(!fresh.ensure_wifi_initialized());  // IP handler registration fails
    assert(fresh.ensure_wifi_initialized());
    assert(fresh.ensure_wifi_initialized());
    assert(station_creates == 1 && access_point_creates == 1);
    assert(driver_inits == 2 && wifi_registrations == 1 && ip_registrations == 2);
}
'''


class NetworkRecoveryTest(unittest.TestCase):
    def test_reconnects_after_a_drop_and_retries_failed_setup(self):
        with tempfile.TemporaryDirectory() as folder:
            cpp, binary = Path(folder) / "network.cpp", Path(folder) / "network"
            cpp.write_text(HARNESS)
            # Production log formats use %lu for uint32_t, which is unsigned
            # long on the ESP32 but not on the host.
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-format",
                            str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10, stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
