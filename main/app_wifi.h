// main/app_wifi.h
// 多网络 STA 管理 + 配网用 SoftAP(APSTA,STA 可保持在线)。
#pragma once

#include "app_model.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    APP_WIFI_OFF = 0,
    APP_WIFI_CONNECTING,
    APP_WIFI_CONNECTED,
    APP_WIFI_FAILED,
} app_wifi_state_t;

typedef struct {
    app_wifi_state_t state;
    char ssid[APP_SSID_MAX + 1];
    char ip[16];
    int rssi;
    bool configured;
    bool ap_active;
} app_wifi_info_t;

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} app_wifi_scan_item_t;

esp_err_t app_wifi_init(void);
// 同步扫描附近网络:按 SSID 去重、按信号从强到弱写入 out。
// 返回写入数量,失败返回 -1。扫描会阻塞约 1-3 秒,只应放在 HTTP 任务里调用。
int app_wifi_scan(app_wifi_scan_item_t *out, int max);
void app_wifi_get_info(app_wifi_info_t *out);
bool app_wifi_connect_saved(int index);
void app_wifi_reconnect(void);
bool app_wifi_start_ap(void);
void app_wifi_stop_ap(void);
bool app_wifi_ap_active(void);
const char *app_wifi_ap_ssid(void);
const char *app_wifi_ap_password(void);
bool app_wifi_ready(void);
void app_wifi_note_config_changed(void);
// 熄屏后放宽 Wi-Fi 省电档以降低待机电流，亮屏时恢复，避免刷新请求被省电拖慢。
void app_wifi_set_low_power(bool on);
