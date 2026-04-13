#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "secrets.h"

// ─── Tags for log output ────────────────────────────────────────────────────
#define TAG_WIFI "WIFI"
#define TAG_HTTP "HTTP"

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
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
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG_WIFI, "Waiting for connection...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);
}

// ─── HTTPS Test ──────────────────────────────────────────────────────────────
#define HTTP_RESPONSE_BUF_SIZE 2048
static char response_buf[HTTP_RESPONSE_BUF_SIZE];
static int  response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (response_len + evt->data_len < HTTP_RESPONSE_BUF_SIZE) {
                memcpy(response_buf + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                response_buf[response_len] = '\0';
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG_HTTP, "Request finished");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG_HTTP, "Disconnected");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void test_https_get(void) {
    response_len = 0;
    memset(response_buf, 0, sizeof(response_buf));

    esp_http_client_config_t config = {
        .url                    = "https://httpbin.org/get",
        .event_handler          = http_event_handler,
        .transport_type         = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,  // OK for testing, remove in production
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG_HTTP, "HTTP status: %d", status);
        ESP_LOGI(TAG_HTTP, "Response body:\n%s", response_buf);
    } else {
        ESP_LOGE(TAG_HTTP, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
void app_main(void) {
    // Init non-volatile storage (required for Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Phase 1: connect to Wi-Fi
    wifi_init();

    // Phase 2: test HTTPS GET
    test_https_get();

    // Idle — future tasks will be started here
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}