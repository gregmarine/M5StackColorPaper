/*
 * M5Stack PaperColor - simple USB-fed photo frame.
 *
 * Boot flow:
 *   - If a USB host is present, expose the internal flash as a USB drive
 *     (mass storage) so photos can be dragged onto it, and stay in that mode
 *     until the host disconnects.
 *   - Otherwise (woken by the power button, or a fresh power-on with no USB
 *     attached), leave the current photo on the panel and wait for the side
 *     keys to step through the photos, then power off when idle.
 *
 * This app is a purpose-built replacement for the official
 * M5PaperColor-UserDemo's main.cpp / app_manager / app_server: it reuses the
 * demo's hal/ and local_photo_slideshow app classes as-is, but wires them
 * together for a single-purpose photo frame instead of the demo's full
 * menu/cloud-sync system.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <M5Unified.h>
#include "tinyusb.h"
#include "hal/hal.h"
#include "hal/storage/hal_storage.h"
#include "apps/local_photo_slideshow/local_photo_slideshow.h"

using namespace hal_wifi;
Hal hal;

namespace {

constexpr const char* PHOTO_DIR = "/data";

// USB "host present" is decided from the PM1 power manager's input-power
// reading, not from TinyUSB's tud_mounted(). Enumeration on the host side can
// take seconds (macOS in particular shows an "allow accessory to connect?"
// prompt and doesn't configure the device until the user answers), so a short
// tud_mounted() poll right after boot reliably concludes "no host" and the
// device powers itself off before the drive ever appears.
constexpr uint16_t USB_VIN_PRESENT_MV = 4000;
constexpr int      USB_VIN_POLL_MS    = 250;

// After waking, how long the device stays awake waiting for the side keys
// (upper = previous, lower = next) before powering itself off. Each press
// restarts the timer.
constexpr uint32_t INTERACTIVE_IDLE_MS = 120000;

bool usbPowerPresent()
{
    m5pm1_pwr_src_t src = M5PM1_PWR_SRC_UNKNOWN;
    if (hal.pm1.getPowerSource(&src) == M5PM1_OK) {
        if (src == M5PM1_PWR_SRC_5VIN || src == M5PM1_PWR_SRC_5VINOUT) return true;
    }
    uint16_t vin_mv = 0;
    if (hal.pm1.readVin(&vin_mv) == M5PM1_OK && vin_mv >= USB_VIN_PRESENT_MV) return true;
    return false;
}

void showNoPhotosPlaceholder()
{
    hal.Canvas->fillScreen(TFT_WHITE);
    hal.Canvas->setTextColor(TFT_BLACK);
    hal.Canvas->setTextDatum(textdatum_t::middle_center);
    hal.Canvas->setTextSize(2);
    hal.Canvas->drawString("Connect via USB", hal.Canvas->width() / 2, hal.Canvas->height() / 2 - 16);
    hal.Canvas->drawString("to add photos", hal.Canvas->width() / 2, hal.Canvas->height() / 2 + 16);
    hal.Canvas->pushSprite(0, 0);
}

// Runs the USB mass-storage "drive" mode until the cable is unplugged.
// Ejecting the drive on the host side is not the exit condition: the host may
// eject and re-mount, and a plain charger keeps VIN up without ever mounting.
// The device simply stays a drive for as long as it has USB power.
void runUsbDriveMode()
{
    storage_mount_to_usb();
    bool was_mounted = false;
    while (usbPowerPresent()) {
        bool mounted = tud_mounted();
        if (mounted != was_mounted) {
            ESP_LOGI("main", "USB host %s", mounted ? "mounted the drive" : "not mounted");
            was_mounted = mounted;
        }
        vTaskDelay(pdMS_TO_TICKS(USB_VIN_POLL_MS));
    }
    ESP_LOGI("main", "USB power removed, leaving drive mode");
    storage_mount_to_app();
}

// Polls the A/B/C keys for `window_ms`, logging the raw GPIO levels whenever
// they change. Returns true if any key was seen pressed. Doubles as a way to
// check the keys are wired the way M5Unified expects (active-low on 10/9/1).
bool waitForKeyPress(uint32_t window_ms)
{
    int last_raw = -1;
    bool pressed = false;
    for (uint32_t elapsed = 0; elapsed < window_ms; elapsed += 20) {
        M5.update();
        int raw = (gpio_get_level(GPIO_NUM_10) << 2) | (gpio_get_level(GPIO_NUM_9) << 1) | gpio_get_level(GPIO_NUM_1);
        if (raw != last_raw) {
            ESP_LOGI("main", "keys raw: G10=%d G9=%d G1=%d  BtnA=%d BtnB=%d BtnC=%d", (raw >> 2) & 1, (raw >> 1) & 1,
                     raw & 1, M5.BtnA.isPressed(), M5.BtnB.isPressed(), M5.BtnC.isPressed());
            last_raw = raw;
        }
        if (M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed()) pressed = true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return pressed;
}

}  // namespace

extern "C" void app_main(void)
{
    hal.init();
    hal.detectWakeSource();
    vTaskDelay(pdMS_TO_TICKS(500));
    hal.settingsInit();

    hal_storage_init(APP_STORAGE_MEDIA_SPIFLASH);
    hal_storage_prepare_photo_fs_access();

    // With USB power present the device normally becomes a drive. Holding any
    // of the A/B/C keys during the first 2 s after boot skips that and runs
    // the slideshow instead, with the USB serial console still attached
    // (TinyUSB never starts, so the USB-Serial-JTAG port stays alive). Handy
    // for viewing photos while charging and for debugging with logs.
    bool usb_at_boot = usbPowerPresent();
    bool key_held    = false;
    if (usb_at_boot) {
        ESP_LOGI("main", "USB power present; hold a side key now to run the slideshow with the console attached");
        key_held = waitForKeyPress(2000);
    }

    if (usb_at_boot && !key_held) {
        ESP_LOGI("main", "Entering USB drive mode");
        runUsbDriveMode();
        // Photos may have just been added/removed; power off without
        // advancing the slideshow since no button was pressed.
        hal.powerOff();
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    static PhotoSlideshow slideshow;
    slideshow.init(PHOTO_DIR);
    if (slideshow.getTotal() == 0) {
        showNoPhotosPlaceholder();
    } else {
        // The panel still shows the last photo (e-paper keeps its image with
        // the power off), so waking does not redraw anything. Photos only
        // change on an explicit key press: upper side key = previous, lower
        // side key = next. On battery, plugging in USB during the window drops
        // straight into drive mode.
        slideshow.resume();
        slideshow.runInteractive(INTERACTIVE_IDLE_MS, usb_at_boot ? nullptr : usbPowerPresent);
        if (!usb_at_boot && usbPowerPresent()) {
            ESP_LOGI("main", "USB power present, entering drive mode");
            runUsbDriveMode();
        }
    }

    hal.powerOff();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
