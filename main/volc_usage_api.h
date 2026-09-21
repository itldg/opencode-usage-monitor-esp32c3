#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 火山 Coding Plan 单档(session/weekly/monthly)用量 */
typedef struct {
    bool    valid;            /* 该档数据有效 */
    int     percent;          /* 已用百分比 0..100 */
    int64_t resets_at_epoch;  /* 重置时间 UTC epoch 秒;-1 未知 */
    int64_t subscribe_at_epoch; /* 窗口起点 epoch 秒(Agent 响应带 SubscribeTime);-1 未知 */
    int     resets_in;        /* 调用时刻距重置剩余秒数;-1 未知(时间未同步) */
} volc_bucket_t;

typedef struct {
    volc_bucket_t session;
    volc_bucket_t weekly;
    volc_bucket_t monthly;
} volc_quota_t;

/**
 * @brief 调用火山 GetCodingPlanUsage 接口并解析(依赖 config_store 中的 Cookie)。
 * 火山无金额换算数据,只取百分比与重置时间。
 * @param out 输出解析结果(失败时各字段 valid=false)
 * @return ESP_OK 表示 HTTP 成功且至少解析出一个有效档
 */
esp_err_t volc_api_fetch(volc_quota_t *out);

/**
 * @brief 调用 GetAgentPlanAFPUsage 接口解析 Agent Plan 用量。
 * 响应为 Result.AFPFiveHour/AFPWeekly/AFPMonthly/AFPDaily 的 {Quota, Used, ResetTime},
 * 换算为百分比;5小时窗口映射到 session 槽,日窗口暂不展示。
 */
esp_err_t volc_api_fetch_agent(volc_quota_t *out);

#ifdef __cplusplus
}
#endif
