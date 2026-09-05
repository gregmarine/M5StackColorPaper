/*
 * Hotspot mode: the frame raises its own Wi-Fi network and serves the photo
 * manager until you dismiss it or it goes idle.
 */
#pragma once

#include <cstdint>

class PhotoSlideshow;

namespace net {

/** @brief Outcome of a hotspot session. */
enum class ApExit {
    Dismissed,    /*!< the top key was pressed again */
    Idle,         /*!< nothing talked to it for AP_IDLE_MS */
    LowBattery,   /*!< refused to start; the panel says so */
    Failed,       /*!< Wi-Fi or the HTTP server would not come up */
};

/**
 * @brief Runs a hotspot session start to finish.
 *
 * Tears down USB mass storage first if it is running (the FAT volume has one
 * owner at a time), draws the credentials on the panel, serves the web app,
 * and on the way out redraws whatever photo was showing.
 *
 * @param slideshow Used to redraw the current photo when the session ends.
 * @return Why the session ended.
 */
ApExit runApMode(PhotoSlideshow& slideshow);

}  // namespace net
