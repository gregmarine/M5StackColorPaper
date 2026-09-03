/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>

bool get_image_size_from_memory(const uint8_t* data, size_t len, int* width, int* height, const char* ext);

bool get_image_size_from_file(const char* path, int* width, int* height);

/**
 * @brief Returns true if the file is a palette-indexed BMP (1/4/8 bits per pixel).
 *
 * Such files come from tools/prepare_photo and already contain only the panel's
 * colours, so the caller can skip the panel's own dithering.
 */
bool is_indexed_bmp_file(const char* path);
