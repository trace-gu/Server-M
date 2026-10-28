// main/app_portal.h
// 手机 SoftAP 网页配置服务。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t app_portal_start(void);
void app_portal_stop(void);
bool app_portal_active(void);
// 手机应访问的地址:连着 Wi-Fi 时是本机局域网 IP,否则是 SoftAP 的 192.168.4.1。
const char *app_portal_ip(void);
// true 表示本次配网复用了已连接的 Wi-Fi,没有另开热点。
bool app_portal_sta_mode(void);
bool app_portal_exit_requested(void);
bool app_portal_take_config_changed(void);
uint32_t app_portal_elapsed_s(void);
