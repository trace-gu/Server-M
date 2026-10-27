// main/app_wifi.c —— STA 多网络切换与配网 SoftAP。
#include "app_wifi.h"

#include "app_config.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_wifi";
#define AP_RETRY_MS 3000

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_retry_timer;
static SemaphoreHandle_t s_info_mutex;
static bool s_wifi_started;
static bool s_ap_active;
static app_wifi_state_t s_state = APP_WIFI_OFF;
static char s_ssid[APP_SSID_MAX + 1];
static char s_ip[16];
static char s_ap_ssid[33];
static char s_ap_password[17];
static int s_rssi;
static int s_attempt;
static int s_fail_count;
static bool s_sntp_started;

static void lock_info(void)
{
    if (s_info_mutex) xSemaphoreTake(s_info_mutex, portMAX_DELAY);
}

static void unlock_info(void)
{
    if (s_info_mutex) xSemaphoreGive(s_info_mutex);
}

static void set_state(app_wifi_state_t state)
{
    lock_info();
    s_state = state;
    unlock_info();
}

static void set_ssid(const char *ssid)
{
    lock_info();
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid ? ssid : "");
    unlock_info();
}

static int saved_wifi_count(void)
{
    app_config_lock();
    int count = app_config_get()->wifi_count;
    app_config_unlock();
    return count;
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    cfg.renew_servers_after_new_IP = false;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP 已启动");
    } else {
        ESP_LOGW(TAG, "SNTP 启动失败: %s", esp_err_to_name(err));
    }
}

static bool wifi_config_for(int index, wifi_config_t *out)
{
    if (!out) return false;
    app_config_lock();
    const app_config_t *cfg = app_config_get();
    bool ok = index >= 0 && index < cfg->wifi_count;
    if (ok) {
        memset(out, 0, sizeof(*out));
        // IDF 的 ssid/password 字段是定长字节数组,用有界拷贝避免 snprintf 截断告警。
        app_utf8_truncate((char *)out->sta.ssid, sizeof(out->sta.ssid), cfg->wifi[index].ssid);
        app_utf8_truncate((char *)out->sta.password, sizeof(out->sta.password),
                          cfg->wifi[index].pass);
        out->sta.threshold.authmode = out->sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        out->sta.pmf_cfg.capable = true;
        out->sta.pmf_cfg.required = false;
        set_ssid(cfg->wifi[index].ssid);
    }
    app_config_unlock();
    return ok;
}

static void connect_attempt(void);

static void retry_timer_cb(void *arg)
{
    (void)arg;
    connect_attempt();
}

static void schedule_retry(int delay_ms)
{
    if (!s_retry_timer) return;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void connect_attempt(void)
{
    int count = saved_wifi_count();
    if (count <= 0) {
        set_state(APP_WIFI_OFF);
        return;
    }
    if (s_attempt < 0 || s_attempt >= count) s_attempt = 0;

    wifi_config_t cfg;
    if (!wifi_config_for(s_attempt, &cfg)) return;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 STA 配置失败: %s", esp_err_to_name(err));
        set_state(APP_WIFI_FAILED);
        schedule_retry(AP_RETRY_MS);
        return;
    }
    set_state(APP_WIFI_CONNECTING);
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(err));
        schedule_retry(AP_RETRY_MS);
    } else {
        ESP_LOGI(TAG, "正在连接 Wi-Fi: %s", cfg.sta.ssid);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (saved_wifi_count() > 0) connect_attempt();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            set_state(APP_WIFI_CONNECTING);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
            lock_info();
            s_ip[0] = '\0';
            s_rssi = 0;
            unlock_info();
            s_fail_count++;
            if (s_fail_count >= 3 && saved_wifi_count() > 1) {
                s_attempt = (s_attempt + 1) % saved_wifi_count();
                s_fail_count = 0;
                ESP_LOGW(TAG, "连续连接失败,尝试下一个已保存网络");
            }
            set_state(APP_WIFI_CONNECTING);
            ESP_LOGW(TAG, "Wi-Fi 断开 (reason=%d), %d 秒后重试", ev ? ev->reason : -1,
                     AP_RETRY_MS / 1000);
            schedule_retry(AP_RETRY_MS);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        lock_info();
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_state = APP_WIFI_CONNECTED;
        s_fail_count = 0;
        unlock_info();
        ESP_LOGI(TAG, "已连接, IP=%s", s_ip);
        start_sntp();
    }
}

esp_err_t app_wifi_init(void)
{
    if (s_wifi_started) return ESP_OK;
    if (!s_info_mutex) {
        s_info_mutex = xSemaphoreCreateMutex();
        if (!s_info_mutex) return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif || !s_ap_netif) return ESP_ERR_NO_MEM;
    esp_netif_set_hostname(s_sta_netif, "ai-passport");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) return err;

    esp_err_t e = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_storage: %s", esp_err_to_name(e));
    e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) return e;
    e = esp_wifi_start();
    if (e != ESP_OK) return e;
    s_wifi_started = true;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AI-Passport-%02X%02X", mac[4], mac[5]);
    } else {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "AI-Passport");
    }
    // 配网页保存着每台服务器的 Token,热点密码不能是固定值:
    // 每次启动随机生成 8 位,显示在屏幕上,只有现场可见的人能连入。
    static const char kPassAlphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (size_t i = 0; i < 8; i++) {
        s_ap_password[i] = kPassAlphabet[esp_random() % (sizeof(kPassAlphabet) - 1)];
    }
    s_ap_password[8] = '\0';

    esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&targs, &s_retry_timer);
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Wi-Fi 就绪, 配网 SSID=%s", s_ap_ssid);
    return ESP_OK;
}

void app_wifi_get_info(app_wifi_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    lock_info();
    out->state = s_state;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    out->rssi = s_rssi;
    out->ap_active = s_ap_active;
    unlock_info();
    out->configured = saved_wifi_count() > 0;
}

bool app_wifi_connect_saved(int index)
{
    if (!s_wifi_started) return false;
    int count = saved_wifi_count();
    if (index < 0 || index >= count) return false;

    wifi_config_t cfg;
    if (!wifi_config_for(index, &cfg)) return false;
    s_attempt = index;
    s_fail_count = 0;
    if (s_retry_timer) esp_timer_stop(s_retry_timer);

    lock_info();
    s_ip[0] = '\0';
    s_rssi = 0;
    s_state = APP_WIFI_CONNECTING;
    unlock_info();

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        set_state(APP_WIFI_FAILED);
        return false;
    }
    // 断开事件本身可能已经安排了重试,这里改为短延迟后主动连接。
    schedule_retry(300);
    return true;
}

void app_wifi_reconnect(void)
{
    if (!s_wifi_started) return;
    int active = 0;
    app_config_lock();
    active = app_config_get()->active_wifi;
    app_config_unlock();
    app_wifi_connect_saved(active);
}

bool app_wifi_start_ap(void)
{
    if (!s_wifi_started) return false;
    if (s_ap_active) return true;

    wifi_config_t ap = {0};
    app_utf8_truncate((char *)ap.ap.ssid, sizeof(ap.ap.ssid), s_ap_ssid);
    ap.ap.ssid_len = (uint8_t)strlen((const char *)ap.ap.ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_ap_password);
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "启动 SoftAP 失败: %s", esp_err_to_name(err));
        return false;
    }
    lock_info();
    s_ap_active = true;
    unlock_info();
    ESP_LOGI(TAG, "配网热点已开启: %s", s_ap_ssid);
    return true;
}

void app_wifi_stop_ap(void)
{
    if (!s_wifi_started || !s_ap_active) return;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) ESP_LOGW(TAG, "关闭 SoftAP 失败: %s", esp_err_to_name(err));
    lock_info();
    s_ap_active = false;
    unlock_info();
    if (saved_wifi_count() > 0) {
        s_attempt = 0;
        app_config_lock();
        s_attempt = app_config_get()->active_wifi;
        app_config_unlock();
        s_fail_count = 0;
        schedule_retry(300);
    }
}

void app_wifi_set_low_power(bool on)
{
    if (!s_wifi_started) return;
    // 配网热点开着时保持 MIN_MODEM：手机的配网请求要被及时响应。
    if (on && s_ap_active) return;
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "切换 Wi-Fi 省电模式失败: %s", esp_err_to_name(err));
    }
}

bool app_wifi_ap_active(void)
{
    bool active;
    lock_info();
    active = s_ap_active;
    unlock_info();
    return active;
}

const char *app_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *app_wifi_ap_password(void)
{
    return s_ap_password;
}

bool app_wifi_ready(void)
{
    return s_wifi_started;
}

void app_wifi_note_config_changed(void)
{
    if (!s_wifi_started) return;
    s_attempt = 0;
    app_config_lock();
    s_attempt = app_config_get()->active_wifi;
    app_config_unlock();
    s_fail_count = 0;
    schedule_retry(300);
}
