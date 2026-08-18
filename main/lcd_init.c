#include "lcd_init.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "touch_ft6336.h"

#define TAG "lcd"

/* ---- Pin & bus config (matches LCKFB ESP32C3 board) ---- */
#define LCD_HOST        SPI2_HOST
#define LCD_PIN_SCLK    3
#define LCD_PIN_MOSI    5
#define LCD_PIN_MISO    -1
#define LCD_PIN_DC      6
#define LCD_PIN_RST     -1
#define LCD_PIN_CS      4
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS    8
#define LCD_PARAM_BITS  8

#define LCD_BK_LIGHT_PIN      2
#define LCD_BK_LIGHT_ON_LEVEL 0       /* low level turns backlight on */
#define LCD_BK_LIGHT_DUTY     0.5f    /* 50% brightness */

#define LVGL_TICK_MS  2
#define LVGL_BUFFER_LINES 20

static lv_disp_drv_t s_disp_drv;   /* must outlive io_config creation */
static lv_indev_drv_t s_indev_drv;
static bool s_touch_active = false;

bool lcd_touch_activity_take(void)
{
    bool active = s_touch_active;
    s_touch_active = false;
    return active;
}

static bool lcd_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx)
{
    lv_disp_drv_t *drv = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(drv);
    return false;
}

static void lcd_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

static void lcd_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    touch_point_t pt;
    data->state = LV_INDEV_STATE_RELEASED;
    if (touch_ft6336_read(&pt) == ESP_OK && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;
            s_touch_active = true;
        }
}

static void lcd_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

esp_err_t lcd_init(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_notify_flush_ready,
        .user_ctx = &s_disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    ESP_LOGI(TAG, "Install ST7789 panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Initialize touch controller FT6336");
    ESP_ERROR_CHECK(touch_ft6336_init());

    ESP_LOGI(TAG, "Initialize backlight");
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = LCD_BK_LIGHT_PIN,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = (LCD_BK_LIGHT_ON_LEVEL == 0),
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    /* 0.5 duty + inverted output = 50% brightness on an active-low backlight */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0,
                                  (uint32_t)((1 << 13) * LCD_BK_LIGHT_DUTY)));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0));

    ESP_LOGI(TAG, "Init LVGL");
    lv_init();
    static lv_color_t buf1[LCD_H_RES * LVGL_BUFFER_LINES];
    static lv_color_t buf2[LCD_H_RES * LVGL_BUFFER_LINES];
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * LVGL_BUFFER_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lcd_lvgl_flush_cb;
    s_disp_drv.draw_buf = &draw_buf;
    s_disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = lcd_lvgl_touch_cb;
    lv_indev_drv_register(&s_indev_drv);

    esp_timer_handle_t tick_timer;
    esp_timer_create_args_t tick_args = {
        .callback = lcd_lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    ESP_LOGI(TAG, "LCD init done");
    return ESP_OK;
}