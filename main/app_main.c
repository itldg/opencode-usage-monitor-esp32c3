#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "lcd_init.h"
#include "usage_api.h"
#include "usage_ui.h"
#include "wifi_app.h"

#define TAG "main"

#define LOOP_DELAY_MS          5
#define TOUCH_DEBOUNCE_MS      5000   /* 触摸触发刷新后的防抖 */
#define WIFI_RETRY_PERIOD_MS   60000  /* WiFi 未连接时的自动重试周期 */
#define SNTP_WAIT_MS           3000   /* 首次 NTP 同步等待(不阻塞 UI 太久) */

static usage_quota_t s_quota;
static bool s_time_synced = false;
static bool s_sntp_initialized = false;
static bool s_touch_pressed = false;
static lv_point_t s_touch_start;

/* 初始化 SNTP 并尝试同步时间(超时 wait_ms,不在此阻塞太久) */
static void sntp_try_sync(int wait_ms)
{
    if (s_time_synced) return;
    if (!s_sntp_initialized) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
            return;
        }
        s_sntp_initialized = true;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(wait_ms)) == ESP_OK) {
        s_time_synced = true;
        ESP_LOGI(TAG, "SNTP time synced");
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout (%d ms), time not ready yet", wait_ms);
    }
}

static esp_err_t do_fetch(void)
{
    /* 时间未同步时每次刷新都再试 NTP(首次等待常超时) */
    if (!s_time_synced) sntp_try_sync(SNTP_WAIT_MS);

    esp_err_t err = usage_api_fetch(&s_quota);
    uint64_t now_ms = esp_timer_get_time() / 1000;
    if (err == ESP_OK) {
        usage_ui_update(&s_quota, now_ms);
        usage_ui_set_error(NULL);
        char buf[32];
        time_t t = time(NULL);
        if (s_time_synced && t > 1000000000LL) {
            struct tm tmv;
            localtime_r(&t, &tmv);
            snprintf(buf, sizeof(buf), "更新于 %02d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            snprintf(buf, sizeof(buf), "更新于 --:--");
        }
        usage_ui_set_time(buf);
        ESP_LOGI(TAG, "UI refreshed");
    } else {
        usage_ui_set_error("获取失败");
    }
    return err;
}

static void refresh_from_touch(void)
{
    usage_ui_refresh_begin();
    if (wifi_app_is_connected()) {
        do_fetch();
    } else if (wifi_app_init(20000) == ESP_OK) {
        usage_ui_splash_status("数据获取中...");
        sntp_try_sync(SNTP_WAIT_MS);
        do_fetch();
    } else {
        usage_ui_set_error("连接失败");
    }
    usage_ui_refresh_end();
}

void app_main(void)
{
    /* 北京时间 UTC+8(无夏令时),须在首次 localtime_r 前设置 */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* NVS 首次烧录可能无空闲页 */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);

    if (strlen(CONFIG_OPENCODE_API_KEY) == 0) {
        ESP_LOGW(TAG, "API key is empty! Run menuconfig -> OpenCode Go Usage Display");
    }
    if (strlen(CONFIG_OPENCODE_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty! Run menuconfig -> OpenCode Go Usage Display");
    }

    ESP_ERROR_CHECK(lcd_init());
    usage_ui_create();
    usage_ui_splash_status("WIFI连接中...");

    esp_err_t werr = wifi_app_init(30000 + CONFIG_OPENCODE_WIFI_RETRY_COUNT * 3000);
    if (werr != ESP_OK) {
        usage_ui_set_error("连接失败");
    } else {
        usage_ui_splash_status("时间获取中...");
        sntp_try_sync(SNTP_WAIT_MS);
        usage_ui_splash_status("数据获取中...");
        do_fetch();
    }

    uint64_t refresh_ms = (uint64_t)CONFIG_OPENCODE_REFRESH_MINUTES * 60 * 1000;
    uint64_t last_auto_ms = 0;
    uint64_t last_touch_ms = 0;
    uint64_t last_try_ms = 0;
    uint64_t last_tick_ms = 0;

    while (1) {
        lv_timer_handler();
        uint64_t now_ms = esp_timer_get_time() / 1000;

        /* 每秒刷新倒计时与更新时间 */
        if ((now_ms - last_tick_ms) >= 1000) {
            last_tick_ms = now_ms;
            usage_ui_tick(now_ms);
        }

        /* 自动定时刷新(CONFIG_OPENCODE_REFRESH_MINUTES,0 表示关闭) */
        if (refresh_ms > 0 && (now_ms - last_auto_ms) >= refresh_ms) {
            last_auto_ms = now_ms;
            if (wifi_app_is_connected()) {
                do_fetch();
            }
        }

        /* 任意横向滑动切页;仅主页下拉触发刷新。 */
        lv_indev_t *indev = lv_indev_get_next(NULL);
        if (indev) {
            lv_indev_state_t state = indev->proc.state;
            lv_point_t point;
            lv_indev_get_point(indev, &point);

            if (state == LV_INDEV_STATE_PR && !s_touch_pressed) {
                s_touch_pressed = true;
                s_touch_start = point;
            } else if (state == LV_INDEV_STATE_REL && s_touch_pressed) {
                s_touch_pressed = false;
                int dx = point.x - s_touch_start.x;
                int dy = point.y - s_touch_start.y;
                int abs_dx = dx < 0 ? -dx : dx;
                int abs_dy = dy < 0 ? -dy : dy;

                if (abs_dx >= 40 && abs_dx > abs_dy) {
                    usage_ui_switch_page(1 - usage_ui_current_page());
                } else if (usage_ui_current_page() == 0 && dy >= 40 && abs_dy > abs_dx
                           && (now_ms - last_touch_ms) >= TOUCH_DEBOUNCE_MS) {
                    last_touch_ms = now_ms;
                    refresh_from_touch();
                }
            }
        }
        (void)lcd_touch_activity_take();

        /* WiFi 断开后定时自动重连重试 */
        if (!wifi_app_is_connected() && (now_ms - last_try_ms) >= WIFI_RETRY_PERIOD_MS) {
            last_try_ms = now_ms;
            if (wifi_app_init(20000) == ESP_OK) {
                usage_ui_splash_status("数据获取中...");
                sntp_try_sync(SNTP_WAIT_MS);
                do_fetch();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));
    }
}