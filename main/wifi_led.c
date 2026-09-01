#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

// Wi-Fi and HTTP Server Headers
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_http_server.h"

// Wi-Fi SoftAP Configuration Placeholders (Customize for your setup)
#define WIFI_SSID       "YOUR_WIFI_SSID"       // e.g. "ESP32_C6_LED_AP"
#define WIFI_PASS       "YOUR_WIFI_PASSWORD"   // e.g. "12345678" or "" for Open network
#define WIFI_CHANNEL    1
#define MAX_CONNECTIONS 4

// Hardware Pin Definitions
#define LED_GPIO        GPIO_NUM_8             // WS2812 / NeoPixel Addressable LED Data Pin
#define BUTTON_GPIO     GPIO_NUM_4
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "WIFI_LED_CTRL";
static QueueHandle_t color_queue = NULL;
static led_strip_handle_t led_strip;

// Embedded HTML Web UI binary pointers (from CMake EMBED_TXTFILES)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

httpd_handle_t server = NULL;

typedef struct 
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

// Initialize WS2812 / NeoPixel Addressable LED Strip via RMT
static void configure_led(void)
{
    ESP_LOGI(TAG, "Configuring addressable RGB LED...");
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1, // On-board single RGB pixel
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    // Clear LED initially (turn off)
    led_strip_clear(led_strip);
}

// HTTP GET / Handler: Serves the embedded HTML Web UI
esp_err_t send_html_handler(httpd_req_t *req)
{
    const size_t html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, html_size);
    return ESP_OK;
}

// HTTP POST /color Handler: Receives rgb(r,g,b) strings from Web UI
esp_err_t catch_color_handler(httpd_req_t *req)
{
    char buff[32];   
    int received = httpd_req_recv(req, buff, sizeof(buff) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buff[received] = '\0';

    int r_in, g_in, b_in;
    if (sscanf(buff, "rgb(%d,%d,%d)", &r_in, &g_in, &b_in) == 3)
    {
        rgb_color_t new_color;
        new_color.r = (r_in > 255) ? 255 : (r_in < 0 ? 0 : (uint8_t)r_in);
        new_color.g = (g_in > 255) ? 255 : (g_in < 0 ? 0 : (uint8_t)g_in);
        new_color.b = (b_in > 255) ? 255 : (b_in < 0 ? 0 : (uint8_t)b_in);

        xQueueSend(color_queue, &new_color, 0);
        ESP_LOGI(TAG, "Color Successfully Set: R=%d, G=%d, B=%d", new_color.r, new_color.g, new_color.b);
        httpd_resp_send(req, "Success", 7);
    }
    else
    {
        ESP_LOGW(TAG, "Unknown payload format: %s", buff);
        httpd_resp_send(req, "Unknown value entered. Please retry!", 36);
    }
            
    return ESP_OK;
}

// FreeRTOS Task: Receives color objects from queue and refreshes LED strip
static void led_control_task(void *arg)
{
    rgb_color_t received_clr;
    for (;;)
    {
        if (xQueueReceive(color_queue, &received_clr, portMAX_DELAY))
        {
            led_strip_set_pixel(led_strip, 0, received_clr.r, received_clr.g, received_clr.b);
            led_strip_refresh(led_strip);
        }
    }
}

// HTTP URI Route: GET /
httpd_uri_t uri_get =
{
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = send_html_handler,
    .user_ctx = NULL
};

// HTTP URI Route: POST /color
httpd_uri_t uri_post =
{
    .uri      = "/color",
    .method   = HTTP_POST,
    .handler  = catch_color_handler,
    .user_ctx = NULL
};

// Wi-Fi Access Point Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Client Connected! MAC: " MACSTR ", AID: %d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Client Disconnected! MAC: " MACSTR ", AID: %d",
                 MAC2STR(event->mac), event->aid);
    }
}

// Initialize Wi-Fi in SoftAP (Access Point) Mode
void wifi_init_softap(void)
{
    // Initialize TCP/IP Network Interface
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default SoftAP netif instance
    esp_netif_create_default_wifi_ap();

    // Initialize Wi-Fi driver with default configuration
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    // Configure Wi-Fi AP settings
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    // If password is empty, set open authentication
    if (strlen(WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Apply configuration and start Wi-Fi AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi SoftAP started successfully. SSID: %s | PASS: %s", WIFI_SSID, WIFI_PASS);
}

// Start Lightweight Embedded HTTP Web Server
void start_web_server(void)
{
    httpd_config_t conf = HTTPD_DEFAULT_CONFIG();
    conf.stack_size = 8192;
    ESP_LOGI(TAG, "Starting HTTP Web Server...");
    if (httpd_start(&server, &conf) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_post);
        ESP_LOGI(TAG, "HTTP Server routes registered successfully.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to start HTTP Web Server!");
    }
}

void app_main(void)
{
    // Initialize NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create FreeRTOS Queue for Color Commands
    color_queue = xQueueCreate(10, sizeof(rgb_color_t));
    xTaskCreate(led_control_task, "led_control_task", 4096, NULL, 3, NULL);

    // Configure Peripherals & Start Services
    configure_led();
    wifi_init_softap();
    start_web_server();
    
    ESP_LOGI(TAG, "ESP32 Wi-Fi LED Controller ready. Connect to AP and open http://192.168.4.1");
}
