// main/app_portal.c
#include "app_portal.h"

#include "app_config.h"
#include "app_http.h"
#include "app_model.h"
#include "app_wifi.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_portal";

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static httpd_handle_t s_server;
static int64_t s_started_ms;
static atomic_bool s_exit_requested;
static atomic_bool s_config_changed;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message ? message : "请求失败");
    return send_json(req, root);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

static char *read_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) return NULL;
    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, (size_t)req->content_len - (size_t)received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    return buf;
}

static cJSON *parse_body(httpd_req_t *req)
{
    char *body = read_body(req);
    if (!body) return NULL;
    cJSON *root = cJSON_Parse(body);
    free(body);
    return root;
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // EMBED_FILES 不追加结尾 0,必须显式使用 _end - _start 的长度。
    return httpd_resp_send(req, portal_html_start,
                           (ssize_t)(portal_html_end - portal_html_start));
}

static esp_err_t config_get(httpd_req_t *req)
{
    app_config_t cfg;
    app_config_snapshot(&cfg);
    app_wifi_info_t wifi;
    app_wifi_get_info(&wifi);

    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(root, "ap_ssid", app_wifi_ap_ssid());
    cJSON_AddStringToObject(root, "ap_pass", app_wifi_ap_password());
    const char *state = "off";
    if (wifi.state == APP_WIFI_CONNECTED) state = "connected";
    else if (wifi.state == APP_WIFI_CONNECTING) state = "connecting";
    else if (wifi.state == APP_WIFI_FAILED) state = "failed";
    cJSON_AddStringToObject(root, "wifi_state", state);
    cJSON_AddStringToObject(root, "wifi_ssid", wifi.ssid);
    cJSON_AddStringToObject(root, "ip", wifi.ip);

    cJSON *wifi_arr = cJSON_AddArrayToObject(root, "wifi");
    for (uint8_t i = 0; i < cfg.wifi_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", cfg.wifi[i].ssid);
        cJSON_AddBoolToObject(item, "active", i == cfg.active_wifi);
        cJSON_AddItemToArray(wifi_arr, item);
    }

    cJSON *srv_arr = cJSON_AddArrayToObject(root, "servers");
    for (uint8_t i = 0; i < cfg.server_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", cfg.servers[i].name);
        cJSON_AddStringToObject(item, "url", cfg.servers[i].url);
        cJSON_AddItemToArray(srv_arr, item);
    }

    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddNumberToObject(settings, "refresh_s", cfg.refresh_s);
    cJSON_AddNumberToObject(settings, "screen_off_s", cfg.screen_off_s);
    cJSON_AddNumberToObject(settings, "brightness", cfg.brightness);
    return send_json(req, root);
}

#define SCAN_MAX 20

static esp_err_t scan_get(httpd_req_t *req)
{
    app_wifi_scan_item_t *items = calloc(SCAN_MAX, sizeof(*items));
    if (!items) return send_error(req, "内存不足");
    int count = app_wifi_scan(items, SCAN_MAX);
    if (count < 0) {
        free(items);
        return send_error(req, "扫描失败，请稍后重试");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(items);
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", items[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", items[i].rssi);
        cJSON_AddBoolToObject(item, "secure", items[i].secure);
        cJSON_AddItemToArray(arr, item);
    }
    free(items);
    return send_json(req, root);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *root = parse_body(req);
    if (!root) return send_error(req, "请求格式错误");
    const char *action = json_string(root, "action");
    app_config_t cfg;
    app_config_snapshot(&cfg);
    esp_err_t err = ESP_OK;

    if (action && strcmp(action, "add") == 0) {
        const char *ssid = json_string(root, "ssid");
        const char *pass = json_string(root, "password");
        if (!ssid) {
            cJSON_Delete(root);
            return send_error(req, "缺少 Wi-Fi 名称");
        }
        if (app_wifi_add(&cfg, ssid, pass ? pass : "") < 0) {
            cJSON_Delete(root);
            return send_error(req, "Wi-Fi 无效、密码不足 8 位或列表已满");
        }
    } else if (action && strcmp(action, "delete") == 0) {
        int index = json_int(root, "index", -1);
        if (!app_wifi_remove(&cfg, index)) {
            cJSON_Delete(root);
            return send_error(req, "索引无效");
        }
    } else if (action && strcmp(action, "activate") == 0) {
        int index = json_int(root, "index", -1);
        if (!app_wifi_set_active(&cfg, index)) {
            cJSON_Delete(root);
            return send_error(req, "索引无效");
        }
    } else {
        cJSON_Delete(root);
        return send_error(req, "未知操作");
    }
    cJSON_Delete(root);
    err = app_config_apply(&cfg);
    if (err != ESP_OK) return send_error(req, "保存配置失败");
    app_wifi_note_config_changed();
    atomic_store(&s_config_changed, true);
    return send_ok(req);
}

static esp_err_t server_post(httpd_req_t *req)
{
    cJSON *root = parse_body(req);
    if (!root) return send_error(req, "请求格式错误");
    const char *action = json_string(root, "action");
    app_config_t cfg;
    app_config_snapshot(&cfg);
    esp_err_t err = ESP_OK;

    if (action && strcmp(action, "add") == 0) {
        const char *name = json_string(root, "name");
        const char *url = json_string(root, "url");
        const char *token = json_string(root, "token");
        if (!name || !url || !token) {
            cJSON_Delete(root);
            return send_error(req, "名称、地址和 Token 都不能为空");
        }
        if (app_server_add(&cfg, name, url, token) < 0) {
            cJSON_Delete(root);
            return send_error(req, "服务器无效或列表已满");
        }
    } else if (action && strcmp(action, "update") == 0) {
        int index = json_int(root, "index", -1);
        if (index < 0 || index >= cfg.server_count) {
            cJSON_Delete(root);
            return send_error(req, "索引无效");
        }
        const char *name = json_string(root, "name");
        const char *url = json_string(root, "url");
        const char *token = json_string(root, "token");
        if (!name || !url) {
            cJSON_Delete(root);
            return send_error(req, "名称和地址都不能为空");
        }
        if (!token || !token[0]) token = cfg.servers[index].token;
        if (!app_server_update(&cfg, index, name, url, token)) {
            cJSON_Delete(root);
            return send_error(req, "服务器参数无效");
        }
    } else if (action && strcmp(action, "delete") == 0) {
        int index = json_int(root, "index", -1);
        if (!app_server_remove(&cfg, index)) {
            cJSON_Delete(root);
            return send_error(req, "索引无效");
        }
    } else {
        cJSON_Delete(root);
        return send_error(req, "未知操作");
    }
    cJSON_Delete(root);
    err = app_config_apply(&cfg);
    if (err != ESP_OK) return send_error(req, "保存配置失败");
    atomic_store(&s_config_changed, true);
    return send_ok(req);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *root = parse_body(req);
    if (!root) return send_error(req, "请求格式错误");
    app_config_t cfg;
    app_config_snapshot(&cfg);
    int refresh = json_int(root, "refresh_s", cfg.refresh_s);
    int screen_off = json_int(root, "screen_off_s", cfg.screen_off_s);
    int brightness = json_int(root, "brightness", cfg.brightness);
    cJSON_Delete(root);

    cfg.refresh_s = (uint16_t)(refresh < APP_REFRESH_MIN_S ? APP_REFRESH_MIN_S : refresh);
    cfg.refresh_s = (uint16_t)(cfg.refresh_s > APP_REFRESH_MAX_S ? APP_REFRESH_MAX_S : cfg.refresh_s);
    cfg.screen_off_s = screen_off < 0 ? 0 : (uint16_t)(screen_off > 3600 ? 3600 : screen_off);
    cfg.brightness = (uint8_t)(brightness < APP_BRIGHTNESS_MIN ? APP_BRIGHTNESS_MIN : brightness);
    cfg.brightness = (uint8_t)(cfg.brightness > APP_BRIGHTNESS_MAX ? APP_BRIGHTNESS_MAX : cfg.brightness);
    if (app_config_apply(&cfg) != ESP_OK) return send_error(req, "保存配置失败");
    atomic_store(&s_config_changed, true);
    return send_ok(req);
}

static esp_err_t test_post(httpd_req_t *req)
{
    cJSON *root = parse_body(req);
    if (!root) return send_error(req, "请求格式错误");
    int index = json_int(root, "index", -1);
    cJSON_Delete(root);

    app_server_cfg_t srv;
    memset(&srv, 0, sizeof(srv));
    app_config_lock();
    const app_config_t *cfg = app_config_get();
    bool exists = index >= 0 && index < cfg->server_count;
    if (exists) srv = cfg->servers[index];
    app_config_unlock();
    if (!exists) return send_error(req, "索引无效");

    app_wifi_info_t wifi;
    app_wifi_get_info(&wifi);
    if (wifi.state != APP_WIFI_CONNECTED) return send_error(req, "设备 Wi-Fi 未连接");
    if (!app_http_time_ready()) return send_error(req, "设备网络对时未完成,请稍后重试");

    app_server_status_t status;
    app_trend_t trend;
    int latency_ms = 0;
    esp_err_t err = app_fetch_status(&srv, &status, &trend, &latency_ms);
    if (err != ESP_OK) {
        return send_error(req, status.err[0] ? status.err : "连接失败");
    }
    cJSON *out = cJSON_CreateObject();
    if (!out) return httpd_resp_send_500(req);
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddNumberToObject(out, "cpu", status.cpu);
    cJSON_AddNumberToObject(out, "mem", status.mem);
    cJSON_AddNumberToObject(out, "disk", status.disk);
    cJSON_AddNumberToObject(out, "latency_ms", latency_ms);
    return send_json(req, out);
}

static esp_err_t exit_post(httpd_req_t *req)
{
    atomic_store(&s_exit_requested, true);
    return send_ok(req);
}

static const httpd_uri_t URIS[] = {
    { .uri = "/", .method = HTTP_GET, .handler = root_get },
    { .uri = "/api/config", .method = HTTP_GET, .handler = config_get },
    { .uri = "/api/scan", .method = HTTP_GET, .handler = scan_get },
    { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
    { .uri = "/api/server", .method = HTTP_POST, .handler = server_post },
    { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
    { .uri = "/api/test", .method = HTTP_POST, .handler = test_post },
    { .uri = "/api/exit", .method = HTTP_POST, .handler = exit_post },
};

esp_err_t app_portal_start(void)
{
    if (s_server) return ESP_OK;
    if (!app_wifi_start_ap()) return ESP_FAIL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = sizeof(URIS) / sizeof(URIS[0]);
    // 配置结构整体约 7.4 KB，处理函数会做快照，HTTP 任务需要足够栈空间。
    // /api/test 会在本任务里同步发起 TLS 请求,配置快照也占用栈空间。
    config.stack_size = 20480;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        s_server = NULL;
        app_wifi_stop_ap();
        return err;
    }
    for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
        err = httpd_register_uri_handler(s_server, &URIS[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 %s 失败: %s", URIS[i].uri, esp_err_to_name(err));
            httpd_stop(s_server);
            s_server = NULL;
            app_wifi_stop_ap();
            return err;
        }
    }
    s_started_ms = esp_timer_get_time() / 1000;
    atomic_store(&s_exit_requested, false);
    atomic_store(&s_config_changed, false);
    ESP_LOGI(TAG, "配网页面已启动: http://192.168.4.1");
    return ESP_OK;
}

void app_portal_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    app_wifi_stop_ap();
    atomic_store(&s_exit_requested, false);
}

bool app_portal_active(void)
{
    return s_server != NULL;
}

bool app_portal_exit_requested(void)
{
    return atomic_load(&s_exit_requested);
}

bool app_portal_take_config_changed(void)
{
    return atomic_exchange(&s_config_changed, false);
}

uint32_t app_portal_elapsed_s(void)
{
    if (!s_server) return 0;
    int64_t now = esp_timer_get_time() / 1000;
    return now > s_started_ms ? (uint32_t)((now - s_started_ms) / 1000) : 0;
}
