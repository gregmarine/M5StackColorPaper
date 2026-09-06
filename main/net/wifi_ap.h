/*
 * Soft-AP bring-up for the on-device photo manager.
 *
 * Deliberately not an implementation of main/hal/wifi/hal_wifi.h: that header
 * (inherited from the M5 demo) declares a thirty-method STA/AP/scan manager
 * with no .cpp anywhere in the tree. The frame only ever raises one WPA2
 * access point and tears it down again, so this is that, and hal_wifi.h stays
 * the unimplemented header it already was rather than becoming a half-filled
 * one that links until someone calls the wrong method.
 */
#pragma once

#include <string>

#include "esp_err.h"

namespace net {

/** @brief What the panel needs to print so a phone can get in. */
struct ApInfo {
    std::string ssid;
    std::string password;
    std::string url;  // http://192.168.4.1
};

/**
 * @brief Starts the access point, generating and persisting credentials once.
 *
 * The SSID is derived from the MAC and the password is drawn from esp_random()
 * on first use, then both are kept in NVS. They have to be stable: they are
 * printed on the panel and typed into a phone, and a password that changed
 * every boot would mean re-pairing every time.
 *
 * @param out Receives the credentials and URL to display.
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t apStart(ApInfo& out);

/** @brief Stops the access point and releases the netif. */
void apStop();

/** @brief Returns the credentials without starting anything. */
esp_err_t apCredentials(ApInfo& out);

/** @brief Number of phones currently associated. */
int apStationCount();

}  // namespace net
