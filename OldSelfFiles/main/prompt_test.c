#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
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

    char auth[128];
    snprintf(auth, sizeof(auth), "Bearer %s", OPENAI_API_KEY);

    esp_http_client_config_t cfg = {
        .url               = "https://api.openai.com/v1/chat/completions",
        .event_handler     = http_event_handler,
        .transport_type    = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method            = HTTP_METHOD_POST,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        // Parse "word":"<value>" out of the OpenAI response
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
                    char json[64];
                    snprintf(json, sizeof(json),
                             "{\"type\":\"move\",\"x\":%d,\"y\":%d,\"penDown\":%s}",
                             x, y, pen ? "true" : "false");
                    ws_broadcast(json);
                    break;
                }

                case PKT_CLEAR:
                    ws_broadcast("{\"type\":\"clear\"}");
                    ESP_LOGI(TAG_NET, "Canvas cleared");
                    break;

                case PKT_PROMPT:
                    fetch_pictionary_prompt();
                    break;

                case PKT_SUBMIT:
                    ESP_LOGI(TAG_NET, "PKT_SUBMIT received — stub (implement in commit 6)");
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

    wifi_init();
    ws_server_init();
    uart_init();
    uart_to_net_queue = xQueueCreate(10, sizeof(uart_packet_t));

    xTaskCreatePinnedToCore(uart_task,    "uart_task", 4096, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(network_task, "net_task",  8192, NULL, 4, NULL, 1);

    // ── Mock: trigger a prompt request ~1s after boot ──
    vTaskDelay(pdMS_TO_TICKS(1000));
    inject_mock(PKT_PROMPT, NULL, 0);
}
