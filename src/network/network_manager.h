#pragma once

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../storage/preferences.h"

enum class NetworkState : uint8_t {
    WIFI_DISABLED,
    WIFI_NO_CREDENTIALS,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_RETRY_WAIT,
    WIFI_SETUP_REQUIRED,
    WIFI_SETUP_AP
};

/** One entry of a station-mode scan, for the provisioning network picker. */
struct WifiScanResult {
    std::string ssid;
    int8_t rssi = 0;
    bool secured = false;
};

class SmartGrindNetworkManager {
public:
    void init(Preferences* preferences);
    void update();

    bool set_enabled(bool enabled);
    bool set_credentials(const std::string& ssid, const std::string& password);
    bool connect_saved_credentials();
    void clear_credentials();
    bool start_setup_access_point(const std::string& ssid, const std::string& password);

    bool is_enabled() const { return enabled_.load(); }
    bool is_connected() const { return state() == NetworkState::WIFI_CONNECTED; }
    bool has_credentials() const;
    NetworkState state() const { return state_.load(); }
    std::string hostname() const;
    std::string device_id() const;
    std::string network_name() const;
    std::string ip_address() const;

    /** IPv4 address of the setup access point, empty when it is not running. */
    std::string access_point_ip() const;

    /** True once the Wi-Fi driver is running in any mode. */
    bool is_radio_started() const { return wifi_started_; }

    /**
     * Scan for nearby networks. This blocks for the duration of the scan, so
     * only call it from the service loop or an HTTP handler.
     */
    std::vector<WifiScanResult> scan_networks();

private:
    static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
    static constexpr uint32_t RETRY_DELAY_MS = 30000;

    Preferences* preferences_ = nullptr;
    std::atomic<NetworkState> state_{NetworkState::WIFI_DISABLED};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> enabled_{false};
    std::atomic<bool> station_got_ip_{false};
    bool ever_connected_ = false;
    bool mdns_started_ = false;
    bool wifi_started_ = false;
    bool sntp_started_ = false;
    uint32_t state_changed_at_ms_ = 0;
    std::string ssid_;
    std::string password_;
    std::string hostname_;
    esp_netif_t* station_netif_ = nullptr;
    esp_netif_t* access_point_netif_ = nullptr;
    mutable SemaphoreHandle_t settings_mutex_ = nullptr;

    void load_settings();
    void begin_connection();
    void handle_connected();
    void stop_network();
    void set_state(NetworkState state);
    void stop_mdns();

    /** Create the Wi-Fi driver and both netifs once, on first use. */
    bool ensure_wifi_initialized();
    /** Switch mode, starting the driver if it is not running yet. */
    bool apply_mode(wifi_mode_t mode);

    static void wifi_event_handler(void* context, esp_event_base_t base, int32_t id, void* data);
    static void ip_event_handler(void* context, esp_event_base_t base, int32_t id, void* data);

    static std::string default_hostname();
    static std::string sanitize_hostname(const std::string& hostname);
};

extern SmartGrindNetworkManager network_manager;
