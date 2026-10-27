idf_component_register(
    SRCS "main.c"
         "app_model.c"
         "app_config.c"
         "app_wifi.c"
         "app_http.c"
         "app_net.c"
         "app_portal.c"
         "app_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_hw_support esp_netif esp_timer esp_wifi nvs_flash
             esp_http_client esp_http_server json lvgl esp-tls
    EMBED_FILES "portal.html"
)

# 中文字体子集(GB2312 + ASCII + 常用标点)体积较大,单独加入本组件。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
)
