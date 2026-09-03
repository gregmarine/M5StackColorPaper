/*
 * Minimal stand-in for the official demo's apps/app_manager module.
 *
 * The real app_manager.h declares a full menu/mode-switching/Wi-Fi-AP
 * subsystem that this photo-frame MVP doesn't use. local_photo_slideshow.cpp
 * (vendored as-is) only calls one function from this header, so that's all
 * we declare here.
 */
#pragma once

/**
 * @brief Updates the refresh-in-progress state.
 *
 * @param in_progress True while a refresh is running, otherwise false.
 */
void app_manager_set_refresh_in_progress(bool in_progress);
