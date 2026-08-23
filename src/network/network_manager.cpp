#include "network_manager.h"

#include <esp_err.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <lwip/ip4_addr.h>
#include <mdns.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../config/build_info.h"
#include "../config/constants.h"
#include "../system/device_info.h"
#include "../system/timing.h"

SmartGrindNetworkManager network_manager;

namespace {

// Some Android and Samsung captive-portal detectors do not launch their sign-in
// UI when the access-point gateway is in private address space, so the setup
// network uses the same public-looking, non-routed subnet as GaggiMate.
constexpr uint8_t kSetupIp[4] = {4, 4, 4, 1};
constexpr uint8_t kSetupNetmask[4] = {255, 255, 255, 0};

constexpr uint16_t kMaxScanResults = 20;

std::string ip4_to_string(const esp_ip4_addr_t& address) {
    char text[16] = {};
    snprintf(text, sizeof(text), IPSTR, IP2STR(&address));
    return text;
}

}  // namespace

void SmartGrindNetworkManager::init(Preferences* preferences) {
    preferences_ = preferences;
    if (!settings_mutex_) settings_mutex_ = xSemaphoreCreateMutex();
    load_settings();
    initialized_.store(true);

    if (!enabled_.load()) {
        set_state(NetworkState::WIFI_DISABLED);
        return;
    }

    if (!has_credentials()) {
        set_state(NetworkState::WIFI_NO_CREDENTIALS);
        return;
    }

    begin_connection();
}

bool SmartGrindNetworkManager::ensure_wifi_initialized() {
    if (station_netif_) return true;

    // esp_netif and the default event loop are created in app_main before any
    // service starts, so only the driver and netifs are set up here.
    station_netif_ = esp_netif_create_default_wifi_sta();
    access_point_netif_ = esp_netif_create_default_wifi_ap();
    if (!station_netif_ || !access_point_netif_) {
        LOG_BLE("[WIFI] Could not create network interfaces\n");
        return false;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        LOG_BLE("[WIFI] Driver init failed: %s\n", esp_err_to_name(err));
        return false;
    }

    // Credentials live in this firmware's own NVS namespace; letting the driver
    // persist a second copy would make the two disagree after a factory reset.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, this, nullptr);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  ip_event_handler, this, nullptr);
    }
    if (err != ESP_OK) {
        LOG_BLE("[WIFI] Event registration failed: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool SmartGrindNetworkManager::apply_mode(wifi_mode_t mode) {
    if (!ensure_wifi_initialized()) return false;

    if (esp_wifi_set_mode(mode) != ESP_OK) return false;
    if (!wifi_started_) {
        const esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            LOG_BLE("[WIFI] Radio start failed: %s\n", esp_err_to_name(err));
            return false;
        }
        wifi_started_ = true;
    }
    return true;
}

void SmartGrindNetworkManager::wifi_event_handler(void* context, esp_event_base_t,
                                                  int32_t id, void*) {
    auto* self = static_cast<SmartGrindNetworkManager*>(context);
    if (!self) return;

    switch (id) {
        case WIFI_EVENT_STA_START:
        case WIFI_EVENT_STA_DISCONNECTED:
            self->station_got_ip_.store(false);
            // Reconnection is driven from update() so the retry and setup-mode
            // timing stays in one place.
            if (id == WIFI_EVENT_STA_DISCONNECTED &&
                self->state() == NetworkState::WIFI_CONNECTING) {
                esp_wifi_connect();
            }
            break;
        default:
            break;
    }
}

void SmartGrindNetworkManager::ip_event_handler(void* context, esp_event_base_t,
                                                int32_t id, void*) {
    auto* self = static_cast<SmartGrindNetworkManager*>(context);
    if (self && id == IP_EVENT_STA_GOT_IP) {
        self->station_got_ip_.store(true);
    }
}

void SmartGrindNetworkManager::update() {
    if (!initialized_.load() || !enabled_.load()) return;

    if (station_got_ip_.load()) {
        if (state() != NetworkState::WIFI_CONNECTED) handle_connected();
        return;
    }

    if (state() == NetworkState::WIFI_CONNECTED) {
        stop_mdns();
        LOG_BLE("[WIFI] Connection lost; retrying in %lus\n", RETRY_DELAY_MS / 1000);
        set_state(NetworkState::WIFI_RETRY_WAIT);
        return;
    }

    const uint32_t elapsed_ms = millis() - state_changed_at_ms_;
    if (state() == NetworkState::WIFI_CONNECTING && elapsed_ms >= CONNECT_TIMEOUT_MS) {
        esp_wifi_disconnect();
        if (ever_connected_) {
            LOG_BLE("[WIFI] Reconnection timed out; retrying in %lus\n", RETRY_DELAY_MS / 1000);
            set_state(NetworkState::WIFI_RETRY_WAIT);
        } else {
            LOG_BLE("[WIFI] Configured network unavailable; starting setup mode\n");
            set_state(NetworkState::WIFI_SETUP_REQUIRED);
        }
    } else if (state() == NetworkState::WIFI_RETRY_WAIT && elapsed_ms >= RETRY_DELAY_MS) {
        begin_connection();
    }
}

bool SmartGrindNetworkManager::set_enabled(bool enabled) {
    if (!preferences_) return false;

    enabled_.store(enabled);
    if (preferences_->putBool("wifi_on", enabled) == 0) return false;

    if (!enabled) {
        stop_network();
    } else if (!has_credentials()) {
        set_state(NetworkState::WIFI_NO_CREDENTIALS);
    } else {
        begin_connection();
    }
    return true;
}

bool SmartGrindNetworkManager::set_credentials(const std::string& ssid,
                                               const std::string& password) {
    if (!preferences_ || ssid.empty() || ssid.length() > 32 || password.length() > 63) {
        return false;
    }
    if (!password.empty() && password.length() < 8) return false;

    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const std::string previous_password = password_;
    if (preferences_->putString("wifi_pass", password) == 0) {
        if (settings_mutex_) xSemaphoreGive(settings_mutex_);
        return false;
    }
    if (preferences_->putString("wifi_ssid", ssid) == 0) {
        preferences_->putString("wifi_pass", previous_password);
        if (settings_mutex_) xSemaphoreGive(settings_mutex_);
        return false;
    }

    ssid_ = ssid;
    password_ = password;
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    // When credentials arrive through the setup AP, keep that link alive long
    // enough to return the success page. ProvisioningService performs a clean
    // reboot into station mode after the HTTP response has been queued.
    if (enabled_.load() && state() != NetworkState::WIFI_SETUP_AP) begin_connection();
    return true;
}

bool SmartGrindNetworkManager::connect_saved_credentials() {
    if (!initialized_.load() || !enabled_.load() || !has_credentials()) return false;
    begin_connection();
    return state() == NetworkState::WIFI_CONNECTING;
}

void SmartGrindNetworkManager::clear_credentials() {
    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    if (preferences_) {
        preferences_->remove("wifi_ssid");
        preferences_->remove("wifi_pass");
    }
    ssid_.clear();
    password_.clear();
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    stop_network();
    set_state(enabled_.load() ? NetworkState::WIFI_NO_CREDENTIALS : NetworkState::WIFI_DISABLED);
}

bool SmartGrindNetworkManager::has_credentials() const {
    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const bool present = !ssid_.empty();
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    return present;
}

std::string SmartGrindNetworkManager::hostname() const {
    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const std::string value = hostname_;
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    return value;
}

std::string SmartGrindNetworkManager::device_id() const {
    char value[13];
    snprintf(value, sizeof(value), "%012llx",
             static_cast<unsigned long long>(device_info::efuse_mac()));
    return value;
}

std::string SmartGrindNetworkManager::network_name() const {
    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const std::string value = ssid_;
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    return value;
}

std::string SmartGrindNetworkManager::ip_address() const {
    if (!is_connected() || !station_netif_) return {};
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(station_netif_, &info) != ESP_OK) return {};
    return ip4_to_string(info.ip);
}

std::string SmartGrindNetworkManager::access_point_ip() const {
    if (state() != NetworkState::WIFI_SETUP_AP || !access_point_netif_) return {};
    esp_netif_ip_info_t info = {};
    if (esp_netif_get_ip_info(access_point_netif_, &info) != ESP_OK) return {};
    return ip4_to_string(info.ip);
}

std::vector<WifiScanResult> SmartGrindNetworkManager::scan_networks() {
    std::vector<WifiScanResult> results;
    if (!wifi_started_) return results;

    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return results;

    uint16_t count = kMaxScanResults;
    std::vector<wifi_ap_record_t> records(count);
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        esp_wifi_clear_ap_list();
        return results;
    }

    results.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
        if (ssid[0] == '\0') continue;
        WifiScanResult entry;
        entry.ssid = ssid;
        entry.rssi = records[i].rssi;
        entry.secured = records[i].authmode != WIFI_AUTH_OPEN;
        results.push_back(std::move(entry));
    }

    // Strongest first, and only the first sighting of each name.
    std::sort(results.begin(), results.end(),
              [](const WifiScanResult& a, const WifiScanResult& b) { return a.rssi > b.rssi; });
    results.erase(std::unique(results.begin(), results.end(),
                              [](const WifiScanResult& a, const WifiScanResult& b) {
                                  return a.ssid == b.ssid;
                              }),
                  results.end());
    return results;
}

void SmartGrindNetworkManager::load_settings() {
    enabled_.store(preferences_ && preferences_->getBool("wifi_on", true));
    ssid_ = preferences_ ? preferences_->getString("wifi_ssid", "") : std::string();
    password_ = preferences_ ? preferences_->getString("wifi_pass", "") : std::string();
    hostname_ = preferences_ ? sanitize_hostname(preferences_->getString("wifi_host", ""))
                             : std::string();
    if (hostname_.empty()) hostname_ = default_hostname();
}

void SmartGrindNetworkManager::begin_connection() {
    if (!enabled_.load()) return;

    if (settings_mutex_) xSemaphoreTake(settings_mutex_, portMAX_DELAY);
    const std::string ssid = ssid_;
    const std::string password = password_;
    const std::string hostname = hostname_;
    if (settings_mutex_) xSemaphoreGive(settings_mutex_);
    if (ssid.empty()) return;

    stop_mdns();
    station_got_ip_.store(false);

    if (!apply_mode(WIFI_MODE_STA)) return;
    esp_netif_set_hostname(station_netif_, hostname.c_str());

    wifi_config_t config = {};
    strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid.c_str(),
            sizeof(config.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(config.sta.password), password.c_str(),
            sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_PSK;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) return;

    esp_wifi_connect();
    set_state(NetworkState::WIFI_CONNECTING);
    LOG_BLE("[WIFI] Connecting to configured network as %s.local\n", hostname.c_str());
}

bool SmartGrindNetworkManager::start_setup_access_point(const std::string& ssid,
                                                       const std::string& password) {
    if (ssid.empty() || (!password.empty() && password.length() < 8)) return false;

    stop_mdns();
    esp_wifi_disconnect();

    // AP+STA keeps the setup network available while the station radio scans for
    // nearby routers for the provisioning dropdown.
    if (!apply_mode(WIFI_MODE_APSTA)) return false;

    esp_netif_ip_info_t ip_info = {};
    IP4_ADDR(&ip_info.ip, kSetupIp[0], kSetupIp[1], kSetupIp[2], kSetupIp[3]);
    IP4_ADDR(&ip_info.gw, kSetupIp[0], kSetupIp[1], kSetupIp[2], kSetupIp[3]);
    IP4_ADDR(&ip_info.netmask, kSetupNetmask[0], kSetupNetmask[1], kSetupNetmask[2],
             kSetupNetmask[3]);
    esp_netif_dhcps_stop(access_point_netif_);
    if (esp_netif_set_ip_info(access_point_netif_, &ip_info) != ESP_OK) return false;
    if (esp_netif_dhcps_start(access_point_netif_) != ESP_OK) return false;

    wifi_config_t config = {};
    strncpy(reinterpret_cast<char*>(config.ap.ssid), ssid.c_str(), sizeof(config.ap.ssid) - 1);
    config.ap.ssid_len = static_cast<uint8_t>(std::min(ssid.length(), sizeof(config.ap.ssid)));
    config.ap.max_connection = 4;
    config.ap.channel = 1;
    if (password.empty()) {
        config.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        strncpy(reinterpret_cast<char*>(config.ap.password), password.c_str(),
                sizeof(config.ap.password) - 1);
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
    if (esp_wifi_set_config(WIFI_IF_AP, &config) != ESP_OK) return false;

    set_state(NetworkState::WIFI_SETUP_AP);
    LOG_BLE("[WIFI] Setup access point started: %s (%s)\n", ssid.c_str(),
            access_point_ip().c_str());
    return true;
}

void SmartGrindNetworkManager::handle_connected() {
    ever_connected_ = true;
    set_state(NetworkState::WIFI_CONNECTED);
    const std::string current_hostname = hostname();
    const std::string current_device_id = device_id();

    mdns_started_ = mdns_init() == ESP_OK;
    if (mdns_started_) {
        mdns_hostname_set(current_hostname.c_str());
        mdns_instance_name_set(BLE_DEVICE_NAME);
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        mdns_txt_item_t txt[] = {
            {"api", "v1"},
            {"protocol", "1"},
            {"version", BUILD_FIRMWARE_VERSION},
            {"id", current_device_id.c_str()},
        };
        mdns_service_add(nullptr, "_smartgrind", "_tcp", 80, txt,
                         sizeof(txt) / sizeof(txt[0]));
    }

    // UTC is rendered in the browser's local timezone. Synchronisation is
    // asynchronous and does not delay the control or UI tasks.
    if (!sntp_started_) {
        esp_sntp_config_t sntp_config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                                                   ESP_SNTP_SERVER_LIST("pool.ntp.org",
                                                                        "time.nist.gov"));
        sntp_started_ = esp_netif_sntp_init(&sntp_config) == ESP_OK;
    }

    LOG_BLE("[WIFI] Connected: %s (%s.local, mDNS=%s)\n", ip_address().c_str(),
            current_hostname.c_str(), mdns_started_ ? "OK" : "FAILED");
}

void SmartGrindNetworkManager::stop_mdns() {
    if (!mdns_started_) return;
    mdns_free();
    mdns_started_ = false;
}

void SmartGrindNetworkManager::stop_network() {
    stop_mdns();
    station_got_ip_.store(false);
    if (wifi_started_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        wifi_started_ = false;
    }
    set_state(NetworkState::WIFI_DISABLED);
}

void SmartGrindNetworkManager::set_state(NetworkState state) {
    state_.store(state);
    state_changed_at_ms_ = millis();
}

std::string SmartGrindNetworkManager::default_hostname() {
    return "smartgrind";
}

std::string SmartGrindNetworkManager::sanitize_hostname(const std::string& hostname) {
    std::string sanitized;
    sanitized.reserve(hostname.length());
    for (size_t i = 0; i < hostname.length() && sanitized.length() < 63; ++i) {
        const char value = static_cast<char>(tolower(static_cast<unsigned char>(hostname[i])));
        if ((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-') {
            sanitized += value;
        }
    }
    while (!sanitized.empty() && sanitized.front() == '-') sanitized.erase(0, 1);
    while (!sanitized.empty() && sanitized.back() == '-') sanitized.pop_back();
    return sanitized;
}
