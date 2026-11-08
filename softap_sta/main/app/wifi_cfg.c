/* wifi_cfg —— WiFi 凭据的 NVS 持久化存储 */
#include <string.h>
#include "nvs.h"
#include "esp_log.h"
#include "wifi_cfg.h"

static const char *TAG = "wifi_cfg";

#define NVS_NAMESPACE   "wifi_cfg"
#define KEY_SSID        "ssid"
#define KEY_PASS        "pass"

esp_err_t wifi_cfg_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "saved ssid:\"%s\" to NVS", ssid);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_cfg_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (ssid == NULL || pass == NULL || ssid_len == 0 || pass_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ssid[0] = '\0';
    pass[0] = '\0';

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;      /* 命名空间不存在 = 从未保存过 */
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = ssid_len;
    err = nvs_get_str(h, KEY_SSID, ssid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(h, KEY_PASS, pass, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';                /* 密码缺失按开放网络处理 */
            err = ESP_OK;
        }
    }
    nvs_close(h);

    if (err == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "loaded ssid:\"%s\" from NVS", ssid);
        return ESP_OK;
    }
    return ESP_ERR_NVS_NOT_FOUND;
}
