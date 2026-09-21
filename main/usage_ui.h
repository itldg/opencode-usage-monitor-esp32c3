#pragma once

#include <stdint.h>

#include "usage_api.h"
#include "volc_usage_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 数据源:OpenCode Go / 火山 Coding Plan / 火山 Agent Plan */
enum {
    UI_SRC_OPENCODE = 0,
    UI_SRC_VOLC = 1,        /* 火山 Coding Plan */
    UI_SRC_VOLC_AGENT = 2,  /* 火山 Agent Plan */
};

/**
 * @brief 创建 UI(背景、左上角标题、右上角更新时间、三档进度条与重置倒计时)。
 *       需在 lv_init 之后调用。
 */
void usage_ui_create(void);

/**
 * @brief 用最新 OpenCode 数据刷新三档进度条与百分比,并记录重置倒计时快照。
 * @param quota        解析结果
 * @param now_uptime_ms 当前 esp_timer 毫秒(用于倒计时基准)
 */
void usage_ui_update(const usage_quota_t *quota, uint64_t now_uptime_ms);

/**
 * @brief 用最新火山数据刷新(仅百分比+重置倒计时,无金额)。
 * @param src           UI_SRC_VOLC 或 UI_SRC_VOLC_AGENT
 * @param quota         解析结果
 * @param now_uptime_ms 当前 esp_timer 毫秒
 */
void usage_ui_update_volc(int src, const volc_quota_t *quota, uint64_t now_uptime_ms);

/**
 * @brief 切换显示的数据源(0=OpenCode 1=火山),并刷新界面。
 */
void usage_ui_set_source(int src);

/** 循环切换到下一个启用的数据源(无其它启用源时不变) */
void usage_ui_cycle_source(void);
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

/** 点击金额标签:切换人民币/美元显示 */
void usage_ui_handle_tap(int x, int y);

#ifdef __cplusplus
}
#endif