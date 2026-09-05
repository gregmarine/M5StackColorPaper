/*
 * HTTP server for the on-device photo manager.
 *
 * Serves the web app (embedded in the app binary, gzipped) and the captive
 * portal redirects. The photo API lands here too.
 */
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace net {

/** @brief Starts the HTTP server and the captive-portal DNS responder. */
esp_err_t webStart();

/** @brief Stops both. */
void webStop();

/** @brief Resets the idle timer; the photo API calls this on every request. */
void webTouch();

/**
 * @brief Milliseconds since the last request from a phone.
 *
 * The AP idle timer is driven from this rather than from a fixed window, so
 * the frame does not power off underneath someone mid-upload.
 */
uint32_t webMsSinceLastRequest();

}  // namespace net
