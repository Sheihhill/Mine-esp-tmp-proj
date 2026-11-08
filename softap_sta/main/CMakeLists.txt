idf_component_register(SRCS "softap_sta.c"
                            "app/user_display.c"
                            "app/wifi_cfg.c"
                            "app/web_server.c"
                            "web/webpage_gz.c"
                    INCLUDE_DIRS "."
                                 "app/inc"
                                 "web")
# 说明：main 组件默认依赖工程内所有组件（esp_http_server / nvs_flash / json / app_update 等），
# 无需在此声明 REQUIRES。
# 修改 main/web/index.html 后，重新运行：
#   python tools/html_to_c.py main/web/index.html --out-dir main
# 即可更新 webpage_gz.c/.h
