#include "usage_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "usage"

#define USAGE_URL "https://opencode.ai/zen/go/v1/usage"
#define HTTP_BUF_SIZE 8192

static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len = 0;

/* 各档美元额度与窗口时长(小时):滚动5h/每周168h/每月720h */
static const double s_limits[3]    = { 12.0, 30.0, 60.0 };
static const double s_windows_h[3] = { 5.0, 168.0, 720.0 };

static usage_analysis_all_t s_analysis;

double usage_api_limit(int idx)
{
    if (idx < 0 || idx > 2) return 0.0;
    return s_limits[idx];
}

usage_analysis_all_t *usage_api_analyze(const usage_quota_t *q)
{
    const usage_bucket_t *b[3] = { &q->rolling, &q->weekly, &q->monthly };
    for (int i = 0; i < 3; i++) {
        usage_analysis_t *a = &s_analysis.row[i];
        memset(a, 0, sizeof(*a));
        a->idx = i;
        a->budget_usd = -1;
        a->burn_usd_h = -1;
        a->reset_hours = -1;
        a->cap_hours = -1;
        a->verdict = 0;

        const usage_bucket_t *bk = b[i];
        if (!bk->valid) continue;
        a->valid = true;

        int p = bk->percent;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        double used = s_limits[i] * p / 100.0;
        a->remaining_usd = s_limits[i] - used;
        a->overspent = (p >= 100);

        if (bk->resets_in >= 0) {
            double rh = bk->resets_in / 3600.0;
            a->reset_hours = rh;
        }
        a->budget_per_day = (i != 0);
        a->budget_usd = s_limits[i] / s_windows_h[i];

        /* 按本轮套餐已过去的时间，计算平均燃烧速率。 */
        double burn = -1;
        if (a->reset_hours >= 0) {
            double elapsed = s_windows_h[i] - a->reset_hours;
            if (p == 0) burn = 0;
            else if (elapsed > 0 && used > 0) burn = used / elapsed;
        }
        a->burn_usd_h = burn;
        if (a->remaining_usd <= 0) {
            a->cap_hours = 0;
        } else if (burn == 0 && a->reset_hours >= 0) {
            a->cap_hours = a->reset_hours;
        } else if (burn > 1e-5) {
            a->cap_hours = a->remaining_usd / burn;
        }

        /* 三色判定:0安全 1注意 2危险 */
        if (p >= 100)                                  a->verdict = 2;
        else if (a->cap_hours >= 0 && a->reset_hours >= 0
                 && a->cap_hours < a->reset_hours)     a->verdict = 2; /* 会先撞限 */
        else if (p >= 90)                              a->verdict = 2;
        else if (p >= 50)                              a->verdict = 1;
        else                                           a->verdict = 0;
    }
    return &s_analysis;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_http_len + evt->data_len < (int)sizeof(s_http_buf)) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "http response too large, truncated");
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ISO-8601 "2026-08-18T12:00:00.000Z" → UTC epoch 秒;失败返回 -1 */
static int64_t parse_iso8601_epoch(const char *s)
{
    if (!s || !*s) return -1;

    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    int n = sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    if (n < 6 || mo < 1 || mo > 12 || d < 1 || d > 31) return -1;

    /* Howard Hinnant days_from_civil */
    long long days;
    {
        int yy = y - (mo <= 2);
        int era = (yy >= 0 ? yy : yy - 399) / 400;
        unsigned yoe = (unsigned)(yy - era * 400);      /* [0, 399] */
        unsigned mp = (unsigned)(mo + 9) % 12;          /* Mar=0 .. Feb=11 */
        unsigned doy = (153 * mp + 2) / 5 + (unsigned)d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        days = (long long)era * 146097 + (long long)doe - 719468;
    }

    return days * 86400LL + (long long)h * 3600LL + (long long)mi * 60LL + se;
}

/* 解析单个 bucket:返回 true 表示 percent 解析成功 */
static bool parse_bucket(cJSON *obj, const char *name, usage_bucket_t *b)
{
    if (!obj) return false;

    cJSON *bucket = cJSON_GetObjectItem(obj, name);
    if (!bucket || !cJSON_IsObject(bucket)) return false;

    b->valid = true;
    b->resets_at_epoch = -1;
    b->resets_in = -1;

    cJSON *status = cJSON_GetObjectItem(bucket, "status");
    if (cJSON_IsString(status)) {
        snprintf(b->status, sizeof(b->status), "%s", status->valuestring);
    }

    cJSON *p = cJSON_GetObjectItem(bucket, "percent");
    if (cJSON_IsNumber(p)) {
        b->percent = p->valueint;
    } else if (cJSON_IsString(p)) {
        b->percent = atoi(p->valuestring);
    } else {
        b->valid = false;
        return false;
    }

    cJSON *ra = cJSON_GetObjectItem(bucket, "resetsAt");
    if (cJSON_IsString(ra)) {
        b->resets_at_epoch = parse_iso8601_epoch(ra->valuestring);
    }

    /* 接口可能直接给 resetsInSeconds;否则用 resetsAt - 当前SNTP时间 */
    cJSON *ri = cJSON_GetObjectItem(bucket, "resetsInSeconds");
    if (cJSON_IsNumber(ri) && ri->valueint >= 0) {
        b->resets_in = ri->valueint;
    } else {
        time_t now = time(NULL);
        if (b->resets_at_epoch > 0 && now > 1000000000LL) {
            int64_t d = b->resets_at_epoch - (int64_t)now;
            b->resets_in = d > 0 ? (int)d : 0;
        }
    }
    return true;
}

static void extract_usage(cJSON *root, usage_quota_t *out)
{
    /* 兼容两种结构:{"usage":{...}} 或裸字段 */
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage && cJSON_IsObject(usage)) {
        parse_bucket(usage, "rolling", &out->rolling);
        parse_bucket(usage, "weekly", &out->weekly);
        parse_bucket(usage, "monthly", &out->monthly);
    } else {
        parse_bucket(root, "rolling", &out->rolling);
        parse_bucket(root, "weekly", &out->weekly);
        parse_bucket(root, "monthly", &out->monthly);
    }
}

esp_err_t usage_api_fetch(usage_quota_t *out)
{
    memset(out, 0, sizeof(*out));

    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", CONFIG_OPENCODE_API_KEY);

    esp_http_client_config_t cfg = {
        .url = USAGE_URL,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Authorization", auth_header);

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http perform failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "HTTP %d bytes", s_http_len);
    if (s_http_len < 2) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGE(TAG, "json parse failed");
        return ESP_FAIL;
    }
    extract_usage(root, out);
    cJSON_Delete(root);

    if (!out->rolling.valid && !out->weekly.valid && !out->monthly.valid) {
        ESP_LOGE(TAG, "no usable bucket parsed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "rolling=%d%% weekly=%d%% monthly=%d%%",
             out->rolling.percent, out->weekly.percent, out->monthly.percent);
        return ESP_OK;
}

void usage_api_dump_raw(char *dst, size_t len)
{
    if (len == 0) return;
    size_t n = s_http_len < (int)(len - 1) ? (size_t)s_http_len : len - 1;
    memcpy(dst, s_http_buf, n);
    dst[n] = '\0';
}