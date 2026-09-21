#include "volc_usage_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "volc"

#define VOLC_URL      "https://console.volcengine.com/api/top/ark/cn-beijing/2024-01-01/GetCodingPlanUsage"
#define VOLC_AGENT_URL "https://console.volcengine.com/api/top/ark/cn-beijing/2024-01-01/GetAgentPlanAFPUsage?"
#define VOLC_ORIGIN   "https://console.volcengine.com"
#define VOLC_REFERER  "https://console.volcengine.com/ark/region:ark+cn-beijing/plan"
#define VOLC_AGENT_REFERER "https://console.volcengine.com/ark/region:cn-beijing/subscription/agent-plan"
#define VOLC_UA       "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"

#define HTTP_BUF_SIZE 8192

static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len = 0;

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

/* 从 Cookie 中提取 CSRF token(兼容多种 cookie 名,大小写不敏感);
 * 找不到返回 NULL。 */
static const char *extract_csrf(const char *cookie, char *out, size_t out_len)
{
    static const char *const names[] = { "csrftoken=", "csrf_token=", "csrf-token=",
                                         "_csrf=", "volc_csrf=", "xsrf-token=" };
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        const char *p = strcasestr(cookie, names[k]);
        if (!p) continue;
        p += strlen(names[k]);
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= out_len) n = out_len - 1;
        memcpy(out, p, n);
        out[n] = '\0';
        if (n) return out;
    }
    return NULL;
}

/* 解析 QuotaUsage 数组;同窗口多条记录优先取下次重置更近(当前窗口)的那条 */
static void parse_quota(cJSON *root, volc_quota_t *out, int64_t now)
{
    cJSON *result = cJSON_GetObjectItem(root, "Result");
    cJSON *arr = result ? cJSON_GetObjectItem(result, "QuotaUsage") : NULL;
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "no Result.QuotaUsage array");
        return;
    }

    volc_bucket_t *slots[3] = { &out->session, &out->weekly, &out->monthly };

    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsObject(it)) continue;

        cJSON *lv = cJSON_GetObjectItem(it, "Level");
        if (!cJSON_IsString(lv)) continue;
        int idx = -1;
        if (!strcmp(lv->valuestring, "session"))      idx = 0;
        else if (!strcmp(lv->valuestring, "weekly"))  idx = 1;
        else if (!strcmp(lv->valuestring, "monthly")) idx = 2;
        if (idx < 0) continue;

        cJSON *p = cJSON_GetObjectItem(it, "Percent");
        if (!cJSON_IsNumber(p)) continue;

        /* Percent 可能是 0~1 小数或 0~100 整数,归一化为百分比 */
        double pd = p->valuedouble;
        if (pd <= 1.0) pd *= 100.0;
        int percent = (int)(pd + 0.5);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        /* ResetTimestamp 可能是秒或毫秒(>1e12 为毫秒) */
        int64_t reset = -1;
        cJSON *rt = cJSON_GetObjectItem(it, "ResetTimestamp");
        if (cJSON_IsNumber(rt)) {
            double ts = rt->valuedouble;
            if (ts > 1e12) ts /= 1000.0;
            reset = (int64_t)ts;
        }

        volc_bucket_t *slot = slots[idx];
        bool take = false;
        if (!slot->valid) {
            take = true;
        } else if (reset >= 0) {
            int64_t cur_d = slot->resets_at_epoch < 0 ? INT64_MAX
                            : llabs(slot->resets_at_epoch - now);
            int64_t new_d = llabs(reset - now);
            if (slot->resets_at_epoch < 0 || new_d < cur_d) take = true;
        }
        if (!take) continue;

        slot->valid = true;
        slot->percent = percent;
        slot->resets_at_epoch = reset;
        /* 系统时间未同步(SNTP)时,重置倒计时无意义 */
        if (reset >= 0 && now > 1000000000LL) {
            int64_t d = reset - now;
            slot->resets_in = d > 0 ? (int)d : 0;
        } else {
            slot->resets_in = -1;
        }
    }
}

/* Agent Plan:Result 为对象,窗口字段 AFPFiveHour/AFPWeekly/AFPMonthly/AFPDaily,
 * 每项 {Quota, Used, ResetTime}(原始数值,非百分比);UI 三行布局展示前三个窗口。 */
static void parse_agent_quota(cJSON *root, volc_quota_t *out, int64_t now)
{
    cJSON *result = cJSON_GetObjectItem(root, "Result");
    if (!cJSON_IsObject(result)) {
        ESP_LOGE(TAG, "no Result object");
        return;
    }

    struct {
        const char *key;
        volc_bucket_t *slot;
    } map[] = {
        { "AFPFiveHour", &out->session }, /* 5小时窗口 → session 槽 */
        { "AFPWeekly",   &out->weekly },
        { "AFPMonthly",  &out->monthly },
        /* AFPDaily(日窗口)三行布局暂不展示,需要时加第 4 行 */
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        cJSON *w = cJSON_GetObjectItem(result, map[i].key);
        if (!cJSON_IsObject(w)) continue;
        cJSON *quota = cJSON_GetObjectItem(w, "Quota");
        cJSON *used = cJSON_GetObjectItem(w, "Used");
        if (!cJSON_IsNumber(quota) || !cJSON_IsNumber(used) || quota->valuedouble <= 0) {
            continue;
        }

        volc_bucket_t *b = map[i].slot;
        /* 未使用(Used=-1)按 0 处理 */
        double used_v = used->valuedouble;
        if (used_v < 0) used_v = 0;
        double pct = used_v / quota->valuedouble * 100.0;
        int percent = (int)(pct + 0.5);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;

        /* ResetTime / SubscribeTime 为毫秒时间戳 */
        int64_t reset = -1, subscribe = -1;
        cJSON *rt = cJSON_GetObjectItem(w, "ResetTime");
        if (cJSON_IsNumber(rt)) {
            double ts = rt->valuedouble;
            if (ts > 1e12) ts /= 1000.0;
            reset = (int64_t)ts;
        }
        cJSON *st = cJSON_GetObjectItem(w, "SubscribeTime");
        if (cJSON_IsNumber(st)) {
            double ts = st->valuedouble;
            if (ts > 1e12) ts /= 1000.0;
            subscribe = (int64_t)ts;
        }

        b->valid = true;
        b->percent = percent;
        b->resets_at_epoch = reset;
        b->subscribe_at_epoch = subscribe;
        if (reset >= 0 && now > 1000000000LL) {
            int64_t d = reset - now;
            b->resets_in = d > 0 ? (int)d : 0;
        } else {
            b->resets_in = -1;
        }
    }
}

typedef void (*volc_parse_fn)(cJSON *root, volc_quota_t *out, int64_t now);

static esp_err_t volc_fetch_url(const char *tag, const char *url, const char *referer,
                                const char *body, volc_parse_fn parse, volc_quota_t *out)
{
    memset(out, 0, sizeof(*out));

    const char *cookie = config_store_volc_cookie();
    if (!cookie || !*cookie) {
        ESP_LOGE(TAG, "volc cookie not configured");
        return ESP_FAIL;
    }

    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,    /* RX */
        .buffer_size_tx = 4096, /* 精简后的 Cookie 仍可能较长,预留余量 */
        .timeout_ms = 30000,    /* 火山接口偶发慢,放宽超时 */
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Cookie", cookie);
    esp_http_client_set_header(client, "Origin", VOLC_ORIGIN);
    esp_http_client_set_header(client, "Referer", referer);
    esp_http_client_set_header(client, "User-Agent", VOLC_UA);
    char csrf[128];
    const char *csrf_val = extract_csrf(cookie, csrf, sizeof(csrf));
    if (csrf_val) {
        esp_http_client_set_header(client, "x-csrf-token", csrf_val);
    }

    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_SUPPORTED) {
            /* 服务器返回 HTTP 401(会话 cookie 缺失/过期) */
            ESP_LOGE(TAG, "[%s] API 401 (cookie session invalid/expired), cookie len=%d",
                     tag, (int)strlen(cookie));
        } else {
            ESP_LOGE(TAG, "[%s] http perform failed: %s", tag, esp_err_to_name(err));
        }
        return err;
    }

    ESP_LOGI(TAG, "[%s] HTTP %d bytes", tag, s_http_len);
    if (s_http_len < 2) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        ESP_LOGE(TAG, "json parse failed");
        return ESP_FAIL;
    }
    parse(root, out, (int64_t)time(NULL));
    cJSON_Delete(root);

    if (!out->session.valid && !out->weekly.valid && !out->monthly.valid) {
        ESP_LOGE(TAG, "[%s] no usable bucket parsed", tag);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "[%s] session=%d%% weekly=%d%% monthly=%d%%",
             tag, out->session.percent, out->weekly.percent, out->monthly.percent);
    return ESP_OK;
}

esp_err_t volc_api_fetch(volc_quota_t *out)
{
    return volc_fetch_url("coding", VOLC_URL, VOLC_REFERER,
                          "{\"ProjectName\":\"default\"}", parse_quota, out);
}

esp_err_t volc_api_fetch_agent(volc_quota_t *out)
{
    return volc_fetch_url("agent", VOLC_AGENT_URL, VOLC_AGENT_REFERER,
                          "{}", parse_agent_quota, out);
}
