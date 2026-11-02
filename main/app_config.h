// main/app_config.h
// NVS 持久化配置。所有读写都必须持有 app_config_lock()。
#pragma once

#include "app_model.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t app_config_init(void);
void app_config_lock(void);
void app_config_unlock(void);
const app_config_t *app_config_get(void);
void app_config_snapshot(app_config_t *out);
esp_err_t app_config_save(void);
esp_err_t app_config_apply(app_config_t *cfg);
void app_config_reset(void);
bool app_config_ready(void);
