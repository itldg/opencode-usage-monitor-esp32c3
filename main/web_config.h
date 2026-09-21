#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动网页配置服务器(http://<设备IP>/)。
 * 密码由 Kconfig OPENCODE_WEB_PASSWORD 决定:留空不校验,非空需登录。
 * 保存配置后置位刷新请求,由主循环消费。
 */
esp_err_t web_config_start(void);

/** 读取并清除刷新请求标志(网页保存配置后为 true) */
bool web_config_take_refresh(void);

/** 当前 STA IP 字符串(静态缓冲,未连接时为 0.0.0.0) */
const char *web_config_ip_str(void);

#ifdef __cplusplus
}
#endif
