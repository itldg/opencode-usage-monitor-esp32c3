#include "usage_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#define TAG "ui"

/* 自定义中文字体(16px,ASCII+常用汉字,由 lv_font_conv 生成) */
extern const lv_font_t lv_font_zh16;
extern const lv_font_t lv_font_zh40;

/* 布局常量 */
#define SCR_W        320
#define TITLE_H      38     /* 标题栏高度(单独背景色) */
#define TITLE_X      10
#define TITLE_Y      11
#define ROW_START_Y  53     /* 标题栏下方开始(内容顶距标题栏 15px) */
#define ROW_H        62     /* 三行垂直居中:三行内容(顶~重置底)+两条空隙占满 38~240,上下各留 15px */
#define NAME_X       8
#define BAR_X        48
#define BAR_W        210
#define BAR_H        16
#define RESET_Y_OFF  32
#define RIGHT_OFF    -8     /* 右侧留白,百分比/金额与之对齐 */
#define ANALYSIS_Y   56     /* 三行文字块高165,在标题栏(38)与屏底(240)间居中 */
#define ANALYSIS_H   63

/* 配色方案(深海军蓝 + 冰蓝/薄荷绿点缀,暗背景下高对比) */
#define COLOR_BG       0x0B1220   /* 背景:深海军蓝 */
#define COLOR_HEADER   0x16233D   /* 标题栏:略亮深蓝,与背景区分 */
#define COLOR_TITLE    0xFFFFFF   /* 标题:纯白 */
#define COLOR_NAME     0x8FCBFF   /* 行名:冰蓝 */
#define COLOR_PCT      0xFFFFFF   /* 百分比:纯白 */
#define COLOR_SUB      0x9DB2CC   /* 辅助信息:灰蓝(时间/重置) */
#define COLOR_AMOUNT   0x3EFFB0   /* 金额:薄荷绿 */
#define COLOR_TRACK    0x2E4058   /* 进度条轨道:调亮,未填充部分可见 */
#define COLOR_TRACK_B  0x44607E   /* 进度条轨道边框 */
#define COLOR_DIV      0x2A3A52   /* 行间分隔线 */
#define COLOR_ERR      0xFF5C5C   /* 错误:亮红 */

#define BAND_GREEN     0x00E585
#define BAND_ORANGE    0xFF9F0A
#define BAND_RED       0xFF3B30
#define COLOR_WARNING  0xFFB020

/* 汇率:1 美元 ≈ 7 元(可随行情调整) */
#define USD_CNY_RATE  7.0f

static lv_obj_t *s_bars[3];
static lv_obj_t *s_pct_labels[3];
static lv_obj_t *s_reset_labels[3];
static lv_obj_t *s_amount_labels[3];
static lv_obj_t *s_analysis_budget[3];
static lv_obj_t *s_analysis_burn[3];
static lv_obj_t *s_analysis_available[3];
static lv_obj_t *s_analysis_reset[3];
static lv_obj_t *s_analysis_verdict[3];
static lv_obj_t *s_title;
static lv_obj_t *s_time;
static lv_obj_t *s_err;
static lv_obj_t *s_splash;
static lv_obj_t *s_splash_status;
static lv_obj_t *s_refresh_overlay;
static lv_obj_t *s_page0;
static lv_obj_t *s_page1;

static int      s_resets_in[3];
static bool     s_resets_known[3];
static uint64_t s_fetch_ms;
static usage_quota_t s_quota_snapshot;
static int s_current_page;

static const char *s_names[3] = { "滚动", "每周", "每月" };

static lv_color_t band_color(int percent)
{
    if (percent < 50) return lv_color_hex(BAND_GREEN);
    if (percent <= 90) return lv_color_hex(BAND_ORANGE);
    return lv_color_hex(BAND_RED);
}

/* 重置倒计时文本:≥1天 "重置于 X 天 Y 小时",≥1时 "重置于 X 小时 Y 分钟",否则 "重置于 X 分钟" */
static void fmt_reset(char *buf, size_t len, int secs, bool known)
{
    if (!known) { snprintf(buf, len, "重置于 --"); return; }
    if (secs <= 0) { snprintf(buf, len, "即将重置"); return; }
    int d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    if (d > 0)      snprintf(buf, len, "重置于 %d 天 %d 小时", d, h);
    else if (h > 0) snprintf(buf, len, "重置于 %d 小时 %d 分钟", h, m);
    else            snprintf(buf, len, "重置于 %d 分钟", m);
}

/* 已用金额:美元额度 × 百分比 × 汇率,人民币展示 */
static void fmt_amount(char *buf, size_t len, int idx, int pct)
{
    if (pct < 0) { snprintf(buf, len, "¥--"); return; }
    double cny = usage_api_limit(idx) * (pct / 100.0) * USD_CNY_RATE;
    if (cny >= 100.0) snprintf(buf, len, "¥%.0f", cny);
    else              snprintf(buf, len, "¥%.1f", cny);
}

static void fmt_short_duration(char *buf, size_t len, int hours)
{
    if (hours >= 24) snprintf(buf, len, " %d 天", hours / 24);
    else             snprintf(buf, len, " %d 小时", hours);
}

static void make_divider(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, w, 1);
    lv_obj_set_pos(div, x, y);
    lv_obj_set_style_bg_color(div, lv_color_hex(COLOR_DIV), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_pad_all(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
}

static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, SCR_W, 240);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    return page;
}

static void make_row(int idx, int y, bool draw_divider)
{
    /* 行间分隔线(浅横线,位于本行底部) */
    if (draw_divider) {
        make_divider(s_page0, NAME_X, y + ROW_H - 7, SCR_W - 2 * NAME_X);  /* 本行内容底(y+48)与下一行顶(y+62)正中 */
    }

    /* 名称标签 */
    lv_obj_t *lbl = lv_label_create(s_page0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COLOR_NAME), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_zh16, 0);
    lv_obj_set_pos(lbl, NAME_X, y);
    lv_label_set_text(lbl, s_names[idx]);

    /* 进度条 */
    lv_obj_t *bar = lv_bar_create(s_page0);
    lv_obj_set_size(bar, BAR_W, BAR_H);
    lv_obj_set_pos(bar, BAR_X, y + 2);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(COLOR_TRACK_B), 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_bg_color(bar, band_color(0), LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    s_bars[idx] = bar;

    /* 百分比标签(右对齐屏幕右缘,贴近进度条) */
    lv_obj_t *pct = lv_label_create(s_page0);
    lv_obj_set_style_text_color(pct, lv_color_hex(COLOR_PCT), 0);
    lv_obj_set_style_text_font(pct, &lv_font_zh16, 0);
    lv_obj_align(pct, LV_ALIGN_TOP_RIGHT, RIGHT_OFF, y);
    lv_label_set_text(pct, "--%");
    s_pct_labels[idx] = pct;

    /* 重置倒计时(进度条下端左侧) */
    lv_obj_t *rst = lv_label_create(s_page0);
    lv_obj_set_style_text_color(rst, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(rst, &lv_font_zh16, 0);
    lv_obj_set_pos(rst, BAR_X, y + RESET_Y_OFF);
    lv_label_set_text(rst, "重置于 --");
    s_reset_labels[idx] = rst;

    /* 已用金额(进度条下端右侧,¥人民币) */
    lv_obj_t *amt = lv_label_create(s_page0);
    lv_obj_set_style_text_color(amt, lv_color_hex(COLOR_AMOUNT), 0);
    lv_obj_set_style_text_font(amt, &lv_font_zh16, 0);
    lv_obj_align(amt, LV_ALIGN_TOP_RIGHT, RIGHT_OFF, y + RESET_Y_OFF);
    lv_label_set_text(amt, "¥--");
    s_amount_labels[idx] = amt;
}

static void make_header(lv_obj_t *page, const char *title, lv_obj_t **title_label)
{
    lv_obj_t *hdr = lv_obj_create(page);
    lv_obj_set_size(hdr, SCR_W, TITLE_H);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(COLOR_HEADER), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);

    *title_label = lv_label_create(page);
    lv_obj_set_style_text_color(*title_label, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(*title_label, &lv_font_zh16, 0);
    lv_obj_set_pos(*title_label, TITLE_X, TITLE_Y);
    lv_label_set_text(*title_label, title);
}

static void make_analysis_row(int idx, int y)
{
    if (idx < 2) {
        /* 分割线位于本行 burn 底(y+39)与下一行 budget 顶(y+63)的正中间 */
        make_divider(s_page1, 8, y + ANALYSIS_H - 12, SCR_W - 16);
    }

    lv_obj_t *budget = lv_label_create(s_page1);
    lv_obj_set_style_text_color(budget, lv_color_hex(COLOR_NAME), 0);
    lv_obj_set_style_text_font(budget, &lv_font_zh16, 0);
    lv_obj_set_pos(budget, 8, y);
    lv_label_set_text(budget, "滚动 日均 ¥ --");
    s_analysis_budget[idx] = budget;

    lv_obj_t *burn = lv_label_create(s_page1);
    lv_obj_set_style_text_color(burn, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(burn, &lv_font_zh16, 0);
    lv_obj_set_pos(burn, 8, y + 23);
    lv_label_set_text(burn, "燃烧 ¥ -- /时");
    s_analysis_burn[idx] = burn;

    lv_obj_t *available = lv_label_create(s_page1);
    lv_obj_set_style_text_color(available, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(available, &lv_font_zh16, 0);
    lv_obj_align(available, LV_ALIGN_TOP_RIGHT, -8, y);
    lv_label_set_text(available, "可用 --");
    s_analysis_available[idx] = available;

    lv_obj_t *reset = lv_label_create(s_page1);
    lv_obj_set_style_text_color(reset, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(reset, &lv_font_zh16, 0);
    lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, -8, y + 23);
    lv_label_set_text(reset, "距重置 --");
    s_analysis_reset[idx] = reset;

    lv_obj_t *verdict = lv_label_create(s_page1);
    lv_obj_set_style_text_color(verdict, lv_color_hex(BAND_GREEN), 0);
    lv_obj_set_style_text_font(verdict, &lv_font_zh16, 0);
    lv_obj_align(verdict, LV_ALIGN_TOP_RIGHT, -104, y);
    lv_label_set_text(verdict, "余量充足");
    s_analysis_verdict[idx] = verdict;
}

static void update_analysis_labels(void)
{
    usage_analysis_all_t *all = usage_api_analyze(&s_quota_snapshot);
    for (int i = 0; i < 3; i++) {
        const usage_analysis_t *a = &all->row[i];
        char budget[48];
        char burn[64];
        char capbuf[16];
        char resetbuf[16];
        char available[32];
        char reset_text[32];
        const char *name = s_names[i];

        if (!a->valid) {
            snprintf(budget, sizeof(budget), "%s 预算 ¥ --", name);
        } else {
            double value = (a->budget_per_day ? a->budget_usd * 24.0 : a->budget_usd) * USD_CNY_RATE;
            snprintf(budget, sizeof(budget), "%s %s ¥ %.1f", name,
                     a->budget_per_day ? "日均" : "时均", value);
        }
        lv_label_set_text(s_analysis_budget[i], budget);

        if (!a->valid || a->reset_hours < 0) {
            snprintf(burn, sizeof(burn), i == 0 ? "燃烧 ¥ -- /时" : "燃烧 ¥ -- /天");
            snprintf(available, sizeof(available), "可用 --");
            snprintf(reset_text, sizeof(reset_text), "距重置 --");
        } else {
            int reset = (int)(a->reset_hours + 0.5);
            fmt_short_duration(resetbuf, sizeof(resetbuf), reset);
            if (a->burn_usd_h >= 0 && a->cap_hours >= 0) {
                int cap = (int)(a->cap_hours + 0.5);
                fmt_short_duration(capbuf, sizeof(capbuf), cap);
                double burn_cny = a->burn_usd_h * USD_CNY_RATE;
                snprintf(burn, sizeof(burn), i == 0 ? "燃烧 ¥ %.1f /时" : "燃烧 ¥ %.1f /天",
                         i == 0 ? burn_cny : burn_cny * 24.0);
                snprintf(available, sizeof(available), "可用%s", capbuf);
            } else {
                snprintf(burn, sizeof(burn), i == 0 ? "燃烧 ¥ -- /时" : "燃烧 ¥ -- /天");
                snprintf(available, sizeof(available), "可用 --");
            }
            snprintf(reset_text, sizeof(reset_text), "距重置%s", resetbuf);
        }
        lv_label_set_text(s_analysis_burn[i], burn);
        lv_label_set_text(s_analysis_available[i], available);
        lv_label_set_text(s_analysis_reset[i], reset_text);
        lv_obj_align(s_analysis_available[i], LV_ALIGN_TOP_RIGHT, -8, ANALYSIS_Y + i * ANALYSIS_H);
        lv_obj_align(s_analysis_reset[i], LV_ALIGN_TOP_RIGHT, -8, ANALYSIS_Y + i * ANALYSIS_H + 23);

        const char *text = !a->valid ? "未知" : (a->overspent ? "已超支"
                           : (a->verdict == 2 ? "即将超限"
                           : (a->verdict == 1 ? "勉强够用" : "余量充足")));
        lv_color_t color = !a->valid ? lv_color_hex(COLOR_DIV)
                         : (a->verdict == 2 ? lv_color_hex(BAND_RED)
                         : (a->verdict == 1 ? lv_color_hex(COLOR_WARNING)
                                            : lv_color_hex(BAND_GREEN)));
        lv_obj_set_style_text_color(s_analysis_verdict[i], color, 0);
        lv_label_set_text(s_analysis_verdict[i], text);
        lv_obj_align(s_analysis_verdict[i], LV_ALIGN_TOP_RIGHT, -104, ANALYSIS_Y + i * ANALYSIS_H);
    }
}

void usage_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* 背景 */
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    s_page0 = make_page(scr);
    s_page1 = make_page(scr);
    lv_obj_set_pos(s_page1, SCR_W, 0);

    make_header(s_page0, "OpenCode Go 用量", &s_title);

    /* 右上角更新时间 */
    s_time = lv_label_create(s_page0);
    lv_obj_set_style_text_color(s_time, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_time, &lv_font_zh16, 0);
    lv_obj_align(s_time, LV_ALIGN_TOP_RIGHT, -10, TITLE_Y);
    lv_label_set_text(s_time, "更新于 --:--");

    /* 错误提示(标题栏右侧,与标题不重叠) */
    s_err = lv_label_create(s_page0);
    lv_obj_set_style_text_color(s_err, lv_color_hex(COLOR_ERR), 0);
    lv_obj_set_style_text_font(s_err, &lv_font_zh16, 0);
    lv_obj_set_pos(s_err, 150, TITLE_Y);
    lv_label_set_text(s_err, "");

    /* 三行(前两行底部画分隔线,最后一行不画) */
    for (int i = 0; i < 3; i++) {
        make_row(i, ROW_START_Y + i * ROW_H, i < 2);
        s_resets_in[i] = -1;
        s_resets_known[i] = false;
    }

    lv_obj_t *analysis_title;
    make_header(s_page1, "用量分析", &analysis_title);
    for (int i = 0; i < 3; i++) make_analysis_row(i, ANALYSIS_Y + i * ANALYSIS_H);

    s_current_page = 0;

    /* 开机画面:覆盖主界面,大标题 "IT老大哥" + 状态小字,首次数据到达后隐藏 */
    s_splash = make_page(scr);

    lv_obj_t *big = lv_label_create(s_splash);
    lv_obj_set_style_text_color(big, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(big, &lv_font_zh40, 0);
    lv_obj_align(big, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(big, "IT老大哥");

    s_splash_status = lv_label_create(s_splash);
    lv_obj_set_style_text_color(s_splash_status, lv_color_hex(COLOR_SUB), 0);
    lv_obj_set_style_text_font(s_splash_status, &lv_font_zh16, 0);
    lv_obj_align(s_splash_status, LV_ALIGN_CENTER, 0, 48);
    lv_label_set_text(s_splash_status, "连接中...");

    ESP_LOGI(TAG, "UI created (zh font)");
}

void usage_ui_update(const usage_quota_t *quota, uint64_t now_uptime_ms)
{
    const usage_bucket_t *buckets[3] = { &quota->rolling, &quota->weekly, &quota->monthly };

    if (s_splash) {           /* 首次成功获取数据,关闭开机画面 */
        lv_obj_del(s_splash);
        s_splash = NULL;
        s_splash_status = NULL;
    }

    s_fetch_ms = now_uptime_ms;
    memcpy(&s_quota_snapshot, quota, sizeof(s_quota_snapshot));
    for (int i = 0; i < 3; i++) {
        const usage_bucket_t *b = buckets[i];
        char buf[16];
        int pct = b->valid ? b->percent : -1;
        if (pct < 0) {
            snprintf(buf, sizeof(buf), "--%%");
            lv_bar_set_value(s_bars[i], 0, LV_ANIM_OFF);
        } else {
            if (pct > 100) pct = 100;
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_obj_set_style_bg_color(s_bars[i], band_color(pct), LV_PART_INDICATOR);
            lv_bar_set_value(s_bars[i], pct, LV_ANIM_ON);
        }
        lv_label_set_text(s_pct_labels[i], buf);

        s_resets_in[i] = b->resets_in;
        s_resets_known[i] = b->valid && b->resets_in >= 0;
        char rbuf[40];
        fmt_reset(rbuf, sizeof(rbuf), s_resets_in[i], s_resets_known[i]);
        lv_label_set_text(s_reset_labels[i], rbuf);

        char abuf[24];
        fmt_amount(abuf, sizeof(abuf), i, pct);
        lv_label_set_text(s_amount_labels[i], abuf);
    }

    update_analysis_labels();
}

void usage_ui_tick(uint64_t now_uptime_ms)
{
    uint64_t elapsed = (now_uptime_ms >= s_fetch_ms) ? (now_uptime_ms - s_fetch_ms) : 0;
    for (int i = 0; i < 3; i++) {
        if (!s_resets_known[i]) continue;
        int64_t remain = (int64_t)s_resets_in[i] - (int64_t)(elapsed / 1000);
        if (remain < 0) remain = 0;
        char rbuf[40];
        fmt_reset(rbuf, sizeof(rbuf), (int)remain, true);
        lv_label_set_text(s_reset_labels[i], rbuf);

        usage_bucket_t *snapshot = i == 0 ? &s_quota_snapshot.rolling
                                  : (i == 1 ? &s_quota_snapshot.weekly : &s_quota_snapshot.monthly);
        snapshot->resets_in = (int)remain;
    }
    update_analysis_labels();
}

void usage_ui_set_time(const char *text)
{
    if (s_time) {
        lv_label_set_text(s_time, text ? text : "更新于 --:--");
    }
}

void usage_ui_splash_status(const char *text)
{
    if (s_splash && s_splash_status) {
        lv_label_set_text(s_splash_status, text ? text : "");
        lv_refr_now(NULL);
    }
}

void usage_ui_set_error(const char *text)
{
    /* 开机画面期间:错误也显示在状态行,否则用户看不到(被遮挡) */
    if (s_splash && s_splash_status) {
        lv_label_set_text(s_splash_status, text ? text : "连接中...");
    }
    if (s_err) {
        lv_label_set_text(s_err, text ? text : "");
    }
}

void usage_ui_refresh_begin(void)
{
    if (s_refresh_overlay) return;

    s_refresh_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_refresh_overlay, SCR_W, 240);
    lv_obj_set_pos(s_refresh_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_refresh_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_refresh_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_refresh_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_refresh_overlay, 0, 0);
    lv_obj_set_style_radius(s_refresh_overlay, 0, 0);
    lv_obj_clear_flag(s_refresh_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(s_refresh_overlay, 900, 70);
    lv_obj_set_size(spinner, 42, 42);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -15);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_AMOUNT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(s_refresh_overlay);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(label, &lv_font_zh16, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 30);
    lv_label_set_text(label, "刷新中...");

    lv_refr_now(NULL);
}

void usage_ui_refresh_end(void)
{
    if (!s_refresh_overlay) return;
    lv_obj_del(s_refresh_overlay);
    s_refresh_overlay = NULL;
}

void usage_ui_switch_page(int page)
{
    if (page < 0 || page > 1 || page == s_current_page || !s_page0 || !s_page1) return;

    s_current_page = page;
    lv_obj_set_x(s_page0, page == 0 ? 0 : -SCR_W);
    lv_obj_set_x(s_page1, page == 0 ? SCR_W : 0);
}

int usage_ui_current_page(void)
{
    return s_current_page;
}
