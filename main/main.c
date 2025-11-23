/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* PPPoS Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"

// #include "esp_https_ota.h"      // For potential OTA configuration
#include "esp_mac.h"
#include "mcp3002.h"
#include "driver/gpio.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "wifi_app.h"
#include "sntp_time_sync.h"

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#define PACKET_TIMEOUT 30                  //30 seconds
#define BROKER_URL "broker.hivemq.com"
#define BROKER_PORT 8883

#define FW_VER "0.03"
// #if defined(CONFIG_EXAMPLE_FLOW_CONTROL_NONE)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
// #elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_SW)
// #define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_SW
// #elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_HW)
// #define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_HW
// #endif


static const char *TAG = "pppos_example";
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int DISCONNECT_BIT = BIT1;
static const int GOT_DATA_BIT = BIT2;
static const int USB_DISCONNECTED_BIT = BIT3; // Used only with USB DTE but we define it unconditionally, to avoid too many #ifdefs in the code

MCP_t dev;
uint8_t mac_addr[6] = {0};
char mac_string[13] = "0123456789AB";
// struct sock_dce::MODEM_DNA_STATS modem_dna;

char data_buff[255];
char topic_buff[255];

#define TOTAL_ZONE 10
static uint8_t zone_alert_state[TOTAL_ZONE];
uint16_t zone_raw_value[TOTAL_ZONE];
// uint16_t zone_lower_limit[TOTAL_ZONE] = {300,300,300,300,300,300,300,300,0,0};
// uint16_t zone_upper_limit[TOTAL_ZONE] = {700,700,700,700,700,700,700,700,0,0};
uint16_t zone_lower_limit[TOTAL_ZONE] = {500,500,500,500,500,500,500,500,0,0};
uint16_t zone_upper_limit[TOTAL_ZONE] = {900,900,900,900,900,900,900,900,0,0};

uint16_t alert_flg = 0;
uint16_t prev_alert_flg = 0;
static uint16_t loop_counter = 0;
// static uint16_t prev_loop_counter = 0; // Removed unused variable


// Public Function Definition
// static bool check_network_registration(esp_modem_dce_t *dce);
// static void check_signal_quality(esp_modem_dce_t *dce);
/*
 * Get the Temperature Values
 * @return temperature value
 */
uint8_t get_temperature(void)
{
  return 10;
}

/*
 * Get the Humidity Values
 * @return humidity value
 */
uint8_t get_humidity(void)
{
  return 90;
}

// Private Function Prototypes
// Removed unused function
// static void wifi_application_connected_events( void )
// {
//   ESP_LOGI(TAG, "WiFi Application Connected!");
//   sntp_time_sync_task_start();
// }


static void periodic_timer_callback(void* arg)
{
    loop_counter++;
    // gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);
    if(zone_raw_value[TOTAL_ZONE-2] != gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN))
    {
        alert_flg |= 0x0100;
        zone_alert_state[TOTAL_ZONE-2] = 0x01;
    }
    else
    {
        alert_flg &= ~0x0100;
    }

    zone_raw_value[TOTAL_ZONE-1] = gpio_get_level((gpio_num_t)39);
    zone_raw_value[TOTAL_ZONE-2] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);
    for(uint8_t i = 0; i < (TOTAL_ZONE-2); i++)
    {
        zone_raw_value[i] = mcpReadData(&dev, i);
        
        uint16_t bitmask = 1 << i;
        if (zone_raw_value[i] < zone_lower_limit[i])
        {
            zone_alert_state[i] |= 0x01;
            alert_flg |= bitmask; 
        }
        else if (zone_raw_value[i] > zone_upper_limit[i])
        {
            zone_alert_state[i] |= 0x02;
            alert_flg |= bitmask; 
        }
        else
        {
            alert_flg &= ~bitmask;
        }
        // ESP_LOGI(TAG, " ARM/DISARM STATUS %d ", gpio_get_level((gpio_num_t)39));
        // ESP_LOGI(TAG, "%d %d %X %X %d %d %d %d", i, bitmask, alert_flg, prev_alert_flg, zone_lower_limit[i], zone_upper_limit[i], zone_raw_value[i], zone_alert_state[i]);
    }
}

#ifdef CONFIG_EXAMPLE_MODEM_DEVICE_CUSTOM
esp_err_t esp_modem_get_time(esp_modem_dce_t *dce_wrap, char *p_time);
#endif

#if defined(CONFIG_EXAMPLE_SERIAL_CONFIG_USB)
#include "esp_modem_usb_c_api.h"
#include "esp_modem_usb_config.h"
#include "freertos/task.h"
static void usb_terminal_error_handler(esp_modem_terminal_error_t err)
{
    if (err == ESP_MODEM_TERMINAL_DEVICE_GONE) {
        ESP_LOGI(TAG, "USB modem disconnected");
        assert(event_group);
        xEventGroupSetBits(event_group, USB_DISCONNECTED_BIT);
    }
}
#define CHECK_USB_DISCONNECTION(event_group) \
if ((xEventGroupGetBits(event_group) & USB_DISCONNECTED_BIT) == USB_DISCONNECTED_BIT) { \
    ESP_LOGE(TAG, "USB_DISCONNECTED_BIT destroying modem dce");                                            \
    esp_modem_destroy(dce); \
    continue; \
}
#else
#define CHECK_USB_DISCONNECTION(event_group)
#endif

static int ppp_fail_count = 0;

// Fixed: Use correct function name for network registration check
static bool check_network_registration(esp_modem_dce_t *dce)
{
    // Use available modem functions to check network status
    int rssi, ber;
    esp_err_t err;
    int retry = 0;
    
    while (retry < 10) {
        err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Signal quality - RSSI: %d, BER: %d", rssi, ber);
            // If we can read signal quality, modem is likely registered
            if (rssi > -113) { // Reasonable signal threshold
                return true;
            }
        }
        ESP_LOGW(TAG, "Waiting for network registration... (attempt %d)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry++;
    }
    
    ESP_LOGE(TAG, "Failed to get proper network signal after %d attempts", retry);
    return false;
}

static void perform_modem_reset(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Performing complete modem reset...");
    
    // Hard reset with proper timing
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));  // Longer reset pulse (1 second)
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    
    // Wait for modem to fully reboot - BG96 needs 30+ seconds
    ESP_LOGI(TAG, "Waiting for BG96 modem to fully reboot (35 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(35000));
}

static bool restart_ppp_connection(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Attempting to restart PPP connection...");
    
    // 1. Ensure we're in command mode first
    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not switch to command mode, modem may be unresponsive");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    
    // 2. Check if modem is still responsive
    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modem not responsive after command mode switch");
        return false;
    }
    ESP_LOGI(TAG, "Modem responsive, signal: rssi=%d, ber=%d", rssi, ber);
    
    // 3. Switch back to data mode to restart PPP
    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to data mode: %s", esp_err_to_name(err));
        return false;
    }
    
    ESP_LOGI(TAG, "PPP restart initiated, waiting for connection...");
    return true;
}

static void check_ppp_connection(esp_modem_dce_t *dce, esp_mqtt_client_handle_t mqtt_client)
{
    static uint32_t last_ppp_check = 0;
    static bool recovery_in_progress = false;
    
    uint32_t current_time = esp_timer_get_time() / 1000000;
    
    // Check every 30 seconds
    if (current_time - last_ppp_check > 30) {
        last_ppp_check = current_time;
        
        bool ppp_connected = ((xEventGroupGetBits(event_group) & CONNECT_BIT) == CONNECT_BIT);
        
        if (!ppp_connected && !recovery_in_progress) {
            ESP_LOGW(TAG, "PPP connection lost, attempting recovery... (fail count: %d)", ppp_fail_count);
            ppp_fail_count++;
            recovery_in_progress = true;
            
            // Stop MQTT client
            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
                ESP_LOGI(TAG, "MQTT client stopped for PPP recovery");
            }
            
            if (ppp_fail_count >= 2) {
                ESP_LOGE(TAG, "Multiple PPP failures, performing modem reset...");
                perform_modem_reset(dce);
                ppp_fail_count = 0; // Reset counter after hard reset
            } else {
                // First failure - try soft restart
                if (restart_ppp_connection(dce)) {
                    // Wait longer for PPP to establish
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
            }
            
            recovery_in_progress = false;
        } else if (ppp_connected && ppp_fail_count > 0) {
            // Connection restored
            ESP_LOGI(TAG, "PPP connection restored, resetting fail count");
            ppp_fail_count = 0;
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRId32, base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        topic_buff[0] = 0;
        sprintf(topic_buff,"/ZIGRON/%s/CLEAR",mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        topic_buff[0] = 0;
        sprintf(topic_buff,"/ZIGRON/%s/COMMAND",mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        // esp_restart();
        xEventGroupClearBits(event_group, CONNECT_BIT);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        // msg_id = esp_mqtt_client_publish(client, "/topic/esp-pppos", "esp32-pppos", 0, 0, 0);
        // ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        // if(event->topic == )
        zone_alert_state[0] = 0;zone_alert_state[1] = 0;zone_alert_state[2] = 0;zone_alert_state[3] = 0;
        zone_alert_state[4] = 0;zone_alert_state[5] = 0;zone_alert_state[6] = 0;zone_alert_state[7] = 0;
        zone_alert_state[8] = 0;
        alert_flg = 0;
        loop_counter = PACKET_TIMEOUT-1;
        xEventGroupSetBits(event_group, GOT_DATA_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        // esp_restart();
        xEventGroupClearBits(event_group, CONNECT_BIT);
        break;
    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}

// Removed unused function
// static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
//     switch (evt->event_id) {
//         case HTTP_EVENT_ERROR:
//             ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
//             break;
//         case HTTP_EVENT_ON_CONNECTED:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
//             break;
//         case HTTP_EVENT_HEADER_SENT:
//             ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
//             break;
//         case HTTP_EVENT_ON_HEADER:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
//             break;
//         case HTTP_EVENT_ON_DATA:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
//             break;
//         case HTTP_EVENT_ON_FINISH:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
//             break;
//         case HTTP_EVENT_DISCONNECTED:
//             ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
//             break;
//         case HTTP_EVENT_REDIRECT:
//             ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
//             break;
//     }
//     return ESP_OK;
// }

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %" PRIu32, event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        /* User interrupted event from esp-netif */
        esp_netif_t **p_netif = event_data;
        ESP_LOGI(TAG, "User interrupted event from netif:%p", *p_netif);
    }
}


static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %" PRIu32, event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_dns_info_t dns_info;

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(event_group, CONNECT_BIT);

        ESP_LOGI(TAG, "GOT ip event!!!");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
        xEventGroupSetBits(event_group, DISCONNECT_BIT);
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ESP_LOGI(TAG, "GOT IPv6 event!");

        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

// static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
//     switch (evt->event_id) {
//         case HTTP_EVENT_ERROR:
//             ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
//             break;
//         case HTTP_EVENT_ON_CONNECTED:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
//             break;
//         case HTTP_EVENT_HEADER_SENT:
//             ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
//             break;
//         case HTTP_EVENT_ON_HEADER:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
//             break;
//         case HTTP_EVENT_ON_DATA:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
//             break;
//         case HTTP_EVENT_ON_FINISH:
//             ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
//             break;
//         case HTTP_EVENT_DISCONNECTED:
//             ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
//             break;
//         case HTTP_EVENT_REDIRECT:
//             ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
//             break;
//     }
//     return ESP_OK;
// }

// // Add near other helper functions (above app_main)
// static esp_err_t perform_http_ota_http(const char *url)
// {
//     esp_err_t err = ESP_OK;
//     esp_http_client_config_t config = {
//         .url = url,
//         .timeout_ms = 20000,
//         .event_handler = http_event_handler,
//         .transport_type = HTTP_TRANSPORT_OVER_TCP,
//         .buffer_size = 4096,
//     };

//     ESP_LOGI(TAG, "Starting HTTP OTA from: %s", url);
//     esp_http_client_handle_t client = esp_http_client_init(&config);
//     if (!client) {
//         ESP_LOGE(TAG, "Failed to initialise HTTP client");
//         return ESP_FAIL;
//     }

//     err = esp_http_client_open(client, 0);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
//         esp_http_client_cleanup(client);
//         return err;
//     }

//     const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
//     if (update_partition == NULL) {
//         ESP_LOGE(TAG, "esp_ota_get_next_update_partition returned NULL");
//         esp_http_client_cleanup(client);
//         return ESP_FAIL;
//     }
//     ESP_LOGI(TAG, "Writing to partition subtype %d, address 0x%08x",
//              update_partition->subtype, update_partition->address);

//     esp_ota_handle_t ota_handle = 0;
//     err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
//         esp_http_client_cleanup(client);
//         return err;
//     }

//     // Read loop
//     const int buf_size = 4096;
//     uint8_t *ota_buffer = (uint8_t *)malloc(buf_size);
//     if (!ota_buffer) {
//         ESP_LOGE(TAG, "No memory for OTA buffer");
//         esp_ota_end(ota_handle);
//         esp_http_client_cleanup(client);
//         return ESP_ERR_NO_MEM;
//     }

//     int data_read;
//     int total = 0;
//     while ((data_read = esp_http_client_read(client, (char *)ota_buffer, buf_size)) > 0) {
//         err = esp_ota_write(ota_handle, (const void *)ota_buffer, data_read);
//         if (err != ESP_OK) {
//             ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
//             free(ota_buffer);
//             esp_ota_end(ota_handle);
//             esp_http_client_cleanup(client);
//             return err;
//         }
//         total += data_read;
//         ESP_LOGD(TAG, "Wrote %d bytes (total %d)", data_read, total);
//     }

//     free(ota_buffer);

//     if (data_read < 0) {
//         ESP_LOGE(TAG, "Error: esp_http_client_read failed: %d", data_read);
//         esp_ota_end(ota_handle);
//         esp_http_client_cleanup(client);
//         return ESP_FAIL;
//     }

//     err = esp_ota_end(ota_handle);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
//         esp_http_client_cleanup(client);
//         return err;
//     }

//     err = esp_ota_set_boot_partition(update_partition);
//     if (err != ESP_OK) {
//         ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
//         esp_http_client_cleanup(client);
//         return err;
//     }

//     ESP_LOGI(TAG, "OTA successful, total bytes written: %d. Boot partition set.", total);
//     esp_http_client_cleanup(client);
//     return ESP_OK;
// }

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void app_main(void)
{

    esp_log_level_set("esp_http_client", ESP_LOG_DEBUG);
    esp_log_level_set("esp_https_ota", ESP_LOG_DEBUG);

    esp_err_t ret = nvs_flash_init();
    if ( (ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND) )
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO, CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);
    esp_read_mac( mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string,"%02X%02X%02X%02X%02X%02X", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC address %s",mac_string);

    event_group = xEventGroupCreate();

    
    // const esp_timer_create_args_t periodic_timer_args = {
    //     .callback = &periodic_timer_callback,
    //     /* name is optional, but may help identify the timer when debugging */
    //     .name = "periodic"
    // };

    // esp_timer_handle_t periodic_timer;
    // ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    // /* The timer has been created but is not running yet */
    // ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 100000));

    /* Init and register system/core components */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    /* Configure the PPP netif */
    esp_err_t err;
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("jazzconnect.mobilinkworld.com");
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    /* Configure the DTE */
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* setup UART specific configuration based on kconfig options */
    dte_config.uart_config.tx_io_num = CONFIG_EXAMPLE_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_EXAMPLE_MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = CONFIG_EXAMPLE_MODEM_UART_RTS_PIN;
    dte_config.uart_config.cts_io_num = CONFIG_EXAMPLE_MODEM_UART_CTS_PIN;
    dte_config.uart_config.flow_control = EXAMPLE_FLOW_CONTROL;
    dte_config.uart_config.rx_buffer_size = 2048;//CONFIG_EXAMPLE_MODEM_UART_RX_BUFFER_SIZE;
    dte_config.uart_config.tx_buffer_size = 1024;//CONFIG_EXAMPLE_MODEM_UART_TX_BUFFER_SIZE;

    ESP_ERROR_CHECK(esp_task_wdt_deinit());
    ESP_LOGI(TAG, "Watchdog temporarily disabled for modem initialization");

    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction( (gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, GPIO_MODE_OUTPUT); 
    
    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);vTaskDelay(100);
    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);

    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, 0);vTaskDelay(100); //high for A and LOW for Sim B

    // esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_BG96, &dte_config, &dce_config, esp_netif);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_config, &dce_config, esp_netif);

    // CRITICAL: Wait for modem to fully boot
    ESP_LOGI(TAG, "Waiting for BG96 modem to boot (15 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(15000)); // Increased to 15 seconds
   
    assert(dce);

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT);

    // Test basic communication first with retry mechanism
    ESP_LOGI(TAG, "Testing basic modem communication...");
    int retry_count = 0;
    
    while (retry_count < 5) {
        int rssi, ber;
        err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Basic communication OK - Signal: rssi=%d, ber=%d", rssi, ber);
            break;
        }
        ESP_LOGW(TAG, "Communication test failed (attempt %d), retrying...", retry_count + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry_count++;
    }

    // Check network registration before starting PPP
    if (!check_network_registration(dce)) {
        ESP_LOGE(TAG, "Cannot proceed without network registration");
        return;
    }

    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DETECT) failed with %d", err);
        return;
    }
    esp_modem_dce_mode_t mode = esp_modem_get_mode(dce);
    ESP_LOGI(TAG, "Mode detection completed: current mode is: %d", mode);
    if (mode == ESP_MODEM_MODE_DATA) {  // set back to command mode
        err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_COMMAND) failed with %d", err);
            return;
        }
        ESP_LOGI(TAG, "Command mode restored");
    }

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT);

    /* Run the modem demo app */
#if CONFIG_EXAMPLE_NEED_SIM_PIN == 1
    // check if PIN needed
    bool pin_ok = false;
    if (esp_modem_read_pin(dce, &pin_ok) == ESP_OK && pin_ok == false) {
        if (esp_modem_set_pin(dce, CONFIG_EXAMPLE_SIM_PIN) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            abort();
        }
    }
#endif

    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_get_signal_quality failed with %d %s", err, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Signal quality: rssi=%d, ber=%d", rssi, ber);

    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DATA) failed with %d", err);
        return;
    }
    
    /* Wait for IP address */
    ESP_LOGI(TAG, "Waiting for IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(60000));
    CHECK_USB_DISCONNECTION(event_group);
    if ((xEventGroupGetBits(event_group) & CONNECT_BIT) != CONNECT_BIT) {
        ESP_LOGW(TAG, "Modem not connected, switching back to the command mode");
        err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_COMMAND) failed with %d", err);
            return;
        }
        ESP_LOGI(TAG, "Command mode restored");
        return;
    }

    // Main application loop with improved connection management
    esp_mqtt_client_handle_t mqtt_client = NULL;
    bool mqtt_started = false;
    uint32_t last_mqtt_publish = 0;
    bool ppp_was_connected = true; // Start assuming connected
    uint32_t last_connection_check = 0;
    uint32_t disconnect_start_time = 0;


    // esp_http_client_config_t config = {       
    //     .url = "http://54.194.219.149:45056/firmware/MultiSerial.ino.bin",
    //     .port = 45056,
    //     .timeout_ms = 10000,  // Increased timeout
    //     .event_handler = http_event_handler,
    //     .transport_type = HTTP_TRANSPORT_OVER_TCP,
    //     .buffer_size = 4096,
    //     .buffer_size_tx = 4096,
    //     .keep_alive_enable = true,
    // };

    // esp_https_ota_config_t ota_config = {
    //     .http_config = &config,
    // };

    // ESP_LOGI(TAG, "Free heap: %ld", esp_get_free_heap_size());
    // vTaskDelay(pdMS_TO_TICKS(500));
    // ret = esp_https_ota(&ota_config);
    // if (ret == ESP_OK) {
    //     esp_restart();
    // } else {
    //     ESP_LOGE(TAG, "OTA failed with error: %s", esp_err_to_name(ret));
    // }

    while (1) {
        bool ppp_connected = ((xEventGroupGetBits(event_group) & CONNECT_BIT) == CONNECT_BIT);
        
        // Log PPP connection state changes
        if (ppp_connected != ppp_was_connected) {
            if (ppp_connected) {
                ESP_LOGI(TAG, "PPP connection established");
                disconnect_start_time = 0; // Reset disconnect timer

            } else {
                ESP_LOGW(TAG, "PPP connection lost");
                if (disconnect_start_time == 0) {
                    disconnect_start_time = esp_timer_get_time() / 1000000;
                }
            }
            ppp_was_connected = ppp_connected;
        }
        
        // Check PPP connection status every 30 seconds
        uint32_t current_time = esp_timer_get_time() / 1000000;
        if (current_time - last_connection_check > 30) {
            last_connection_check = current_time;
            check_ppp_connection(dce, mqtt_client);
        }
        
        // Handle extended disconnection (emergency reset)
        if (!ppp_connected && disconnect_start_time > 0) {
            uint32_t disconnect_duration = current_time - disconnect_start_time;
            if (disconnect_duration > 300) { // 5 minutes without connection
                ESP_LOGW(TAG, "PPP disconnected for 5 minutes, forcing emergency modem reset");
                perform_modem_reset(dce);
                disconnect_start_time = 0;
                // Reset fail count to allow fresh recovery attempts
                ppp_fail_count = 0;
            }
        }
        
        // Handle PPP connection state
        if (!ppp_connected) {
            // Ensure MQTT is stopped when PPP is down
            if (mqtt_client && mqtt_started) {
                esp_mqtt_client_stop(mqtt_client);
                mqtt_started = false;
                ESP_LOGI(TAG, "MQTT client stopped due to PPP disconnect");
            }
            
            vTaskDelay(pdMS_TO_TICKS(10000)); // Longer delay when disconnected
            continue;
        }
        
        // Start MQTT if PPP is connected and MQTT isn't running
        /* ---------- START: Corrected OTA block (paste over existing OTA code) ---------- */

        /* Only attempt OTA once, before creating MQTT client.
        Use HTTPS and provide CA cert for server verification.
        server_cert_pem_start/_end must be provided as binary in project (CMake install).
        */

        if (!mqtt_client) {
            esp_http_client_config_t config = {       
                .url = "http://54.194.219.149:45056/firmware/MultiSerial.ino.bin",
                // "http://raw.githubusercontent.com/ata-rehman/smarthome/main/Geyser_switch_test.ino.nodemcu.bin",
                // .cert_pem = (char *)server_cert_pem_start,
                .event_handler = _http_event_handler,
                .keep_alive_enable = true,
                // .buffer_size = 4096,
                // .buffer_size_tx = 4096,
                // .keep_alive_enable = true,
            };

            config.skip_cert_common_name_check = true;

            esp_https_ota_config_t ota_config = {
                .http_config = &config,
            };

            ESP_LOGI(TAG, "Free heap: %ld", esp_get_free_heap_size());
            vTaskDelay(pdMS_TO_TICKS(500));
            ret = esp_https_ota(&ota_config);
            if (ret == ESP_OK) {
                esp_restart();
            } else {
                ESP_LOGE(TAG, "OTA failed with error: %s", esp_err_to_name(ret));
            }

            // ESP_LOGI(TAG, "Starting OTA (insecure HTTP)...");
            // const char *http_ota_url = "http://54.194.219.149:45056/firmware/MultiSerial.ino.bin";
            // // const char *http_ota_url = "http://raw.githubusercontent.com/ata-rehman/smarthome/main/Geyser_switch_test.ino.nodemcu.bin";

            // esp_err_t ota_ret = perform_http_ota_http(http_ota_url);
            // if (ota_ret == ESP_OK) {
            //     ESP_LOGI(TAG, "OTA success, restarting to apply new firmware...");
            //     vTaskDelay(pdMS_TO_TICKS(200)); // flush logs
            //     esp_restart(); // reboot to new image
            //     // won't return
            // } else {
            //     ESP_LOGE(TAG, "OTA failed: %s (0x%x). Continuing normal startup.", esp_err_to_name(ota_ret), ota_ret);
            //     // continue to initialize MQTT and normal app behavior
            // }

            // ESP_LOGI(TAG, "OTA procedure finished (result %s). Initializing MQTT client...", esp_err_to_name(ota_ret));

            /* initialize MQTT client after OTA attempt */
            esp_mqtt_client_config_t mqtt_config = {
                .broker.address.uri = "mqtt://zigron:zigron123@54.194.219.149:45055",
                .network.timeout_ms = 20000,
                .session.keepalive = 60,
                .network.disable_auto_reconnect = false,
            };
            mqtt_client = esp_mqtt_client_init(&mqtt_config);
            esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        }

/* ---------- END: Corrected OTA block ---------- */

        
        if (!mqtt_started) {
            ESP_LOGI(TAG, "Starting MQTT client...");
            if (esp_mqtt_client_start(mqtt_client) == ESP_OK) {
                mqtt_started = true;
                vTaskDelay(pdMS_TO_TICKS(5000)); // Wait for connection
            } else {
                ESP_LOGE(TAG, "Failed to start MQTT client");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }

        // Data publishing logic
        // if (loop_counter % PACKET_TIMEOUT == 0 && (current_time - last_mqtt_publish) >= PACKET_TIMEOUT) {  
        if (loop_counter % PACKET_TIMEOUT == 0) {
            ESP_LOGW(TAG, "loop counter %d reached PACKET_TIMEOUT", loop_counter);
            loop_counter = 0; // Reset counter after publishing attempt
            // Double-check connections before publishing
            if (mqtt_started && ppp_connected) {
                data_buff[0] = 0;
                topic_buff[0] = 0;
                
                // Use snprintf for safety
                int data_len = snprintf(data_buff, sizeof(data_buff),
                    "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],\"DNA\":[\"%s\",%lld],\"FW\":\"%s\"}",
                    zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3], 
                    zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
                    zone_raw_value[8], zone_raw_value[9], 
                    zone_alert_state[0], zone_alert_state[1], zone_alert_state[2], zone_alert_state[3],
                    zone_alert_state[4], zone_alert_state[5], zone_alert_state[6], zone_alert_state[7],
                    zone_alert_state[8], zone_alert_state[9],
                    mac_string, esp_timer_get_time() / 1000000,
                    FW_VER);
                
                if (data_len > 0 && data_len < sizeof(data_buff)) {
                    if (prev_alert_flg != alert_flg) {
                        snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/ALERT", mac_string);
                    } else {
                        snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/HB", mac_string);
                    }
                    
                    int publish_response = esp_mqtt_client_publish(mqtt_client, topic_buff, data_buff, 0, 0, 0);
                    
                    if (publish_response == -1) {
                        ESP_LOGE(TAG, "MQTT publish failed");
                        // Increment fail count to trigger recovery sooner
                        if (ppp_fail_count < 2) {
                            ppp_fail_count++;
                        }
                    } else {
                        ESP_LOGI(TAG, "Published: %s -> %s", topic_buff, data_buff);
                        last_mqtt_publish = current_time;
                    }
                }
           // } else {
                // ESP_LOGW(TAG, "Skipping MQTT publish - no connection");
            }
        }
        loop_counter++;
        prev_alert_flg = alert_flg;
        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Increased delay to reduce CPU usage
        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}