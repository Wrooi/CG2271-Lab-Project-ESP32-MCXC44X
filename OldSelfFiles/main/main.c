#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "secrets.h"

// ─── Log Tags ─────────────────────────────────────────────────────────────────
#define TAG_WIFI "WIFI"
#define TAG_WS   "WS"
#define TAG_UART "UART"
#define TAG_NET  "NET"
#define TAG_HTTP "HTTP"

// ─── Wi-Fi ────────────────────────────────────────────────────────────────────
#define WIFI_CONNECTED_BIT BIT0

static const char *openai_root_ca = 
"-----BEGIN CERTIFICATE-----\n"
"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\n"
"-----END CERTIFICATE-----\n";


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

static void sync_time(void) {
    ESP_LOGI(TAG_WIFI, "Initializing SNTP...");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    
    // Wait until the year is > 1970 (i.e. time is synced)
    while (timeinfo.tm_year < (2020 - 1900) && ++retry < 15) {
        ESP_LOGI(TAG_WIFI, "Waiting for system time to be set... (%d/15)", retry);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    
    if (timeinfo.tm_year < (2020 - 1900)) {
        ESP_LOGE(TAG_WIFI, "Failed to get time from NTP server! HTTPS requests will fail.");
    } else {
        // Set timezone to UTC (or your local timezone) so mbedTLS is happy
        setenv("TZ", "UTC0", 1);
        tzset();
        ESP_LOGI(TAG_WIFI, "Time is correct! Current year: %d", timeinfo.tm_year + 1900);
    }
}

// ─── HTTP Response Buffer ─────────────────────────────────────────────────────
#define HTTP_RESPONSE_BUF_SIZE 4096
static char response_buf[HTTP_RESPONSE_BUF_SIZE];
static int  response_len = 0;
static SemaphoreHandle_t http_mutex;

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
        default:
            break;
    }
    return ESP_OK;
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
    uint8_t buf[64] = {0};
    httpd_ws_frame_t pkt = { .payload = buf };
    httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    .uri = "/ws", .method = HTTP_GET,
    .handler = ws_handler, .is_websocket = true,
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

// ─── UART + Packet Protocol ───────────────────────────────────────────────────
#define UART_PORT   UART_NUM_1
#define UART_TX_PIN GPIO_NUM_17
#define UART_RX_PIN GPIO_NUM_16
#define UART_BAUD   9600
#define UART_BUF    1024
#define SOF         0xAA

#define PKT_STROKE  0x01
#define PKT_PEN     0x02
#define PKT_CLEAR   0x03
#define PKT_SUBMIT  0x04
#define PKT_PROMPT  0x05
#define PKT_ACK     0x81
#define PKT_NACK    0x82

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint8_t len;
    uint8_t payload[32];
} uart_packet_t;

static QueueHandle_t uart_to_net_queue;

static uint8_t crc8(uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

static bool parse_packet(uint8_t *buf, int len, uart_packet_t *out) {
    if (len < 5 || buf[0] != SOF) return false;
    out->type = buf[1];
    out->seq  = buf[2];
    out->len  = buf[3];
    if (len < 5 + out->len || out->len > 32) return false;
    memcpy(out->payload, &buf[4], out->len);
    return crc8(&buf[1], 3 + out->len) == buf[4 + out->len];
}

static void send_raw_packet(uint8_t type, uint8_t seq,
                             uint8_t *payload, uint8_t plen) {
    uint8_t buf[64];
    buf[0] = SOF; buf[1] = type; buf[2] = seq; buf[3] = plen;
    if (plen > 0 && payload) memcpy(&buf[4], payload, plen);
    buf[4 + plen] = crc8(&buf[1], 3 + plen);
    uart_write_bytes(UART_PORT, (char *)buf, 5 + plen);
}

static void send_ack(uint8_t seq)  { uint8_t p[] = {seq}; send_raw_packet(PKT_ACK,  0, p, 1); }
static void send_nack(uint8_t seq) { uint8_t p[] = {seq}; send_raw_packet(PKT_NACK, 0, p, 1); }

static void uart_init(void) {
    uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, -1, -1);
    uart_driver_install(UART_PORT, UART_BUF, UART_BUF, 0, NULL, 0);
    ESP_LOGI(TAG_UART, "UART1 ready @ %d baud", UART_BAUD);
}

static void inject_mock(uint8_t type, uint8_t *payload, uint8_t plen) {
    uart_packet_t pkt = { .type = type, .seq = 1, .len = plen };
    if (plen && payload) memcpy(pkt.payload, payload, plen);
    xQueueSend(uart_to_net_queue, &pkt, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG_UART, "Injected mock type=0x%02X", type);
}

// ─── Framebuffer ──────────────────────────────────────────────────────────────
// NEW in this commit: ESP32 now tracks every pixel drawn.
// This is the source-of-truth image that will be sent to the vision API in commit 7.
#define FB_W 128
#define FB_H 128
static uint8_t framebuffer[FB_H][FB_W];  // 1 = ink, 0 = blank
static SemaphoreHandle_t fb_mutex;

// ─── OpenAI: Prompt Generation ────────────────────────────────────────────────
static char current_prompt[64] = {0};

static void fetch_pictionary_prompt(void) {
    xSemaphoreTake(http_mutex, portMAX_DELAY);
    response_len = 0;
    memset(response_buf, 0, sizeof(response_buf));

    const char *body =
        "{\"model\":\"gpt-4o-mini\","
        "\"messages\":[{\"role\":\"user\","
        "\"content\":\"Give me one simple Pictionary noun. "
        "Respond ONLY with JSON: {\\\"word\\\":\\\"<noun>\\\"}\"}]}";

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", OPENAI_API_KEY);

    esp_http_client_config_t cfg = {
        .url               = "https://api.openai.com/v1/chat/completions",
        .event_handler     = http_event_handler,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem          = openai_root_ca,           // Must use our perfectly formatted cert
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        char *w = strstr(response_buf, "\"word\"");
        if (w) {
            w = strchr(w, ':'); w++;
            while (*w == ' ' || *w == '"') w++;
            char *end = strchr(w, '"');
            if (end) {
                size_t wlen = end - w;
                strncpy(current_prompt, w, wlen);
                current_prompt[wlen] = '\0';
                ESP_LOGI(TAG_NET, "Prompt word: %s", current_prompt);
                char json[96];
                snprintf(json, sizeof(json),
                         "{\"type\":\"prompt\",\"word\":\"%s\"}", current_prompt);
                ws_broadcast(json);
            }
        } else {
            ESP_LOGW(TAG_NET, "Could not parse word from response:\n%s", response_buf);
        }
    } else {
        ESP_LOGE(TAG_NET, "Prompt API failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(http_mutex);
}

// ─── UART Task (Core 0) ───────────────────────────────────────────────────────
static void uart_task(void *pv) {
    uint8_t buf[128];
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf) - 1, pdMS_TO_TICKS(20));
        if (len > 0) {
            uart_packet_t pkt;
            if (parse_packet(buf, len, &pkt)) {
                send_ack(pkt.seq);
                if (xQueueSend(uart_to_net_queue, &pkt, pdMS_TO_TICKS(10)) != pdTRUE)
                    ESP_LOGW(TAG_UART, "Queue full, dropped 0x%02X", pkt.type);
            } else {
                send_nack(buf[2]);
                ESP_LOGW(TAG_UART, "Bad packet — CRC or SOF mismatch");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Network Task (Core 1) ────────────────────────────────────────────────────
static void network_task(void *pv) {
    uart_packet_t pkt;
    while (1) {
        if (xQueueReceive(uart_to_net_queue, &pkt, pdMS_TO_TICKS(100))) {
            ESP_LOGI(TAG_NET, "Packet type=0x%02X seq=%d", pkt.type, pkt.seq);
            switch (pkt.type) {

                case PKT_STROKE: {
                    uint16_t x   = (pkt.payload[0] << 8) | pkt.payload[1];
                    uint16_t y   = (pkt.payload[2] << 8) | pkt.payload[3];
                    uint8_t  pen = pkt.payload[4];

                    // NEW: write into framebuffer under mutex
                    if (pen && x < FB_W && y < FB_H) {
                        xSemaphoreTake(fb_mutex, portMAX_DELAY);
                        framebuffer[y][x] = 1;
                        ESP_LOGD(TAG_NET, "FB[%d][%d] = 1", y, x);
                        xSemaphoreGive(fb_mutex);
                    }

                    char json[64];
                    snprintf(json, sizeof(json),
                             "{\"type\":\"move\",\"x\":%d,\"y\":%d,\"penDown\":%s}",
                             x, y, pen ? "true" : "false");
                    ws_broadcast(json);
                    break;
                }

                case PKT_CLEAR:
                    // NEW: clear framebuffer under mutex
                    xSemaphoreTake(fb_mutex, portMAX_DELAY);
                    memset(framebuffer, 0, sizeof(framebuffer));
                    xSemaphoreGive(fb_mutex);

                    ws_broadcast("{\"type\":\"clear\"}");
                    ESP_LOGI(TAG_NET, "Canvas + framebuffer cleared");
                    break;

                case PKT_PROMPT:
                    fetch_pictionary_prompt();
                    break;

                case PKT_SUBMIT:
                    // TODO commit 7: encode framebuffer as PNG → base64 → gpt-4o vision API
                    ESP_LOGI(TAG_NET, "PKT_SUBMIT received — vision API stub");
                    break;

                default:
                    ESP_LOGW(TAG_NET, "Unknown packet type 0x%02X", pkt.type);
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ─── Entry Point ──────────────────────────────────────────────────────────────
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    http_mutex = xSemaphoreCreateMutex();
    fb_mutex   = xSemaphoreCreateMutex();  // NEW

    wifi_init();
    sync_time();
    ws_server_init();
    uart_init();
    uart_to_net_queue = xQueueCreate(10, sizeof(uart_packet_t));

    xTaskCreatePinnedToCore(uart_task,    "uart_task", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(network_task, "net_task",  8192, NULL, 4, NULL, 0);

    // ── Mock: prompt + a few strokes + clear to verify framebuffer ──
    vTaskDelay(pdMS_TO_TICKS(1000));
    inject_mock(PKT_PROMPT, NULL, 0);

    vTaskDelay(pdMS_TO_TICKS(3000));  // wait for prompt API to finish
    uint8_t s1[] = {0x00, 60, 0x00, 60, 1};
    uint8_t s2[] = {0x00, 70, 0x00, 70, 1};
    uint8_t s3[] = {0x00, 80, 0x00, 80, 1};
    inject_mock(PKT_STROKE, s1, 5);
    inject_mock(PKT_STROKE, s2, 5);
    inject_mock(PKT_STROKE, s3, 5);

    // Verify: check LOGD output for "FB[y][x] = 1" entries
    // Then test clear:
    vTaskDelay(pdMS_TO_TICKS(2000));
    inject_mock(PKT_CLEAR, NULL, 0);
}
