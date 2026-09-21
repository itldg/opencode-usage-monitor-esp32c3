#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_KEY_MAX_LEN    256    /* OpenCode API Key 上限 */
#define CFG_COOKIE_MAX_LEN 3072   /* 火山 Cookie 上限(精简后仍含 digest/userInfo JWT) */

/* 火山 Plan 查询类型:Cookie 为账号级登录态,需显式指定查询哪个 Plan */
enum {
    VOLC_PLAN_CODING = 0,   /* 仅 Coding Plan */
    VOLC_PLAN_AGENT  = 1,   /* 仅 Agent Plan */
    VOLC_PLAN_BOTH   = 2,   /* Coding + Agent */
};

/**
 * @brief 初始化配置存储(NVS 命名空间 "cfg")。
 * 数据源(OpenCode Key / 火山 Cookie)均由网页配置页写入,重启保留。
 */
esp_err_t config_store_init(void);

/** 返回静态缓冲,空字符串表示未配置。 */
const char *config_store_opencode_key(void);
const char *config_store_volc_cookie(void);

/** 是否启用(字符串非空)。网页保存空字符串即停用该数据源。 */
bool config_store_opencode_enabled(void);
bool config_store_volc_enabled(void);

/** 保存到 NVS(空字符串也会写入,用于停用),重启后保留。 */
esp_err_t config_store_set_opencode_key(const char *key);
esp_err_t config_store_set_volc_cookie(const char *cookie);

/**
 * @brief 校验火山 Cookie 是否含必要字段(登录凭证 digest/userInfo + csrfToken)。
 * 空 Cookie 视为合法(用于停用火山);失败时 err 返回缺失说明。
 */
bool config_store_volc_cookie_ok(const char *cookie, char *err, size_t err_len);

/** 火山 Plan 查询类型:VOLC_PLAN_CODING/AGENT/BOTH,默认 CODING */
int config_store_volc_plan(void);
esp_err_t config_store_set_volc_plan(int plan);

#ifdef __cplusplus
}
#endif
