// main/app_portal.h
// 手机 SoftAP 网页配置服务。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t app_portal_start(void);
void app_portal_stop(void);
bool app_portal_active(void);
bool app_portal_exit_requested(void);
bool app_portal_take_config_changed(void);
uint32_t app_portal_elapsed_s(void);
