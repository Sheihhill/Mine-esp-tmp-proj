/*
 * web_server —— Web 配网 HTTP 服务器
 *
 * 连上 ESP32 热点后，浏览器访问 http://192.168.66.1/ 打开配置页：
 *   GET  /        gzip 压缩的 HTML 配置页面
 *   GET  /status  返回 NVS 中已保存的 SSID（JSON）
 *   POST /save    接收 {"ssid":"...","pass":"..."}，保存到 NVS，2 秒后重启
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 配网 HTTP 服务器（端口 80，非阻塞，服务器运行在独立任务）
 */
esp_err_t web_server_start(void);

#ifdef __cplusplus
}
#endif
