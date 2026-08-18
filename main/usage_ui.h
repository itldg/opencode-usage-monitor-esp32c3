#pragma once

#include <stdint.h>

#include "usage_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 UI(背景、左上角标题、右上角更新时间、三档进度条与重置倒计时)。
 *        需在 lv_init 之后调用。
 */
void usage_ui_create(void);

/**
 * @brief 用最新数据刷新三档进度条与百分比,并记录重置倒计时快照。
 * @param quota        解析结果
 * @param now_uptime_ms 当前 esp_timer 毫秒(用于倒计时基准)
 */
void usage_ui_update(const usage_quota_t *quota, uint64_t now_uptime_ms);

/**
 * @brief 主循环每秒调用,刷新各档"重置倒计时"文本。
 * @param now_uptime_ms 当前 esp_timer 毫秒
 */
void usage_ui_tick(uint64_t now_uptime_ms);

/**
 * @brief 设置右上角更新时间文本(如 "更新于 12:30");传 NULL 显示 "更新于 --:--"。
 */
void usage_ui_set_time(const char *text);

/**
 * @brief 设置开机画面状态小字(如 "WIFI连接中..."、"数据获取中...")。
 *        开机画面在首次成功更新数据后自动消失。
 */
void usage_ui_splash_status(const char *text);

/**
 * @brief 设置错误提示(红色,右上角时间下方);传 NULL 清除。
 */
void usage_ui_set_error(const char *text);

/** 显示或关闭下拉刷新遮罩。 */
void usage_ui_refresh_begin(void);
void usage_ui_refresh_end(void);

/**
 * @brief 切换页面:0=用量总览 1=用量分析。
 */
void usage_ui_switch_page(int page);

/** 当前页:0=用量总览 1=用量分析 */
int usage_ui_current_page(void);

#ifdef __cplusplus
}
#endif