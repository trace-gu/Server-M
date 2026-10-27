// tests/test_app_model.c —— 服务器监控应用的纯逻辑主机测试。
#include "app_model.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_utf8(void)
{
    CHECK(app_utf8_valid("web-01"));
    CHECK(app_utf8_valid("生产服务器"));
    CHECK(!app_utf8_valid("bad\xff"));
    CHECK(!app_utf8_valid("\xe4\xb8"));          // truncated 3-byte char
    CHECK(!app_utf8_valid("\xc0\xaf"));          // overlong

    char buf[8];
    size_t n = app_utf8_truncate(buf, sizeof(buf), "中文服务器");
    CHECK(n == 6);
    CHECK(strcmp(buf, "中文") == 0);
    CHECK(app_utf8_valid(buf));

    n = app_utf8_truncate(buf, 4, "中文");
    CHECK(n == 3);
    CHECK(strcmp(buf, "中") == 0);
}

static void test_url_token_name(void)
{
    CHECK(app_url_valid("https://monitor.example.com/status"));
    CHECK(app_url_valid("https://10.0.0.5:8443/status"));
    CHECK(!app_url_valid("http://monitor.example.com/status"));
    CHECK(!app_url_valid("https://"));
    CHECK(!app_url_valid("https:///status"));
    CHECK(!app_url_valid("https://monitor.example.com/has space"));

    char long_url[APP_URL_MAX + 2];
    memset(long_url, 'a', sizeof(long_url));
    long_url[0] = '\0';
    CHECK(!app_url_valid(long_url));

    CHECK(app_token_valid("abcDEF0123-_"));
    CHECK(!app_token_valid(""));
    CHECK(!app_token_valid("has space"));
    CHECK(!app_token_valid("换行\t"));

    CHECK(app_name_valid("生产环境 Web-01"));
    CHECK(!app_name_valid(""));
    CHECK(!app_name_valid("line\nbreak"));
}

static void test_config_server(void)
{
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;
    cfg.refresh_s = 10;
    cfg.screen_off_s = 60;

    int a = app_server_add(&cfg, "网页服务器", "https://a.example.com/status", "token-a");
    int b = app_server_add(&cfg, "数据库", "https://b.example.com/status", "token-b");
    CHECK(a == 0 && b == 1);
    CHECK(cfg.server_count == 2);
    CHECK(app_server_add(&cfg, "bad", "http://x/status", "t") == -1);
    CHECK(app_server_add(&cfg, "bad", "https://x/status", "with space") == -1);

    CHECK(app_server_update(&cfg, 1, "数据库主", "https://b.example.com/status", "token-b2"));
    CHECK(strcmp(cfg.servers[1].name, "数据库主") == 0);
    CHECK(!app_server_update(&cfg, 5, "x", "https://x/status", "t"));

    CHECK(app_server_remove(&cfg, 0));
    CHECK(cfg.server_count == 1);
    CHECK(strcmp(cfg.servers[0].name, "数据库主") == 0);

    // 超长名称必须在 UTF-8 边界截断,而不是截半个汉字。
    char long_name[APP_NAME_MAX + 8];
    memset(long_name, 0, sizeof(long_name));
    for (int i = 0; i < APP_NAME_MAX / 3 + 2; i++) strcat(long_name, "服");
    CHECK(app_server_add(&cfg, long_name, "https://c.example.com/status", "token-c") == 1);
    CHECK(app_name_valid(cfg.servers[1].name));
}

static void test_config_sanitize(void)
{
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 99;
    cfg.server_count = 3;
    strcpy(cfg.servers[0].name, "ok");
    strcpy(cfg.servers[0].url, "https://ok.example.com/status");
    strcpy(cfg.servers[0].token, "tok");
    strcpy(cfg.servers[1].name, "bad");
    strcpy(cfg.servers[1].url, "http://bad/status");
    strcpy(cfg.servers[1].token, "tok");
    strcpy(cfg.servers[2].name, "ok2");
    strcpy(cfg.servers[2].url, "https://ok2.example.com/status");
    strcpy(cfg.servers[2].token, "tok2");
    cfg.wifi_count = 3;
    strcpy(cfg.wifi[0].ssid, "home");
    strcpy(cfg.wifi[0].pass, "12345678");
    strcpy(cfg.wifi[1].ssid, "");
    strcpy(cfg.wifi[2].ssid, "office");
    strcpy(cfg.wifi[2].pass, "short");
    cfg.active_wifi = 2;
    cfg.refresh_s = 0;
    cfg.screen_off_s = 5000;

    app_config_sanitize(&cfg);
    CHECK(cfg.version == 1);
    CHECK(cfg.server_count == 2);
    CHECK(strcmp(cfg.servers[1].name, "ok2") == 0);
    CHECK(cfg.wifi_count == 1);
    CHECK(cfg.active_wifi == 0);
    CHECK(cfg.refresh_s == APP_DEFAULT_REFRESH_S);
    CHECK(cfg.screen_off_s == APP_DEFAULT_SCREEN_OFF_S);
}

static void test_wifi(void)
{
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    CHECK(app_wifi_add(&cfg, "home", "12345678") == 0);
    CHECK(app_wifi_add(&cfg, "office", "abcdefgh") == 1);
    CHECK(app_wifi_find(&cfg, "office") == 1);
    CHECK(app_wifi_add(&cfg, "home", "87654321") == 0);   // 更新密码
    CHECK(strcmp(cfg.wifi[0].pass, "87654321") == 0);
    CHECK(cfg.wifi_count == 2);

    CHECK(app_wifi_set_active(&cfg, 1));
    CHECK(cfg.active_wifi == 1);
    CHECK(app_wifi_remove(&cfg, 0));
    CHECK(cfg.active_wifi == 0);
    CHECK(strcmp(cfg.wifi[0].ssid, "office") == 0);
    CHECK(app_wifi_remove(&cfg, 0));
    CHECK(cfg.wifi_count == 0);
    CHECK(!app_wifi_remove(&cfg, 0));

    CHECK(app_wifi_add(&cfg, "short", "123") == -1);
    CHECK(app_wifi_add(&cfg, "open", "") == 0);
}

static void test_selection_refresh(void)
{
    CHECK(app_selection_move(0, 5, 1) == 1);
    CHECK(app_selection_move(4, 5, 1) == 0);
    CHECK(app_selection_move(0, 5, -1) == 4);
    CHECK(app_selection_move(-1, 5, 0) == 0);
    CHECK(app_selection_move(0, 0, 1) == 0);

    app_refresh_ctx_t ctx = {
        .screen_on = true,
        .detail_active = true,
        .have_server = true,
        .inflight = false,
        .now_ms = 100000,
        .last_start_ms = 0,
        .last_result_ms = 0,
        .interval_s = 10,
    };
    CHECK(app_refresh_due(&ctx));

    ctx.last_start_ms = 91000;
    CHECK(!app_refresh_due(&ctx));
    ctx.now_ms = 101001;
    CHECK(app_refresh_due(&ctx));

    ctx.inflight = true;
    CHECK(!app_refresh_due(&ctx));
    ctx.inflight = false;
    ctx.detail_active = false;
    CHECK(!app_refresh_due(&ctx));
    ctx.detail_active = true;
    ctx.screen_on = false;
    CHECK(!app_refresh_due(&ctx));

    CHECK(app_refresh_interval_ms(0) == 5000);
    CHECK(app_refresh_interval_ms(10) == 10000);
    CHECK(app_refresh_interval_ms(9999) == 300000);
}

static void test_trend(void)
{
    uint8_t src[APP_TREND_POINTS];
    uint8_t dst[APP_TREND_BARS];

    for (int i = 0; i < APP_TREND_POINTS; i++) src[i] = (uint8_t)(i % 100);
    app_trend_reduce(src, APP_TREND_POINTS, dst, APP_TREND_BARS);
    CHECK(dst[0] == 5);    // 0..11 的平均值 5.5 -> 5
    // 276..287 -> 76..87, 平均 81.5 -> 81
    CHECK(dst[23] == 81);

    uint8_t few[10];
    for (int i = 0; i < 10; i++) few[i] = 50;
    app_trend_reduce(few, 10, dst, APP_TREND_BARS);
    for (int b = 0; b < 23; b++) CHECK(dst[b] == 0xff);
    CHECK(dst[23] == 50);

    app_trend_reduce(NULL, 0, dst, APP_TREND_BARS);
    CHECK(dst[0] == 0xff);
    CHECK(app_clamp_percent(-5) == 0);
    CHECK(app_clamp_percent(105) == 100);
    CHECK(app_clamp_percent(55) == 55);
}

static void test_format(void)
{
    char buf[64];

    app_fmt_percent(buf, sizeof(buf), 48);
    CHECK(strcmp(buf, "48%") == 0);
    app_fmt_percent(buf, sizeof(buf), -1);
    CHECK(strcmp(buf, "--") == 0);
    app_fmt_percent(buf, sizeof(buf), 130);
    CHECK(strcmp(buf, "100%") == 0);

    app_fmt_rate(buf, sizeof(buf), 512);
    CHECK(strcmp(buf, "512 B/s") == 0);
    app_fmt_rate(buf, sizeof(buf), 2048);
    CHECK(strcmp(buf, "2.0 K/s") == 0);
    app_fmt_rate(buf, sizeof(buf), 3u * 1024u * 1024u + 512u * 1024u);
    CHECK(strcmp(buf, "3.5 M/s") == 0);

    app_fmt_uptime(buf, sizeof(buf), 30);
    CHECK(strcmp(buf, "30秒") == 0);
    app_fmt_uptime(buf, sizeof(buf), 3 * 3600 + 20 * 60);
    CHECK(strcmp(buf, "3时20分") == 0);
    app_fmt_uptime(buf, sizeof(buf), 12 * 86400 + 3 * 3600);
    CHECK(strcmp(buf, "12天3时") == 0);

    app_fmt_load(buf, sizeof(buf), 42);
    CHECK(strcmp(buf, "0.42") == 0);
    app_fmt_load(buf, sizeof(buf), 1234);
    CHECK(strcmp(buf, "12.34") == 0);
    app_fmt_temp(buf, sizeof(buf), 452);
    CHECK(strcmp(buf, "45.2°C") == 0);
    app_fmt_ago(buf, sizeof(buf), 3000);
    CHECK(strcmp(buf, "刚刚") == 0);
    app_fmt_ago(buf, sizeof(buf), 65000);
    CHECK(strcmp(buf, "1分前") == 0);
    app_fmt_days(buf, sizeof(buf), 0);
    CHECK(strcmp(buf, "今天") == 0);
    app_fmt_days(buf, sizeof(buf), 8);
    CHECK(strcmp(buf, "8天") == 0);

    CHECK(app_level_from_percent(10) == APP_LEVEL_OK);
    CHECK(app_level_from_percent(75) == APP_LEVEL_WARN);
    CHECK(app_level_from_percent(95) == APP_LEVEL_CRIT);

    app_server_status_t st = {0};
    CHECK(strcmp(app_status_text(&st), "未获取") == 0);
    st.valid = true;
    CHECK(strcmp(app_status_text(&st), "离线") == 0);
    st.online = true;
    CHECK(strcmp(app_status_text(&st), "在线") == 0);
}

int main(void)
{
    test_utf8();
    test_url_token_name();
    test_config_server();
    test_config_sanitize();
    test_wifi();
    test_selection_refresh();
    test_trend();
    test_format();

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_app_model: PASS\n");
    return 0;
}
