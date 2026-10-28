// main/app_ui.c —— 服务器监控应用的界面层。
//
// 视觉方向：深色仪表盘 + 青色强调，首页一次只呈现一台服务器的实时状态，
// 与基线 demo 的浅色测试菜单和 ui_pixel 外壳完全不同。
// 免责：所有函数都要求调用方已持有 bsp_lvgl_lock()。
#include "app_ui.h"

#include "app_model.h"

#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

static const char *TAG = "app_ui";

#define C_BG       0x0B1220u
#define C_BODY     0x0B1220u
#define C_CARD     0x141E30u
#define C_CARD_SEL 0x1D2B45u
#define C_LINE     0x24334Cu
#define C_INK      0xE9EFF8u
#define C_MUTED    0x8FA2BCu
#define C_ACCENT   0x3AD7C4u
#define C_OK       0x46D68C
#define C_WARN     0xF3B94B
#define C_CRIT     0xFF5F6D

#define HOME_BODY_Y 36
#define HOME_BODY_H 284
#define WIFI_ROWS   (1 + APP_MAX_WIFI)

static lv_obj_t *s_home;
static lv_obj_t *s_home_body;
static lv_obj_t *s_home_empty;
static lv_obj_t *s_home_batt;
static lv_obj_t *s_home_name;
static lv_obj_t *s_home_idx;
static lv_obj_t *s_home_state;
static lv_obj_t *s_home_extra;
static lv_obj_t *s_arc_cpu;
static lv_obj_t *s_arc_mem;
static lv_obj_t *s_arc_cpu_pct;
static lv_obj_t *s_arc_mem_pct;
static lv_obj_t *s_home_disk;
static lv_obj_t *s_home_swap;
static lv_obj_t *s_home_load;
static lv_obj_t *s_home_temp;
static lv_obj_t *s_home_rx;
static lv_obj_t *s_home_tx;
static lv_obj_t *s_home_uptime;
static lv_obj_t *s_chart_cpu;
static lv_obj_t *s_chart_mem;
static lv_chart_series_t *s_ser_cpu;
static lv_chart_series_t *s_ser_mem;
static int64_t s_chart_fetched_ms;
static int s_chart_index = -1;

static lv_obj_t *s_settings;
static lv_obj_t *s_set_row[7];
static lv_obj_t *s_set_val[7];

static lv_obj_t *s_wifi;
static lv_obj_t *s_wifi_state;
static lv_obj_t *s_wifi_row[WIFI_ROWS];
static lv_obj_t *s_wifi_name[WIFI_ROWS];
static lv_obj_t *s_wifi_val[WIFI_ROWS];

static lv_obj_t *s_portal;
static lv_obj_t *s_portal_ssid;
static lv_obj_t *s_portal_pass;
static lv_obj_t *s_portal_elapsed;

static const char *const SET_LABELS[7] = {
    "Wi-Fi 网络",
    "屏幕亮度",
    "刷新间隔",
    "自动熄屏",
    "手机配网",
    "设备信息",
    "返回主界面",
};

static lv_obj_t *screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              uint32_t color, int x, int y, int width)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (width > 0) {
        lv_obj_set_width(label, width);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }
    lv_obj_set_pos(label, x, y);
    return label;
}

static void text_set(lv_obj_t *label, const char *text)
{
    if (!label) return;
    const char *current = lv_label_get_text(label);
    if (!current || strcmp(current, text) != 0) lv_label_set_text(label, text);
}

static void text_center(lv_obj_t *label)
{
    if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void text_right(lv_obj_t *label)
{
    if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
}

static void screen_swap(lv_obj_t **slot, lv_obj_t *next)
{
    lv_obj_t *old = *slot;
    *slot = next;
    lv_screen_load(next);
    if (old) lv_obj_delete(old);
}

// 只缓存当前页,避免首页/设置/Wi-Fi 页同时常驻挤爆 LVGL 堆。
// 配网页由 portal_show 单独管理,不在这里删除。
static void drop_screen(lv_obj_t **slot)
{
    if (*slot) {
        lv_obj_delete(*slot);
        *slot = NULL;
    }
}

static uint32_t level_color(int value)
{
    switch (app_level_from_percent(value)) {
    case APP_LEVEL_CRIT: return C_CRIT;
    case APP_LEVEL_WARN: return C_WARN;
    default: return C_OK;
    }
}

// ---------------------------------------------------------------------------
// 字体覆盖自检
// ---------------------------------------------------------------------------
static uint32_t utf8_next(const char **cursor)
{
    const unsigned char *p = (const unsigned char *)*cursor;
    if (!p || p[0] == 0) return 0;
    uint32_t cp;
    int extra;
    if (p[0] < 0x80) {
        cp = p[0];
        extra = 0;
    } else if ((p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F;
        extra = 1;
    } else if ((p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F;
        extra = 2;
    } else if ((p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07;
        extra = 3;
    } else {
        *cursor += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *cursor += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *cursor += extra + 1;
    return cp;
}

static bool font_has_glyph(const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    memset(&dsc, 0, sizeof(dsc));
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0)) return false;
    return !dsc.is_placeholder;
}

static void font_check_text(const lv_font_t *font, const char *name, const char *text)
{
    const char *cursor = text;
    int missing = 0;
    while (cursor && *cursor) {
        uint32_t cp = utf8_next(&cursor);
        if (cp == 0) break;
        if (cp < 0x80) continue;
        if (!font_has_glyph(font, cp)) {
            ESP_LOGE(TAG, "%s 缺少字形 U+%04X", name, (unsigned)cp);
            missing++;
        }
    }
    if (missing == 0) ESP_LOGI(TAG, "%s 中文字形覆盖检查通过", name);
}

esp_err_t app_ui_init(void)
{
    static const char *probe =
        "服务器监控电量在线离线等待数据毫秒磁盘交换负载温度下行上行运行时长天时备份正常失败"
        "还没有服务器长按确定进入设置用手机配网添加未配置未连接连接中已连接已保存正在常亮开启"
        "刷新间隔自动熄屏设备信息返回主界面网络热点名称密码浏览器打开已连接热点后按网页提示配置"
        "和都可上下切换请先在拉取无法面板刚刚秒前分前时前今天连接失败"
        "使命内状尚退获取屏幕亮度";
    font_check_text(&app_font_16, "app_font_16", probe);
    font_check_text(&app_font_20, "app_font_20", probe);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 首页
// ---------------------------------------------------------------------------
static lv_obj_t *arc_create(lv_obj_t *parent, int x, int y, int size)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    return arc;
}

static lv_obj_t *chart_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_pos(chart, x, y);
    lv_obj_set_size(chart, w, h);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, APP_TREND_BARS);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_pad_column(chart, 2, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(color), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(chart, 1, LV_PART_ITEMS);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    return chart;
}

static void home_build(void)
{
    lv_obj_t *scr = screen_create();

    label_create(scr, "服务器监控", &app_font_20, C_INK, 10, 6, 0);
    s_home_batt = label_create(scr, "电量 --", &app_font_16, C_MUTED, 146, 10, 84);
    text_right(s_home_batt);
    lv_obj_t *rule = panel_create(scr, 10, 34, 220, 1, C_LINE);
    lv_obj_set_style_radius(rule, 0, 0);

    s_home_body = lv_obj_create(scr);
    lv_obj_set_pos(s_home_body, 0, HOME_BODY_Y);
    lv_obj_set_size(s_home_body, 240, HOME_BODY_H);
    lv_obj_set_style_bg_opa(s_home_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_body, 0, 0);
    lv_obj_set_style_pad_all(s_home_body, 0, 0);
    lv_obj_remove_flag(s_home_body, LV_OBJ_FLAG_SCROLLABLE);

    s_home_name = label_create(s_home_body, "未命名服务器", &app_font_20, C_INK, 10, 4, 158);
    s_home_idx = label_create(s_home_body, "0/0", &app_font_16, C_MUTED, 180, 8, 50);
    text_right(s_home_idx);
    s_home_state = label_create(s_home_body, "等待 Wi-Fi", &app_font_16, C_MUTED, 10, 28, 220);
    s_home_extra = label_create(s_home_body, "请在设置中配置", &app_font_16, C_MUTED, 10, 46, 220);

    s_arc_cpu = arc_create(s_home_body, 22, 64, 76);
    s_arc_mem = arc_create(s_home_body, 142, 64, 76);
    // 16px 字体的行高是 20px,每行至少间隔 20px,否则相邻文字会压在一起。
    s_arc_cpu_pct = label_create(s_home_body, "--", &app_font_20, C_INK, 22, 84, 76);
    text_center(s_arc_cpu_pct);
    s_arc_mem_pct = label_create(s_home_body, "--", &app_font_20, C_INK, 142, 84, 76);
    text_center(s_arc_mem_pct);
    lv_obj_t *cpu_name = label_create(s_home_body, "CPU", &app_font_16, C_MUTED, 22, 112, 76);
    text_center(cpu_name);
    lv_obj_t *mem_name = label_create(s_home_body, "内存", &app_font_16, C_MUTED, 142, 112, 76);
    text_center(mem_name);

    label_create(s_home_body, "磁盘", &app_font_16, C_MUTED, 10, 144, 40);
    s_home_disk = label_create(s_home_body, "--", &app_font_16, C_INK, 54, 144, 56);
    label_create(s_home_body, "交换", &app_font_16, C_MUTED, 124, 144, 40);
    s_home_swap = label_create(s_home_body, "--", &app_font_16, C_INK, 168, 144, 52);

    label_create(s_home_body, "负载", &app_font_16, C_MUTED, 10, 164, 40);
    s_home_load = label_create(s_home_body, "--", &app_font_16, C_INK, 54, 164, 56);
    label_create(s_home_body, "温度", &app_font_16, C_MUTED, 124, 164, 40);
    s_home_temp = label_create(s_home_body, "--", &app_font_16, C_INK, 168, 164, 58);

    label_create(s_home_body, "下行", &app_font_16, C_MUTED, 10, 184, 40);
    s_home_rx = label_create(s_home_body, "--", &app_font_16, C_INK, 54, 184, 66);
    label_create(s_home_body, "上行", &app_font_16, C_MUTED, 124, 184, 40);
    s_home_tx = label_create(s_home_body, "--", &app_font_16, C_INK, 168, 184, 58);

    label_create(s_home_body, "运行时长", &app_font_16, C_MUTED, 10, 204, 72);
    s_home_uptime = label_create(s_home_body, "--", &app_font_16, C_INK, 86, 204, 140);

    label_create(s_home_body, "CPU", &app_font_16, C_MUTED, 6, 224, 30);
    s_chart_cpu = chart_create(s_home_body, 36, 228, 194, 10, C_ACCENT);
    s_ser_cpu = lv_chart_add_series(s_chart_cpu, lv_color_hex(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
    label_create(s_home_body, "内存", &app_font_16, C_MUTED, 6, 244, 30);
    s_chart_mem = chart_create(s_home_body, 36, 248, 194, 10, C_WARN);
    s_ser_mem = lv_chart_add_series(s_chart_mem, lv_color_hex(C_WARN), LV_CHART_AXIS_PRIMARY_Y);

    lv_obj_t *hint = label_create(s_home_body, "上下切换  长按确定设置", &app_font_16, C_MUTED,
                                  10, 264, 220);
    text_center(hint);

    s_home_empty = lv_obj_create(scr);
    lv_obj_set_pos(s_home_empty, 0, HOME_BODY_Y);
    lv_obj_set_size(s_home_empty, 240, HOME_BODY_H);
    lv_obj_set_style_bg_opa(s_home_empty, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home_empty, 0, 0);
    lv_obj_set_style_pad_all(s_home_empty, 0, 0);
    lv_obj_remove_flag(s_home_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *empty_title = label_create(s_home_empty, "还没有服务器", &app_font_20, C_INK,
                                         0, 64, 240);
    text_center(empty_title);
    lv_obj_t *empty_hint = label_create(s_home_empty, "用手机配网添加服务器", &app_font_16,
                                        C_MUTED, 0, 104, 240);
    text_center(empty_hint);
    lv_obj_t *empty_key = label_create(s_home_empty, "长按确定进入设置", &app_font_16,
                                       C_ACCENT, 0, 132, 240);
    text_center(empty_key);

    screen_swap(&s_home, scr);
}

static void set_percent(lv_obj_t *label, int value)
{
    char buf[8];
    app_fmt_percent(buf, sizeof(buf), value);
    text_set(label, buf);
}

static void set_arc(lv_obj_t *arc, lv_obj_t *label, int value)
{
    int safe = value < 0 ? 0 : (value > 100 ? 100 : value);
    if (lv_arc_get_value(arc) != safe) lv_arc_set_value(arc, safe);
    uint32_t color = value < 0 ? C_MUTED : level_color(safe);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    set_percent(label, value);
}

static void trend_apply(lv_obj_t *chart, lv_chart_series_t *series,
                        const app_trend_t *trend, const uint8_t *values)
{
    uint8_t bars[APP_TREND_BARS];
    uint16_t count = trend ? trend->count : 0;
    app_trend_reduce(values, count, bars, APP_TREND_BARS);
    for (uint16_t i = 0; i < APP_TREND_BARS; i++) {
        lv_chart_set_series_value_by_id(chart, series, i, bars[i] == 0xFF ? 0 : bars[i]);
    }
    lv_chart_refresh(chart);
}

static void home_update(const app_config_t *cfg, int selected,
                        const app_server_status_t *status, const app_trend_t *trend,
                        int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    if (!s_home || !cfg || !wifi) return;

    char buf[96];
    if (battery_soc >= 0) snprintf(buf, sizeof(buf), "电量 %d%%", battery_soc);
    else snprintf(buf, sizeof(buf), "电量 --");
    text_set(s_home_batt, buf);

    if (cfg->server_count == 0) {
        lv_obj_add_flag(s_home_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_home_empty, LV_OBJ_FLAG_HIDDEN);
        text_set(s_home_idx, "0/0");
        return;
    }

    lv_obj_remove_flag(s_home_body, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_home_empty, LV_OBJ_FLAG_HIDDEN);

    if (selected < 0 || selected >= cfg->server_count) selected = 0;
    text_set(s_home_name, cfg->servers[selected].name);
    snprintf(buf, sizeof(buf), "%d/%d", selected + 1, cfg->server_count);
    text_set(s_home_idx, buf);

    if (wifi->state != APP_WIFI_CONNECTED) {
        if (wifi->state == APP_WIFI_CONNECTING) text_set(s_home_state, "Wi-Fi 连接中");
        else text_set(s_home_state, "Wi-Fi 未连接");
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(C_WARN), 0);
        text_set(s_home_extra, "请先在设置中连接 Wi-Fi");
        lv_obj_set_style_text_color(s_home_extra, lv_color_hex(C_MUTED), 0);
    } else if (!status || !status->valid) {
        text_set(s_home_state, "等待数据");
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(C_MUTED), 0);
        text_set(s_home_extra, "正在拉取服务器状态");
        lv_obj_set_style_text_color(s_home_extra, lv_color_hex(C_MUTED), 0);
    } else if (!status->online) {
        text_set(s_home_state, "离线");
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(C_CRIT), 0);
        text_set(s_home_extra, status->err[0] ? status->err : "无法连接服务器");
        lv_obj_set_style_text_color(s_home_extra, lv_color_hex(C_CRIT), 0);
    } else {
        char ago[24] = "";
        if (status->fetched_ms > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            app_fmt_ago(ago, sizeof(ago), now_ms - status->fetched_ms);
        }
        if (status->panel[0] && ago[0]) {
            snprintf(buf, sizeof(buf), "在线  %d毫秒  %s  %s", latency_ms, status->panel, ago);
        } else if (status->panel[0]) {
            snprintf(buf, sizeof(buf), "在线  %d毫秒  %s", latency_ms, status->panel);
        } else if (ago[0]) {
            snprintf(buf, sizeof(buf), "在线  %d毫秒  %s", latency_ms, ago);
        } else {
            snprintf(buf, sizeof(buf), "在线  %d毫秒", latency_ms);
        }
        text_set(s_home_state, buf);
        lv_obj_set_style_text_color(s_home_state, lv_color_hex(C_OK), 0);

        buf[0] = '\0';
        if (status->docker_run >= 0) {
            snprintf(buf, sizeof(buf), "Docker %d/%d", status->docker_run,
                     status->docker_bad >= 0 ? status->docker_bad : 0);
        }
        if (status->ssl_days >= 0) {
            char days[16];
            app_fmt_days(days, sizeof(days), status->ssl_days);
            size_t used = strlen(buf);
            snprintf(buf + used, sizeof(buf) - used, "%sSSL %s", used ? "   " : "", days);
        }
        if (status->backup_state >= 0) {
            size_t used = strlen(buf);
            snprintf(buf + used, sizeof(buf) - used, "%s%s", used ? "   " : "",
                     status->backup_state ? "备份正常" : "备份失败");
        }
        if (buf[0] == '\0') snprintf(buf, sizeof(buf), "数据正常");
        text_set(s_home_extra, buf);
        lv_obj_set_style_text_color(s_home_extra, lv_color_hex(C_MUTED), 0);
    }

    bool have_metrics = status && status->valid && status->cpu >= 0;
    set_arc(s_arc_cpu, s_arc_cpu_pct, have_metrics ? status->cpu : -1);
    set_arc(s_arc_mem, s_arc_mem_pct, have_metrics ? status->mem : -1);

    char tmp[24];
    set_percent(s_home_disk, have_metrics ? status->disk : -1);
    set_percent(s_home_swap, have_metrics ? status->swap : -1);
    app_fmt_load(tmp, sizeof(tmp), have_metrics ? status->load1_x100 : -1);
    text_set(s_home_load, tmp);
    app_fmt_temp(tmp, sizeof(tmp), have_metrics ? status->temp_c_x10 : -1);
    text_set(s_home_temp, tmp);
    if (have_metrics) {
        app_fmt_rate(tmp, sizeof(tmp), status->net_rx_bps);
        text_set(s_home_rx, tmp);
        app_fmt_rate(tmp, sizeof(tmp), status->net_tx_bps);
        text_set(s_home_tx, tmp);
        app_fmt_uptime(tmp, sizeof(tmp), status->uptime_s);
        text_set(s_home_uptime, tmp);
    } else {
        text_set(s_home_rx, "--");
        text_set(s_home_tx, "--");
        text_set(s_home_uptime, "--");
    }

    bool chart_valid = have_metrics && status->fetched_ms > 0;
    if (selected != s_chart_index ||
        (chart_valid && status->fetched_ms != s_chart_fetched_ms)) {
        s_chart_index = selected;
        s_chart_fetched_ms = chart_valid ? status->fetched_ms : 0;
        if (chart_valid && trend) {
            trend_apply(s_chart_cpu, s_ser_cpu, trend, trend->cpu);
            trend_apply(s_chart_mem, s_ser_mem, trend, trend->mem);
        } else {
            app_trend_t empty;
            memset(&empty, 0, sizeof(empty));
            trend_apply(s_chart_cpu, s_ser_cpu, &empty, empty.cpu);
            trend_apply(s_chart_mem, s_ser_mem, &empty, empty.mem);
        }
    }
}

void app_ui_home_show(const app_config_t *cfg, int selected,
                      const app_server_status_t *status, const app_trend_t *trend,
                      int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    if (!s_home) home_build();
    home_update(cfg, selected, status, trend, latency_ms, wifi, battery_soc);
    lv_screen_load(s_home);
    drop_screen(&s_settings);
    drop_screen(&s_wifi);
}

void app_ui_home_update(const app_config_t *cfg, int selected,
                        const app_server_status_t *status, const app_trend_t *trend,
                        int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    home_update(cfg, selected, status, trend, latency_ms, wifi, battery_soc);
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
static void header_add(lv_obj_t *scr, const char *title, const char *subtitle)
{
    label_create(scr, title, &app_font_20, C_INK, 10, 6, 0);
    lv_obj_t *sub = label_create(scr, subtitle, &app_font_16, C_MUTED, 156, 10, 74);
    text_right(sub);
    lv_obj_t *rule = panel_create(scr, 10, 34, 220, 1, C_LINE);
    lv_obj_set_style_radius(rule, 0, 0);
}

static const char *wifi_state_cn(const app_wifi_info_t *wifi)
{
    if (!wifi) return "未连接";
    switch (wifi->state) {
    case APP_WIFI_CONNECTED: return "已连接";
    case APP_WIFI_CONNECTING: return "连接中";
    case APP_WIFI_FAILED: return "连接失败";
    default: return "未连接";
    }
}

static lv_obj_t *settings_build(const app_config_t *cfg, const app_wifi_info_t *wifi,
                                int selected, int battery_soc)
{
    (void)cfg;
    (void)wifi;
    (void)selected;
    (void)battery_soc;
    lv_obj_t *scr = screen_create();
    header_add(scr, "设置", "长按返回");
    // 7 行必须压缩行距:40px 间距会超出 240px 屏幕。
    for (int i = 0; i < 7; i++) {
        s_set_row[i] = panel_create(scr, 10, 42 + i * 28, 220, 26, C_CARD);
        label_create(s_set_row[i], SET_LABELS[i], &app_font_16, C_INK, 12, 3, 118);
        s_set_val[i] = label_create(s_set_row[i], "", &app_font_16, C_MUTED, 128, 3, 78);
        text_right(s_set_val[i]);
    }
    return scr;
}

static void settings_apply(const app_config_t *cfg, const app_wifi_info_t *wifi,
                           int selected, int battery_soc)
{
    if (!s_settings) return;
    for (int i = 0; i < 7; i++) {
        bool sel = i == selected;
        lv_obj_set_style_bg_color(s_set_row[i], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_set_row[i], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_set_row[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_color(s_set_val[i], lv_color_hex(sel ? C_ACCENT : C_MUTED), 0);
    }

    char buf[64];
    if (wifi && wifi->configured) text_set(s_set_val[0], wifi_state_cn(wifi));
    else text_set(s_set_val[0], "未配置");

    snprintf(buf, sizeof(buf), "%u%%", (unsigned)cfg->brightness);
    text_set(s_set_val[1], buf);
    snprintf(buf, sizeof(buf), "%u 秒", (unsigned)cfg->refresh_s);
    text_set(s_set_val[2], buf);
    if (cfg->screen_off_s == 0) text_set(s_set_val[3], "常亮");
    else {
        snprintf(buf, sizeof(buf), "%u 秒", (unsigned)cfg->screen_off_s);
        text_set(s_set_val[3], buf);
    }
    text_set(s_set_val[4], "开启");
    if (battery_soc >= 0) snprintf(buf, sizeof(buf), "电量 %d%%", battery_soc);
    else snprintf(buf, sizeof(buf), "电量 --");
    text_set(s_set_val[5], buf);
    text_set(s_set_val[6], "");
}

void app_ui_settings_show(const app_config_t *cfg, const app_wifi_info_t *wifi,
                          int selected, int battery_soc)
{
    lv_obj_t *next = settings_build(cfg, wifi, selected, battery_soc);
    screen_swap(&s_settings, next);
    settings_apply(cfg, wifi, selected, battery_soc);
    drop_screen(&s_home);
    drop_screen(&s_wifi);
}

void app_ui_settings_update(const app_config_t *cfg, const app_wifi_info_t *wifi,
                            int selected, int battery_soc)
{
    settings_apply(cfg, wifi, selected, battery_soc);
}

// ---------------------------------------------------------------------------
// Wi-Fi 页
// ---------------------------------------------------------------------------
static lv_obj_t *wifi_build(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected)
{
    (void)selected;
    (void)wifi;
    int rows = 1 + cfg->wifi_count;
    if (rows > WIFI_ROWS) rows = WIFI_ROWS;
    lv_obj_t *scr = screen_create();
    header_add(scr, "Wi-Fi 网络", "长按返回");
    s_wifi_state = label_create(scr, "", &app_font_16, C_MUTED, 10, 38, 220);

    for (int i = 0; i < rows; i++) {
        s_wifi_row[i] = panel_create(scr, 10, 60 + i * 34, 220, 32, C_CARD);
        const char *name = i == 0 ? "手机网页配网" : cfg->wifi[i - 1].ssid;
        s_wifi_name[i] = label_create(s_wifi_row[i], name, &app_font_16, C_INK, 12, 6, 126);
        s_wifi_val[i] = label_create(s_wifi_row[i], "", &app_font_16, C_MUTED, 142, 6, 66);
        text_right(s_wifi_val[i]);
    }
    for (int i = rows; i < WIFI_ROWS; i++) {
        s_wifi_row[i] = NULL;
        s_wifi_name[i] = NULL;
        s_wifi_val[i] = NULL;
    }
    return scr;
}

static void wifi_apply(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected)
{
    if (!s_wifi) return;
    int rows = 1 + cfg->wifi_count;
    if (rows > WIFI_ROWS) rows = WIFI_ROWS;

    char buf[64];
    if (!wifi || !wifi->configured) snprintf(buf, sizeof(buf), "尚未保存 Wi-Fi");
    else if (wifi->state == APP_WIFI_CONNECTED && wifi->ip[0])
        snprintf(buf, sizeof(buf), "已连接 %s", wifi->ip);
    else snprintf(buf, sizeof(buf), "%s", wifi_state_cn(wifi));
    text_set(s_wifi_state, buf);
    lv_obj_set_style_text_color(s_wifi_state,
        lv_color_hex(wifi && wifi->state == APP_WIFI_CONNECTED ? C_OK : C_MUTED), 0);

    for (int i = 0; i < rows; i++) {
        bool sel = i == selected;
        lv_obj_set_style_bg_color(s_wifi_row[i], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_wifi_row[i], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_wifi_row[i], lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_text_color(s_wifi_val[i], lv_color_hex(sel ? C_ACCENT : C_MUTED), 0);

        if (i == 0) {
            text_set(s_wifi_val[i], "开启");
            continue;
        }
        int index = i - 1;
        bool active = cfg->active_wifi == index;
        if (active && wifi && wifi->state == APP_WIFI_CONNECTED) text_set(s_wifi_val[i], "已连接");
        else if (active && wifi && wifi->state == APP_WIFI_CONNECTING) text_set(s_wifi_val[i], "连接中");
        else if (active) text_set(s_wifi_val[i], "未连接");
        else text_set(s_wifi_val[i], "已保存");
    }
}

void app_ui_wifi_show(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected)
{
    lv_obj_t *next = wifi_build(cfg, wifi, selected);
    screen_swap(&s_wifi, next);
    wifi_apply(cfg, wifi, selected);
    drop_screen(&s_home);
    drop_screen(&s_settings);
}

void app_ui_wifi_update(const app_config_t *cfg, const app_wifi_info_t *wifi, int selected)
{
    wifi_apply(cfg, wifi, selected);
}

// ---------------------------------------------------------------------------
// 手机配网页
// ---------------------------------------------------------------------------
static void portal_build(void)
{
    lv_obj_t *scr = screen_create();
    header_add(scr, "手机配网", "长按退出");

    lv_obj_t *card = panel_create(scr, 10, 50, 220, 190, C_CARD);
    label_create(card, "热点名称", &app_font_16, C_MUTED, 16, 10, 188);
    s_portal_ssid = label_create(card, "AI-Passport", &app_font_20, C_INK, 16, 28, 188);
    label_create(card, "热点密码", &app_font_16, C_MUTED, 16, 60, 188);
    s_portal_pass = label_create(card, "--------", &app_font_20, C_INK, 16, 78, 188);
    label_create(card, "浏览器打开", &app_font_16, C_MUTED, 16, 110, 188);
    lv_obj_t *url = label_create(card, "http://192.168.4.1", &app_font_20, C_ACCENT, 16, 128, 188);
    (void)url;
    s_portal_elapsed = label_create(card, "已开启 0 秒", &app_font_16, C_MUTED, 16, 158, 188);

    lv_obj_t *hint1 = label_create(scr, "连接热点后按网页提示配置", &app_font_16, C_INK,
                                   10, 250, 220);
    text_center(hint1);
    lv_obj_t *hint2 = label_create(scr, "服务器和 Wi-Fi 都可添加", &app_font_16, C_MUTED,
                                   10, 272, 220);
    text_center(hint2);
    lv_obj_t *exit = label_create(scr, "长按确定退出配网", &app_font_16, C_ACCENT,
                                  10, 298, 220);
    text_center(exit);
    s_portal = scr;
}

static void portal_apply(uint32_t elapsed_s, bool ap_active)
{
    if (!s_portal) return;
    char buf[48];
    if (ap_active) snprintf(buf, sizeof(buf), "已开启 %u 秒", (unsigned)elapsed_s);
    else snprintf(buf, sizeof(buf), "配网热点未开启");
    text_set(s_portal_elapsed, buf);
    lv_obj_set_style_text_color(s_portal_elapsed, lv_color_hex(ap_active ? C_OK : C_CRIT), 0);
}

void app_ui_portal_show(const char *ssid, const char *pass, uint32_t elapsed_s, bool ap_active)
{
    if (!s_portal) portal_build();
    text_set(s_portal_ssid, ssid && ssid[0] ? ssid : "AI-Passport");
    text_set(s_portal_pass, pass && pass[0] ? pass : "--------");
    portal_apply(elapsed_s, ap_active);
    lv_screen_load(s_portal);
    drop_screen(&s_home);
    drop_screen(&s_settings);
    drop_screen(&s_wifi);
}

void app_ui_portal_update(uint32_t elapsed_s, bool ap_active)
{
    portal_apply(elapsed_s, ap_active);
}
