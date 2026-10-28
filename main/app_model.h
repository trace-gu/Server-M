// main/app_model.h
// 应用层共享数据模型与纯逻辑。本文件不得依赖 ESP-IDF 或 LVGL,
// 以便在主机上用 cc 直接编译 tests/test_app_model.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MAX_SERVERS 20
#define APP_MAX_WIFI 6
#define APP_NAME_MAX 48
#define APP_URL_MAX 191
#define APP_TOKEN_MAX 95
#define APP_SSID_MAX 32
#define APP_PASS_MAX 64
#define APP_TREND_POINTS 288
#define APP_TREND_BARS 24

#define APP_REFRESH_MIN_S 5
#define APP_REFRESH_MAX_S 300
#define APP_DEFAULT_REFRESH_S 10
#define APP_DEFAULT_SCREEN_OFF_S 60
#define APP_BRIGHTNESS_MIN 10
#define APP_BRIGHTNESS_MAX 100
#define APP_DEFAULT_BRIGHTNESS 65

typedef struct {
    char name[APP_NAME_MAX + 1];
    char url[APP_URL_MAX + 1];
    char token[APP_TOKEN_MAX + 1];
} app_server_cfg_t;

typedef struct {
    char ssid[APP_SSID_MAX + 1];
    char pass[APP_PASS_MAX + 1];
} app_wifi_cfg_t;

typedef struct {
    uint16_t version;
    uint8_t server_count;
    uint8_t wifi_count;
    uint8_t active_wifi;
    uint16_t refresh_s;
    uint16_t screen_off_s;   // 0 = 常亮
    app_server_cfg_t servers[APP_MAX_SERVERS];
    app_wifi_cfg_t wifi[APP_MAX_WIFI];
    // 追加在末尾:旧固件的 NVS blob 更短,读入后本字段为 0,由 sanitize 补默认值。
    uint8_t brightness;      // 背光百分比 10..100
} app_config_t;

// 一台服务器最近一次拉取到的状态。0 值代表未知。
typedef struct {
    bool valid;
    bool online;
    int64_t fetched_ms;      // esp_timer 毫秒, 0 = 从未成功
    uint32_t server_ts;      // 服务器上报的 Unix 时间
    int8_t cpu;              // 0..100, -1 未知
    int8_t mem;
    int8_t swap;
    int8_t disk;
    int16_t load1_x100;      // 0.42 -> 42
    uint32_t net_rx_bps;
    uint32_t net_tx_bps;
    uint32_t uptime_s;
    int16_t temp_c_x10;      // 45.2C -> 452
    int16_t ssl_days;        // -1 未知
    int16_t docker_run;      // -1 未知
    int16_t docker_total;    // 运行中 + 已停止, -1 未知
    int16_t docker_bad;      // 非 running 的容器数, -1 未知
    int8_t backup_state;     // -1 未知, 0 失败, 1 正常
    char panel[16];
    char err[48];
} app_server_status_t;

// 24 小时趋势。values 中 0xFF 表示该桶没有样本。
typedef struct {
    uint16_t step_s;
    uint16_t count;
    uint8_t cpu[APP_TREND_POINTS];
    uint8_t mem[APP_TREND_POINTS];
} app_trend_t;

typedef enum {
    APP_LEVEL_OK = 0,
    APP_LEVEL_WARN,
    APP_LEVEL_CRIT,
} app_level_t;

typedef struct {
    bool screen_on;
    bool detail_active;
    bool have_server;
    bool inflight;
    int64_t now_ms;
    int64_t last_start_ms;
    int64_t last_result_ms;
    uint32_t interval_s;
} app_refresh_ctx_t;

// -------- 配置校验 --------
bool app_utf8_valid(const char *text);
size_t app_utf8_truncate(char *dst, size_t dst_size, const char *src);
bool app_url_valid(const char *url);
bool app_name_valid(const char *name);
bool app_token_valid(const char *token);
void app_config_sanitize(app_config_t *cfg);
int app_server_add(app_config_t *cfg, const char *name, const char *url, const char *token);
bool app_server_update(app_config_t *cfg, int index, const char *name, const char *url, const char *token);
bool app_server_remove(app_config_t *cfg, int index);
int app_wifi_add(app_config_t *cfg, const char *ssid, const char *pass);
bool app_wifi_remove(app_config_t *cfg, int index);
bool app_wifi_set_active(app_config_t *cfg, int index);
int app_wifi_find(const app_config_t *cfg, const char *ssid);

// -------- 导航与刷新策略 --------
int app_selection_move(int selected, int count, int delta);
bool app_refresh_due(const app_refresh_ctx_t *ctx);
uint32_t app_refresh_interval_ms(uint32_t interval_s);

// 把 count 个采样压成 bars 个柱。没有样本的柱写 0xFF。
void app_trend_reduce(const uint8_t *src, uint16_t count, uint8_t *dst, uint16_t bars);
// 解析趋势数组时把字符串数值限制在 0..100。
int app_clamp_percent(long value);

// -------- 显示格式化 --------
void app_fmt_percent(char *buf, size_t size, int value);
void app_fmt_rate(char *buf, size_t size, uint32_t bytes_per_s);
void app_fmt_uptime(char *buf, size_t size, uint32_t seconds);
void app_fmt_load(char *buf, size_t size, int value_x100);
void app_fmt_temp(char *buf, size_t size, int value_x10);
void app_fmt_ago(char *buf, size_t size, int64_t age_ms);
void app_fmt_days(char *buf, size_t size, int days);
const char *app_level_color_name(app_level_t level);
app_level_t app_level_from_percent(int value);
const char *app_status_text(const app_server_status_t *status);
