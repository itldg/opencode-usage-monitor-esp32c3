#include "web_config.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "config_store.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TAG "web"

#define REFRESH_BIT   BIT0
#define COOKIE_NAME   "web_auth"

static EventGroupHandle_t s_eg;
static char s_token[33];             /* 32 位随机 hex 会话令牌 */
static bool s_password_required;

/* ---- 配置页 HTML(自包含,无外部依赖) ---- */
static const char WEB_PAGE_HTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>用量仪表盘配置</title><style>"
    "body{background:#0b1220;color:#e6edf6;font-family:system-ui,'Segoe UI',Arial,sans-serif;margin:0;padding:16px}"
    ".card{max-width:560px;margin:0 auto;background:#16233d;border-radius:12px;padding:24px}"
    "h1{font-size:20px;margin:0 0 4px}.ip{color:#9db2cc;font-size:13px;margin-bottom:18px}"
    "label{display:block;font-size:14px;margin:14px 0 6px;color:#8fcbff}.tip{color:#9db2cc;font-weight:normal}"
    "input,textarea,select{width:100%;box-sizing:border-box;background:#0b1220;color:#e6edf6;"
    "border:1px solid #2e4058;border-radius:8px;padding:10px;font-size:14px}"
    "textarea{resize:vertical}select{cursor:pointer}button{width:100%;margin-top:18px;background:#00b378;border:none;"
    "border-radius:8px;padding:12px;font-size:15px;font-weight:bold;color:#06281c;cursor:pointer}"
    ".err{color:#ff5c5c;font-size:13px;margin-top:8px}.msg{color:#3effb0;font-size:13px;margin-top:8px}"
    "</style></head><body><div class=\"card\">"
    "<h1>用量仪表盘配置</h1><div class=\"ip\" id=\"ip\">设备 IP: --</div>"
    "<div id=\"login\" style=\"display:none\">"
    "<label>网页密码</label><input type=\"password\" id=\"pw\" placeholder=\"请输入菜单中配置的密码\">"
    "<button onclick=\"doLogin()\">登录</button><div class=\"err\" id=\"loginErr\"></div></div>"
    "<div id=\"config\" style=\"display:none\">"
    "<label>OpenCode API Key <span class=\"tip\">(留空则不启用)</span></label>"
    "<input type=\"text\" id=\"ocKey\" autocomplete=\"off\" maxlength=\"256\" placeholder=\"opencode-go 的 key\">"
    "<label>火山 Plan Cookie <span class=\"tip\">(Coding/Agent Plan 通用;留空则不启用,保存时自动精简并校验)</span></label>"
    "<textarea id=\"volcCookie\" rows=\"6\" placeholder=\"粘贴 DevTools 复制的完整 Cookie(不限长度,保存时自动精简)\"></textarea>"
    "<label>火山 Plan 类型 <span class=\"tip\">(Cookie 为账号级,需指定查询哪个 Plan)</span></label>"
    "<select id=\"volcPlan\">"
    "<option value=\"0\">仅 Coding Plan</option>"
    "<option value=\"1\">仅 Agent Plan</option>"
    "<option value=\"2\">Coding + Agent</option>"
    "</select>"
    "<button onclick=\"doSave()\">保存配置</button><div class=\"msg\" id=\"saveMsg\"></div></div>"
    "</div><script>"
    "function post(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify(body)}).then(r=>r.json().catch(()=>({})));}"
    "function load(){fetch('/api/state').then(r=>{"
    "if(r.status===401){document.getElementById('login').style.display='block';return;}return r.json();"
    "}).then(st=>{if(!st)return;document.getElementById('ip').textContent='设备 IP: '+(st.ip||'--');"
    "document.getElementById('ocKey').value=st.opencode_key||'';"
    "document.getElementById('volcCookie').value=st.volc_cookie||'';"
    "document.getElementById('volcPlan').value=st.volc_plan||0;"
    "document.getElementById('config').style.display='block';});}"
    "function doLogin(){post('/api/login',{password:document.getElementById('pw').value}).then(r=>{"
    "if(r.ok){document.cookie='" COOKIE_NAME "='+r.token+'; path=/';load();}"
    "else{document.getElementById('loginErr').textContent='密码错误';}});}"
    "function doSave(){var m=document.getElementById('saveMsg');"
    "var raw=document.getElementById('volcCookie').value.trim();"
    "post('/api/config',{opencode_key:document.getElementById('ocKey').value,volc_cookie:raw,"
    "volc_plan:parseInt(document.getElementById('volcPlan').value)||0}).then(function(r){"
    "m.className=r.ok?'msg':'err';"
    "m.textContent=r.ok?'已保存,设备正在刷新用量…':(r.error||'保存失败');});}"
    "load();</script></body></html>";

static esp_err_t send_json(httpd_req_t *req, int status, const char *json)
{
    const char *text = status == 200 ? "200 OK"
                     : status == 401 ? "401 Unauthorized"
                     : status == 403 ? "403 Forbidden"
                     : "400 Bad Request";
    httpd_resp_set_status(req, text);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

/* 密码非空时校验 Cookie 中的会话令牌 */
static bool auth_ok(httpd_req_t *req)
{
    if (!s_password_required) return true;
    char cookie[512];
    int len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len <= 0 || len + 1 > (int)sizeof(cookie)) return false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char expected[64];
    snprintf(expected, sizeof(expected), COOKIE_NAME "=%s", s_token);
    return strstr(cookie, expected) != NULL;
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, WEB_PAGE_HTML);
}

/* 浏览器自动请求 favicon,返回 204 避免 404 日志噪音 */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_sendstr(req, "");
}

static esp_err_t state_handler(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "password_required", s_password_required);
    cJSON_AddStringToObject(root, "ip", web_config_ip_str());
    cJSON_AddStringToObject(root, "opencode_key", config_store_opencode_key());
    cJSON_AddStringToObject(root, "volc_cookie", config_store_volc_cookie());
    cJSON_AddNumberToObject(root, "volc_plan", config_store_volc_plan());
    char *s = cJSON_PrintUnformatted(root);
    esp_err_t err = send_json(req, 200, s);
    cJSON_free(s);
    cJSON_Delete(root);
    return err;
}

static esp_err_t login_handler(httpd_req_t *req)
{
    if (!s_password_required) {
        /* 未设密码:无需登录,直接返回 ok */
        return send_json(req, 200, "{\"ok\":true}");
    }

    char buf[256];
    int total = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (total <= 0) return send_json(req, 400, "{\"error\":\"bad request\"}");
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    const char *pw = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "password")) : NULL;
    bool ok = pw && strcmp(pw, CONFIG_OPENCODE_WEB_PASSWORD) == 0;
    cJSON_Delete(root);
    if (!ok) {
        return send_json(req, 403, "{\"error\":\"wrong password\"}");
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    char cookie[128];
    snprintf(cookie, sizeof(cookie), COOKIE_NAME "=%s; Path=/; SameSite=Lax", s_token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"token\":\"%s\"}", s_token);
    return httpd_resp_sendstr(req, resp);
}

static esp_err_t config_handler(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return send_json(req, 401, "{\"error\":\"unauthorized\"}");
    }

    /* Cookie 可长达 4KB,请求体放堆上解析,避免撑爆 httpd 栈 */
    size_t alloc = 6144;
    char *buf = malloc(alloc);
    if (!buf) {
        return send_json(req, 500, "{\"error\":\"no memory\"}");
    }
    int total = httpd_req_recv(req, buf, alloc - 1);
    if (total <= 0) {
        free(buf);
        return send_json(req, 400, "{\"error\":\"bad request\"}");
    }
    buf[total] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return send_json(req, 400, "{\"error\":\"bad json\"}");

    const char *k = cJSON_GetStringValue(cJSON_GetObjectItem(root, "opencode_key"));
    const char *c = cJSON_GetStringValue(cJSON_GetObjectItem(root, "volc_cookie"));
    cJSON *plan = cJSON_GetObjectItem(root, "volc_plan");
    if (k) config_store_set_opencode_key(k);
    if (cJSON_IsNumber(plan)) config_store_set_volc_plan(plan->valueint);
    if (c) {
        /* 非空 Cookie 必须含必要字段,否则拒绝保存并提示 */
        char err[64];
        if (!config_store_volc_cookie_ok(c, err, sizeof(err))) {
            char resp[160];
            snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", err);
            cJSON_Delete(root);
            return send_json(req, 400, resp);
        }
        config_store_set_volc_cookie(c);
    }
    cJSON_Delete(root);

    if (s_eg) xEventGroupSetBits(s_eg, REFRESH_BIT);
    ESP_LOGI(TAG, "config saved via web");
    return send_json(req, 200, "{\"ok\":true}");
}

esp_err_t web_config_start(void)
{
    s_eg = xEventGroupCreate();
    s_password_required = strlen(CONFIG_OPENCODE_WEB_PASSWORD) > 0;

    /* esp_http_server 建 socket 需要 lwIP 栈已就绪;
     * esp_netif_init/esp_event_loop_create_default 幂等(wifi_app 里已容错)。 */
    esp_err_t netif_err = esp_netif_init();
    if (netif_err != ESP_OK && netif_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(netif_err));
        return netif_err;
    }
    esp_err_t evt_err = esp_event_loop_create_default();
    if (evt_err != ESP_OK && evt_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop create failed: %s", esp_err_to_name(evt_err));
        return evt_err;
    }

    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        snprintf(s_token + i * 2, 3, "%02x", rnd[i]);
    }
    ESP_LOGI(TAG, "password %s", s_password_required ? "required" : "disabled");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return ESP_FAIL;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",          .method = HTTP_GET,  .handler = root_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler },
        { .uri = "/api/state", .method = HTTP_GET,  .handler = state_handler },
        { .uri = "/api/login", .method = HTTP_POST, .handler = login_handler },
        { .uri = "/api/config", .method = HTTP_POST, .handler = config_handler },
    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK) {
            ESP_LOGW(TAG, "register uri %s failed", uris[i].uri);
        }
    }
    return ESP_OK;
}

bool web_config_take_refresh(void)
{
    if (!s_eg) return false;
    bool req = (xEventGroupGetBits(s_eg) & REFRESH_BIT) != 0;
    if (req) xEventGroupClearBits(s_eg, REFRESH_BIT);
    return req;
}

const char *web_config_ip_str(void)
{
    static char s_ip[16] = "0.0.0.0";
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(n, &info) == ESP_OK) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
        }
    }
    return s_ip;
}
