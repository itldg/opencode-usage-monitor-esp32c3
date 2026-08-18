#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool     pressed; /* true 表示当前有有效触点 */
} touch_point_t;

/**
 * @brief Init FT6336 over I2C master (SDA=GPIO0, SCL=GPIO1, 400kHz, addr 0x38)
 */
esp_err_t touch_ft6336_init(void);

/**
 * @brief Get the I2C master bus handle used by FT6336 (for sharing with codec).
 */
esp_err_t touch_i2c_bus_handle(i2c_master_bus_handle_t *out);

/**
 * @brief Read the first touch point and convert to LVGL landscape coords.
 * @param pt  output point
 * @return ESP_OK even when not pressed (pt->pressed == false)
 */
esp_err_t touch_ft6336_read(touch_point_t *pt);

#ifdef __cplusplus
}
#endif