// main/app_model.c —— 与硬件无关的应用逻辑,可在主机上单测。
#include "app_model.h"

#include <stdio.h>
#include <string.h>

#define APP_CFG_VERSION 1

static size_t bounded_len(const char *text, size_t max)
{
    size_t len = 0;
    while (len < max && text[len] != '\0') len++;
    return len;
}

static void copy_truncated(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    size_t len = bounded_len(src ? src : "", dst_size - 1);
    memcpy(dst, src ? src : "", len);
    dst[len] = '\0';
}

static bool is_printable_ascii(unsigned char c)
{
    return c >= 0x21 && c <= 0x7e;
}

bool app_utf8_valid(const char *text)
{
    if (!text) return false;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80) {
            cp = *p;
            extra = 0;
        } else if ((*p & 0xe0) == 0xc0) {
            cp = *p & 0x1f;
            extra = 1;
        } else if ((*p & 0xf0) == 0xe0) {
            cp = *p & 0x0f;
            extra = 2;
        } else if ((*p & 0xf8) == 0xf0) {
            cp = *p & 0x07;
            extra = 3;
        } else {
            return false;
        }
        for (int i = 0; i < extra; i++) {
            p++;
            if ((*p & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (*p & 0x3f);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10ffff) return false;
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        if (cp == 0) return false;
        p++;
    }
    return true;
}

size_t app_utf8_truncate(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return 0;
    dst[0] = '\0';
    if (!src) return 0;

    size_t used = 0;
    const unsigned char *p = (const unsigned char *)src;
    while (*p && used < dst_size - 1) {
        int extra;
        if (*p < 0x80) {
            extra = 0;
        } else if ((*p & 0xe0) == 0xc0) {
            extra = 1;
        } else if ((*p & 0xf0) == 0xe0) {
            extra = 2;
        } else if ((*p & 0xf8) == 0xf0) {
            extra = 3;
        } else {
            break;
        }
        size_t needed = (size_t)extra + 1;
        if (used + needed > dst_size - 1) break;
        bool valid = true;
        for (size_t i = 1; i < needed; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                valid = false;
                break;
            }
        }
        if (!valid) break;
        memcpy(dst + used, p, needed);
        used += needed;
        p += needed;
    }
    dst[used] = '\0';
    return used;
}

bool app_url_valid(const char *url)
{
    static const char scheme[] = "https://";
    if (!url) return false;
    size_t len = strlen(url);
    if (len <= sizeof(scheme) - 1 || len > APP_URL_MAX) return false;
    if (strncmp(url, scheme, sizeof(scheme) - 1) != 0) return false;
    if (!app_utf8_valid(url)) return false;

    const char *host = url + sizeof(scheme) - 1;
    size_t host_len = 0;
    while (host[host_len] && host[host_len] != '/' && host[host_len] != ':') host_len++;
    if (host_len == 0) return false;

    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool app_name_valid(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > APP_NAME_MAX) return false;
    if (!app_utf8_valid(name)) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

bool app_token_valid(const char *token)
{
    if (!token) return false;
    size_t len = strlen(token);
    if (len == 0 || len > APP_TOKEN_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)token; *p; p++) {
        if (!is_printable_ascii(*p)) return false;
    }
    return true;
}

void app_config_sanitize(app_config_t *cfg)
{
    if (!cfg) return;
    cfg->version = APP_CFG_VERSION;
    if (cfg->refresh_s < APP_REFRESH_MIN_S || cfg->refresh_s > APP_REFRESH_MAX_S) {
        cfg->refresh_s = APP_DEFAULT_REFRESH_S;
    }
    if (cfg->screen_off_s > 3600) cfg->screen_off_s = APP_DEFAULT_SCREEN_OFF_S;
    if (cfg->brightness < APP_BRIGHTNESS_MIN || cfg->brightness > APP_BRIGHTNESS_MAX) {
        cfg->brightness = APP_DEFAULT_BRIGHTNESS;
    }

    if (cfg->server_count > APP_MAX_SERVERS) cfg->server_count = APP_MAX_SERVERS;
    uint8_t keep = 0;
    for (uint8_t i = 0; i < cfg->server_count; i++) {
        app_server_cfg_t *srv = &cfg->servers[i];
        srv->name[APP_NAME_MAX] = '\0';
        srv->url[APP_URL_MAX] = '\0';
        srv->token[APP_TOKEN_MAX] = '\0';
        if (!app_name_valid(srv->name) || !app_url_valid(srv->url) || !app_token_valid(srv->token)) {
            continue;
        }
        if (keep != i) cfg->servers[keep] = *srv;
        keep++;
    }
    cfg->server_count = keep;

    if (cfg->wifi_count > APP_MAX_WIFI) cfg->wifi_count = APP_MAX_WIFI;
    keep = 0;
    for (uint8_t i = 0; i < cfg->wifi_count; i++) {
        app_wifi_cfg_t *wifi = &cfg->wifi[i];
        wifi->ssid[APP_SSID_MAX] = '\0';
        wifi->pass[APP_PASS_MAX] = '\0';
        size_t ssid_len = strlen(wifi->ssid);
        size_t pass_len = strlen(wifi->pass);
        if (ssid_len == 0 || ssid_len > APP_SSID_MAX || pass_len > APP_PASS_MAX) continue;
        if (!app_utf8_valid(wifi->ssid) || (pass_len > 0 && pass_len < 8)) continue;
        if (keep != i) cfg->wifi[keep] = *wifi;
        keep++;
    }
    cfg->wifi_count = keep;
    if (cfg->active_wifi >= cfg->wifi_count) cfg->active_wifi = 0;
}

int app_server_add(app_config_t *cfg, const char *name, const char *url, const char *token)
{
    if (!cfg || cfg->server_count >= APP_MAX_SERVERS) return -1;
    char n[APP_NAME_MAX + 1], u[APP_URL_MAX + 1], t[APP_TOKEN_MAX + 1];
    copy_truncated(n, sizeof(n), name);
    copy_truncated(u, sizeof(u), url);
    copy_truncated(t, sizeof(t), token);
    if (!app_name_valid(n) || !app_url_valid(u) || !app_token_valid(t)) return -1;

    int index = cfg->server_count;
    cfg->servers[index].name[0] = '\0';
    copy_truncated(cfg->servers[index].name, sizeof(cfg->servers[index].name), n);
    copy_truncated(cfg->servers[index].url, sizeof(cfg->servers[index].url), u);
    copy_truncated(cfg->servers[index].token, sizeof(cfg->servers[index].token), t);
    cfg->server_count++;
    return index;
}

bool app_server_update(app_config_t *cfg, int index, const char *name, const char *url, const char *token)
{
    if (!cfg || index < 0 || index >= cfg->server_count) return false;
    char n[APP_NAME_MAX + 1], u[APP_URL_MAX + 1], t[APP_TOKEN_MAX + 1];
    copy_truncated(n, sizeof(n), name);
    copy_truncated(u, sizeof(u), url);
    copy_truncated(t, sizeof(t), token);
    if (!app_name_valid(n) || !app_url_valid(u) || !app_token_valid(t)) return false;
    copy_truncated(cfg->servers[index].name, sizeof(cfg->servers[index].name), n);
    copy_truncated(cfg->servers[index].url, sizeof(cfg->servers[index].url), u);
    copy_truncated(cfg->servers[index].token, sizeof(cfg->servers[index].token), t);
    return true;
}

bool app_server_remove(app_config_t *cfg, int index)
{
    if (!cfg || index < 0 || index >= cfg->server_count) return false;
    for (int i = index; i + 1 < cfg->server_count; i++) {
        cfg->servers[i] = cfg->servers[i + 1];
    }
    cfg->server_count--;
    memset(&cfg->servers[cfg->server_count], 0, sizeof(cfg->servers[0]));
    return true;
}

int app_wifi_add(app_config_t *cfg, const char *ssid, const char *pass)
{
    if (!cfg) return -1;
    char s[APP_SSID_MAX + 1], p[APP_PASS_MAX + 1];
    copy_truncated(s, sizeof(s), ssid);
    copy_truncated(p, sizeof(p), pass);
    size_t ssid_len = strlen(s);
    size_t pass_len = strlen(p);
    if (ssid_len == 0 || ssid_len > APP_SSID_MAX) return -1;
    if (!app_utf8_valid(s) || (pass_len > 0 && pass_len < 8)) return -1;

    int existing = app_wifi_find(cfg, s);
    if (existing >= 0) {
        copy_truncated(cfg->wifi[existing].pass, sizeof(cfg->wifi[existing].pass), p);
        return existing;
    }
    if (cfg->wifi_count >= APP_MAX_WIFI) return -1;
    int index = cfg->wifi_count;
    copy_truncated(cfg->wifi[index].ssid, sizeof(cfg->wifi[index].ssid), s);
    copy_truncated(cfg->wifi[index].pass, sizeof(cfg->wifi[index].pass), p);
    cfg->wifi_count++;
    return index;
}

bool app_wifi_remove(app_config_t *cfg, int index)
{
    if (!cfg || index < 0 || index >= cfg->wifi_count) return false;
    for (int i = index; i + 1 < cfg->wifi_count; i++) {
        cfg->wifi[i] = cfg->wifi[i + 1];
    }
    cfg->wifi_count--;
    memset(&cfg->wifi[cfg->wifi_count], 0, sizeof(cfg->wifi[0]));
    if (cfg->wifi_count == 0) {
        cfg->active_wifi = 0;
    } else if (cfg->active_wifi > index) {
        cfg->active_wifi--;
    } else if (cfg->active_wifi >= cfg->wifi_count) {
        cfg->active_wifi = 0;
    }
    return true;
}

bool app_wifi_set_active(app_config_t *cfg, int index)
{
    if (!cfg || index < 0 || index >= cfg->wifi_count) return false;
    cfg->active_wifi = (uint8_t)index;
    return true;
}

int app_wifi_find(const app_config_t *cfg, const char *ssid)
{
    if (!cfg || !ssid) return -1;
    for (uint8_t i = 0; i < cfg->wifi_count; i++) {
        if (strcmp(cfg->wifi[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

int app_selection_move(int selected, int count, int delta)
{
    if (count <= 0) return 0;
    if (selected < 0 || selected >= count) selected = 0;
    int next = (selected + delta) % count;
    if (next < 0) next += count;
    return next;
}

uint32_t app_refresh_interval_ms(uint32_t interval_s)
{
    if (interval_s < APP_REFRESH_MIN_S) return (uint32_t)APP_REFRESH_MIN_S * 1000u;
    if (interval_s > APP_REFRESH_MAX_S) return (uint32_t)APP_REFRESH_MAX_S * 1000u;
    return interval_s * 1000u;
}

bool app_refresh_due(const app_refresh_ctx_t *ctx)
{
    if (!ctx || !ctx->screen_on || !ctx->detail_active ||
        !ctx->have_server || ctx->inflight) {
        return false;
    }
    uint32_t interval_ms = app_refresh_interval_ms(ctx->interval_s);
    if (ctx->last_start_ms <= 0) return true;
    return ctx->now_ms - ctx->last_start_ms >= (int64_t)interval_ms;
}

void app_trend_reduce(const uint8_t *src, uint16_t count, uint8_t *dst, uint16_t bars)
{
    if (!dst || bars == 0) return;
    for (uint16_t b = 0; b < bars; b++) dst[b] = 0xff;
    if (!src || count == 0 || bars > APP_TREND_BARS) return;
    if (count > APP_TREND_POINTS) {
        src += count - APP_TREND_POINTS;
        count = APP_TREND_POINTS;
    }

    // 采样按“最新样本贴住最右侧柱”放置,这样刚启动时不会伪造 24h 历史。
    const uint16_t points_per_bar = APP_TREND_POINTS / APP_TREND_BARS;
    const uint16_t offset = APP_TREND_POINTS - count;
    for (uint16_t b = 0; b < bars; b++) {
        uint16_t lo = b * points_per_bar;
        uint16_t hi = lo + points_per_bar;
        if (lo < offset) lo = offset;
        if (hi > APP_TREND_POINTS) hi = APP_TREND_POINTS;
        if (lo >= hi) continue;
        uint32_t sum = 0;
        uint16_t n = 0;
        for (uint16_t i = lo; i < hi; i++) {
            sum += src[i - offset];
            n++;
        }
        if (n > 0) dst[b] = (uint8_t)(sum / n);
    }
}

int app_clamp_percent(long value)
{
    if (value < 0) return 0;
    if (value > 100) return 100;
    return (int)value;
}

void app_fmt_percent(char *buf, size_t size, int value)
{
    if (!buf || size == 0) return;
    if (value < 0) {
        snprintf(buf, size, "--");
    } else {
        if (value > 100) value = 100;
        snprintf(buf, size, "%d%%", value);
    }
}

void app_fmt_rate(char *buf, size_t size, uint32_t bytes_per_s)
{
    if (!buf || size == 0) return;
    if (bytes_per_s < 1024u) {
        snprintf(buf, size, "%u B/s", (unsigned)bytes_per_s);
    } else if (bytes_per_s < 1024u * 1024u) {
        unsigned k = bytes_per_s / 1024u;
        unsigned d = (bytes_per_s % 1024u) * 10u / 1024u;
        snprintf(buf, size, "%u.%u K/s", k, d);
    } else {
        unsigned m = bytes_per_s / (1024u * 1024u);
        unsigned d = (bytes_per_s % (1024u * 1024u)) * 10u / (1024u * 1024u);
        snprintf(buf, size, "%u.%u M/s", m, d);
    }
}

void app_fmt_uptime(char *buf, size_t size, uint32_t seconds)
{
    if (!buf || size == 0) return;
    uint32_t days = seconds / 86400u;
    uint32_t hours = (seconds % 86400u) / 3600u;
    uint32_t mins = (seconds % 3600u) / 60u;
    if (days > 0) {
        snprintf(buf, size, "%u天%u时", (unsigned)days, (unsigned)hours);
    } else if (hours > 0) {
        snprintf(buf, size, "%u时%u分", (unsigned)hours, (unsigned)mins);
    } else if (mins > 0) {
        snprintf(buf, size, "%u分", (unsigned)mins);
    } else {
        snprintf(buf, size, "%u秒", (unsigned)seconds);
    }
}

void app_fmt_load(char *buf, size_t size, int value_x100)
{
    if (!buf || size == 0) return;
    if (value_x100 < 0) {
        snprintf(buf, size, "--");
    } else {
        snprintf(buf, size, "%d.%02d", value_x100 / 100, value_x100 % 100);
    }
}

void app_fmt_temp(char *buf, size_t size, int value_x10)
{
    if (!buf || size == 0) return;
    if (value_x10 < 0) {
        snprintf(buf, size, "--");
    } else {
        snprintf(buf, size, "%d.%d°C", value_x10 / 10, value_x10 % 10);
    }
}

void app_fmt_ago(char *buf, size_t size, int64_t age_ms)
{
    if (!buf || size == 0) return;
    if (age_ms < 0) age_ms = 0;
    int64_t s = age_ms / 1000;
    if (s < 5) {
        snprintf(buf, size, "刚刚");
    } else if (s < 60) {
        snprintf(buf, size, "%d秒前", (int)s);
    } else if (s < 3600) {
        snprintf(buf, size, "%d分前", (int)(s / 60));
    } else {
        snprintf(buf, size, "%d时前", (int)(s / 3600));
    }
}

void app_fmt_days(char *buf, size_t size, int days)
{
    if (!buf || size == 0) return;
    if (days < 0) {
        snprintf(buf, size, "--");
    } else if (days == 0) {
        snprintf(buf, size, "今天");
    } else {
        snprintf(buf, size, "%d天", days);
    }
}

const char *app_level_color_name(app_level_t level)
{
    switch (level) {
    case APP_LEVEL_WARN: return "warn";
    case APP_LEVEL_CRIT: return "crit";
    default: return "ok";
    }
}

app_level_t app_level_from_percent(int value)
{
    if (value >= 90) return APP_LEVEL_CRIT;
    if (value >= 70) return APP_LEVEL_WARN;
    return APP_LEVEL_OK;
}

const char *app_status_text(const app_server_status_t *status)
{
    if (!status || !status->valid) return "未获取";
    return status->online ? "在线" : "离线";
}
