/*
 * wifi_cfg —— WiFi 凭据的 NVS 持久化存储
 *
 * 存储位置：NVS 命名空间 "wifi_cfg"，键 "ssid" / "pass"
 * 网页配网保存后，下次开机 wifi_init_sta() 优先使用这里的配置。
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 把 WiFi 凭据保存到 NVS（立即生效于下次开机）
 *
 * @param ssid  WiFi 名称（1~32 字节）
 * @param pass  密码（空字符串表示开放网络）
 * @return ESP_OK 成功
 */
esp_err_t wifi_cfg_save(const char *ssid, const char *pass);

/**
 * @brief 读取 NVS 中保存的 WiFi 凭据
 *
 * 只有存在已保存的非空 SSID 时才返回 ESP_OK；
 * 否则 ssid/pass 填空字符串并返回 ESP_ERR_NVS_NOT_FOUND（调用方回退默认值）。
 */
esp_err_t wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

#ifdef __cplusplus
}
#endif
