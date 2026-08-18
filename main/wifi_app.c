#include "wifi_app.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define TAG "wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_event_group;
    static bool s_initialized = false;
    static int s_retry_num = 0;

    static void wifi_event_handler(void *arg, esp_event_base_t base,
                                   int32_t event_id, void *event_data)
    {
        (void)arg;
        (void)event_data;

        if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_retry_num < CONFIG_OPENCODE_WIFI_RETRY_COUNT) {
                s_retry_num++;
                ESP_LOGW(TAG, "retry %d/%d", s_retry_num, CONFIG_OPENCODE_WIFI_RETRY_COUNT);
                esp_wifi_connect();
            } else {
                ESP_LOGE(TAG, "wifi connect failed after %d retries", CONFIG_OPENCODE_WIFI_RETRY_COUNT);
                xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            }
        } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "connected, ip=" IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        }
    }

    /* 幂等:底层 netif/event_loop/wifi 仅初始化一次,
     * 因为重复调用会返回 ESP_ERR_INVALID_STATE 并触发 ESP_ERROR_CHECK。 */
    static void wifi_ensure_initialized(void)
    {
        if (s_initialized) {
            return;
        }

        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(err);
        }

        s_event_group = xEventGroupCreate();

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL));

        s_initialized = true;
}

    esp_err_t wifi_app_init(int timeout_ms)
    {
        wifi_ensure_initialized();

        wifi_config_t wcfg = {0};
        snprintf((char *)wcfg.sta.ssid, sizeof(wcfg.sta.ssid), "%s", CONFIG_OPENCODE_WIFI_SSID);
        snprintf((char *)wcfg.sta.password, sizeof(wcfg.sta.password), "%s", CONFIG_OPENCODE_WIFI_PASSWORD);
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        /* 每次调用都重新开始连接(停止当前状态,清事件位) */
        s_retry_num = 0;
        xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        esp_wifi_stop(); /* 未启动时返回 ESP_ERR_INVALID_STATE,忽略 */

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (bits & WIFI_CONNECTED_BIT) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "wifi connect timeout");
        return ESP_FAIL;
    }

    bool wifi_app_is_connected(void)
    {
        if (s_event_group) {
            return (xEventGroupGetBits(s_event_group) & WIFI_CONNECTED_BIT) != 0;
        }
        return false;
    }