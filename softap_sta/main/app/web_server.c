/* web_server —— Web 配网 HTTP 服务器（网页存放在 webpage_gz.c 的 gzip 数组里） */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "web_server.h"
#include "wifi_cfg.h"
#include "webpage_gz.h"

static const char *TAG = "web_server";

static httpd_handle_t s_server = NULL;
static esp_timer_handle_t s_restart_timer = NULL;

/* 保存成功后延时重启，让浏览器先收到 HTTP 响应 */
static void restart_timer_cb(void *arg)
{
    ESP_LOGW(TAG, "restarting to apply new WiFi config...");
    esp_restart();
}

/* GET / : 返回 gzip 压缩的配置页面（浏览器自动解压） */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)webpage_gz, WEBPAGE_GZ_LEN);
}

/* GET /status : 返回 WiFi 连接状态 + NVS 中已保存的 SSID */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    /* NVS 保存的 WiFi */
    char saved[33] = {0};
    char pass[65] = {0};
    if (wifi_cfg_load(saved, sizeof(saved), pass, sizeof(pass)) != ESP_OK) {
        saved[0] = '\0';
    }

    /* STA 当前是否连上路由器 */
    wifi_ap_record_t ap_info = {0};
    bool sta_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    char sta_ip[16] = "";
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif != NULL && esp_netif_get_ip_info(sta_netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(sta_ip, sizeof(sta_ip), IPSTR, IP2STR(&ip.ip));
    }

    char ap_ip[16] = "";
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &ip) == ESP_OK) {
        snprintf(ap_ip, sizeof(ap_ip), IPSTR, IP2STR(&ip.ip));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ssid", saved);
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddStringToObject(root, "sta_ssid", sta_connected ? (const char *)ap_info.ssid : "");
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddStringToObject(root, "ap_ssid", CONFIG_ESP_WIFI_AP_SSID);
    cJSON_AddStringToObject(root, "ap_ip", ap_ip);
    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

/* POST /save : 接收 {"ssid":"...","pass":"..."}，校验后保存到 NVS */
static esp_err_t save_post_handler(httpd_req_t *req)
{
    /* 1. 读请求体 */
    int total = req->content_len;
    if (total <= 0 || total > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    char buf[513];
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        received += ret;
    }
    buf[received] = '\0';

    /* 2. 解析 JSON 并校验 */
    cJSON *root = cJSON_Parse(buf);
    cJSON *j_ssid = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "ssid") : NULL;
    cJSON *j_pass = cJSON_IsObject(root) ? cJSON_GetObjectItem(root, "pass") : NULL;

    const char *ssid = cJSON_IsString(j_ssid) ? j_ssid->valuestring : NULL;
    const char *pass = cJSON_IsString(j_pass) ? j_pass->valuestring : NULL;

    const char *err_msg = NULL;
    if (ssid == NULL || ssid[0] == '\0') {
        err_msg = "WiFi 名称不能为空";
    } else if (strlen(ssid) > 32) {
        err_msg = "WiFi 名称过长（最多 32 字节）";
    } else if (pass == NULL) {
        pass = "";
    } else if (pass[0] != '\0' && (strlen(pass) < 8 || strlen(pass) > 63)) {
        err_msg = "密码需 8~63 位；开放网络请留空";
    }

    /* 3. 保存到 NVS */
    esp_err_t err = ESP_OK;
    if (err_msg == NULL) {
        err = wifi_cfg_save(ssid, pass);
        if (err != ESP_OK) {
            err_msg = "NVS 写入失败";
        }
    }

    /* 4. 组织 JSON 响应 */
    cJSON *resp = cJSON_CreateObject();
    if (err_msg == NULL) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "msg", "已保存！设备 2 秒后重启并尝试连接新 WiFi");
        esp_timer_start_once(s_restart_timer, 2000 * 1000);   /* 2s 后重启 */
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "msg", err_msg);
    }
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return send_err;
}

/* POST /ota : 接收固件 .bin（原始二进制），写入 OTA 备份分区并切换启动项 */
#define OTA_BUF_SIZE 4096

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();
    const char *err_msg = NULL;
    esp_ota_handle_t ota = 0;
    bool begun = false;
    char *buf = NULL;

    int total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);

    if (part == NULL) {
        err_msg = "找不到 OTA 分区";
    } else if (total <= 0) {
        err_msg = "固件包为空";
    } else if ((size_t)total > part->size) {
        err_msg = "固件包超过 OTA 分区容量";
    }

    if (err_msg == NULL) {
        buf = malloc(OTA_BUF_SIZE);
        if (buf == NULL) {
            err_msg = "内存不足";
        }
    }

    if (err_msg == NULL) {
        esp_err_t err = esp_ota_begin(part, (size_t)total, &ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
            err_msg = "OTA 初始化失败";
        } else {
            begun = true;
        }
    }

    /* 分块接收并写入 flash */
    int received = 0;
    int timeouts = 0;
    bool first_chunk = true;
    while (err_msg == NULL && received < total) {
        int chunk = total - received;
        if (chunk > OTA_BUF_SIZE) {
            chunk = OTA_BUF_SIZE;
        }
        int ret = httpd_req_recv(req, buf, chunk);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                err_msg = "接收超时";
            }
            continue;
        }
        timeouts = 0;
        if (ret <= 0) {
            err_msg = "接收中断，升级已取消";
            break;
        }
        if (first_chunk) {
            first_chunk = false;
            if ((uint8_t)buf[0] != 0xE9) {
                err_msg = "不是有效的 ESP32 固件（缺少 0xE9 魔数）";
                break;
            }
        }
        if (esp_ota_write(ota, buf, ret) != ESP_OK) {
            err_msg = "flash 写入失败";
            break;
        }
        received += ret;
    }

    /* esp_ota_end 会校验镜像完整性，损坏/不匹配的包在这里被拒绝 */
    if (err_msg == NULL && begun) {
        esp_err_t err = esp_ota_end(ota);
        begun = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            err_msg = (err == ESP_ERR_OTA_VALIDATE_FAILED) ?
                      "固件校验失败（包损坏或不是本工程的固件）" : "OTA 完成校验失败";
        } else if (esp_ota_set_boot_partition(part) != ESP_OK) {
            err_msg = "设置启动分区失败";
        }
    }

    if (begun) {
        esp_ota_abort(ota);
    }
    free(buf);

    if (err_msg == NULL) {
        ESP_LOGW(TAG, "OTA ok: %d bytes, boot partition -> %s", received, part->label);
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "msg", "升级成功！设备 3 秒后重启进入新固件");
        esp_timer_start_once(s_restart_timer, 3000 * 1000);
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", err_msg);
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "msg", err_msg);
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return send_err;
}

esp_err_t web_server_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    /* 延时重启定时器（一次性） */
    const esp_timer_create_args_t timer_args = {
        .callback = restart_timer_cb,
        .name = "web_restart",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_restart_timer));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler,
    };
    const httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler,
    };
    const httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = save_post_handler,
    };
    const httpd_uri_t uri_ota = {
        .uri = "/ota", .method = HTTP_POST, .handler = ota_post_handler,
    };
    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_save);
    httpd_register_uri_handler(s_server, &uri_ota);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}
