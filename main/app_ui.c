// main/app_ui.c
//
// 视觉方向：深色科技风，首屏为服务器列表（名称 + CPU/内存条），
// 确认进入单台详情（双圆弧 + 24 小时迷你趋势）。
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

#define C_BG       0x080D16u
#define C_CARD     0x121C2Du
#define C_CARD_SEL 0x1B2942u
#define C_LINE     0x22324Bu
#define C_INK      0xE9EFF8u
#define C_MUTED    0x7E93ADu
#define C_ACCENT   0x35E0C8u
#define C_OK       0x46D68C
#define C_WARN     0xF3B94B
#define C_CRIT     0xFF5F6D

// 列表：4 行一屏，超出时窗口跟随选中项滚动。
#define LIST_ROWS   4
#define LIST_Y0     38
#define LIST_PITCH  64
#define LIST_ROW_H  62
#define BAR_MAX_W   120
#define BAR_H       5

#define WIFI_ROWS   (1 + APP_MAX_WIFI)

// ---- 列表页控件 ----
static lv_obj_t *s_list;
static lv_obj_t *s_list_batt;
static lv_obj_t *s_list_empty;
static lv_obj_t *s_row[LIST_ROWS];
static lv_obj_t *s_row_name[LIST_ROWS];
static lv_obj_t *s_row_state[LIST_ROWS];
static lv_obj_t *s_row_cpu_pct[LIST_ROWS];
static lv_obj_t *s_row_mem_pct[LIST_ROWS];
static lv_obj_t *s_row_cpu_fill[LIST_ROWS];
static lv_obj_t *s_row_mem_fill[LIST_ROWS];

// ---- 详情页控件 ----
static lv_obj_t *s_detail;
static lv_obj_t *s_detail_name;
static lv_obj_t *s_detail_batt;
static lv_obj_t *s_detail_state;
static lv_obj_t *s_arc_cpu;
static lv_obj_t *s_arc_mem;
static lv_obj_t *s_arc_cpu_pct;
static lv_obj_t *s_arc_mem_pct;
static lv_obj_t *s_detail_disk;
static lv_obj_t *s_detail_diskio;
static lv_obj_t *s_detail_load;
static lv_obj_t *s_detail_docker;
static lv_obj_t *s_detail_rx;
static lv_obj_t *s_detail_tx;
static lv_obj_t *s_detail_uptime;
static lv_obj_t *s_spark_cpu;
static lv_obj_t *s_spark_mem;
static int64_t s_chart_fetched_ms;
static int s_chart_index = -1;

// ---- 设置 / Wi-Fi / 配网 ----
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

// 只缓存当前页,避免列表/详情/设置/Wi-Fi 页同时常驻挤爆 LVGL 堆。
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
        "服务器监控列表详情磁盘IO负载下行上行运行时长在线离线待拉取未联网电量"
        "长按设置返回刷新趋势当前台个已启动运行中暂无还没有服务器用手机配网添加"
        "屏幕亮度间隔自动熄屏网络热点名称密码浏览器打开已连接提示配置失败成功"
        "进入主界面数据正在等待毫秒刚刚秒前分前时前今天";
    font_check_text(&app_font_16, "app_font_16", probe);
    font_check_text(&app_font_20, "app_font_20", probe);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 通用小部件
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

// 趋势用自绘迷你柱状图:24 根柱由一次 DRAW_MAIN 画完,不额外占用 LVGL 对象内存。
#define SPARK_W 180
#define SPARK_H 12

typedef struct {
    uint8_t bars[APP_TREND_BARS];
    uint32_t color;
} spark_t;

static spark_t s_spark_cpu_data = { .color = C_ACCENT };
static spark_t s_spark_mem_data = { .color = C_WARN };

static void spark_draw(lv_event_t *e)
{
    spark_t *data = lv_event_get_user_data(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!data || !obj || !layer) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    int w = lv_area_get_width(&coords);
    int h = lv_area_get_height(&coords);
    if (w <= 0 || h <= 0) return;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.base.layer = layer;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 1;
    dsc.border_width = 0;

    int pitch = w / APP_TREND_BARS;
    if (pitch < 2) pitch = 2;
    int bar_w = pitch - 2;
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < APP_TREND_BARS; i++) {
        int value = data->bars[i];
        int bar_h;
        if (value == 0xFF) {
            bar_h = 1;
            dsc.bg_color = lv_color_hex(C_LINE);
        } else {
            bar_h = value * h / 100;
            if (bar_h < 1) bar_h = 1;
            dsc.bg_color = lv_color_hex(data->color);
        }
        lv_area_t bar;
        bar.x1 = coords.x1 + i * pitch;
        if (bar.x1 > coords.x2) break;
        bar.x2 = bar.x1 + bar_w - 1;
        if (bar.x2 > coords.x2) bar.x2 = coords.x2;
        bar.y2 = coords.y2;
        bar.y1 = coords.y2 - bar_h + 1;
        lv_draw_rect(layer, &dsc, &bar);
    }
}

static lv_obj_t *spark_create(lv_obj_t *parent, int x, int y, spark_t *data)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, SPARK_W, SPARK_H);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, spark_draw, LV_EVENT_DRAW_MAIN, data);
    return obj;
}

static void spark_set(lv_obj_t *obj, spark_t *data, const app_trend_t *trend,
                      const uint8_t *values)
{
    app_trend_reduce(values, trend ? trend->count : 0, data->bars, APP_TREND_BARS);
    if (obj) lv_obj_invalidate(obj);
}

static lv_obj_t *bar_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *bar = panel_create(parent, x, y, w, h, color);
    lv_obj_set_style_radius(bar, 3, 0);
    return bar;
}

static void set_percent(lv_obj_t *label, int value)
{
    char buf[8];
    app_fmt_percent(buf, sizeof(buf), value);
    text_set(label, buf);
}

static void set_bar(lv_obj_t *fill, int value)
{
    int safe = value < 0 ? 0 : (value > 100 ? 100 : value);
    lv_obj_set_width(fill, safe * BAR_MAX_W / 100);
}

// ---------------------------------------------------------------------------
// 服务器列表
// ---------------------------------------------------------------------------
static void header_add(lv_obj_t *scr, const char *title, const char *subtitle)
{
    label_create(scr, title, &app_font_20, C_INK, 10, 6, 0);
    lv_obj_t *sub = label_create(scr, subtitle, &app_font_16, C_MUTED, 120, 10, 110);
    text_right(sub);
    lv_obj_t *rule = panel_create(scr, 10, 34, 220, 1, C_LINE);
    lv_obj_set_style_radius(rule, 0, 0);
}

static void list_build(void)
{
    lv_obj_t *scr = screen_create();
    label_create(scr, "Server-M监控", &app_font_20, C_INK, 10, 6, 0);
    s_list_batt = label_create(scr, "电量 --", &app_font_16, C_MUTED, 146, 10, 84);
    text_right(s_list_batt);
    lv_obj_t *rule = panel_create(scr, 10, 34, 220, 1, C_LINE);
    lv_obj_set_style_radius(rule, 0, 0);

    for (int i = 0; i < LIST_ROWS; i++) {
        int y = LIST_Y0 + i * LIST_PITCH;
        s_row[i] = panel_create(scr, 8, y, 224, LIST_ROW_H, C_CARD);
        s_row_name[i] = label_create(s_row[i], "", &app_font_16, C_INK, 12, 0, 148);
        s_row_state[i] = label_create(s_row[i], "", &app_font_16, C_MUTED, 158, 0, 54);
        text_right(s_row_state[i]);

        label_create(s_row[i], "CPU", &app_font_16, C_MUTED, 12, 22, 34);
        bar_create(s_row[i], 50, 28, BAR_MAX_W, BAR_H, C_LINE);
        s_row_cpu_fill[i] = bar_create(s_row[i], 50, 28, 0, BAR_H, C_ACCENT);
        s_row_cpu_pct[i] = label_create(s_row[i], "--", &app_font_16, C_ACCENT, 174, 20, 38);
        text_right(s_row_cpu_pct[i]);

        label_create(s_row[i], "内存", &app_font_16, C_MUTED, 12, 40, 34);
        bar_create(s_row[i], 50, 46, BAR_MAX_W, BAR_H, C_LINE);
        s_row_mem_fill[i] = bar_create(s_row[i], 50, 46, 0, BAR_H, C_WARN);
        s_row_mem_pct[i] = label_create(s_row[i], "--", &app_font_16, C_WARN, 174, 38, 38);
        text_right(s_row_mem_pct[i]);
    }

    s_list_empty = lv_obj_create(scr);
    lv_obj_set_pos(s_list_empty, 0, LIST_Y0);
    lv_obj_set_size(s_list_empty, 240, 240);
    lv_obj_set_style_bg_opa(s_list_empty, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_empty, 0, 0);
    lv_obj_set_style_pad_all(s_list_empty, 0, 0);
    lv_obj_remove_flag(s_list_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = label_create(s_list_empty, "还没有服务器", &app_font_20, C_INK, 0, 56, 240);
    text_center(title);
    lv_obj_t *hint = label_create(s_list_empty, "用手机配网添加", &app_font_16, C_MUTED, 0, 96, 240);
    text_center(hint);
    lv_obj_t *key = label_create(s_list_empty, "长按确定进入设置", &app_font_16, C_ACCENT, 0, 124, 240);
    text_center(key);

    lv_obj_t *foot = label_create(scr, "长按确定设置", &app_font_16, C_MUTED, 10, 294, 220);
    text_center(foot);

    s_list = scr;
}

static const char *row_state_text(const app_server_status_t *st, const app_wifi_info_t *wifi)
{
    bool linked = wifi && wifi->state == APP_WIFI_CONNECTED;
    if (!st || !st->valid) return linked ? "待拉取" : "未联网";
    if (!st->online) return "离线";
    return "在线";
}

static uint32_t row_state_color(const char *text)
{
    if (strcmp(text, "在线") == 0) return C_OK;
    if (strcmp(text, "离线") == 0) return C_CRIT;
    if (strcmp(text, "未联网") == 0) return C_WARN;
    return C_MUTED;
}

static void list_apply(const app_config_t *cfg, int selected,
                       const app_server_status_t *statuses, const app_wifi_info_t *wifi,
                       int battery_soc)
{
    if (!s_list || !cfg) return;

    char buf[64];
    if (battery_soc >= 0) snprintf(buf, sizeof(buf), "电量 %d%%", battery_soc);
    else snprintf(buf, sizeof(buf), "电量 --");
    text_set(s_list_batt, buf);

    int count = cfg->server_count;
    if (count == 0) {
        lv_obj_add_flag(s_row[0], LV_OBJ_FLAG_HIDDEN);
        for (int i = 1; i < LIST_ROWS; i++) lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_list_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_list_empty, LV_OBJ_FLAG_HIDDEN);

    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    // 窗口滚动：选中项尽量保持在第二行,避免每按一下都跳屏。
    int win = selected - 1;
    if (win < 0) win = 0;
    if (win > count - LIST_ROWS) win = count - LIST_ROWS;
    if (win < 0) win = 0;

    for (int i = 0; i < LIST_ROWS; i++) {
        int index = win + i;
        if (index >= count) {
            lv_obj_add_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_row[i], LV_OBJ_FLAG_HIDDEN);

        bool sel = index == selected;
        lv_obj_set_style_bg_color(s_row[i], lv_color_hex(sel ? C_CARD_SEL : C_CARD), 0);
        lv_obj_set_style_border_width(s_row[i], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_row[i], lv_color_hex(C_ACCENT), 0);

        snprintf(buf, sizeof(buf), "%d  %s", index + 1, cfg->servers[index].name);
        text_set(s_row_name[i], buf);

        const app_server_status_t *st = statuses ? &statuses[index] : NULL;
        const char *state = row_state_text(st, wifi);
        text_set(s_row_state[i], state);
        lv_obj_set_style_text_color(s_row_state[i], lv_color_hex(row_state_color(state)), 0);

        bool known = st && st->valid && st->cpu >= 0 && st->online;
        set_percent(s_row_cpu_pct[i], known ? st->cpu : -1);
        set_percent(s_row_mem_pct[i], known ? st->mem : -1);
        set_bar(s_row_cpu_fill[i], known ? st->cpu : -1);
        set_bar(s_row_mem_fill[i], known ? st->mem : -1);
        lv_obj_set_style_bg_color(s_row_cpu_fill[i],
                                  lv_color_hex(known ? level_color(st->cpu) : C_LINE), 0);
    }
}

void app_ui_list_show(const app_config_t *cfg, int selected,
                      const app_server_status_t *statuses,
                      const app_wifi_info_t *wifi, int battery_soc)
{
    if (!s_list) list_build();
    list_apply(cfg, selected, statuses, wifi, battery_soc);
    lv_screen_load(s_list);
    drop_screen(&s_detail);
    drop_screen(&s_settings);
    drop_screen(&s_wifi);
}

void app_ui_list_update(const app_config_t *cfg, int selected,
                        const app_server_status_t *statuses,
                        const app_wifi_info_t *wifi, int battery_soc)
{
    list_apply(cfg, selected, statuses, wifi, battery_soc);
}

// ---------------------------------------------------------------------------
// 单台服务器详情
// ---------------------------------------------------------------------------
static void detail_build(void)
{
    lv_obj_t *scr = screen_create();
    s_detail_name = label_create(scr, "服务器", &app_font_20, C_INK, 10, 4, 148);
    s_detail_batt = label_create(scr, "电量 --", &app_font_16, C_MUTED, 156, 8, 74);
    text_right(s_detail_batt);
    lv_obj_t *rule = panel_create(scr, 10, 32, 220, 1, C_LINE);
    lv_obj_set_style_radius(rule, 0, 0);

    s_detail_state = label_create(scr, "等待数据", &app_font_16, C_MUTED, 10, 38, 220);

    // 双圆弧仪表:左右对称,下沿让给指标卡片。
    s_arc_cpu = arc_create(scr, 22, 60, 76);
    s_arc_mem = arc_create(scr, 142, 60, 76);
    s_arc_cpu_pct = label_create(scr, "--", &app_font_20, C_INK, 22, 80, 76);
    text_center(s_arc_cpu_pct);
    s_arc_mem_pct = label_create(scr, "--", &app_font_20, C_INK, 142, 80, 76);
    text_center(s_arc_mem_pct);
    lv_obj_t *cpu_name = label_create(scr, "CPU", &app_font_16, C_MUTED, 22, 108, 76);
    text_center(cpu_name);
    lv_obj_t *mem_name = label_create(scr, "内存", &app_font_16, C_MUTED, 142, 108, 76);
    text_center(mem_name);

    // 指标卡片:2 列 × 4 行,左列标签 + 右对齐数值,避免逐行散排。
    lv_obj_t *grid = panel_create(scr, 8, 136, 224, 92, C_CARD);
    for (int i = 0; i < 4; i++) {
        int y = 4 + i * 22;
        if (i < 3) {
            lv_obj_t *rule2 = panel_create(grid, 112, y + 9, 1, 14, C_LINE);
            lv_obj_set_style_radius(rule2, 0, 0);
        }
    }
    label_create(grid, "磁盘", &app_font_16, C_MUTED, 10, 4, 40);
    s_detail_disk = label_create(grid, "--", &app_font_16, C_INK, 44, 4, 60);
    text_right(s_detail_disk);
    label_create(grid, "磁盘IO", &app_font_16, C_MUTED, 116, 4, 52);
    s_detail_diskio = label_create(grid, "--", &app_font_16, C_INK, 168, 4, 48);
    text_right(s_detail_diskio);

    label_create(grid, "负载", &app_font_16, C_MUTED, 10, 26, 40);
    s_detail_load = label_create(grid, "--", &app_font_16, C_INK, 44, 26, 60);
    text_right(s_detail_load);
    label_create(grid, "容器", &app_font_16, C_MUTED, 116, 26, 52);
    s_detail_docker = label_create(grid, "--", &app_font_16, C_INK, 168, 26, 48);
    text_right(s_detail_docker);

    label_create(grid, "下行", &app_font_16, C_MUTED, 10, 48, 40);
    s_detail_rx = label_create(grid, "--", &app_font_16, C_INK, 44, 48, 60);
    text_right(s_detail_rx);
    label_create(grid, "上行", &app_font_16, C_MUTED, 116, 48, 52);
    s_detail_tx = label_create(grid, "--", &app_font_16, C_INK, 168, 48, 48);
    text_right(s_detail_tx);

    label_create(grid, "运行时长", &app_font_16, C_MUTED, 10, 70, 64);
    s_detail_uptime = label_create(grid, "--", &app_font_16, C_INK, 78, 70, 138);
    text_right(s_detail_uptime);

    // 24 小时趋势:自绘柱状图,两行各 20px 行高,不能靠得更近,否则文字会重叠。
    label_create(scr, "CPU", &app_font_16, C_MUTED, 10, 232, 34);
    s_spark_cpu = spark_create(scr, 48, 236, &s_spark_cpu_data);
    label_create(scr, "内存", &app_font_16, C_MUTED, 10, 254, 34);
    s_spark_mem = spark_create(scr, 48, 258, &s_spark_mem_data);

    lv_obj_t *foot = label_create(scr, "确定刷新   长按确定返回", &app_font_16, C_MUTED,
                                  10, 286, 220);
    text_center(foot);

    s_chart_index = -1;
    s_chart_fetched_ms = 0;
    s_detail = scr;
}

static void set_rate(lv_obj_t *label, uint32_t bps, bool known)
{
    if (!known) {
        text_set(label, "--");
        return;
    }
    char buf[24];
    app_fmt_rate(buf, sizeof(buf), bps);
    // 去掉单位前的空格,把 "1.2 M/s" 压成 "1.2M/s",窄列也放得下。
    char compact[24];
    size_t j = 0;
    for (size_t i = 0; buf[i] && j + 1 < sizeof(compact); i++) {
        if (buf[i] != ' ') compact[j++] = buf[i];
    }
    compact[j] = 0;
    text_set(label, compact);
}

static void set_arc(lv_obj_t *arc, lv_obj_t *label, int value)
{
    int safe = value < 0 ? 0 : (value > 100 ? 100 : value);
    if (lv_arc_get_value(arc) != safe) lv_arc_set_value(arc, safe);
    uint32_t color = value < 0 ? C_MUTED : level_color(safe);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    set_percent(label, value);
}

static void detail_apply(const app_config_t *cfg, int selected,
                         const app_server_status_t *status, const app_trend_t *trend,
                         int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    if (!s_detail || !cfg) return;

    char buf[96];
    if (battery_soc >= 0) snprintf(buf, sizeof(buf), "电量 %d%%", battery_soc);
    else snprintf(buf, sizeof(buf), "电量 --");
    text_set(s_detail_batt, buf);

    if (selected < 0 || selected >= cfg->server_count) {
        text_set(s_detail_name, "服务器");
    } else {
        text_set(s_detail_name, cfg->servers[selected].name);
    }

    if (!wifi || wifi->state != APP_WIFI_CONNECTED) {
        text_set(s_detail_state, "Wi-Fi 未连接");
        lv_obj_set_style_text_color(s_detail_state, lv_color_hex(C_WARN), 0);
    } else if (!status || !status->valid) {
        text_set(s_detail_state, "等待数据");
        lv_obj_set_style_text_color(s_detail_state, lv_color_hex(C_MUTED), 0);
    } else if (!status->online) {
        snprintf(buf, sizeof(buf), "离线 %s", status->err[0] ? status->err : "");
        text_set(s_detail_state, buf);
        lv_obj_set_style_text_color(s_detail_state, lv_color_hex(C_CRIT), 0);
    } else {
        char ago[24] = "";
        if (status->fetched_ms > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            app_fmt_ago(ago, sizeof(ago), now_ms - status->fetched_ms);
        }
        if (ago[0]) snprintf(buf, sizeof(buf), "在线 %d毫秒 %s", latency_ms, ago);
        else snprintf(buf, sizeof(buf), "在线 %d毫秒", latency_ms);
        text_set(s_detail_state, buf);
        lv_obj_set_style_text_color(s_detail_state, lv_color_hex(C_OK), 0);
    }

    bool have = status && status->valid && status->cpu >= 0 && status->online;
    set_arc(s_arc_cpu, s_arc_cpu_pct, have ? status->cpu : -1);
    set_arc(s_arc_mem, s_arc_mem_pct, have ? status->mem : -1);

    char tmp[24];
    set_percent(s_detail_disk, have ? status->disk : -1);
    set_rate(s_detail_diskio, have ? status->disk_io_bps : 0, have);
    app_fmt_load(tmp, sizeof(tmp), have ? status->load1_x100 : -1);
    text_set(s_detail_load, tmp);

    if (have && status->docker_run >= 0) {
        if (status->docker_total >= 0) {
            snprintf(tmp, sizeof(tmp), "%d/%d", status->docker_run, status->docker_total);
        } else {
            snprintf(tmp, sizeof(tmp), "%d", status->docker_run);
        }
        text_set(s_detail_docker, tmp);
    } else {
        text_set(s_detail_docker, "--");
    }

    if (have) {
        set_rate(s_detail_rx, status->net_rx_bps, true);
        set_rate(s_detail_tx, status->net_tx_bps, true);
        app_fmt_uptime(tmp, sizeof(tmp), status->uptime_s);
        text_set(s_detail_uptime, tmp);
    } else {
        text_set(s_detail_rx, "--");
        text_set(s_detail_tx, "--");
        text_set(s_detail_uptime, "--");
    }

    bool chart_valid = have && status->fetched_ms > 0;
    if (selected != s_chart_index ||
        (chart_valid && status->fetched_ms != s_chart_fetched_ms)) {
        s_chart_index = selected;
        s_chart_fetched_ms = chart_valid ? status->fetched_ms : 0;
        const app_trend_t *use = chart_valid ? trend : NULL;
        spark_set(s_spark_cpu, &s_spark_cpu_data, use, use ? use->cpu : NULL);
        spark_set(s_spark_mem, &s_spark_mem_data, use, use ? use->mem : NULL);
    }
}

void app_ui_detail_show(const app_config_t *cfg, int selected,
                        const app_server_status_t *status, const app_trend_t *trend,
                        int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    drop_screen(&s_detail);
    detail_build();
    detail_apply(cfg, selected, status, trend, latency_ms, wifi, battery_soc);
    lv_screen_load(s_detail);
    drop_screen(&s_list);
    drop_screen(&s_settings);
    drop_screen(&s_wifi);
}

void app_ui_detail_update(const app_config_t *cfg, int selected,
                          const app_server_status_t *status, const app_trend_t *trend,
                          int latency_ms, const app_wifi_info_t *wifi, int battery_soc)
{
    detail_apply(cfg, selected, status, trend, latency_ms, wifi, battery_soc);
}

// ---------------------------------------------------------------------------
// 设置页
// ---------------------------------------------------------------------------
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

static lv_obj_t *settings_build(void)
{
    lv_obj_t *scr = screen_create();
    header_add(scr, "设置", "长按确定返回");
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
    lv_obj_t *next = settings_build();
    screen_swap(&s_settings, next);
    settings_apply(cfg, wifi, selected, battery_soc);
    drop_screen(&s_list);
    drop_screen(&s_detail);
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
static lv_obj_t *wifi_build(const app_config_t *cfg, int selected)
{
    (void)selected;
    int rows = 1 + cfg->wifi_count;
    if (rows > WIFI_ROWS) rows = WIFI_ROWS;
    lv_obj_t *scr = screen_create();
    header_add(scr, "Wi-Fi 网络", "长按确定返回");
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
    lv_obj_t *next = wifi_build(cfg, selected);
    screen_swap(&s_wifi, next);
    wifi_apply(cfg, wifi, selected);
    drop_screen(&s_list);
    drop_screen(&s_detail);
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
    header_add(scr, "手机配网", "长按确定退出");

    lv_obj_t *card = panel_create(scr, 10, 50, 220, 190, C_CARD);
    label_create(card, "热点名称", &app_font_16, C_MUTED, 16, 10, 188);
    s_portal_ssid = label_create(card, "AI-Passport", &app_font_20, C_INK, 16, 28, 188);
    label_create(card, "热点密码", &app_font_16, C_MUTED, 16, 60, 188);
    s_portal_pass = label_create(card, "--------", &app_font_20, C_INK, 16, 78, 188);
    label_create(card, "浏览器打开", &app_font_16, C_MUTED, 16, 110, 188);
    label_create(card, "http://192.168.4.1", &app_font_20, C_ACCENT, 16, 128, 188);
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
    drop_screen(&s_list);
    drop_screen(&s_detail);
    drop_screen(&s_settings);
    drop_screen(&s_wifi);
}

void app_ui_portal_update(uint32_t elapsed_s, bool ap_active)
{
    portal_apply(elapsed_s, ap_active);
}
