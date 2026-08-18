#ifndef WIFI_APP_H
#define WIFI_APP_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Blocking connect to WIFI_SSID. Uses OpenCode wifi_ssid/password Kconfig.
 * @param timeout_ms  how long to wait for an IP before giving up
 * @return ESP_OK on connected + got IP
 */
esp_err_t wifi_app_init(int timeout_ms);

/** @brief 是否已连接到 AP 并取得 IP(线程安全读位图) */
bool wifi_app_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_APP_H */