#include "touch_ft6336.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_init.h"

#define TAG "touch"

#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_SDA_IO      0
#define TOUCH_SCL_IO      1
#define TOUCH_I2C_FREQ_HZ 400000
#define TOUCH_ADDR        0x38

#define FT_REG_NUM_POINTS 0x02
#define FT_REG_TOUCH_DATA 0x03

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t touch_ft6336_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_SDA_IO,
        .scl_io_num = TOUCH_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = TOUCH_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "FT6336 ready at 0x%02X", TOUCH_ADDR);
    return ESP_OK;
}

static esp_err_t ft6336_read_register(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

esp_err_t touch_ft6336_read(touch_point_t *pt)
{
    pt->pressed = false;
    pt->x = 0;
    pt->y = 0;

    uint8_t num_points = 0;
    if (ft6336_read_register(FT_REG_NUM_POINTS, &num_points, 1) != ESP_OK) {
        return ESP_FAIL;
    }
    /* 点数异常直接视为无触摸 */
    if (num_points == 0 || num_points > 5) {
        return ESP_OK;
    }

    uint8_t raw[6] = {0};
    /* 0x03 起每个触点 6 字节,我们只要第一个 */
    if (ft6336_read_register(FT_REG_TOUCH_DATA, raw, sizeof(raw)) != ESP_OK) {
        return ESP_FAIL;
    }

    uint16_t rx = ((raw[0] & 0x0F) << 8) | raw[1];
    uint16_t ry = ((raw[2] & 0x0F) << 8) | raw[3];

    /* 坐标映射:与官方 esp_lcd_touch(swap_xy=1, mirror_x=1, mirror_y=0)一致。
     * 先镜像(仅 X),再交换 X/Y,得到 LVGL 横屏 320x240 坐标。 */
    int32_t lx = (int32_t)ry;        /* y_max 侧(320) */
    int32_t ly = LCD_V_RES - (int32_t)rx; /* 240 - rx */
    if (lx < 0) lx = 0;
    if (lx >= LCD_H_RES) lx = LCD_H_RES - 1;
    if (ly < 0) ly = 0;
    if (ly >= LCD_V_RES) ly = LCD_V_RES - 1;

    pt->x = (uint16_t)lx;
    pt->y = (uint16_t)ly;
    pt->pressed = true;
    return ESP_OK;
}