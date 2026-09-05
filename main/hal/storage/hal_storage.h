/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

#ifndef PHOTO_OPS_FORCE_APP_MOUNT
#define PHOTO_OPS_FORCE_APP_MOUNT (1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Storage media type.
 */
typedef enum {
    APP_STORAGE_MEDIA_SPIFLASH = 0, /*!< Internal SPI Flash */
    APP_STORAGE_MEDIA_SDMMC,        /*!< External SD/MMC card */
} hal_storage_media_t;

/**
 * @brief Initializes storage and installs the TinyUSB MSC driver.
 *
 * @param media Selected storage media.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code on failure
 */
esp_err_t hal_storage_init(hal_storage_media_t media);

/**
 * @brief Switches storage media by remounting from the current backend to the new one.
 *
 * @param media New storage media.
 *
 * @return
 *      - ESP_OK on success
 *      - An ESP error code on failure
 */
esp_err_t hal_storage_switch(hal_storage_media_t media);

/**
 * @brief Returns the current storage media.
 */
hal_storage_media_t hal_storage_get_media(void);

/**
 * @brief Mounts storage for app use.
 */
void storage_mount_to_app(void);

/**
 * @brief Mounts storage for USB host access (exposes it as a USB drive).
 *
 * Added for the photo-frame firmware: mirrors storage_mount_to_app() but
 * switches the mount point the other direction. Not present in the
 * upstream M5PaperColor-UserDemo, which only ever switches back to APP.
 */
void storage_mount_to_usb(void);

/**
 * @brief Prepares storage ownership before photo filesystem access.
 */
void hal_storage_prepare_photo_fs_access(void);

/**
 * @brief Detaches the device from USB, leaving the volume mounted by the app.
 *
 * Added for the photo-frame firmware. Hotspot mode needs the FAT volume for
 * itself -- it has one owner at a time -- and dropping off the USB bus is
 * cleaner than leaving the host holding a drive that has stopped answering.
 * It also hands the port back to the USB-Serial-JTAG console, which is what
 * makes hotspot mode debuggable over the cable.
 *
 * The host will report that the drive was removed improperly if it still had
 * it mounted. That is unavoidable: the alternative is corrupting it.
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t hal_storage_usb_detach(void);

/**
 * @brief Re-attaches the device to USB after hal_storage_usb_detach().
 *
 * @return ESP_OK on success, otherwise an ESP error code.
 */
esp_err_t hal_storage_usb_attach(void);

/** @brief Returns whether the TinyUSB device driver is currently installed. */
bool hal_storage_usb_attached(void);

/** @brief Locks storage access. */
void hal_storage_lock(void);
/** @brief Unlocks storage access. */
void hal_storage_unlock(void);

extern bool s_spi_bus_inited;

#ifdef __cplusplus
}
#endif
