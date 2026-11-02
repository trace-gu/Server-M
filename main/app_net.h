// main/app_net.h
// 单请求网络任务:同一时刻只拉取“当前显示”的那台服务器。
#pragma once

#include "app_model.h"
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    int index;
    uint32_t gen;
    int64_t finished_ms;
    int latency_ms;
    app_server_status_t status;
    app_trend_t trend;
} app_fetch_result_t;

esp_err_t app_net_init(void);
bool app_net_request(int index);
void app_net_cancel(void);
bool app_net_busy(void);
bool app_net_poll(app_fetch_result_t *out);
