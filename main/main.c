// main/main.c —— AI Passport 服务器监控应用入口。
//
// 交互模型：首页只显示当前选中服务器，并且只为它按需拉取；
// 屏幕熄灭或离开首页后停止轮询。
//
// 按键语义：
//   上/下 短按   首页=切换服务器；列表页=移动选中项
//   确定  短按   首页=立即刷新；列表页=执行；设置页=切换/进入
//   确定  长按   返回上一级；首页=进入设置；配网页=退出配网
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"

#include "app_config.h"
#include "app_model.h"
#include "app_net.h"
#include "app_portal.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "key_guard.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "passport";

#define INPUT_QUEUE_DEPTH 12
#define TASK_PERIOD_MS 50
#define UI_TICK_MS 1000
#define BATTERY_PERIOD_MS 30000

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_app_task;
static key_guard_t s_key_guard;
static bool s_input_ready;
static app_view_t s_view = APP_VIEW_LIST;
static int s_sel;
static int s_set_sel;
static int s_wifi_sel;
static app_server_status_t s_status[APP_MAX_SERVERS];
static app_trend_t s_trend;
static int s_latency_ms;
static int s_battery_soc = -1;
static bool s_screen_on = true;
static bool s_wake_swallow;
static int64_t s_last_input_ms;
static int64_t s_attempt_ms[APP_MAX_SERVERS];   // 每台上一次发起请求的时间
static bool s_force_refresh;
static int64_t s_last_keepalive_ms;             // 熄屏保活请求的上一次时间
static uint8_t s_applied_brightness;
static app_config_t s_ui_cfg;

// 列表一屏显示 4 台，轮询只覆盖这 4 台，顺序与 app_ui 的滚动窗口一致。
#define APP_LIST_ROWS 4

static const uint16_t REFRESH_STEPS[] = {1, 3, 5, 10, 15, 30, 60, 120, 300};
static const uint16_t SCREEN_OFF_STEPS[] = {0, 30, 60, 120, 300};
// 背光是整机最大的一笔固定功耗，默认 65% 观感几乎无损。
static const uint8_t BRIGHTNESS_STEPS[] = {10, 20, 30, 50, 65, 80, 100};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready) return;
    if (!s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void sync_brightness(void)
{
    if (!s_screen_on) return;
    uint8_t brightness = s_ui_cfg.brightness;
    if (brightness < APP_BRIGHTNESS_MIN || brightness > APP_BRIGHTNESS_MAX) {
        brightness = APP_DEFAULT_BRIGHTNESS;
    }
    if (brightness == s_applied_brightness) return;
    s_applied_brightness = brightness;
    bsp_display_backlight(brightness);
}

static void render_view(bool full)
{
    app_config_snapshot(&s_ui_cfg);
    // 设置页或手机网页改了亮度后，下一次渲染就会生效。
    sync_brightness();

    app_wifi_info_t wifi;
    app_wifi_get_info(&wifi);

    const app_server_status_t *status = NULL;
    if (s_ui_cfg.server_count > 0) {
        if (s_sel >= s_ui_cfg.server_count) s_sel = s_ui_cfg.server_count - 1;
        if (s_sel < 0) s_sel = 0;
        status = &s_status[s_sel];
    } else {
        s_sel = 0;
    }

    if (!bsp_lvgl_lock(500)) return;
    switch (s_view) {
    case APP_VIEW_SETTINGS:
        if (full) app_ui_settings_show(&s_ui_cfg, &wifi, s_set_sel, s_battery_soc);
        else app_ui_settings_update(&s_ui_cfg, &wifi, s_set_sel, s_battery_soc);
        break;
    case APP_VIEW_WIFI:
        if (full) app_ui_wifi_show(&s_ui_cfg, &wifi, s_wifi_sel);
        else app_ui_wifi_update(&s_ui_cfg, &wifi, s_wifi_sel);
        break;
    case APP_VIEW_PORTAL:
        if (full) {
            const char *portal_ssid = app_wifi_ap_active() ? app_wifi_ap_ssid() : wifi.ssid;
            app_ui_portal_show(portal_ssid, app_wifi_ap_password(), app_portal_ip(),
                               app_portal_elapsed_s(), app_wifi_ap_active());
        } else {
            app_ui_portal_update(app_portal_elapsed_s(), app_wifi_ap_active());
        }
        break;
    case APP_VIEW_LIST:
    default:
        if (full) {
            app_ui_list_show(&s_ui_cfg, s_sel, s_status, &wifi, s_battery_soc);
        } else {
            app_ui_list_update(&s_ui_cfg, s_sel, s_status, &wifi, s_battery_soc);
        }
        break;
    case APP_VIEW_DETAIL:
        if (full) {
            app_ui_detail_show(&s_ui_cfg, s_sel, status, &s_trend, s_latency_ms,
                               &wifi, s_battery_soc);
        } else {
            app_ui_detail_update(&s_ui_cfg, s_sel, status, &s_trend, s_latency_ms,
                                 &wifi, s_battery_soc);
        }
        break;
    }
    bsp_lvgl_unlock();
}

// 与 app_ui 的滚动窗口一致：选中项尽量保持在第二行，便于上下各看一眼。
static int list_window_start(int count, int rows)
{
    int win = s_sel - 1;
    if (win > count - rows) win = count - rows;
    if (win < 0) win = 0;
    return win;
}

static void maybe_request_refresh(int64_t now)
{
    if (!s_screen_on) return;
    if (app_net_busy()) return;
    int count = s_ui_cfg.server_count;
    if (count == 0) return;

    // 只有列表页和详情页需要数据:列表页按“屏幕上的 4 行”逐台补齐,
    // 详情页只拉当前这台;设置/Wi-Fi/配网页完全不发请求。
    // 是否该拉取按“每台自己的上次尝试时间”判断,这样列表一轮正好等于刷新间隔,
    // 不会被全局计时器拖成 N 倍间隔。
    const int64_t interval_ms = (int64_t)s_ui_cfg.refresh_s * 1000;

    int target = -1;
    int64_t oldest = 0;
    if (s_view == APP_VIEW_DETAIL) {
        int64_t last = s_attempt_ms[s_sel];
        if (s_force_refresh || last == 0 || now - last >= interval_ms) {
            target = s_sel;
            oldest = last;
        }
    } else if (s_view == APP_VIEW_LIST) {
        int rows = count < APP_LIST_ROWS ? count : APP_LIST_ROWS;
        int start = list_window_start(count, rows);
        for (int i = 0; i < rows; i++) {
            int index = start + i;
            int64_t last = s_attempt_ms[index];
            if (!s_force_refresh && last != 0 && now - last < interval_ms) continue;
            if (target < 0 || last < oldest) {
                target = index;
                oldest = last;
            }
        }
    } else {
        return;
    }
    if (target < 0) return;

    if (app_net_request(target)) {
        s_attempt_ms[target] = now;
        s_force_refresh = false;
    }
}

static void reset_attempts(void)
{
    memset(s_attempt_ms, 0, sizeof(s_attempt_ms));
}

// 熄屏后完全停止轮询会带来一个副作用:路由器会把长时间没有数据往来的客户端
// 踢下线,唤醒时只能重新扫描+DHCP,看起来就是"唤醒好久才连上"。
// 每 90 秒发一次轻量请求,既保持关联存活,又不会明显抬高待机电流。
#define KEEPALIVE_MS (90 * 1000)

static void maybe_keepalive(int64_t now)
{
    if (s_screen_on) return;
    if (app_portal_active()) return;   // 配网页开着时手机流量本身就在保活
    if (s_last_keepalive_ms != 0 && now - s_last_keepalive_ms < KEEPALIVE_MS) return;

    app_wifi_info_t wifi;
    app_wifi_get_info(&wifi);
    s_last_keepalive_ms = now;
    if (wifi.state != APP_WIFI_CONNECTED) {
        // 熄屏期间被路由器踢下线的话,在这里就重连,别等唤醒才补救。
        app_wifi_kick();
        return;
    }
    if (app_net_busy() || s_ui_cfg.server_count <= 0) return;
    (void)app_net_request(s_sel);
}

static void enter_list(void)
{
    s_view = APP_VIEW_LIST;
    s_last_input_ms = now_ms();
    app_net_cancel();
    s_force_refresh = false;
    reset_attempts();
    render_view(true);
    maybe_request_refresh(now_ms());
}

static void enter_detail(int index)
{
    if (s_ui_cfg.server_count <= 0) return;
    if (index < 0 || index >= s_ui_cfg.server_count) index = 0;
    s_sel = index;
    s_view = APP_VIEW_DETAIL;
    s_last_input_ms = now_ms();
    app_net_cancel();
    memset(&s_trend, 0, sizeof(s_trend));
    s_latency_ms = 0;
    reset_attempts();
    render_view(true);
    maybe_request_refresh(now_ms());
}

static void enter_settings(void)
{
    s_view = APP_VIEW_SETTINGS;
    s_last_input_ms = now_ms();
    app_net_cancel();
    render_view(true);
}

static void enter_wifi(void)
{
    s_view = APP_VIEW_WIFI;
    s_last_input_ms = now_ms();
    s_wifi_sel = 0;
    app_net_cancel();
    render_view(true);
}

static void start_portal(void)
{
    // 先放弃在途的 HTTPS 请求:httpd 任务要 20 KB 栈,与 TLS 握手抢堆时
    // httpd_start 会失败,表现出来就是"按确定没反应"。
    app_net_cancel();
    esp_err_t err = app_portal_start();
    if (err != ESP_OK) {
        // 网络任务收尾后才释放握手用的缓冲区,稍等一下再补一次。
        vTaskDelay(pdMS_TO_TICKS(400));
        err = app_portal_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动手机配网失败: %s", esp_err_to_name(err));
        if (bsp_lvgl_lock(200)) {
            app_ui_portal_error();
            bsp_lvgl_unlock();
        }
        render_view(false);
        return;
    }
    s_view = APP_VIEW_PORTAL;
    s_last_input_ms = now_ms();
    render_view(true);
}

static void exit_portal(void)
{
    bool reconnect = app_portal_take_connect_requested() || app_portal_take_config_changed();
    // 先切回本地界面再关闭服务:httpd_stop 会等手机侧连接收尾,
    // 先返回界面才不会让人觉得"按了确定没反应"。
    // 刚从网页配完 Wi-Fi 时直接进 Wi-Fi 页,那里会显示连上后的 IP。
    if (reconnect) enter_wifi();
    else enter_settings();
    app_portal_stop();
    // 热点关闭、模式切回 STA 之后再发起连接。若在热点还开着时连接,
    // 紧接着的模式切换会把刚建立的连接打断,表现为"配完网没连上"。
    if (reconnect) app_wifi_note_config_changed();
}

static void switch_server(int delta)
{
    if (s_ui_cfg.server_count <= 1) return;
    s_sel = app_selection_move(s_sel, s_ui_cfg.server_count, delta);
    s_last_input_ms = now_ms();
    if (s_view == APP_VIEW_DETAIL) {
        app_net_cancel();
        memset(&s_trend, 0, sizeof(s_trend));
        s_latency_ms = 0;
        s_attempt_ms[s_sel] = 0;
        render_view(true);
        maybe_request_refresh(now_ms());
    } else {
        render_view(false);
    }
}

static void cycle_refresh(void)
{
    size_t count = sizeof(REFRESH_STEPS) / sizeof(REFRESH_STEPS[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (REFRESH_STEPS[i] == s_ui_cfg.refresh_s) {
            index = i;
            break;
        }
    }
    app_config_snapshot(&s_ui_cfg);
    s_ui_cfg.refresh_s = REFRESH_STEPS[(index + 1) % count];
    if (app_config_apply(&s_ui_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "保存刷新间隔失败");
    }
    render_view(false);
}

static void cycle_brightness(void)
{
    app_config_snapshot(&s_ui_cfg);
    size_t count = sizeof(BRIGHTNESS_STEPS) / sizeof(BRIGHTNESS_STEPS[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (BRIGHTNESS_STEPS[i] == s_ui_cfg.brightness) {
            index = i;
            break;
        }
    }
    s_ui_cfg.brightness = BRIGHTNESS_STEPS[(index + 1) % count];
    if (app_config_apply(&s_ui_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "保存屏幕亮度失败");
    }
    render_view(false);
}

static void cycle_screen_off(void)
{
    size_t count = sizeof(SCREEN_OFF_STEPS) / sizeof(SCREEN_OFF_STEPS[0]);
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (SCREEN_OFF_STEPS[i] == s_ui_cfg.screen_off_s) {
            index = i;
            break;
        }
    }
    app_config_snapshot(&s_ui_cfg);
    s_ui_cfg.screen_off_s = SCREEN_OFF_STEPS[(index + 1) % count];
    if (app_config_apply(&s_ui_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "保存熄屏时间失败");
    }
    s_last_input_ms = now_ms();
    render_view(false);
}

static void activate_wifi(int index)
{
    if (!app_wifi_set_active(&s_ui_cfg, index)) return;
    if (app_config_apply(&s_ui_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "保存 Wi-Fi 选择失败");
        return;
    }
    if (!app_wifi_connect_saved(index)) {
        ESP_LOGW(TAG, "切换 Wi-Fi 失败");
    }
    render_view(false);
}

static void screen_off(void)
{
    if (!s_screen_on) return;
    s_screen_on = false;
    bsp_display_backlight(0);
    s_applied_brightness = 0;
    app_net_cancel();
    ESP_LOGI(TAG, "熄屏，停止轮询");
}

static void wake_screen(void)
{
    const int64_t now = now_ms();
    s_screen_on = true;
    s_last_input_ms = now;
    s_applied_brightness = 0;   // 让 render_view 重新套用配置里的亮度
    // 熄屏几个小时后路由器可能已经把这个空闲客户端踢下线,
    // 这里立刻重连,不再等断线退避(最长 10 秒)走完。
    app_wifi_kick();
    reset_attempts();
    s_last_keepalive_ms = now;
    render_view(false);
    maybe_request_refresh(now);
    ESP_LOGI(TAG, "屏幕唤醒");
}

static void handle_back(void)
{
    switch (s_view) {
    case APP_VIEW_SETTINGS:
        enter_list();
        break;
    case APP_VIEW_WIFI:
        enter_settings();
        break;
    case APP_VIEW_PORTAL:
        exit_portal();
        break;
    case APP_VIEW_DETAIL:
        enter_list();
        break;
    case APP_VIEW_LIST:
    default:
        enter_settings();
        break;
    }
}

static void handle_click(bsp_btn_t btn)
{
    app_config_snapshot(&s_ui_cfg);

    switch (s_view) {
    case APP_VIEW_LIST:
        if (btn == BSP_BTN_UP) switch_server(-1);
        else if (btn == BSP_BTN_DOWN) switch_server(1);
        else if (btn == BSP_BTN_OK && s_ui_cfg.server_count > 0) enter_detail(s_sel);
        break;

    case APP_VIEW_DETAIL:
        if (btn == BSP_BTN_UP) switch_server(-1);
        else if (btn == BSP_BTN_DOWN) switch_server(1);
        else if (btn == BSP_BTN_OK) {
            // 立即刷新：放弃在途请求并排到队首；若网络任务仍在收尾，
            // s_force_refresh 会保证它一空闲就立刻重新发起。
            app_net_cancel();
            s_force_refresh = true;
            s_attempt_ms[s_sel] = 0;
            maybe_request_refresh(now_ms());
        }
        break;

    case APP_VIEW_SETTINGS:
        if (btn == BSP_BTN_UP) {
            if (s_set_sel > 0) s_set_sel--;
            render_view(false);
        } else if (btn == BSP_BTN_DOWN) {
            if (s_set_sel < 6) s_set_sel++;
            render_view(false);
        } else if (btn == BSP_BTN_OK) {
            switch (s_set_sel) {
            case 0: enter_wifi(); break;
            case 1: cycle_brightness(); break;
            case 2: cycle_refresh(); break;
            case 3: cycle_screen_off(); break;
            case 4:
                if (app_portal_active()) exit_portal();
                else start_portal();
                break;
            case 5: break;
            default: enter_list(); break;
            }
        }
        break;

    case APP_VIEW_WIFI:
        if (btn == BSP_BTN_UP) {
            if (s_wifi_sel > 0) s_wifi_sel--;
            render_view(false);
        } else if (btn == BSP_BTN_DOWN) {
            if (s_wifi_sel < s_ui_cfg.wifi_count) s_wifi_sel++;
            render_view(false);
        } else if (btn == BSP_BTN_OK) {
            if (s_wifi_sel == 0) {
                if (app_portal_active()) exit_portal();
                else start_portal();
            }
            else activate_wifi(s_wifi_sel - 1);
        }
        break;

    case APP_VIEW_PORTAL:
        // 配网页就是"开启"状态:短按确定关闭配网,手机随即无法再访问。
        if (btn == BSP_BTN_OK) exit_portal();
        break;

    default:
        break;
    }
}

static void handle_input(const input_event_t *input)
{
    int64_t now = now_ms();

    if (!s_screen_on) {
        if (input->event == BSP_BTN_RELEASE) {
            s_wake_swallow = false;
            return;
        }
        wake_screen();
        s_wake_swallow = true;
        return;
    }
    if (s_wake_swallow) {
        if (input->event == BSP_BTN_RELEASE) s_wake_swallow = false;
        return;
    }

    if (input->event == BSP_BTN_LONG) {
        s_last_input_ms = now;
        if (input->btn == BSP_BTN_OK) {
            handle_back();
        } else if (input->btn == BSP_BTN_UP) {
            // 硬件电源键直连供电,固件读不到;用长按上键代替手动关屏。
            if (s_view == APP_VIEW_LIST || s_view == APP_VIEW_DETAIL) screen_off();
        }
        return;
    }
    if (input->event != BSP_BTN_CLICK) return;

    s_last_input_ms = now;
    handle_click(input->btn);
}

static void merge_stale(app_server_status_t *merged, const app_server_status_t *previous)
{
    if (merged->cpu < 0) merged->cpu = previous->cpu;
    if (merged->mem < 0) merged->mem = previous->mem;
    if (merged->swap < 0) merged->swap = previous->swap;
    if (merged->disk < 0) merged->disk = previous->disk;
    if (merged->load1_x100 < 0) merged->load1_x100 = previous->load1_x100;
    if (merged->temp_c_x10 < 0) merged->temp_c_x10 = previous->temp_c_x10;
    if (merged->ssl_days < 0) merged->ssl_days = previous->ssl_days;
    if (merged->docker_run < 0) merged->docker_run = previous->docker_run;
    if (merged->docker_total < 0) merged->docker_total = previous->docker_total;
    if (merged->docker_bad < 0) merged->docker_bad = previous->docker_bad;
    if (merged->backup_state < 0) merged->backup_state = previous->backup_state;
    if (merged->net_rx_bps == 0) merged->net_rx_bps = previous->net_rx_bps;
    if (merged->net_tx_bps == 0) merged->net_tx_bps = previous->net_tx_bps;
    if (merged->disk_io_bps == 0) merged->disk_io_bps = previous->disk_io_bps;
    if (merged->uptime_s == 0) merged->uptime_s = previous->uptime_s;
}

static void handle_result(const app_fetch_result_t *result)
{
    if (result->index < 0 || result->index >= APP_MAX_SERVERS) return;

    app_server_status_t merged = result->status;
    const app_server_status_t *previous = &s_status[result->index];
    if (!merged.online && previous->valid && previous->online) {
        merge_stale(&merged, previous);
    }
    s_status[result->index] = merged;

    if (result->index == s_sel) {
        s_latency_ms = result->latency_ms;
        s_trend = result->trend;
    }

    if (s_screen_on && (s_view == APP_VIEW_LIST || s_view == APP_VIEW_DETAIL)) {
        render_view(false);
    }
}

static void maybe_screen_off(int64_t now)
{
    if (!s_screen_on || s_ui_cfg.screen_off_s == 0) return;
    if (now - s_last_input_ms < (int64_t)s_ui_cfg.screen_off_s * 1000) return;
    screen_off();
}

static void app_task(void *arg)
{
    (void)arg;
    int64_t last_ui_ms = 0;
    int64_t last_battery_ms = 0;
    int64_t last_portal_ms = 0;

    for (;;) {
        const int64_t now = now_ms();

        input_event_t input;
        while (xQueueReceive(s_input_queue, &input, 0) == pdTRUE) {
            handle_input(&input);
        }

        app_fetch_result_t result;
        while (app_net_poll(&result)) {
            handle_result(&result);
        }

        if (app_portal_active()) {
            if (app_portal_exit_requested()) {
                exit_portal();
            } else if (app_portal_take_config_changed()) {
                if (s_wifi_sel > s_ui_cfg.wifi_count) s_wifi_sel = s_ui_cfg.wifi_count;
                if (s_view == APP_VIEW_PORTAL) render_view(false);
            } else if (s_view == APP_VIEW_PORTAL && now - last_portal_ms >= UI_TICK_MS) {
                last_portal_ms = now;
                render_view(false);
            }
        }

        if (s_screen_on) {
            if (now - last_battery_ms >= BATTERY_PERIOD_MS) {
                last_battery_ms = now;
                s_battery_soc = bsp_battery_soc();
            }
            if (now - last_ui_ms >= UI_TICK_MS) {
                last_ui_ms = now;
                render_view(false);
            }
            maybe_request_refresh(now);
            maybe_screen_off(now);
        } else {
            maybe_keepalive(now);
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重新初始化: %s", esp_err_to_name(err));
        (void)nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Server-M监控启动");

    esp_err_t err = nvs_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示或 LVGL 初始化失败，应用无法继续");
        return;
    }
    bsp_display_backlight(APP_DEFAULT_BRIGHTNESS);

    if (app_config_init() != ESP_OK) {
        ESP_LOGW(TAG, "配置初始化异常，使用内存默认值");
    }

    if (app_wifi_init() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 初始化失败");
    }
    if (app_net_init() != ESP_OK) {
        ESP_LOGE(TAG, "网络任务初始化失败");
    }

    app_config_snapshot(&s_ui_cfg);
    s_sel = 0;
    s_last_input_ms = now_ms();

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bsp_lvgl_unlock();
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "按键队列创建失败");
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败，无法操作界面");
    } else {
        // 深睡按键唤醒会带着“键仍按下”的状态重启,先武装守卫吞掉那次幻影长按;
        // 按键驱动初始化完成后按物理电平同步一次,已松手就立即撤防。
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
            key_guard_arm(&s_key_guard, esp_timer_get_time(), 3000000);
        }
        key_guard_sync_released(&s_key_guard,
                                bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
        s_input_ready = true;
    }

    if (bsp_battery_init() == ESP_OK) s_battery_soc = bsp_battery_soc();

    ESP_LOGI(TAG, "配置: %u 台服务器, %u 个 Wi-Fi",
             (unsigned)s_ui_cfg.server_count, (unsigned)s_ui_cfg.wifi_count);

    if (s_ui_cfg.wifi_count == 0 && app_portal_start() == ESP_OK) {
        s_view = APP_VIEW_PORTAL;
        ESP_LOGI(TAG, "未配置 Wi-Fi，已自动进入手机配网");
    }
    render_view(true);

    if (xTaskCreate(app_task, "passport_app", 8192, NULL, 5, &s_app_task) != pdPASS) {
        ESP_LOGE(TAG, "应用任务创建失败");
        s_app_task = NULL;
    }
}
