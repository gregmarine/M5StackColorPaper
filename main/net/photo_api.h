/*
 * The photo manager's HTTP API.
 *
 * Endpoints (all under the hotspot, no auth beyond the WPA2 key):
 *   GET    /api/photos          list what is on the device
 *   GET    /api/storage         bytes used and free on the FAT volume
 *   GET    /api/status          whether a panel refresh is in progress
 *   GET    /photo/<name>        the raw 4-bit BMP, for library thumbnails
 *   POST   /api/photos?name=X   upload a finished BMP (raw body, no multipart)
 *   DELETE /api/photos/<name>   remove one
 *   POST   /api/show            draw a photo on the panel now
 *   POST   /api/reorder         rewrite the filename prefixes to set the order
 */
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_http_server.h"

class PhotoSlideshow;

namespace net {

/** @brief Registers the photo endpoints on a running server. */
esp_err_t photoApiRegister(httpd_handle_t server);

/**
 * @brief Takes a pending "draw this photo" request, if there is one.
 *
 * A panel refresh takes 15-30 s, far longer than a phone will wait on an HTTP
 * request, so /api/show only records the request and returns. The hotspot loop
 * picks it up here and does the drawing, and the web app watches /api/status.
 *
 * @param index Receives the requested photo index.
 * @return True if a request was pending.
 */
bool photoApiTakeShowRequest(uint16_t& index);

/** @brief Tells the API a refresh is running, so /api/status can report it. */
void photoApiSetRefreshing(bool refreshing);

}  // namespace net
