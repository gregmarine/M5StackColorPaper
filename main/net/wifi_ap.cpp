/*
 * SPDX-License-Identifier: MIT
 */
#include "net/wifi_ap.h"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "hal/hal.h"

extern Hal hal;

namespace net {
namespace {

constexpr const char* TAG = "wifi_ap";

// One phone is the normal case; four leaves room without reserving buffers
// for a crowd that will never turn up.
constexpr uint8_t AP_MAX_STATIONS = 4;
constexpr uint8_t AP_CHANNEL      = 6;

// Ambiguous characters are left out: this password gets read off an e-paper
// panel at 180 dpi and typed into a phone, so 0/O and 1/l/I are a real cost.
constexpr const char PASSWORD_ALPHABET[] = "abcdefghijkmnopqrstuvwxyz23456789";
constexpr int PASSWORD_LENGTH            = 10;  // WPA2 needs at least 8

esp_netif_t* s_ap_netif = nullptr;
bool s_running          = false;

std::string defaultSsid()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    std::snprintf(ssid, sizeof(ssid), "PaperColor-%02X%02X", mac[4], mac[5]);
    return ssid;
}

std::string generatePassword()
{
    const int alphabet = sizeof(PASSWORD_ALPHABET) - 1;
    std::string out;
    out.reserve(PASSWORD_LENGTH);
    for (int i = 0; i < PASSWORD_LENGTH; i++) {
        out.push_back(PASSWORD_ALPHABET[esp_random() % alphabet]);
    }
    return out;
}

// Reads the stored credentials, minting and saving them the first time.
void loadOrCreateCredentials(std::string& ssid, std::string& password)
{
    hal.settingsLock();
    ssid     = hal.settings.wifi_ssid;
    password = hal.settings.wifi_password;
    hal.settingsUnlock();

    bool ssid_changed     = false;
    bool password_changed = false;

    if (ssid.empty()) {
        ssid         = defaultSsid();
        ssid_changed = true;
    }
    // A too-short password would be rejected by esp_wifi as WPA2, so treat
    // anything unusable as absent rather than failing to start the AP.
    if (password.size() < 8) {
        password         = generatePassword();
        password_changed = true;
    }

    if (ssid_changed || password_changed) {
        hal.settingsLock();
        std::strncpy(hal.settings.wifi_ssid, ssid.c_str(), sizeof(hal.settings.wifi_ssid) - 1);
        hal.settings.wifi_ssid[sizeof(hal.settings.wifi_ssid) - 1] = '\0';
        std::strncpy(hal.settings.wifi_password, password.c_str(), sizeof(hal.settings.wifi_password) - 1);
        hal.settings.wifi_password[sizeof(hal.settings.wifi_password) - 1] = '\0';
        hal.settingsUnlock();
        if (ssid_changed) hal.settingsSave(SETTING_WIFI_SSID);
        if (password_changed) hal.settingsSave(SETTING_WIFI_PASSWORD);
        ESP_LOGI(TAG, "Generated new AP credentials (ssid=%s)", ssid.c_str());
    }
}

}  // namespace

esp_err_t apCredentials(ApInfo& out)
{
    loadOrCreateCredentials(out.ssid, out.password);
    out.url = "http://192.168.4.1";
    return ESP_OK;
}

esp_err_t apStart(ApInfo& out)
{
    if (s_running) return apCredentials(out);

    ESP_RETURN_ON_ERROR(apCredentials(out), TAG, "credentials");

    // settingsInit() already ran nvs_flash_init(); esp_wifi needs it too and
    // calling it twice is harmless.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs init");
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == nullptr) {
        ESP_LOGE(TAG, "failed to create AP netif");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");

    wifi_config_t wifi_cfg = {};
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.ap.ssid), out.ssid.c_str(), sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = static_cast<uint8_t>(out.ssid.size());
    std::strncpy(reinterpret_cast<char*>(wifi_cfg.ap.password), out.password.c_str(),
                 sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.channel        = AP_CHANNEL;
    wifi_cfg.ap.max_connection = AP_MAX_STATIONS;
    wifi_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "set config");

    // The radio is the single biggest current draw the board ever sees, and
    // the phone is a couple of feet away. Backing the power off keeps the
    // brownout detector out of it on a half-empty battery.
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    esp_wifi_set_max_tx_power(52);  // 13 dBm, in 0.25 dBm units

    // RFC 8910: hands the portal URL to the phone over DHCP, which modern iOS
    // and Android prefer over guessing from a hijacked probe request.
    const char* portal_uri = "http://192.168.4.1";
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           const_cast<char*>(portal_uri), std::strlen(portal_uri));

    s_running = true;
    ESP_LOGI(TAG, "AP up: ssid=%s pass=%s at %s", out.ssid.c_str(), out.password.c_str(),
             out.url.c_str());
    return ESP_OK;
}

void apStop()
{
    if (!s_running) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    if (s_ap_netif != nullptr) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
    s_running = false;
    ESP_LOGI(TAG, "AP down");
}

int apStationCount()
{
    if (!s_running) return 0;
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

}  // namespace net
