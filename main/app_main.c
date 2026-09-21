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

#include "audio_player.h"
#include "config_store.h"
#include "lcd_init.h"
#include "usage_api.h"
#include "usage_ui.h"
#include "volc_usage_api.h"
#include "web_config.h"
#include "wifi_app.h"

#define TAG "main"

#define LOOP_DELAY_MS          5
#define TOUCH_DEBOUNCE_MS      5000   /* 下拉刷新防抖 */
#define SWITCH_DEBOUNCE_MS     1000   /* 上滑切换数据源防抖 */
#define WIFI_RETRY_PERIOD_MS   60000  /* WiFi 未连接时的自动重试周期 */

static usage_quota_t s_quota;
static volc_quota_t  s_volc_coding;
static volc_quota_t  s_volc_agent;
static bool s_time_synced = false;
static bool s_sntp_initialized = false;
static bool s_time_ui_updated = false;
static bool s_touch_pressed = false;
static lv_point_t s_touch_start;

/* SNTP 后台同步成功回调(置位后由主循环补显示时间) */
static void sntp_sync_cb(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP time synced (cb)");
}

/* 确保 SNTP 已在后台运行;不阻塞等待,同步完成由 sync_cb 通知 */
static void sntp_ensure(void)
{
    if (s_sntp_initialized) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = sntp_sync_cb;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_initialized = true;
    } else {
        ESP_LOGE(TAG, "SNTP init failed");
    }
}

/* 按当前时间刷新"更新于"标签(SNTP 未同步时显示 --:--) */
static void update_time_label(void)
{
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
}

static esp_err_t do_fetch(void)
{
    /* 时间未同步时确保 SNTP 已在后台运行 */
    sntp_ensure();

    /* HTTPS 证书校验需要正确时间:冷启动时钟为 1970,SNTP 未同步时
     * TLS 握手会因证书有效期校验失败;先等同步(最多 ~10s)再拉取。 */
    if (time(NULL) <= 1000000000LL) {
        for (int i = 0; i < 50 && !s_time_synced; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    uint64_t now_ms = esp_timer_get_time() / 1000;
    bool any = false;

    if (config_store_opencode_enabled()) {
        if (usage_api_fetch(&s_quota) == ESP_OK) {
            any = true;
            audio_player_report(&s_quota); /* 用量档位播报(30/50/.../100%) + 窗口重置提示 */
            usage_ui_update(&s_quota, now_ms);
        } else {
            ESP_LOGW(TAG, "opencode fetch failed");
            usage_ui_set_error("获取失败");
        }
    }

    if (config_store_volc_enabled()) {
        int plan = config_store_volc_plan();
        if (plan == VOLC_PLAN_CODING || plan == VOLC_PLAN_BOTH) {
            if (volc_api_fetch(&s_volc_coding) == ESP_OK) {
                any = true;
                audio_player_report_volc(&s_volc_coding, 0); /* Coding 档位播报 */
                usage_ui_update_volc(UI_SRC_VOLC, &s_volc_coding, now_ms);
            } else {
                ESP_LOGW(TAG, "volc coding fetch failed");
                if (!any) usage_ui_set_error("Volc失败");
            }
        }
        if (plan == VOLC_PLAN_AGENT || plan == VOLC_PLAN_BOTH) {
            if (volc_api_fetch_agent(&s_volc_agent) == ESP_OK) {
                any = true;
                audio_player_report_volc(&s_volc_agent, 1); /* Agent 档位播报 */
                usage_ui_update_volc(UI_SRC_VOLC_AGENT, &s_volc_agent, now_ms);
            } else {
                ESP_LOGW(TAG, "volc agent fetch failed");
                if (!any) usage_ui_set_error("Agent失败");
            }
        }
    }

    if (any) {
        usage_ui_set_error(NULL);
        update_time_label();
        ESP_LOGI(TAG, "UI refreshed");
    }
    return any ? ESP_OK : ESP_FAIL;
}

/* 连接 WiFi(若未连)后拉取一次用量 */
static void fetch_ensure_connected(void)
{
    usage_ui_splash_status("数据获取中...");
    if (!wifi_app_is_connected() && wifi_app_init(20000) != ESP_OK) {
        usage_ui_set_error("连接失败");
        return;
    }
    sntp_ensure();
    do_fetch();
}

static void refresh_from_touch(void)
{
    usage_ui_refresh_begin();
    fetch_ensure_connected();
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

    config_store_init();

    if (strlen(CONFIG_OPENCODE_WIFI_SSID) == 0) {
        ESP_LOGW(TAG, "WiFi SSID is empty! Run menuconfig -> OpenCode Go Usage Display");
    }
    if (!config_store_opencode_enabled() && !config_store_volc_enabled()) {
        ESP_LOGW(TAG, "no data source configured! Configure via http://<IP>/");
    }

    ESP_ERROR_CHECK(lcd_init());
    usage_ui_create();
    usage_ui_splash_status("WIFI连接中...");

    web_config_start(); /* 网页配置服务器(http://<设备IP>/),连上 WiFi 即可访问 */

    if (audio_player_init() == ESP_OK) {
        audio_player_play(AUDIO_BOOT); /* 开机提示音 */
    }

    /* 默认数据源:未配置 OpenCode 时,按 Plan 类型选火山 Coding/Agent */
    if (!config_store_opencode_enabled() && config_store_volc_enabled()) {
        usage_ui_set_source(config_store_volc_plan() == VOLC_PLAN_AGENT
                                ? UI_SRC_VOLC_AGENT : UI_SRC_VOLC);
    }

    esp_err_t werr = wifi_app_init(30000 + CONFIG_OPENCODE_WIFI_RETRY_COUNT * 3000);
    if (werr != ESP_OK) {
        usage_ui_set_error("连接失败");
    } else {
        usage_ui_splash_status("数据获取中...");
        sntp_ensure(); /* SNTP 后台同步,时间就绪后由主循环补显 */
        do_fetch();
        /* 未配置任何数据源:提示访问网页配置页 */
        if (!config_store_opencode_enabled() && !config_store_volc_enabled()) {
            char hint[48];
            snprintf(hint, sizeof(hint), "Config at http://%s/", web_config_ip_str());
            usage_ui_splash_status(hint);
        }
    }

    uint64_t refresh_ms = (uint64_t)CONFIG_OPENCODE_REFRESH_MINUTES * 60 * 1000;
    uint64_t last_auto_ms = 0;
    uint64_t last_touch_ms = 0;
    uint64_t last_switch_ms = 0;
    uint64_t last_try_ms = 0;
    uint64_t last_tick_ms = 0;

    while (1) {
        lv_timer_handler();
        uint64_t now_ms = esp_timer_get_time() / 1000;

        /* 每秒刷新倒计时与更新时间 */
        if ((now_ms - last_tick_ms) >= 1000) {
            last_tick_ms = now_ms;
            usage_ui_tick(now_ms);

            /* SNTP 后台同步成功后,补一次时间显示(首次启动不再阻塞等待) */
            if (s_time_synced && !s_time_ui_updated) {
                s_time_ui_updated = true;
                update_time_label();
            }
        }

        /* 自动定时刷新(CONFIG_OPENCODE_REFRESH_MINUTES,0 表示关闭) */
        if (refresh_ms > 0 && (now_ms - last_auto_ms) >= refresh_ms) {
            last_auto_ms = now_ms;
            if (wifi_app_is_connected()) {
                do_fetch();
            }
        }

        /* 网页保存配置后立即刷新一次用量 */
        if (web_config_take_refresh()) {
            fetch_ensure_connected();
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
                } else if (usage_ui_current_page() == 0 && abs_dy > abs_dx && abs_dy >= 40) {
                    if (dy < 0) {
                        /* 上滑:循环切换数据源(Plan),防抖 1s */
                        if ((now_ms - last_switch_ms) >= SWITCH_DEBOUNCE_MS) {
                            last_switch_ms = now_ms;
                            usage_ui_cycle_source();
                        }
                    } else if ((now_ms - last_touch_ms) >= TOUCH_DEBOUNCE_MS) {
                        /* 下拉:刷新 */
                        last_touch_ms = now_ms;
                        refresh_from_touch();
                    }
                } else if (abs_dx < 40 && abs_dy < 40) {
                    /* 轻点:命中金额标签则切换货币 */
                    usage_ui_handle_tap(point.x, point.y);
                }
            }
        }

        /* WiFi 断开后定时自动重连重试 */
        if (!wifi_app_is_connected() && (now_ms - last_try_ms) >= WIFI_RETRY_PERIOD_MS) {
            last_try_ms = now_ms;
            fetch_ensure_connected();
        }

        vTaskDelay(pdMS_TO_TICKS(LOOP_DELAY_MS));
    }
}