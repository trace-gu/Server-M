// main/app_net.c
#include "app_net.h"

#include "app_config.h"
#include "app_http.h"
#include "app_wifi.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_net";

typedef struct {
    int index;
    uint32_t gen;
} net_cmd_t;

static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_result_queue;
static volatile uint32_t s_gen;
static volatile bool s_inflight;

// 只有仍是当前代际的请求才有权清除 busy 标记,避免旧请求把新请求标记为已结束。
static void finish_gen(uint32_t gen)
{
    if (gen == s_gen) s_inflight = false;
}

static void post_failure(int index, uint32_t gen, const char *message)
{
    app_fetch_result_t result = {0};
    result.index = index;
    result.gen = gen;
    result.finished_ms = esp_timer_get_time() / 1000;
    result.status.valid = true;
    result.status.online = false;
    result.status.fetched_ms = result.finished_ms;
    result.status.cpu = -1;
    result.status.mem = -1;
    result.status.swap = -1;
    result.status.disk = -1;
    result.status.load1_x100 = -1;
    result.status.temp_c_x10 = -1;
    result.status.ssl_days = -1;
    result.status.docker_run = -1;
    result.status.docker_bad = -1;
    result.status.backup_state = -1;
    snprintf(result.status.err, sizeof(result.status.err), "%s", message);
    (void)xQueueSend(s_result_queue, &result, 0);
}

static void net_task(void *arg)
{
    (void)arg;
    net_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd.gen != s_gen) continue;
        if (cmd.index < 0 || cmd.index >= APP_MAX_SERVERS) {
            post_failure(cmd.index, cmd.gen, "无效的服务器");
            finish_gen(cmd.gen);
            continue;
        }

        app_server_cfg_t srv;
        memset(&srv, 0, sizeof(srv));
        app_config_lock();
        const app_config_t *cfg = app_config_get();
        bool exists = cmd.index < cfg->server_count;
        if (exists) srv = cfg->servers[cmd.index];
        app_config_unlock();
        if (!exists) {
            post_failure(cmd.index, cmd.gen, "服务器已删除");
            finish_gen(cmd.gen);
            continue;
        }

        app_wifi_info_t wifi;
        app_wifi_get_info(&wifi);
        if (wifi.state != APP_WIFI_CONNECTED) {
            post_failure(cmd.index, cmd.gen, "Wi-Fi 未连接");
            finish_gen(cmd.gen);
            continue;
        }

        if (!app_http_time_ready()) {
            esp_err_t sync = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000));
            if (cmd.gen != s_gen) continue;
            if (sync != ESP_OK || !app_http_time_ready()) {
                post_failure(cmd.index, cmd.gen, "网络对时未完成");
                finish_gen(cmd.gen);
                continue;
            }
        }

        app_server_status_t status;
        app_trend_t trend;
        int latency_ms = 0;
        esp_err_t err = app_fetch_status(&srv, &status, &trend, &latency_ms);
        status.fetched_ms = esp_timer_get_time() / 1000;
        if (err != ESP_OK) {
            status.online = false;
            if (status.err[0] == '\0') {
                snprintf(status.err, sizeof(status.err), "%s", esp_err_to_name(err));
            }
            ESP_LOGW(TAG, "拉取服务器 %d 失败: %s", cmd.index, status.err);
        } else {
            ESP_LOGI(TAG, "服务器 %d 在线: CPU=%d%% MEM=%d%% 延迟=%dms",
                     cmd.index, status.cpu, status.mem, latency_ms);
        }

        if (cmd.gen == s_gen) {
            app_fetch_result_t result = {
                .index = cmd.index,
                .gen = cmd.gen,
                .finished_ms = status.fetched_ms,
                .latency_ms = latency_ms,
                .status = status,
                .trend = trend,
            };
            (void)xQueueSend(s_result_queue, &result, 0);
        }
        finish_gen(cmd.gen);
    }
}

esp_err_t app_net_init(void)
{
    if (s_cmd_queue) return ESP_OK;
    s_cmd_queue = xQueueCreate(1, sizeof(net_cmd_t));
    s_result_queue = xQueueCreate(2, sizeof(app_fetch_result_t));
    if (!s_cmd_queue || !s_result_queue) return ESP_ERR_NO_MEM;
    // HTTPS 握手在 mbedTLS 里栈开销较大,8 KB 容易溢出。
    if (xTaskCreate(net_task, "app_net", 12288, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool app_net_request(int index)
{
    if (!s_cmd_queue) return false;
    if (s_inflight) return false;
    // 先占位再入队:否则网络任务可能瞬间完成并清除标记,随后这里又把它置为 true。
    net_cmd_t cmd = { .index = index, .gen = ++s_gen };
    s_inflight = true;
    if (xQueueOverwrite(s_cmd_queue, &cmd) != pdTRUE) {
        s_inflight = false;
        return false;
    }
    return true;
}

void app_net_cancel(void)
{
    s_gen++;
    s_inflight = false;
}

bool app_net_busy(void)
{
    return s_inflight;
}

bool app_net_poll(app_fetch_result_t *out)
{
    if (!s_result_queue || !out) return false;
    // 丢弃已被取消的请求留下的结果,避免切页后又把旧数据写回界面。
    while (xQueueReceive(s_result_queue, out, 0) == pdTRUE) {
        if (out->gen == s_gen) return true;
    }
    return false;
}
