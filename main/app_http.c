// main/app_http.c
#include "app_http.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESP_MAX 16384
// TLS 握手在弱信号下可能超过 8 秒; 15 秒 + 一次重试能显著减少误报离线。
#define HTTP_TIMEOUT_MS 15000
#define HTTP_ATTEMPTS 2

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool overflow;
} resp_buf_t;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    resp_buf_t *ctx = (resp_buf_t *)ev->user_data;
    if (!ctx) return ESP_OK;
    if (ev->event_id == HTTP_EVENT_ON_DATA) {
        if (ev->data_len <= 0) return ESP_OK;
        if (ctx->len + (size_t)ev->data_len >= ctx->cap) {
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->len, ev->data, (size_t)ev->data_len);
        ctx->len += (size_t)ev->data_len;
    }
    return ESP_OK;
}

static const cJSON *json_item(const cJSON *root, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(root, key);
}

static bool json_num(const cJSON *root, const char *key, double *out)
{
    const cJSON *item = json_item(root, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valuedouble;
    return true;
}

static int json_int_or(const cJSON *root, const char *key, int fallback)
{
    double value;
    if (!json_num(root, key, &value)) return fallback;
    if (value > 2147483000.0) value = 2147483000.0;
    if (value < -2147483000.0) value = -2147483000.0;
    return (int)llround(value);
}

static int clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void copy_field(char *dst, size_t size, const cJSON *root, const char *key)
{
    const cJSON *item = json_item(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) return;
    snprintf(dst, size, "%s", item->valuestring);
}

static void parse_trend(const cJSON *root, app_trend_t *trend)
{
    if (!trend) return;
    memset(trend, 0, sizeof(*trend));
    trend->step_s = 300;
    const cJSON *node = json_item(root, "trend");
    if (!cJSON_IsObject(node)) return;

    int step = json_int_or(node, "step", 300);
    trend->step_s = (uint16_t)clamp_int(step, 30, 3600);

    const cJSON *cpu = json_item(node, "cpu");
    const cJSON *mem = json_item(node, "mem");
    if (!cJSON_IsArray(cpu) || !cJSON_IsArray(mem)) return;
    int cpu_n = cJSON_GetArraySize(cpu);
    int mem_n = cJSON_GetArraySize(mem);
    int total = cpu_n < mem_n ? cpu_n : mem_n;
    int offset = total > APP_TREND_POINTS ? total - APP_TREND_POINTS : 0;
    int count = total - offset;
    trend->count = (uint16_t)count;
    for (int i = 0; i < count; i++) {
        const cJSON *c = cJSON_GetArrayItem(cpu, offset + i);
        const cJSON *m = cJSON_GetArrayItem(mem, offset + i);
        double cv = cJSON_IsNumber(c) ? c->valuedouble : 0;
        double mv = cJSON_IsNumber(m) ? m->valuedouble : 0;
        trend->cpu[i] = (uint8_t)clamp_int((int)llround(cv), 0, 100);
        trend->mem[i] = (uint8_t)clamp_int((int)llround(mv), 0, 100);
    }
}

static esp_err_t parse_body(const char *body, app_server_status_t *out, app_trend_t *trend)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->cpu = -1;
    out->mem = -1;
    out->swap = -1;
    out->disk = -1;
    out->load1_x100 = -1;
    out->temp_c_x10 = -1;
    out->ssl_days = -1;
    out->docker_run = -1;
    out->docker_total = -1;
    out->docker_bad = -1;
    out->backup_state = -1;

    out->online = true;
    const cJSON *online = json_item(root, "online");
    if (cJSON_IsBool(online)) out->online = cJSON_IsTrue(online);
    else if (cJSON_IsNumber(online)) out->online = online->valuedouble != 0;

    double value;
    if (json_num(root, "cpu", &value)) out->cpu = (int8_t)clamp_int((int)llround(value), 0, 100);
    if (json_num(root, "mem", &value)) out->mem = (int8_t)clamp_int((int)llround(value), 0, 100);
    if (json_num(root, "swap", &value)) out->swap = (int8_t)clamp_int((int)llround(value), 0, 100);
    if (json_num(root, "disk", &value)) out->disk = (int8_t)clamp_int((int)llround(value), 0, 100);
    if (json_num(root, "load1", &value)) out->load1_x100 = (int16_t)clamp_int((int)llround(value * 100), 0, 10000);
    if (json_num(root, "temp", &value)) out->temp_c_x10 = (int16_t)clamp_int((int)llround(value * 10), -400, 1500);
    if (json_num(root, "ssl_days", &value)) out->ssl_days = (int16_t)clamp_int((int)llround(value), -1, 3650);
    if (json_num(root, "docker_run", &value)) out->docker_run = (int16_t)clamp_int((int)llround(value), -1, 10000);
    if (json_num(root, "docker_total", &value)) out->docker_total = (int16_t)clamp_int((int)llround(value), -1, 10000);
    if (json_num(root, "docker_bad", &value)) out->docker_bad = (int16_t)clamp_int((int)llround(value), -1, 10000);
    if (json_num(root, "net_rx", &value)) {
        out->net_rx_bps = value < 0 ? 0 : (value > 4294967295.0 ? 4294967295u : (uint32_t)value);
    }
    if (json_num(root, "net_tx", &value)) {
        out->net_tx_bps = value < 0 ? 0 : (value > 4294967295.0 ? 4294967295u : (uint32_t)value);
    }
    uint32_t disk_read = 0;
    uint32_t disk_write = 0;
    if (json_num(root, "disk_read", &value)) {
        disk_read = value < 0 ? 0 : (value > 4294967295.0 ? 4294967295u : (uint32_t)value);
    }
    if (json_num(root, "disk_write", &value)) {
        disk_write = value < 0 ? 0 : (value > 4294967295.0 ? 4294967295u : (uint32_t)value);
    }
    if (disk_read > UINT32_MAX - disk_write) out->disk_io_bps = UINT32_MAX;
    else out->disk_io_bps = disk_read + disk_write;
    if (json_num(root, "uptime", &value)) out->uptime_s = value < 0 ? 0 : (uint32_t)value;
    if (json_num(root, "ts", &value)) out->server_ts = value < 0 ? 0 : (uint32_t)value;

    const cJSON *backup = json_item(root, "backup_ok");
    if (cJSON_IsBool(backup)) out->backup_state = cJSON_IsTrue(backup) ? 1 : 0;
    else if (cJSON_IsNumber(backup)) out->backup_state = backup->valuedouble != 0 ? 1 : 0;

    copy_field(out->panel, sizeof(out->panel), root, "panel");
    parse_trend(root, trend);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_fetch_status(const app_server_cfg_t *srv, app_server_status_t *out,
                           app_trend_t *trend, int *latency_ms)
{
    if (!srv || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->online = false;
    out->cpu = -1;
    out->mem = -1;
    out->swap = -1;
    out->disk = -1;
    out->load1_x100 = -1;
    out->temp_c_x10 = -1;
    out->ssl_days = -1;
    out->docker_run = -1;
    out->docker_total = -1;
    out->docker_bad = -1;
    out->backup_state = -1;
    if (trend) memset(trend, 0, sizeof(*trend));

    if (!app_url_valid(srv->url)) {
        snprintf(out->err, sizeof(out->err), "地址必须以 https:// 开头");
        return ESP_ERR_INVALID_ARG;
    }

    resp_buf_t ctx = {0};
    ctx.buf = calloc(1, RESP_MAX);
    if (!ctx.buf) {
        snprintf(out->err, sizeof(out->err), "内存不足");
        return ESP_ERR_NO_MEM;
    }
    ctx.cap = RESP_MAX;

    esp_err_t err = ESP_FAIL;
    int status = 0;
    int elapsed_ms = 0;
    for (int attempt = 0; attempt < HTTP_ATTEMPTS; attempt++) {
        ctx.len = 0;
        ctx.overflow = false;

        esp_http_client_config_t cfg = {
            .url = srv->url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size = 2048,
            .buffer_size_tx = 512,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = http_event,
            .user_data = &ctx,
            .disable_auto_redirect = false,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            free(ctx.buf);
            snprintf(out->err, sizeof(out->err), "创建 HTTPS 客户端失败");
            return ESP_ERR_NO_MEM;
        }
        esp_http_client_set_header(client, "X-Api-Token", srv->token);
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "User-Agent", "AI-Passport-Monitor/1.0");

        int64_t started = esp_timer_get_time();
        err = esp_http_client_perform(client);
        elapsed_ms = (int)((esp_timer_get_time() - started) / 1000);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK) break;
        // 只重试网络层瞬时错误; 证书、参数错误重试也没用。
        bool retryable = err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT ||
                         err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTION_CLOSED;
        if (!retryable) break;
    }

    if (latency_ms) *latency_ms = elapsed_ms;

    if (err != ESP_OK) {
        snprintf(out->err, sizeof(out->err), "%s", esp_err_to_name(err));
        free(ctx.buf);
        return err;
    }
    if (status != 200) {
        snprintf(out->err, sizeof(out->err), "HTTP %d", status);
        free(ctx.buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (ctx.overflow) {
        snprintf(out->err, sizeof(out->err), "响应过大");
        free(ctx.buf);
        return ESP_ERR_INVALID_SIZE;
    }
    ctx.buf[ctx.len] = '\0';
    err = parse_body(ctx.buf, out, trend);
    free(ctx.buf);
    if (err != ESP_OK) {
        snprintf(out->err, sizeof(out->err), "JSON 解析失败");
    }
    return err;
}

bool app_http_time_ready(void)
{
    return time(NULL) > 1700000000;
}
