// main/app_config.c
#include "app_config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

static const char *TAG = "app_config";
static const char *NVS_NAMESPACE = "passport";
static const char *NVS_KEY = "cfg";

static app_config_t s_cfg;
static SemaphoreHandle_t s_mutex;
static bool s_ready;

static void set_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = 1;
    cfg->refresh_s = APP_DEFAULT_REFRESH_S;
    cfg->screen_off_s = APP_DEFAULT_SCREEN_OFF_S;
}

static esp_err_t save_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, NVS_KEY, &s_cfg, sizeof(s_cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t app_config_init(void)
{
    if (s_ready) return ESP_OK;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }

    set_defaults(&s_cfg);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 失败: %s", esp_err_to_name(err));
        s_ready = true;
        return err;
    }

    app_config_t loaded;
    size_t size = sizeof(loaded);
    err = nvs_get_blob(handle, NVS_KEY, &loaded, &size);
    bool changed = false;
    if (err == ESP_OK && size == sizeof(loaded) && loaded.version == 1) {
        s_cfg = loaded;
        app_config_sanitize(&s_cfg);
        changed = memcmp(&loaded, &s_cfg, sizeof(s_cfg)) != 0;
    } else {
        ESP_LOGI(TAG, "未找到有效配置,使用默认值");
        changed = true;
    }
    nvs_close(handle);

    s_ready = true;
    if (changed && save_locked() != ESP_OK) {
        ESP_LOGW(TAG, "写入默认配置失败");
    }
    ESP_LOGI(TAG, "配置就绪: %u 台服务器, %u 个 Wi-Fi",
             (unsigned)s_cfg.server_count, (unsigned)s_cfg.wifi_count);
    return ESP_OK;
}

void app_config_lock(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

void app_config_unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

const app_config_t *app_config_get(void)
{
    return &s_cfg;
}

void app_config_snapshot(app_config_t *out)
{
    if (!out) return;
    app_config_lock();
    *out = s_cfg;
    app_config_unlock();
}

esp_err_t app_config_save(void)
{
    app_config_lock();
    esp_err_t err = save_locked();
    app_config_unlock();
    return err;
}

esp_err_t app_config_apply(app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    app_config_lock();
    app_config_sanitize(cfg);
    s_cfg = *cfg;
    esp_err_t err = save_locked();
    app_config_unlock();
    return err;
}

void app_config_reset(void)
{
    app_config_lock();
    set_defaults(&s_cfg);
    (void)save_locked();
    app_config_unlock();
}

bool app_config_ready(void)
{
    return s_ready;
}
