#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "secrets.h"

// ─── Log Tags ─────────────────────────────────────────────────────────────────
#define TAG_WIFI "WIFI"
#define TAG_WS   "WS"

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT BIT0
static EventGroupHandle_t wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG_WIFI, "Disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_WIFI, "Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASSWORD },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG_WIFI, "Waiting for connection...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
}

// ─── WebSocket Server ─────────────────────────────────────────────────────────
static httpd_handle_t ws_server    = NULL;
static int            ws_client_fd = -1;

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ws_client_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG_WS, "Client connected, fd=%d", ws_client_fd);
        return ESP_OK;
    }
    // Drain incoming frames from browser (unused for now)
    uint8_t buf[64] = {0};
    httpd_ws_frame_t pkt = { .payload = buf };
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    .uri          = "/ws",
    .method       = HTTP_GET,
    .handler      = ws_handler,
    .is_websocket = true,
};

static void ws_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = 80;
    httpd_start(&ws_server, &config);
    httpd_register_uri_handler(ws_server, &ws_uri);
    ESP_LOGI(TAG_WS, "WebSocket ready — connect at ws://<IP>/ws");
}

static void ws_broadcast(const char *json) {
    if (ws_server == NULL || ws_client_fd < 0) return;
    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    if (httpd_ws_send_frame_async(ws_server, ws_client_fd, &pkt) != ESP_OK) {
        ESP_LOGW(TAG_WS, "Send failed — client disconnected");
        ws_client_fd = -1;
    }
}

// ─── Entry Point ──────────────────────────────────────────────────────────────
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    wifi_init();
    ws_server_init();

    // Heartbeat: browser canvas clears every 3s to confirm connection
    while (1) {
        ws_broadcast("{\"type\":\"clear\"}");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
