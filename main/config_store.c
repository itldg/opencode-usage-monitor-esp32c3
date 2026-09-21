#include "config_store.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"

#define TAG "cfg"

#define CFG_NS     "cfg"
#define KEY_OC     "oc_key"
#define KEY_VOLC   "volc_ck"
#define KEY_PLAN   "volc_plan"

static char s_oc_key[CFG_KEY_MAX_LEN + 1];
static char s_volc_cookie[CFG_COOKIE_MAX_LEN + 1];
static int  s_volc_plan = VOLC_PLAN_CODING;

static bool contains_ci(const char *str, const char *sub)
{
    return strcasestr(str, sub) != NULL;
}

/* Cookie 精简:仅保留鉴权/会话/CSRF 相关字段,丢弃埋点等无用项。
 * 火山登录凭证是 digest/userInfo 两个 JWT(signin_credential),必须保留。 */
static bool keep_cookie_key(const char *key)
{
    static const char *const pats[] = {
        "session", "sso", "passport", "csrf", "akid",
        "uid", "sid", "login", "auth", "account", "token",
        "digest", "userinfo", "credential",
    };
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        if (contains_ci(key, pats[i])) return true;
    }
    return false;
}

static void filter_cookie(const char *in, char *out, size_t out_len)
{
    size_t used = 0;
    out[0] = '\0';
    const char *p = in;
    while (p && *p && used + 1 < out_len) {
        const char *semi = strchr(p, ';');
        const char *pair_end = semi ? semi : p + strlen(p);

        /* 提取 key(pair 内 '=' 之前的非空白部分) */
        const char *key_b = p;
        while (key_b < pair_end && (*key_b == ' ' || *key_b == '\t')) key_b++;
        const char *eq = memchr(key_b, '=', (size_t)(pair_end - key_b));
        size_t key_len = eq ? (size_t)(eq - key_b) : (size_t)(pair_end - key_b);

        bool keep = false;
        char key[128];
        if (key_len > 0) {
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            memcpy(key, key_b, key_len);
            key[key_len] = '\0';
            keep = keep_cookie_key(key);
        }

        if (keep) {
            const char *b = p, *e = pair_end;
            while (b < e && (*b == ' ' || *b == '\t')) b++;
            while (e > b && (*(e - 1) == ' ' || *(e - 1) == '\t')) e--;
            size_t n = (size_t)(e - b);
            if (used + n + 1 < out_len) {
                if (used > 0) {
                    out[used++] = ';';
                    out[used++] = ' ';
                }
                memcpy(out + used, b, n);
                used += n;
            } else {
                ESP_LOGW(TAG, "cookie filter: output full, drop '%s'", key);
            }
        }

        if (!semi) break;
        p = semi + 1;
    }
    out[used] = '\0';
}

/* 读一个字符串;key 不存在时回退到 fallback(NULL 表示置空) */
static void read_str(nvs_handle_t h, const char *key, char *buf, size_t len, const char *fallback)
{
    size_t n = len;
    esp_err_t err = nvs_get_str(h, key, buf, &n);
    if (err == ESP_OK) {
        buf[len - 1] = '\0';
        return;
    }
    if (fallback) {
        snprintf(buf, len, "%s", fallback);
    } else {
        buf[0] = '\0';
    }
}

static esp_err_t write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open rw failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 首次运行无此命名空间:两个数据源均未配置,留待网页配置 */
        s_oc_key[0] = '\0';
        s_volc_cookie[0] = '\0';
        ESP_LOGI(TAG, "cfg ns missing, nothing configured");
        return ESP_OK;
    }
    read_str(h, KEY_OC, s_oc_key, sizeof(s_oc_key), NULL);
    read_str(h, KEY_VOLC, s_volc_cookie, sizeof(s_volc_cookie), NULL);
    uint8_t plan = 0;
    if (nvs_get_u8(h, KEY_PLAN, &plan) != ESP_OK || plan > VOLC_PLAN_BOTH) {
        plan = VOLC_PLAN_CODING;
    }
    s_volc_plan = plan;
    nvs_close(h);
    ESP_LOGI(TAG, "opencode key: %s, volc cookie: %s, plan: %d",
             s_oc_key[0] ? "set" : "empty", s_volc_cookie[0] ? "set" : "empty", s_volc_plan);
    return ESP_OK;
}

const char *config_store_opencode_key(void)
{
    return s_oc_key;
}

const char *config_store_volc_cookie(void)
{
    return s_volc_cookie;
}

int config_store_volc_plan(void)
{
    return s_volc_plan;
}

esp_err_t config_store_set_volc_plan(int plan)
{
    if (plan < VOLC_PLAN_CODING || plan > VOLC_PLAN_BOTH) plan = VOLC_PLAN_CODING;
    s_volc_plan = plan;

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, KEY_PLAN, (uint8_t)plan);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool config_store_opencode_enabled(void)
{
    return s_oc_key[0] != '\0';
}

bool config_store_volc_enabled(void)
{
    return s_volc_cookie[0] != '\0';
}

esp_err_t config_store_set_opencode_key(const char *key)
{
    snprintf(s_oc_key, sizeof(s_oc_key), "%s", key ? key : "");
    return write_str(KEY_OC, s_oc_key);
}

esp_err_t config_store_set_volc_cookie(const char *cookie)
{
    /* 保存前自动精简:只留鉴权/会话/CSRF 字段 */
    filter_cookie(cookie ? cookie : "", s_volc_cookie, sizeof(s_volc_cookie));
    return write_str(KEY_VOLC, s_volc_cookie);
}

bool config_store_volc_cookie_ok(const char *cookie, char *err, size_t err_len)
{
    if (!cookie || !*cookie) {
        return true; /* 空 = 停用火山 */
    }

    bool has_digest = false, has_userinfo = false, has_csrf = false;
    const char *p = cookie;
    while (p && *p) {
        const char *semi = strchr(p, ';');
        const char *end = semi ? semi : p + strlen(p);
        const char *key_b = p;
        while (key_b < end && (*key_b == ' ' || *key_b == '\t')) key_b++;
        const char *eq = memchr(key_b, '=', (size_t)(end - key_b));
        size_t klen = eq ? (size_t)(eq - key_b) : (size_t)(end - key_b);
        char key[64];
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        memcpy(key, key_b, klen);
        key[klen] = '\0';

        if      (strcasecmp(key, "digest") == 0)     has_digest = true;
        else if (strcasecmp(key, "userinfo") == 0)   has_userinfo = true;
        else if (strcasecmp(key, "csrftoken") == 0)  has_csrf = true;

        if (!semi) break;
        p = semi + 1;
    }

    if (!has_digest && !has_userinfo) {
        snprintf(err, err_len, "Cookie 缺少登录凭证(digest/userInfo)");
        return false;
    }
    if (!has_csrf) {
        snprintf(err, err_len, "Cookie 缺少 csrfToken");
        return false;
    }
    return true;
}
