// main/app_http.h
// 单台服务器的 HTTPS 拉取与 JSON 解析。
#pragma once

#include "app_model.h"
#include "esp_err.h"

esp_err_t app_fetch_status(const app_server_cfg_t *srv, app_server_status_t *out,
                           app_trend_t *trend, int *latency_ms);
bool app_http_time_ready(void);
