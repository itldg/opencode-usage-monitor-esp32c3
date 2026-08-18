#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LCD resolution in landscape orientation (after swap_xy) */
#define LCD_H_RES 320
#define LCD_V_RES 240

/**
 * @brief Initialize ST7789 (SPI) + LVGL + touch + backlight.
 * @return ESP_OK on success
 */
esp_err_t lcd_init(void);

/**
 * @brief Non-blocking touch activity check (set by LVGL touch handler).
 * @return true once per press, cleared after read
 */
bool lcd_touch_activity_take(void);

#ifdef __cplusplus
}
#endif