/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * @brief Initialize image display and show the first image from SD card
 * @return true if images found and displayed, false otherwise
 */
bool image_display_init(void);

/**
 * @brief Show the next image in the list
 * @return true if successful, false otherwise
 */
bool image_display_next(void);

/**
 * @brief Show the previous image in the list
 * @return true if successful, false otherwise
 */
bool image_display_prev(void);

/**
 * @brief Show a specific image by filename
 * @param filename Image filename (e.g., "photo.jpg")
 * @return true if image found and displayed, false otherwise
 */
bool image_display_by_name(const char *filename);

/**
 * @brief Cleanup image display resources
 */
void image_display_cleanup(void);

/**
 * @brief Get current image index
 * @return Current image index
 */
int image_display_get_current_index(void);

/**
 * @brief Get total number of images
 * @return Total image count
 */
int image_display_get_count(void);

/**
 * @brief Start video playback (loop through all images)
 * @param fps Target frames per second (1-120)
 * @return true if started, false otherwise
 */
bool video_playback_start(int fps);

/**
 * @brief Stop video playback
 */
void video_playback_stop(void);

/**
 * @brief Get actual FPS
 * @return Current FPS
 */
int video_get_fps(void);
