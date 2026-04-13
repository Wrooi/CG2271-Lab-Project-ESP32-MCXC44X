#include "app_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"
#include "mbedtls/base64.h"

#define APP_HTTP_RESPONSE_BUFFER_SIZE 8192
#define APP_AI_CONTENT_BUFFER_SIZE 768
#define APP_HTTP_TIMEOUT_MS 30000
#define APP_HTTP_PERFORM_MAX_ATTEMPTS 3
#define APP_HTTP_FALLBACK_CONNECT_RETRIES 3
#define APP_SUBMIT_STUB_GUESS "stub-guess"
#define APP_SUBMIT_STUB_CONFIDENCE 7
#define APP_SUBMIT_STUB_DELAY_MS 2000

typedef struct {
    char *buffer;
    size_t capacity;
    size_t length;
} app_http_response_buffer_t;

static const char *TAG = "app_api";

static int app_parse_confidence_value(const cJSON *value)
{
    if (cJSON_IsNumber(value)) {
        return (int)value->valuedouble;
    }

    if (cJSON_IsString(value) && value->valuestring != NULL) {
        int parsed = 0;
        if (sscanf(value->valuestring, "%d", &parsed) == 1) {
            return parsed;
        }
    }

    return -1;
}

static bool app_copy_guess_and_confidence(const cJSON *obj,
                                          char *out_guess,
                                          size_t out_guess_len,
                                          int *out_confidence)
{
    if (obj == NULL || out_guess == NULL || out_guess_len < 2U || out_confidence == NULL) {
        return false;
    }

    const cJSON *guess_obj = cJSON_GetObjectItemCaseSensitive(obj, "guess");
    if (!cJSON_IsString(guess_obj) || guess_obj->valuestring == NULL) {
        guess_obj = cJSON_GetObjectItemCaseSensitive(obj, "object");
    }
    if (!cJSON_IsString(guess_obj) || guess_obj->valuestring == NULL) {
        guess_obj = cJSON_GetObjectItemCaseSensitive(obj, "label");
    }
    if (!cJSON_IsString(guess_obj) || guess_obj->valuestring == NULL) {
        guess_obj = cJSON_GetObjectItemCaseSensitive(obj, "word");
    }

    const cJSON *confidence_obj = cJSON_GetObjectItemCaseSensitive(obj, "confidence");
    if (confidence_obj == NULL) {
        confidence_obj = cJSON_GetObjectItemCaseSensitive(obj, "score");
    }

    const int parsed_confidence = app_parse_confidence_value(confidence_obj);
    if (!cJSON_IsString(guess_obj) || guess_obj->valuestring == NULL || parsed_confidence < 0) {
        return false;
    }

    const size_t guess_len = strnlen(guess_obj->valuestring, out_guess_len - 1U);
    memcpy(out_guess, guess_obj->valuestring, guess_len);
    out_guess[guess_len] = '\0';
    *out_confidence = parsed_confidence;
    return true;
}

static bool app_try_extract_from_plain_text(const char *text,
                                            char *out_guess,
                                            size_t out_guess_len,
                                            int *out_confidence)
{
    if (text == NULL || out_guess == NULL || out_confidence == NULL || out_guess_len < 2U) {
        return false;
    }

    const char *guess_marker = strstr(text, "guess");
    if (guess_marker == NULL) {
        return false;
    }

    const char *colon = strchr(guess_marker, ':');
    if (colon == NULL) {
        return false;
    }

    const char *start = colon + 1;
    while (*start != '\0' && (isspace((unsigned char)*start) || *start == '"' || *start == '\'')) {
        start++;
    }

    size_t i = 0U;
    while (start[i] != '\0' && i + 1U < out_guess_len) {
        const char ch = start[i];
        if (!(isalnum((unsigned char)ch) || ch == ' ' || ch == '-' || ch == '_')) {
            break;
        }
        out_guess[i] = ch;
        i++;
    }
    out_guess[i] = '\0';
    if (out_guess[0] == '\0') {
        return false;
    }

    int confidence = 5;
    const char *confidence_marker = strstr(text, "confidence");
    if (confidence_marker != NULL) {
        const char *conf_colon = strchr(confidence_marker, ':');
        if (conf_colon != NULL) {
            int parsed = 0;
            if (sscanf(conf_colon + 1, "%d", &parsed) == 1) {
                confidence = parsed;
            }
        }
    }

    *out_confidence = confidence;
    return true;
}

static bool app_try_parse_json_fragment(const char *text,
                                        char *out_guess,
                                        size_t out_guess_len,
                                        int *out_confidence)
{
    if (text == NULL) {
        return false;
    }

    cJSON *parsed = cJSON_Parse(text);
    if (parsed != NULL) {
        const bool ok = app_copy_guess_and_confidence(parsed, out_guess, out_guess_len, out_confidence);
        cJSON_Delete(parsed);
        if (ok) {
            return true;
        }
    }

    const char *json_start = strchr(text, '{');
    const char *json_end = strrchr(text, '}');
    if (json_start == NULL || json_end == NULL || json_end <= json_start) {
        return false;
    }

    const size_t frag_len = (size_t)(json_end - json_start + 1);
    char *frag = (char *)malloc(frag_len + 1U);
    if (frag == NULL) {
        return false;
    }

    memcpy(frag, json_start, frag_len);
    frag[frag_len] = '\0';
    parsed = cJSON_Parse(frag);
    free(frag);
    if (parsed == NULL) {
        return false;
    }

    const bool ok = app_copy_guess_and_confidence(parsed, out_guess, out_guess_len, out_confidence);
    cJSON_Delete(parsed);
    return ok;
}

static bool app_is_api_key_configured(void)
{
    return (OPENAI_API_KEY[0] != '\0') && (strcmp(OPENAI_API_KEY, "YOUR_OPENAI_API_KEY") != 0);
}

static esp_err_t app_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_http_response_buffer_t *response = (app_http_response_buffer_t *)evt->user_data;
    if (response == NULL) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0) {
        if (response->length >= response->capacity) {
            return ESP_OK;
        }

        const size_t available = response->capacity - response->length - 1U;
        if (available == 0U) {
            return ESP_OK;
        }

        const size_t copy_len = (evt->data_len < (int)available) ? (size_t)evt->data_len : available;
        memcpy(response->buffer + response->length, evt->data, copy_len);
        response->length += copy_len;
        response->buffer[response->length] = '\0';
    }

    return ESP_OK;
}

static esp_err_t app_http_perform_with_retry(esp_http_client_handle_t client, const char *phase)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= APP_HTTP_PERFORM_MAX_ATTEMPTS; ++attempt) {
        err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            return ESP_OK;
        }

        if (err != ESP_ERR_HTTP_EAGAIN || attempt == APP_HTTP_PERFORM_MAX_ATTEMPTS) {
            ESP_LOGE(TAG, "%s HTTP POST failed: %s", phase, esp_err_to_name(err));
            return err;
        }

        ESP_LOGW(TAG,
                 "%s HTTP POST timed out waiting for data (attempt %d/%d), retrying",
                 phase,
                 attempt,
                 APP_HTTP_PERFORM_MAX_ATTEMPTS);
    }

    return err;
}

static esp_err_t app_http_post_json(const char *url,
                                    const char *body,
                                    char *out_response,
                                    size_t out_response_len)
{
    if (url == NULL || body == NULL || out_response == NULL || out_response_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    out_response[0] = '\0';
    app_http_response_buffer_t response = {
        .buffer = out_response,
        .capacity = out_response_len,
        .length = 0U,
    };

    char auth_header[256];
    const int auth_written = snprintf(auth_header, sizeof(auth_header), "Bearer %s", OPENAI_API_KEY);
    if (auth_written <= 0 || (size_t)auth_written >= sizeof(auth_header)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = app_http_event_handler,
        .user_data = &response,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = APP_HTTP_TIMEOUT_MS,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = app_http_perform_with_retry(client, "Verified TLS");
    if (err != ESP_OK) {
        // Keep secure verification first. Retry once without certificate verification only
        // when the verified TLS connection cannot be established.
        if (err == ESP_ERR_HTTP_CONNECT) {
            ESP_LOGW(TAG, "Retrying HTTPS POST once with insecure TLS fallback for API testing");

            response.length = 0U;
            out_response[0] = '\0';
            config.crt_bundle_attach = NULL;

            for (int connect_attempt = 1; connect_attempt <= APP_HTTP_FALLBACK_CONNECT_RETRIES; ++connect_attempt) {
                esp_http_client_cleanup(client);
                client = esp_http_client_init(&config);
                if (client == NULL) {
                    return ESP_ERR_NO_MEM;
                }

                esp_http_client_set_header(client, "Content-Type", "application/json");
                esp_http_client_set_header(client, "Authorization", auth_header);
                esp_http_client_set_post_field(client, body, strlen(body));

                char phase[48];
                snprintf(phase, sizeof(phase), "Insecure fallback #%d", connect_attempt);
                err = app_http_perform_with_retry(client, phase);
                if (err == ESP_OK) {
                    ESP_LOGW(TAG, "Insecure TLS fallback succeeded (testing mode)");
                    break;
                }

                if (err != ESP_ERR_HTTP_CONNECT || connect_attempt == APP_HTTP_FALLBACK_CONNECT_RETRIES) {
                    break;
                }

                ESP_LOGW(TAG,
                         "Insecure fallback connect failed (%d/%d), retrying fresh connection",
                         connect_attempt,
                         APP_HTTP_FALLBACK_CONNECT_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(750));
            }
        }
    }

    esp_http_client_cleanup(client);
    return err;
}

static bool app_extract_vision_result(const char *response_json,
                                      char *out_guess,
                                      size_t out_guess_len,
                                      int *out_confidence)
{
    if (response_json == NULL || out_guess == NULL || out_confidence == NULL || out_guess_len < 2U) {
        ESP_LOGE(TAG, "Invalid arguments to app_extract_vision_result");
        return false;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse vision API response JSON");
        return false;
    }

    bool ok = app_copy_guess_and_confidence(root, out_guess, out_guess_len, out_confidence);
    if (!ok) {
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
        const cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        const cJSON *message = cJSON_IsObject(choice0) ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
        const cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

        if (cJSON_IsString(content) && content->valuestring != NULL) {
            ok = app_try_parse_json_fragment(content->valuestring, out_guess, out_guess_len, out_confidence);
            if (!ok) {
                ok = app_try_extract_from_plain_text(content->valuestring,
                                                     out_guess,
                                                     out_guess_len,
                                                     out_confidence);
            }
        }
    }

    if (ok) {
        ESP_LOGI(TAG, "Vision API result: guess='%s', confidence=%d", out_guess, *out_confidence);
    }

    cJSON_Delete(root);
    return ok;
}

static bool app_extract_chat_content(const char *response_json,
                                     char *out_content,
                                     size_t out_content_len)
{
    if (response_json == NULL || out_content == NULL || out_content_len == 0U) {
        return false;
    }

    cJSON *root = cJSON_Parse(response_json);
    if (root == NULL) {
        return false;
    }

    const cJSON *choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    const cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    const cJSON *message = cJSON_IsObject(choice0) ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    const cJSON *content = cJSON_IsObject(message) ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    bool ok = false;
    if (cJSON_IsString(content) && content->valuestring != NULL) {
        const size_t content_len = strnlen(content->valuestring, out_content_len - 1U);
        memcpy(out_content, content->valuestring, content_len);
        out_content[content_len] = '\0';
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

static void app_sanitize_token(const char *input, char *output, size_t output_len)
{
    if (output == NULL || output_len == 0U) {
        return;
    }

    output[0] = '\0';
    if (input == NULL) {
        return;
    }

    size_t in_idx = 0U;
    while (input[in_idx] != '\0' && !isalnum((unsigned char)input[in_idx])) {
        in_idx++;
    }

    size_t out_idx = 0U;
    while (input[in_idx] != '\0' && out_idx + 1U < output_len) {
        const char ch = input[in_idx];
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == ' ')) {
            break;
        }
        output[out_idx++] = ch;
        in_idx++;
    }
    output[out_idx] = '\0';
}

static bool app_try_extract_prompt_word(const char *content,
                                        char *out_word,
                                        size_t out_word_len)
{
    if (content == NULL || out_word == NULL || out_word_len < 2U) {
        return false;
    }

    cJSON *content_json = cJSON_Parse(content);
    if (content_json != NULL) {
        const cJSON *word = cJSON_GetObjectItemCaseSensitive(content_json, "word");
        if (cJSON_IsString(word) && word->valuestring != NULL) {
            app_sanitize_token(word->valuestring, out_word, out_word_len);
            cJSON_Delete(content_json);
            return out_word[0] != '\0';
        }
        cJSON_Delete(content_json);
    }

    app_sanitize_token(content, out_word, out_word_len);
    return out_word[0] != '\0';
}

static bool app_extract_framebuffer_base64(const char *payload,
                                           char *out_base64,
                                           size_t out_base64_len)
{
    if (payload == NULL || out_base64 == NULL || out_base64_len < 2U) {
        return false;
    }

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        return false;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");

    bool ok = false;
    if (cJSON_IsString(type) && type->valuestring != NULL && strcmp(type->valuestring, "frame") == 0 &&
        cJSON_IsString(data) && data->valuestring != NULL) {
        const size_t data_len = strnlen(data->valuestring, out_base64_len - 1U);
        memcpy(out_base64, data->valuestring, data_len);
        out_base64[data_len] = '\0';
        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

esp_err_t app_api_submit_drawing(const char *payload,
                                 size_t payload_len,
                                 const char *active_prompt_word,
                                 app_ai_submit_result_t *out_result,
                                 bool *out_submit_success,
                                 bool submit_stub_success_flag)
{
    if (payload == NULL || payload_len == 0U || out_result == NULL || out_submit_success == NULL || active_prompt_word == NULL) {
        ESP_LOGE(TAG, "app_api_submit_drawing: Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }

    (void)submit_stub_success_flag;

    ESP_LOGI(TAG, "Starting framebuffer submission (payload_len=%zu)", payload_len);

    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OpenAI API key not configured, unable to submit drawing");
        *out_submit_success = false;
        strncpy(out_result->guess, "unknown", sizeof(out_result->guess) - 1U);
        out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
        out_result->confidence = 0;
        out_result->correct = false;
        return ESP_ERR_INVALID_STATE;
    }

    char *framebuffer_base64 = (char *)malloc(payload_len + 1U);
    if (framebuffer_base64 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer base64 buffer");
        *out_submit_success = false;
        return ESP_ERR_NO_MEM;
    }

    if (!app_extract_framebuffer_base64(payload, framebuffer_base64, payload_len + 1U)) {
        ESP_LOGW(TAG, "Framebuffer payload was not a frame JSON envelope; using raw payload text");
        const size_t fallback_len = strnlen(payload, payload_len);
        memcpy(framebuffer_base64, payload, fallback_len);
        framebuffer_base64[fallback_len] = '\0';
    }

    uint8_t raw_fb[2048] = {0};
    size_t raw_len = 0;
    mbedtls_base64_decode(raw_fb, sizeof(raw_fb), &raw_len, (const unsigned char *)framebuffer_base64, strlen(framebuffer_base64));

    // Convert 128x128 1bpp raw framebuffer to a 32x32 ASCII grid
    char ascii_art[1088] = {0}; // 32 * 32 + 32 newlines + 1 null = 1057
    size_t ascii_idx = 0;
    for (int dy = 0; dy < 32; dy++) {
        for (int dx = 0; dx < 32; dx++) {
            bool drawn = false;
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int x = dx * 4 + px;
                    int y = dy * 4 + py;
                    int bit_index = y * 128 + x;
                    int byte_index = bit_index / 8;
                    int bit_mask = 0x80 >> (bit_index % 8);
                    if (raw_fb[byte_index] & bit_mask) {
                        drawn = true;
                        break;
                    }
                }
                if (drawn) break;
            }
            ascii_art[ascii_idx++] = drawn ? '#' : '.';
        }
        ascii_art[ascii_idx++] = '\n';
    }
    ascii_art[ascii_idx] = '\0';

    ESP_LOGI(TAG, "Debugging Generated 32x32 ASCII Art sent to LLM:\n%s", ascii_art);

    const char *instruction =
        "You are an AI judge for a Pictionary-style drawing game testing if a user successfully drew a '%s'. "
        "I am sharing a 32x32 ASCII art representation of their drawing. "
        "Pixels drawn are '#' and empty background is '.'. "
        "Step 1: Notice the geometric shapes or lines formed by the '#' characters. "
        "Step 2: Are the shapes coherent? E.g., three connected sides is a triangle; two stacked circles might be an 8. "
        "Step 3: If it is completely blank, return 0. If it is a completely random squiggle or noise with no cohesive geometry, score 1-3. "
        "Step 4: If it clearly forms the basic shape or number '%s', return a confidence score of >= 8. "
        "Return ONLY valid JSON: {\"guess\":\"<what_it_looks_like>\",\"confidence\":<0-10>}";

    char formatted_instruction[1024];
    snprintf(formatted_instruction, sizeof(formatted_instruction), instruction, active_prompt_word, active_prompt_word);

    const size_t request_text_len = strlen(formatted_instruction) + strlen(ascii_art) + 64U;
    char *request_text = (char *)malloc(request_text_len);
    if (request_text == NULL) {
        free(framebuffer_base64);
        ESP_LOGE(TAG, "Failed to allocate request text buffer");
        *out_submit_success = false;
        return ESP_ERR_NO_MEM;
    }

    const int request_text_written = snprintf(request_text,
                                              request_text_len,
                                              "%s\nASCII_ART:\n%s",
                                              formatted_instruction,
                                              ascii_art);
    if (request_text_written <= 0 || (size_t)request_text_written >= request_text_len) {
        free(request_text);
        free(framebuffer_base64);
        ESP_LOGE(TAG, "Failed to format request text");
        *out_submit_success = false;
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *request = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *message = cJSON_CreateObject();
    cJSON *response_format = cJSON_CreateObject();
    if (request == NULL || messages == NULL || message == NULL || response_format == NULL) {
        cJSON_Delete(request);
        cJSON_Delete(messages);
        cJSON_Delete(message);
        cJSON_Delete(response_format);
        free(request_text);
        free(framebuffer_base64);
        ESP_LOGE(TAG, "Failed to allocate request JSON objects");
        *out_submit_success = false;
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(request, "model", "gpt-4o-mini");
    cJSON_AddNumberToObject(request, "temperature", 0);
    cJSON_AddStringToObject(response_format, "type", "json_object");
    cJSON_AddItemToObject(request, "response_format", response_format);

    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", request_text);
    cJSON_AddItemToArray(messages, message);
    cJSON_AddItemToObject(request, "messages", messages);

    char *request_body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    free(request_text);
    free(framebuffer_base64);

    if (request_body == NULL) {
        ESP_LOGE(TAG, "Failed to serialize request JSON");
        *out_submit_success = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Sending framebuffer submission request to OpenAI API...");

    // Send request to OpenAI API. Keep this buffer on heap to avoid task stack overflow.
    char *http_response = (char *)malloc(APP_HTTP_RESPONSE_BUFFER_SIZE);
    if (http_response == NULL) {
        free(request_body);
        ESP_LOGE(TAG, "Failed to allocate HTTP response buffer");
        *out_submit_success = false;
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t http_err = app_http_post_json("https://api.openai.com/v1/chat/completions",
                                                   request_body,
                                                   http_response,
                                                   APP_HTTP_RESPONSE_BUFFER_SIZE);
    free(request_body);

    if (http_err != ESP_OK) {
        free(http_response);
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(http_err));
        *out_submit_success = false;
        strncpy(out_result->guess, "error", sizeof(out_result->guess) - 1U);
        out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
        out_result->confidence = 0;
        out_result->correct = false;
        return http_err;
    }

    ESP_LOGI(TAG, "Received response from OpenAI API, parsing...");

    // Parse the vision API response
    char guess[64] = {0};
    int confidence = 0;
    const bool parse_success = app_extract_vision_result(http_response, guess, sizeof(guess), &confidence);

    if (!parse_success) {
        ESP_LOGW(TAG, "Failed to parse vision API response");
        char response_preview[301];
        const size_t preview_len = strnlen(http_response, sizeof(response_preview) - 1U);
        memcpy(response_preview, http_response, preview_len);
        response_preview[preview_len] = '\0';
        ESP_LOGW(TAG, "Response preview: %s", response_preview);
        free(http_response);
        *out_submit_success = false;
        strncpy(out_result->guess, "unknown", sizeof(out_result->guess) - 1U);
        out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
        out_result->confidence = 0;
        out_result->correct = false;
        return ESP_ERR_INVALID_RESPONSE;
    }

    free(http_response);

    // Store the results
    strncpy(out_result->guess, guess, sizeof(out_result->guess) - 1U);
    out_result->guess[sizeof(out_result->guess) - 1U] = '\0';
    out_result->confidence = (confidence < 1) ? 1 : (confidence > 10) ? 10 : confidence;
    out_result->correct = (out_result->confidence >= 6);

    *out_submit_success = true;

    ESP_LOGI(TAG,
             "Framebuffer submission successful: guess='%s', confidence=%d, correct=%d",
             out_result->guess,
             out_result->confidence,
             out_result->correct);

    return ESP_OK;
}

esp_err_t app_api_fetch_and_publish_prompt(app_api_send_frame_fn send_frame,
                                           char *io_active_prompt_word,
                                           size_t io_active_prompt_word_len)
{
    if (send_frame == NULL || io_active_prompt_word == NULL || io_active_prompt_word_len < 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!app_is_api_key_configured()) {
        ESP_LOGW(TAG, "OPENAI_API_KEY is not configured; using default prompt '%s'", io_active_prompt_word);
    } else {
        cJSON *root = cJSON_CreateObject();
        cJSON *messages = cJSON_CreateArray();
        cJSON *message = cJSON_CreateObject();
        if (root == NULL || messages == NULL || message == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(messages);
            cJSON_Delete(message);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(root, "model", "gpt-4o-mini");
        cJSON_AddStringToObject(message, "role", "user");
        cJSON_AddStringToObject(message,
                                "content",
                                "Give me one simple Pictionary noun. Respond ONLY with JSON: {\"word\":\"<noun>\"}");
        cJSON_AddItemToArray(messages, message);
        cJSON_AddItemToObject(root, "messages", messages);

        char *request_body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (request_body == NULL) {
            return ESP_ERR_NO_MEM;
        }

        char http_response[APP_HTTP_RESPONSE_BUFFER_SIZE];
        const esp_err_t prompt_err = app_http_post_json("https://api.openai.com/v1/chat/completions",
                                                        request_body,
                                                        http_response,
                                                        sizeof(http_response));
        free(request_body);
        if (prompt_err == ESP_OK) {
            char content[APP_AI_CONTENT_BUFFER_SIZE];
            if (app_extract_chat_content(http_response, content, sizeof(content))) {
                char prompt_word[APP_API_PROMPT_WORD_BUFFER_SIZE];
                if (app_try_extract_prompt_word(content, prompt_word, sizeof(prompt_word))) {
                    strncpy(io_active_prompt_word, prompt_word, io_active_prompt_word_len - 1U);
                    io_active_prompt_word[io_active_prompt_word_len - 1U] = '\0';
                }
            }
        } else {
            ESP_LOGW(TAG, "Prompt fetch failed: %s", esp_err_to_name(prompt_err));
        }
    }

    char prompt_json[128];
    const int prompt_written = snprintf(prompt_json,
                                        sizeof(prompt_json),
                                        "{\"type\":\"prompt\",\"word\":\"%s\"}",
                                        io_active_prompt_word);
    if (prompt_written <= 0 || (size_t)prompt_written >= sizeof(prompt_json)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ws_err = send_frame(prompt_json, (size_t)prompt_written);
    if (ws_err != ESP_OK) {
        ESP_LOGW(TAG, "Prompt websocket send failed: %s", esp_err_to_name(ws_err));
    }

    ESP_LOGI(TAG, "Active prompt word: %s", io_active_prompt_word);
    return ESP_OK;
}
