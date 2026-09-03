/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "local_photo_slideshow.h"
#include <cstring>
#include <cctype>
#include <algorithm>
#include <dirent.h>
#include <strings.h>
#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_timer.h"
#include <M5Unified.h>
#include "hal/storage/hal_storage.h"
#include "hal/hal.h"
#include "hal/utils/audio/audio.h"
#include "hal/utils/image/image_utils.h"
#include "freertos/task.h"
#include "apps/app_manager/app_manager.h"

static const char *TAG = "Slideshow";

static constexpr uint8_t RX8130_RAM_INDEX_CURRENT_STORAGE = 0;

// Define an invalid index to represent "no photo is currently displayed"
#define NO_PHOTO 0xFFFF

// Panel rotations used to match each image's orientation. The panel is
// natively 400x600 (portrait, rotation 0). Landscape 3 is what hal.init()
// sets and what the upstream demo uses; the demo's "portrait" setting was
// display rotation 3 plus a canvas rotation of 1, i.e. net rotation 0, so
// portrait 0 matches it. Swap 0 <-> 2 (or 1 <-> 3) if images come out upside
// down for the way the frame is held.
static constexpr uint8_t ROTATION_LANDSCAPE = 3;
static constexpr uint8_t ROTATION_PORTRAIT  = 0;

namespace {

void resetPhotoIndexState(uint16_t &pending_index)
{
    pending_index = NO_PHOTO;
    hal.rx8130RamWrite(RX8130_RAM_INDEX_CURRENT_STORAGE, 0);
    hal.rx8130RamWrite(RX8130_RAM_INDEX_CURRENT_STORAGE + 1, 0);
}

esp_err_t selectStorageMedia(bool sd_inserted, bool &sd_fallback_locked)
{
    sd_fallback_locked = false;

    if (sd_inserted) {
        esp_err_t ret = hal_storage_init(APP_STORAGE_MEDIA_SDMMC);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        sd_fallback_locked = true;
        ESP_LOGW(TAG, "SD init failed, fallback to SPI flash: %s", esp_err_to_name(ret));
    }

    return hal_storage_init(APP_STORAGE_MEDIA_SPIFLASH);
}

void ensureStorageMedia(bool sd_inserted, bool &last_sd_inserted, bool &sd_fallback_locked, uint16_t &pending_index)
{
    if (!sd_inserted) {
        last_sd_inserted   = false;
        sd_fallback_locked = false;
    } else if (!last_sd_inserted) {
        last_sd_inserted   = true;
        sd_fallback_locked = false;
    }

    hal_storage_media_t target_media =
        (sd_inserted && !sd_fallback_locked) ? APP_STORAGE_MEDIA_SDMMC : APP_STORAGE_MEDIA_SPIFLASH;
    if (hal_storage_get_media() == target_media) {
        return;
    }

    esp_err_t ret = hal_storage_switch(target_media);
    if (ret == ESP_OK) {
        resetPhotoIndexState(pending_index);
        return;
    }

    if (!sd_inserted) {
        ESP_LOGW(TAG, "SPI flash switch failed: %s", esp_err_to_name(ret));
        return;
    }

    sd_fallback_locked = true;
    ESP_LOGW(TAG, "SD switch failed, fallback to SPI flash: %s", esp_err_to_name(ret));
    if (hal_storage_get_media() != APP_STORAGE_MEDIA_SPIFLASH) {
        esp_err_t fallback_ret = hal_storage_switch(APP_STORAGE_MEDIA_SPIFLASH);
        if (fallback_ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI flash fallback switch failed: %s", esp_err_to_name(fallback_ret));
            return;
        }
    }
    resetPhotoIndexState(pending_index);
}

}  // namespace

/* ---------- millis() ---------- */
static inline uint32_t millis_()
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* ====================== Lifecycle ====================== */
bool PhotoSlideshow::init(const char *dir_path, uint8_t interval_min)
{
    strncpy(_dir_path, dir_path, DIR_PATH_MAX - 1);
    _dir_path[DIR_PATH_MAX - 1] = '\0';
    _interval_min               = interval_min;
    _current_index              = NO_PHOTO;
    _pending_index              = NO_PHOTO;
    _running                    = false;
    _needs_refresh              = false;
    _last_btn_c                 = false;
    _last_btn_b                 = false;
    _last_sd_inserted           = hal.isSDCardInserted();
    _sd_fallback_locked         = false;
    _photo_list.clear();
    _scr_w = hal.Canvas->width();
    _scr_h = hal.Canvas->height();

    M5.Speaker.setVolume(120);

    selectStorageMedia(_last_sd_inserted, _sd_fallback_locked);

    scanPhotos();
    hal.statusEventSend(OPERATION_EVENT_STARTUP_SUCCESS);

    ESP_LOGI(TAG, "Init: Found %d photos, mode: %s%s", (int)_photo_list.size(), interval_min == 0 ? "manual" : "auto",
             interval_min > 0 ? (", interval: " + std::to_string(interval_min) + " min").c_str() : "");
    return true;
}

void PhotoSlideshow::deinit()
{
    stop();
    _photo_list.clear();
    _current_index = NO_PHOTO;
    _pending_index = NO_PHOTO;
}

/* ====================== Playback control ====================== */
void PhotoSlideshow::start()
{
    scanPhotos();  // Refresh the list first
    _running         = true;
    _current_index   = NO_PHOTO;
    _pending_index   = NO_PHOTO;
    _needs_refresh   = false;
    _last_refresh_ms = millis_();

    if (!_photo_list.empty()) {
        // ESP_LOGI(TAG, "Start → displaying first photo...");
        // displayPhoto(0);
        // _current_index = 0;
        // _pending_index = 0;
        uint8_t b0, b1;
        hal.rx8130RamRead(RX8130_RAM_INDEX_CURRENT, &b0);
        hal.rx8130RamRead(RX8130_RAM_INDEX_CURRENT + 1, &b1);
        _pending_index = (uint16_t)(b1 << 8 | b0);
    } else {
        // Starting without photos is fine; nothing will be displayed
        ESP_LOGW(TAG, "Start → No photos found, running in background...");
    }
}

void PhotoSlideshow::stop()
{
    _running       = false;
    _needs_refresh = false;
}

/* ====================== Manual navigation ====================== */
void PhotoSlideshow::next()
{
    // Rescan first; return immediately if there are no photos to avoid division by zero
    if (!rescanAndClamp()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);
    uint16_t new_index = 0;
    if (_pending_index == NO_PHOTO) {
        new_index = 0;  // The screen was blank before, so show the first photo directly
    } else {
        new_index = (_pending_index + 1) % (uint16_t)_photo_list.size();
    }
    requestRefresh(new_index);
}

void PhotoSlideshow::prev()
{
    if (!rescanAndClamp()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);

    uint16_t photo_count = (uint16_t)_photo_list.size();
    uint16_t new_index   = 0;
    if (_pending_index == NO_PHOTO) {
        new_index = 0;
    } else {
        new_index = (_pending_index == 0) ? (photo_count - 1) : (_pending_index - 1);
    }
    requestRefresh(new_index);
}

void PhotoSlideshow::goTo(uint16_t index)
{
    if (!rescanAndClamp()) return;
    index = index % (uint16_t)_photo_list.size();
    requestRefresh(index);
}

void PhotoSlideshow::setInterval(uint8_t min)
{
    _interval_min = min;
}
void PhotoSlideshow::setSettleTime(uint32_t ms)
{
    _settle_ms = ms;
}

bool PhotoSlideshow::resume()
{
    if (!rescanAndClamp()) {
        ESP_LOGW(TAG, "Resume: no photos found");
        return false;
    }

    uint8_t b0 = 0;
    uint8_t b1 = 0;
    hal.rx8130RamRead(RX8130_RAM_INDEX_CURRENT, &b0);
    hal.rx8130RamRead(RX8130_RAM_INDEX_CURRENT + 1, &b1);
    _pending_index = (uint16_t)(b1 << 8 | b0);
    clampIndices();
    // The stored position is where the next press moves *from*, but nothing is
    // marked as displayed: the file list may have changed since the panel was
    // last drawn, and with a single file next/prev wrap onto the same index,
    // which requestRefresh() would otherwise treat as "already showing" and
    // skip. Leaving _current_index at NO_PHOTO makes the first press always
    // draw. The panel is not touched here.
    _current_index   = NO_PHOTO;
    _needs_refresh   = false;
    _last_refresh_ms = millis_();
    if (_pending_index == NO_PHOTO) {
        ESP_LOGI(TAG, "Resume: no photo shown yet, %d available", (int)_photo_list.size());
    } else {
        ESP_LOGI(TAG, "Resume at [%d/%d]: %s (not redrawn)", _pending_index + 1, (int)_photo_list.size(),
                 _photo_list[_pending_index].c_str());
    }
    return true;
}

void PhotoSlideshow::runInteractive(uint32_t idle_ms, bool (*abort_cb)())
{
    // Short settle so a quick double-press skips two photos with one refresh
    // instead of paying for two 15-30 s panel refreshes.
    const uint32_t settle_ms = 400;
    uint32_t last_activity   = millis_();
    _needs_refresh           = false;

    while (millis_() - last_activity < idle_ms) {
        if (abort_cb && abort_cb()) {
            ESP_LOGI(TAG, "Interactive window aborted");
            return;
        }

        M5.update();
        // M5Unified maps the PaperColor keys as A = G10 (upper side key),
        // B = G9 (lower side key), C = G1 (top edge). Upper key moves up the
        // list, lower key moves down it. C is reserved and does nothing.
        bool up   = M5.BtnA.wasPressed();
        bool down = M5.BtnB.wasPressed();
        M5.BtnC.wasPressed();  // consume so it can't be mistaken for a stale press later
        if (up) {
            prev();
            last_activity = millis_();
        } else if (down) {
            next();
            last_activity = millis_();
        }

        if (_needs_refresh && millis_() - _last_button_ms >= settle_ms) {
            ESP_LOGI(TAG, "Button → refreshing [%d/%d]: %s", _pending_index + 1, (int)_photo_list.size(),
                     _photo_list[_pending_index].c_str());
            displayPhoto(_pending_index);
            _current_index   = _pending_index;
            _needs_refresh   = false;
            _last_refresh_ms = millis_();
            // Presses during the long refresh are stale; drop them.
            M5.update();
            M5.BtnA.wasPressed();
            M5.BtnB.wasPressed();
            M5.BtnC.wasPressed();
            last_activity = millis_();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "Interactive window idle for %lu ms, leaving", (unsigned long)idle_ms);
}

void PhotoSlideshow::toggleRotation()
{
    if (!rescanAndClamp()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    hal.statusEventSend(OPERATION_EVENT_SUCCESS);

    // Toggle between Rotation(0) and Rotation(3)
    uint8_t current = hal.Canvas->getRotation();
    uint8_t next    = (current == 0) ? 1 : 0;

    hal.settingsLock();
    hal.settings.rotation = next;
    hal.settingsUnlock();

    hal.Canvas->setRotation(next);

    // Update screen size
    _scr_w = hal.Canvas->width();
    _scr_h = hal.Canvas->height();

    ESP_LOGI(TAG, "Toggle rotation to %d", next);
    ESP_LOGI(TAG, "Toggle screen size to %d, %d", _scr_w, _scr_h);

    // Redisplay the current photo
    displayPhoto(_pending_index);
}

void PhotoSlideshow::syncSettings()
{
    hal.settingsLock();
    uint8_t target_interval =
        (hal.settings.auto_slideshow && hal.settings.interval_minutes > 0) ? (uint8_t)hal.settings.interval_minutes : 0;
    uint8_t target_rotation = hal.settings.rotation;
    hal.settingsUnlock();

    if (_interval_min != target_interval) {
        _interval_min = target_interval;
    }

    if (hal.Canvas->getRotation() != target_rotation) {
        hal.Canvas->setRotation(target_rotation);
        _scr_w = hal.Canvas->width();
        _scr_h = hal.Canvas->height();
    }
}

/* ====================== Main loop ====================== */
void PhotoSlideshow::update()
{
    if (!_running) return;

    syncSettings();

    ensureStorageMedia(hal.isSDCardInserted(), _last_sd_inserted, _sd_fallback_locked, _pending_index);

    // Check buttons
    handleButtons();

    // Check whether the settle delay has elapsed
    if (_needs_refresh) {
        if (millis_() - _last_button_ms >= _settle_ms) {
            if (!rescanAndClamp()) {
                _needs_refresh = false;
                ESP_LOGW(TAG, "No photos after rescan, cancel refresh");
                return;
            }
            // Skip refresh only when something is already displayed (!= NO_PHOTO) and the index is unchanged
            if (_pending_index == _current_index && _current_index != NO_PHOTO) {
                _needs_refresh = false;
                ESP_LOGI(TAG, "After rescan, pending == current, skip refresh");
                return;
            }
            ESP_LOGI(TAG, "Settle OK → refreshing [%d/%d]: %s", _pending_index + 1, (int)_photo_list.size(),
                     _photo_list[_pending_index].c_str());
            displayPhoto(_pending_index);
            _current_index   = _pending_index;
            _needs_refresh   = false;
            _last_refresh_ms = millis_();
        }
        return;
    }

    // ③ Auto slideshow (_interval_min > 0 and no refresh is pending)
    if (_interval_min > 0) {
        uint32_t interval_ms = (uint32_t)_interval_min * 60000UL;
        if (millis_() - _last_refresh_ms >= interval_ms) {
            // Always reset the time first to avoid scanning the SD card every frame when the directory is empty
            _last_refresh_ms = millis_();

            if (!rescanAndClamp()) {
                ESP_LOGD(TAG, "Auto: no photos, skip this interval");
                return;
            }

            // If the screen was blank before, show index 0; otherwise show the next photo
            uint16_t next_index =
                (_current_index == NO_PHOTO) ? 0 : ((_current_index + 1) % (uint16_t)_photo_list.size());

            ESP_LOGI(TAG, "Auto → [%d/%d]: %s", next_index + 1, (int)_photo_list.size(),
                     _photo_list[next_index].c_str());
            displayPhoto(next_index);
            _current_index = next_index;
            _pending_index = next_index;
        }
    }
}

/* ====================== Status queries ====================== */
uint16_t PhotoSlideshow::getTotal() const
{
    return (uint16_t)_photo_list.size();
}
uint16_t PhotoSlideshow::getCurrentIndex() const
{
    return (_current_index == NO_PHOTO) ? 0 : _current_index;
}
uint16_t PhotoSlideshow::getPendingIndex() const
{
    return (_pending_index == NO_PHOTO) ? 0 : _pending_index;
}
bool PhotoSlideshow::isRunning() const
{
    return _running;
}
bool PhotoSlideshow::isWaitingSettle() const
{
    return _needs_refresh;
}

/* ====================== Button handling ====================== */
void PhotoSlideshow::handleButtons()
{  // A 523
    bool button_a_pressed  = M5.BtnA.wasPressed();
    bool button_a_released = M5.BtnA.wasClicked();
    bool button_c_pressed  = M5.BtnC.wasPressed();
    bool button_b_pressed  = M5.BtnB.wasPressed();
    if (button_c_pressed) audio::play_tone_from_midi(119, 0.08);
    if (button_b_pressed) audio::play_tone_from_midi(120, 0.08);
    if (button_a_pressed) audio::play_tone_from_midi(121, 0.08);
    if (button_a_released && !_last_btn_a) toggleRotation();
    if (button_c_pressed && !_last_btn_c) prev();
    if (button_b_pressed && !_last_btn_b) next();
    _last_btn_c = button_c_pressed;
    _last_btn_b = button_b_pressed;
    _last_btn_a = button_a_released;
}

/* ============== Rescan ============== */
bool PhotoSlideshow::rescanAndClamp()
{
    bool has_photos = scanPhotos();
    clampIndices();
    if (!has_photos) {
        ESP_LOGD(TAG, "Rescan: directory empty");
        return false;
    }
    return true;
}

void PhotoSlideshow::clampIndices()
{
    uint16_t photo_count = (uint16_t)_photo_list.size();
    if (photo_count == 0) return;  // Preserve the NO_PHOTO state when empty

    // Only wrap out-of-range indices when not in the NO_PHOTO state
    if (_current_index != NO_PHOTO && _current_index >= photo_count) {
        _current_index = _current_index % photo_count;
    }
    if (_pending_index != NO_PHOTO && _pending_index >= photo_count) {
        _pending_index = _pending_index % photo_count;
    }
}

/* ================ Settle ================ */
void PhotoSlideshow::requestRefresh(uint16_t new_index)
{
    _pending_index = new_index;
    // Cancel refresh if we looped back to the current photo and a photo is actually displayed
    if (_pending_index == _current_index && _current_index != NO_PHOTO) {
        _needs_refresh = false;
        ESP_LOGI(TAG, "Back to current [%d/%d], cancel refresh", _current_index + 1, (int)_photo_list.size());
        return;
    }
    _needs_refresh  = true;
    _last_button_ms = millis_();
    hal.rx8130RamWrite(RX8130_RAM_INDEX_CURRENT, (uint8_t)(_pending_index & 0xFF));
    hal.rx8130RamWrite(RX8130_RAM_INDEX_CURRENT + 1, (uint8_t)(_pending_index >> 8) & 0xFF);
    ESP_LOGI(TAG, "Pending → [%d/%d]: %s", _pending_index + 1, (int)_photo_list.size(),
             _photo_list[_pending_index].c_str());
}

/* ====================== Display photo ====================== */
void PhotoSlideshow::displayPhoto(uint16_t index)
{
    int image_width = 0, image_height = 0;

    if (index >= _photo_list.size()) return;
    const char *path = _photo_list[index].c_str();

    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    if (!get_image_size_from_file(path, &image_width, &image_height)) {
        hal_storage_unlock();
        ESP_LOGE(TAG, "Failed to read image size: %s", path);
        hal.statusEventSend(OPERATION_EVENT_ERROR_IMAGE_READ);
        return;
    }

    // Rotate the panel to match the image: portrait images are shown with the
    // panel in portrait, landscape ones in landscape, so a pre-dithered
    // 400x600 or 600x400 BMP always lands 1:1 on the pixels. Recreating the
    // canvas is the only way to change its dimensions.
    uint8_t want_rotation = (image_height > image_width) ? ROTATION_PORTRAIT : ROTATION_LANDSCAPE;
    if (M5.Display.getRotation() != want_rotation) {
        M5.Display.setRotation(want_rotation);
    }
    // The canvas must be an unrotated, same-size copy of the panel so that
    // pushSprite(0, 0) lands pixels 1:1. A sprite keeps its own rotation
    // across createSprite(), so force it to 0 rather than assume it.
    if (hal.Canvas->getRotation() != 0 || hal.Canvas->width() != M5.Display.width() ||
        hal.Canvas->height() != M5.Display.height()) {
        hal.Canvas->deleteSprite();
        hal.Canvas->setRotation(0);
        hal.Canvas->createSprite(M5.Display.width(), M5.Display.height());
        ESP_LOGI(TAG, "Panel rotation %d, canvas %dx%d", want_rotation, hal.Canvas->width(), hal.Canvas->height());
    }
    _scr_w = hal.Canvas->width();
    _scr_h = hal.Canvas->height();

    float scale = std::min((float)_scr_w / image_width, (float)_scr_h / image_height);
    int draw_x  = (_scr_w - (int)(image_width * scale)) / 2;
    int draw_y  = (_scr_h - (int)(image_height * scale)) / 2;

    hal.Canvas->fillScreen(TFT_WHITE);

    const char *fname = strrchr(path, '/');
    fname             = fname ? fname + 1 : path;
    // Pre-dithered images (indexed BMPs from tools/prepare_photo, or the
    // upstream demo's "imageN*" convention) already contain only the panel's
    // six colours. Push them in epd_fastest, the driver's no-dither mode, so
    // its nearest-colour lookup maps each pixel 1:1 instead of re-dithering
    // an already dithered picture.
    bool use_fastest = (strncmp(fname, "imageN", 6) == 0) || is_indexed_bmp_file(path);
    if (use_fastest) {
        ESP_LOGI(TAG, "Pre-quantized image, no panel dithering");
        M5.Display.setEpdMode(epd_mode_t::epd_fastest);
    }

    const char *dot = strrchr(path, '.');
    if (dot) {
        if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            hal.Canvas->drawJpgFile(path, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        } else if (strcasecmp(dot, ".bmp") == 0) {
            hal.Canvas->drawBmpFile(path, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        } else if (strcasecmp(dot, ".png") == 0) {
            hal.Canvas->drawPngFile(path, draw_x, draw_y, 0, 0, 0, 0, scale, scale);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_START);
            app_manager_set_refresh_in_progress(true);
            hal.Canvas->pushSprite(0, 0);
            app_manager_set_refresh_in_progress(false);
            hal.statusEventSend(OPERATION_EVENT_REFRESH_COMPLETE);
        }
    }

    if (use_fastest) {
        M5.Display.setEpdMode(epd_mode_t::epd_quality);
    }
    hal_storage_unlock();
}

/* ====================== Display photo by path ====================== */
void PhotoSlideshow::displayPhotoByPath(const char *path)
{
    if (!rescanAndClamp()) {
        hal.statusEventSend(OPERATION_EVENT_FAILED);
        return;
    }
    for (uint16_t photo_index = 0; photo_index < _photo_list.size(); photo_index++) {
        if (_photo_list[photo_index] == path) {
            displayPhoto(photo_index);
            _current_index = photo_index;
            _pending_index = photo_index;
            ESP_LOGI(TAG, "Display by path: [%d/%d]: %s", photo_index + 1, (int)_photo_list.size(), path);
            return;
        }
    }
    ESP_LOGW(TAG, "Photo not found in list: %s", path);
}

/* ====================== File scan ====================== */
bool PhotoSlideshow::scanPhotos()
{
    _photo_list.clear();
    hal_storage_prepare_photo_fs_access();
    hal_storage_lock();
    DIR *dir = opendir(_dir_path);
    if (!dir) {
        hal_storage_unlock();
        ESP_LOGW(TAG, "Cannot open dir: %s", _dir_path);
        return false;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (_photo_list.size() >= MAX_PHOTOS) break;
        if (entry->d_type == DT_DIR) continue;
        // Skip hidden files. macOS Finder writes "._name.jpg" AppleDouble
        // metadata next to every file it copies onto a FAT drive; those are
        // not images even though the extension matches.
        if (entry->d_name[0] == '.') continue;
        if (isImageFile(entry->d_name)) {
            std::string full = std::string(_dir_path) + "/" + entry->d_name;
            _photo_list.push_back(full);
        }
    }
    closedir(dir);
    hal_storage_unlock();
    std::sort(_photo_list.begin(), _photo_list.end());
    ESP_LOGD(TAG, "Scanned %d photos in %s", (int)_photo_list.size(), _dir_path);
    return !_photo_list.empty();
}

bool PhotoSlideshow::isImageFile(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".png") == 0;
}
