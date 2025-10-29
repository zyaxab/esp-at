#include "at_http_get_to_ram.h"

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_http_client.h"

#define AT_NETWORK_TIMEOUT_MS       (5000)

//! Buffer to store the file & read from it
static struct
{
    size_t length;
    uint8_t buffer[64 * 1024];
} file_store = {};

//! Context struct for +HTTPGET_TO_RAM command
typedef struct
{
    esp_http_client_handle_t client;

    size_t received_size;
    size_t total_size;
} at_httpget_to_ram_t;

static const char * TAG = "at_http_to_ram";
at_httpget_to_ram_t at_httpget_to_ram_context;

static esp_err_t at_http_event_handler(esp_http_client_event_t * event)
{
    at_httpget_to_ram_t * context = &at_httpget_to_ram_context;

    ESP_AT_LOGI(TAG, "HTTP event id = %d", event->event_id);

    switch(event->event_id)
    {
        case HTTP_EVENT_ON_HEADER:
            if(strcmp(event->header_key, "Content-Length") == 0)
            {
                context->total_size = atoi(event->header_value);
                ESP_AT_LOGI(TAG, "Total size is %d bytes", context->total_size);
            }
            break;

        case HTTP_EVENT_ON_DATA:
            context->received_size += event->data_len;
            ESP_AT_LOGI(TAG, "Received %d / %d bytes", context->received_size, context->total_size);
            break;

        default:
            break;
    }

    return ESP_OK;
}

static uint8_t at_setup_cmd_httpget_to_ram(uint8_t para_num)
{
    if(para_num != 1)
        return ESP_AT_RESULT_CODE_ERROR;

    at_httpget_to_ram_t * context = &at_httpget_to_ram_context;
    esp_err_t ret = ESP_OK;

    uint8_t * url;
    if(esp_at_get_para_as_str(0, &url) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(at_str_is_null(url))
        return ESP_AT_RESULT_CODE_ERROR;

    ESP_AT_LOGI(TAG, "Ready to download \"%s\" to RAM", url);

    // Initialize HTTP client & HTTPGET context
    esp_http_client_config_t config =
    {
        .url = (char *) url,
        .event_handler = at_http_event_handler,
        .timeout_ms = AT_NETWORK_TIMEOUT_MS,
        .buffer_size_tx = 4096,
    };

    memset(context, 0x00, sizeof(at_httpget_to_ram_context));
    context->client = esp_http_client_init(&config);

    memset(file_store.buffer, 0x00, sizeof(file_store.length));
    file_store.length = 0;
    
    if(context->client == NULL)
    {
        ret = ESP_FAIL;
        goto cmd_exit;
    }

    esp_http_client_set_method(context->client, HTTP_METHOD_GET);

    // Establish connection
    ret = esp_http_client_open(context->client, 0);
    if(ret != ESP_OK)
        goto cmd_exit;

    esp_http_client_fetch_headers(context->client);
    int status_code = esp_http_client_get_status_code(context->client);
    ESP_AT_LOGE(TAG, "HTTP status code: %d", status_code);
    if(status_code >= HttpStatus_BadRequest)
    {
        ret = ESP_FAIL;
        goto cmd_exit;
    }

    if(sizeof(file_store.buffer) < context->total_size)
    {
        ESP_AT_LOGE(TAG, "File store is %zu bytes but file is %zu, not enough memory", sizeof(file_store.buffer), context->total_size);
        ret = ESP_ERR_NO_MEM;
        goto cmd_exit;
    }

    // Download the data
    while(file_store.length < context->total_size)
    {
        int bytes_read = esp_http_client_read(context->client, (char *) file_store.buffer + file_store.length, context->total_size - file_store.length);
        if(bytes_read < 0)
        {
            ESP_AT_LOGE(TAG, "Connection aborted");
            ret = ESP_FAIL;
            goto cmd_exit;
        }

        file_store.length += bytes_read;
    }

    // Respond with size
    char header[64];
    int headerLen = snprintf(header, sizeof(header), "+HTTPGET_TO_RAM:%" PRId32 "\r\n", file_store.length);
    if(esp_at_port_write_data((uint8_t *) header, headerLen) != headerLen)
    {
        ESP_AT_LOGE(TAG, "Failed to send response");
        ret = ESP_FAIL;
        goto cmd_exit;
    }

cmd_exit:
    if(ret != ESP_OK)
    {
        ESP_AT_LOGE(TAG, "Failed with: 0x%X", ret);
        return ESP_AT_RESULT_CODE_ERROR;
    }

    ESP_AT_LOGI(TAG, "Downloaded file of %d bytes", (int) file_store.length);

    return ESP_AT_RESULT_CODE_OK;
}

static uint8_t at_setup_cmd_httpget_from_ram(uint8_t para_num)
{
    if(para_num != 2)
        return ESP_AT_RESULT_CODE_ERROR;

    if(file_store.length == 0)
        return ESP_AT_RESULT_CODE_ERROR;

    // Collect parameters
    int32_t offset, length;
    if(esp_at_get_para_as_digit(0, &offset) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_get_para_as_digit(1, &length) != ESP_AT_PARA_PARSE_RESULT_OK)
        return ESP_AT_RESULT_CODE_ERROR;

    if((offset < 0) || (offset >= file_store.length) || (length <= 0))
        return ESP_AT_RESULT_CODE_ERROR;

    // Guard against overreads
    if(offset + length > file_store.length)
        length = file_store.length - offset;

    char header[64];
    int headerLen = snprintf(header, sizeof(header), "+HTTPGET_FROM_RAM:%" PRId32 ",", length);

    // Send chunk
    if(esp_at_port_write_data((uint8_t *) header, headerLen) != headerLen)
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_port_write_data(file_store.buffer + offset, length) != length)
        return ESP_AT_RESULT_CODE_ERROR;

    if(esp_at_port_write_data((uint8_t *) "\r\n", 2) != 2)
        return ESP_AT_RESULT_CODE_ERROR;

    return ESP_AT_RESULT_CODE_OK;
}

static const esp_at_cmd_struct at_http_to_ram_cmd[] =
{
    { "+HTTPGET_TO_RAM"     , NULL, NULL, at_setup_cmd_httpget_to_ram   , NULL },
    { "+HTTPGET_FROM_RAM"   , NULL, NULL, at_setup_cmd_httpget_from_ram , NULL },
};

bool esp_at_httpget_to_ram_cmd_register(void)
{
    return esp_at_custom_cmd_array_regist(at_http_to_ram_cmd, sizeof(at_http_to_ram_cmd) / sizeof(esp_at_cmd_struct));
}

ESP_AT_CMD_SET_INIT_FN(esp_at_httpget_to_ram_cmd_register, 1);