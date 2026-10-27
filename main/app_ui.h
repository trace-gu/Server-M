// main/app_ui.h
// 重新设计的服务器监控界面：仪表盘首页、设置、Wi-Fi 与手机配网四个页面。
// 所有函数都必须在持有 bsp_lvgl_lock() 时调用。
#pragma once

#include "app_model.h"
#include "app_wifi.h"
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    APP_VIEW_HOME = 0,
    APP_VIEW_SETTINGS,
    APP_VIEW_WIFI,
    APP_VIEW_PORTAL,
} app_view_t;

// 初始化视图层（字体覆盖自检）。不创建页面。
esp_err_t app_ui_init(void);

// 首页：服务器仪表盘。仅显示当前选中的那一台。
void app_ui_home_show(const app_config_t *cfg, int selected,
                      const app_server_status_t *status, const app_trend_t *trend,
                      int latency_ms, const app_wifi_info_t *wifi, int battery_soc);
void app_ui_home_update(const app_config_t *cfg, int selected,
                        const app_server_status_t *status, const app_trend_t *trend,
                        int latency_ms, const app_wifi_info_t *wifi, int battery_soc);

// 设置页：进入时重建，之后原地更新选中项与数值。
void app_ui_settings_show(const app_config_t *cfg, const app_wifi_info_t *wifi,
                          int selected, int battery_soc);
void app_ui_settings_update(const app_config_t *cfg, const app_wifi_info_t *wifi,
                            int selected, int battery_soc);

// Wi-Fi 页：第 0 行是手机网页配网，其余是已保存网络。
void app_ui_wifi_show(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected);
void app_ui_wifi_update(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected);

// 手机配网页。
void app_ui_portal_show(const char *ssid, const char *pass, uint32_t elapsed_s, bool ap_active);
void app_ui_portal_update(uint32_t elapsed_s, bool ap_active);
