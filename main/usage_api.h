#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单档(rolling/weekly/monthly)用量 */
typedef struct {
    bool     valid;      /* 该档数据有效 */
    char     status[16]; /* ok / warn / ... */
    int      percent;    /* 已用百分比 0..100 */
    int64_t  resets_at_epoch; /* resetsAt 解析为 UTC epoch 秒;-1 表示无法解析 */
    int      resets_in;   /* 调用时刻距重置的剩余秒数;-1 表示未知 */
} usage_bucket_t;

typedef struct {
    usage_bucket_t rolling;
    usage_bucket_t weekly;
    usage_bucket_t monthly;
} usage_quota_t;

/**
 * @brief 调用 OpenCode Go usage 接口并解析(防御式:cJSON 逐层判空,
 *        兼容 usage. 前缀与裸字段;percent 兼容数字/字符串)。
 * @param out 输出解析结果(失败时各字段 valid=false)
 * @return ESP_OK 表示 HTTP 请求成功且 JSON 至少解析出一个有效档
 */
esp_err_t usage_api_fetch(usage_quota_t *out);

/** 内部暴露供调试:原始响应文本复制到 dst */
void usage_api_dump_raw(char *dst, size_t len);

/* ---- 用量分析(第二页) ---- */
typedef struct {
    bool   valid;          /* 该档数据有效 */
    int    idx;            /* 0=滚动 1=每周 2=每月 */
    bool   overspent;      /* 已用 ≥100%,额度已耗尽 */
    double remaining_usd;  /* 剩余美元额度 */
    bool   budget_per_day; /* true=日均预算(距重置≥24h); false=时均预算 */
    double budget_usd;     /* 整个周期的日均/时均额度; -1 未知 */
    double burn_usd_h;     /* 燃烧速率 美元/小时; -1 未知 */
    double reset_hours;    /* 距重置小时; -1 未知 */
    double cap_hours;      /* 按当前速率预计触顶小时; -1 未知 */
    int    verdict;        /* 0=安全 1=注意 2=危险 */
} usage_analysis_t;

typedef struct {
    usage_analysis_t row[3];
} usage_analysis_all_t;

/** 各档美元额度(滚动/每周/每月) */
double usage_api_limit(int idx);

/**
 * @brief 用量分析:日均预算 / 燃烧速率 / 撞限倒计时 / 三色判定。
 * @param q 最近一次 fetch 结果
 * @return 指向静态缓冲,有效期至下次调用。
 */
usage_analysis_all_t *usage_api_analyze(const usage_quota_t *q);

#ifdef __cplusplus
}
#endif