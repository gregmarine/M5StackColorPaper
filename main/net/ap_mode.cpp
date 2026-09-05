/*
 * SPDX-License-Identifier: MIT
 */
#include "net/ap_mode.h"

#include <M5Unified.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "apps/local_photo_slideshow/local_photo_slideshow.h"
#include "hal/hal.h"
#include "hal/storage/hal_storage.h"
#include "net/photo_api.h"
#include "net/web_server.h"
#include "net/wifi_ap.h"

extern Hal hal;

namespace net {
namespace {

constexpr const char* TAG = "ap_mode";

// Far longer than the slideshow's two minutes: preparing and uploading a photo
// on a phone is a minutes-long job, and powering off mid-upload would be
// worse than the battery cost. The timer is reset by every HTTP request and
// held off entirely while a phone is associated, so this only expires after
// the phone has actually gone away.
constexpr uint32_t AP_IDLE_MS = 15 * 60 * 1000;

// A backstop on top of that. Holding the session open while a phone is
// associated is what stops the frame powering off mid-upload, but phones stay
// associated long after anyone is looking at them, so on its own it would let
// a forgotten hotspot run the battery flat. An hour is far longer than the job
// takes and far shorter than the battery lasts.
constexpr uint32_t AP_MAX_SESSION_MS = 60 * 60 * 1000;

// The radio transmits in bursts far heavier than anything else the board does.
// On a nearly flat cell that is how you get a brownout reset instead of a
// photo, so the hotspot declines to start rather than risk it. hal.init()
// already refuses to boot at all below 3100 mV.
constexpr uint16_t AP_MIN_BATTERY_MV = 3500;

constexpr uint8_t ROTATION_LANDSCAPE = 3;

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// Puts the canvas into landscape at panel size, whatever the last photo left
// behind. Mirrors what PhotoSlideshow::displayPhoto() does: sprites keep their
// rotation across createSprite(), so the canvas rotation is forced to 0 and
// the panel does the rotating.
void prepareLandscapeCanvas()
{
    if (M5.Display.getRotation() != ROTATION_LANDSCAPE) {
        M5.Display.setRotation(ROTATION_LANDSCAPE);
    }
    if (hal.Canvas->getRotation() != 0 || hal.Canvas->width() != M5.Display.width() ||
        hal.Canvas->height() != M5.Display.height()) {
        hal.Canvas->deleteSprite();
        hal.Canvas->setRotation(0);
        hal.Canvas->createSprite(M5.Display.width(), M5.Display.height());
    }
}

void drawCentredMessage(const char* line1, const char* line2)
{
    prepareLandscapeCanvas();
    hal.Canvas->fillScreen(TFT_WHITE);
    hal.Canvas->setTextColor(TFT_BLACK);
    hal.Canvas->setTextDatum(textdatum_t::middle_center);
    hal.Canvas->setTextSize(2);
    const int cx = hal.Canvas->width() / 2, cy = hal.Canvas->height() / 2;
    hal.Canvas->drawString(line1, cx, cy - 16);
    if (line2 != nullptr) hal.Canvas->drawString(line2, cx, cy + 16);
    hal.Canvas->pushSprite(0, 0);
}

// The credentials screen. This costs a full 15-30 s colour refresh, which is
// why the AP is already up before it starts drawing: the phone can join and
// the app can load while the panel is still painting.
void drawApScreen(const ApInfo& info)
{
    prepareLandscapeCanvas();
    hal.Canvas->fillScreen(TFT_WHITE);
    hal.Canvas->setTextColor(TFT_BLACK);

    const int w = hal.Canvas->width(), h = hal.Canvas->height();
    const int cx = w / 2;

    hal.Canvas->setTextDatum(textdatum_t::middle_center);
    hal.Canvas->setTextSize(3);
    hal.Canvas->drawString("Photo manager", cx, 46);

    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("Join this Wi-Fi network", cx, 92);

    // Labels left, values right, so the eye can run down the values while
    // typing them into a phone.
    const int label_x = 60, value_x = 210;
    int y             = 148;
    const int step    = 42;
    hal.Canvas->setTextDatum(textdatum_t::middle_left);

    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("Network", label_x, y);
    hal.Canvas->setTextSize(3);
    hal.Canvas->drawString(info.ssid.c_str(), value_x, y);

    y += step;
    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("Password", label_x, y);
    hal.Canvas->setTextSize(3);
    hal.Canvas->drawString(info.password.c_str(), value_x, y);

    y += step;
    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("Then open", label_x, y);
    hal.Canvas->setTextSize(3);
    hal.Canvas->drawString(info.url.c_str(), value_x, y);

    hal.Canvas->setTextDatum(textdatum_t::middle_center);
    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("It should open by itself once you join.", cx, h - 74);
    hal.Canvas->drawString("Top key again to finish.", cx, h - 42);

    hal.Canvas->pushSprite(0, 0);
}

bool batteryOkForRadio()
{
    uint16_t mv = 0;
    if (hal.pm1.readVbat(&mv) != M5PM1_OK) {
        // Without a reading, trust the board: it booted, so it has some charge.
        ESP_LOGW(TAG, "could not read battery; starting the hotspot anyway");
        return true;
    }
    ESP_LOGI(TAG, "battery %u mV", mv);
    return mv >= AP_MIN_BATTERY_MV;
}

// USB mass storage and the app cannot both own the FAT volume, so the drive
// goes away for the duration.
void stopUsbMassStorage()
{
    if (hal_storage_usb_attached()) hal_storage_usb_detach();
}

// True once the top key has been released and pressed again. The press that
// got us here is consumed first so it cannot immediately close the session.
//
// This loop also performs any panel refresh the web app asked for: /api/show
// only records the request, because a 15-30 s refresh would outlast the
// phone's patience for an HTTP response.
bool waitForDismissOrIdle(PhotoSlideshow& slideshow)
{
    M5.update();
    M5.BtnA.wasPressed();
    M5.BtnB.wasPressed();
    M5.BtnC.wasPressed();

    const uint32_t started_ms = nowMs();
    bool counting_idle        = false;
    uint32_t idle_started     = 0;

    while (true) {
        M5.update();
        if (M5.BtnC.wasPressed()) return true;

        if (nowMs() - started_ms >= AP_MAX_SESSION_MS) {
            ESP_LOGI(TAG, "hotspot hit its one-hour session cap");
            return false;
        }

        uint16_t requested = 0;
        if (photoApiTakeShowRequest(requested)) {
            ESP_LOGI(TAG, "web app asked for photo %u", (unsigned)requested);
            photoApiSetRefreshing(true);
            slideshow.showIndex(requested);
            photoApiSetRefreshing(false);
            // The refresh just ate half a minute; do not let that count as
            // idle time.
            counting_idle = false;
            M5.update();
            M5.BtnC.wasPressed();
            continue;
        }

        const bool busy = apStationCount() > 0 || webMsSinceLastRequest() < 30000;
        if (busy) {
            counting_idle = false;
        } else if (!counting_idle) {
            counting_idle = true;
            idle_started  = nowMs();
        } else if (nowMs() - idle_started >= AP_IDLE_MS) {
            ESP_LOGI(TAG, "hotspot idle for %u minutes", (unsigned)(AP_IDLE_MS / 60000));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace

ApExit runApMode(PhotoSlideshow& slideshow)
{
    if (!batteryOkForRadio()) {
        ESP_LOGW(TAG, "battery too low for the hotspot");
        drawCentredMessage("Battery too low", "for the photo manager");
        return ApExit::LowBattery;
    }

    stopUsbMassStorage();

    ApInfo info;
    if (apStart(info) != ESP_OK) {
        ESP_LOGE(TAG, "could not start the access point");
        drawCentredMessage("Could not start", "the photo manager");
        return ApExit::Failed;
    }
    if (webStart() != ESP_OK) {
        ESP_LOGE(TAG, "could not start the web server");
        apStop();
        drawCentredMessage("Could not start", "the photo manager");
        return ApExit::Failed;
    }

    // Radio first, panel second: the refresh takes 15-30 s and the phone can
    // join during it.
    drawApScreen(info);

    const bool dismissed = waitForDismissOrIdle(slideshow);
    ESP_LOGI(TAG, "hotspot session ending (%s)", dismissed ? "dismissed" : "idle");

    webStop();
    apStop();

    // Photos may have been added, removed or reordered, and the panel is
    // showing the credentials rather than a photo either way.
    slideshow.redrawCurrent();

    return dismissed ? ApExit::Dismissed : ApExit::Idle;
}

}  // namespace net
