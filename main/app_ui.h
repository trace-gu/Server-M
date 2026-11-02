// main/app_ui.h
// 重新设计的服务器监控界面：首屏是服务器列表，进入单台后是仪表盘详情。
// 所有函数都必须在持有 bsp_lvgl_lock() 时调用。
#pragma once

#include "app_model.h"
#include "app_wifi.h"
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    APP_VIEW_LIST = 0,      // 首屏：服务器列表（名称 + CPU/内存条）
    APP_VIEW_DETAIL,        // 单台服务器详情
    APP_VIEW_SETTINGS,
    APP_VIEW_WIFI,
    APP_VIEW_PORTAL,
} app_view_t;

// 初始化视图层（字体覆盖自检）。不创建页面。
esp_err_t app_ui_init(void);

// 服务器列表。statuses 是 APP_MAX_SERVERS 长度的数组,未拉取过的项 valid=false。
void app_ui_list_show(const app_config_t *cfg, int selected,
                      const app_server_status_t *statuses,
                      const app_wifi_info_t *wifi, int battery_soc);
void app_ui_list_update(const app_config_t *cfg, int selected,
                        const app_server_status_t *statuses,
                        const app_wifi_info_t *wifi, int battery_soc);

// 详情页：只显示当前这一台。
void app_ui_detail_show(const app_config_t *cfg, int selected,
                        const app_server_status_t *status, const app_trend_t *trend,
                        int latency_ms, const app_wifi_info_t *wifi, int battery_soc);
void app_ui_detail_update(const app_config_t *cfg, int selected,
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

// 手机配网页。url 是手机应访问的地址(局域网 IP 或热点地址)。
void app_ui_portal_show(const char *ssid, const char *pass, const char *url,
                        uint32_t elapsed_s, bool ap_active);
void app_ui_portal_update(uint32_t elapsed_s, bool ap_active);

// 配网启动失败时在设置页第 4 行短暂显示"启动失败",避免按了确定却毫无反馈。
void app_ui_portal_error(void);
