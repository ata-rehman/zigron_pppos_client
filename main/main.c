/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* WiFi-first MQTT + OTA with GSM/PPP as backup
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "mcp3002.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
// WiFi headers for ESP-IDF v5.5.1
#include "esp_wifi.h"
#include "esp_wifi_default.h"

#define PACKET_TIMEOUT      30          // 30 seconds
#define FW_VER              "0.04"      // Updated version
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
#define WIFI_CONNECT_TIMEOUT_MS 30000   // 30 seconds WiFi timeout
#define MAX_WIFI_RETRIES   3

static const char *TAG = "Zigron_Dual_MQTT";

/* Event group for connectivity status */
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT          = BIT0;
static const int DISCONNECT_BIT       = BIT1;
static const int GOT_DATA_BIT         = BIT2;
static const int USB_DISCONNECTED_BIT = BIT3;
static const int OTA_TRIGGER_BIT      = BIT4;   // MQTT-triggered OTA
static const int WIFI_CONNECTED_BIT   = BIT5;   // WiFi is connected
static const int GSM_CONNECTED_BIT    = BIT6;   // GSM/PPP is connected
static const int MQTT_CONNECTED_BIT   = BIT7;   // MQTT is connected

/* Connectivity mode */
typedef enum {
    CONN_MODE_NONE = 0,
    CONN_MODE_WIFI,
    CONN_MODE_GSM,
    CONN_MODE_MAX
} conn_mode_t;

static conn_mode_t current_conn_mode = CONN_MODE_NONE;

/* Shared data */
MCP_t   dev;
uint8_t mac_addr[6] = {0};
char    mac_string[13] = "0123456789AB";

char data_buff[255];
char topic_buff[255];

#define TOTAL_ZONE 10
static uint8_t  zone_alert_state[TOTAL_ZONE];
static uint16_t zone_raw_value[TOTAL_ZONE];

static uint16_t zone_lower_limit[TOTAL_ZONE] = {0,0,0,0,0,0,0,0,0,0};
static uint16_t zone_upper_limit[TOTAL_ZONE] = {900,900,900,900,900,900,900,900,0,0};

static uint16_t alert_flg       = 0;
static uint16_t prev_alert_flg  = 0;
static uint16_t loop_counter    = 0;

/* PPP recovery state */
static int      ppp_fail_count  = 0;

/* WiFi connection attempts */
static int      wifi_retry_count = 0;

/* Protect shared sensor + alert data between tasks & MQTT callback */
static SemaphoreHandle_t data_mutex = NULL;
static SemaphoreHandle_t conn_mutex = NULL;  // For connectivity mode changes

/* Forward declarations */
static void init_wifi(void);
static bool connect_wifi(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);

static bool check_network_registration(esp_modem_dce_t *dce);
static void perform_modem_reset(esp_modem_dce_t *dce);
static bool restart_ppp_connection(esp_modem_dce_t *dce);
static void check_ppp_connection(esp_modem_dce_t *dce, esp_mqtt_client_handle_t mqtt_client);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);

static void sensor_task(void *arg);
static void mqtt_task(void *arg);
static void ota_task(void *arg);
static void connectivity_manager_task(void *arg);

/* WiFi Configuration */
static wifi_config_t wifi_config = {
    .sta = {
        .ssid = "11/2 homes A",
        .password = "03345472486",
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        .pmf_cfg = {
            .capable = true,
            .required = false
        },
    },
};

/* -------------------------------------------------------------------------- */
/* WiFi Functions                                                             */
/* -------------------------------------------------------------------------- */

static void init_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    ESP_LOGI(TAG, "WiFi initialization complete");
}

static bool connect_wifi(void)
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", wifi_config.sta.ssid);
    
    esp_err_t ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return false;
    }
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi: %s", esp_err_to_name(ret));
        return false;
    }
    
    // Wait for connection with timeout
    EventBits_t bits = xEventGroupWaitBits(event_group, 
                                          WIFI_CONNECTED_BIT,
                                          pdFALSE, pdFALSE, 
                                          pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        wifi_retry_count = 0;  // Reset retry count on successful connection
        return true;
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
        esp_wifi_disconnect();
        esp_wifi_stop();
        return false;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi station connected to AP");
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi station disconnected");
                xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
                
                if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (current_conn_mode == CONN_MODE_WIFI) {
                        // Only log, don't change connection mode - let connectivity manager handle it
                        ESP_LOGI(TAG, "WiFi disconnected, will attempt GSM fallback");
                    }
                    xSemaphoreGive(conn_mutex);
                }
                
                if (wifi_retry_count < MAX_WIFI_RETRIES) {
                    wifi_retry_count++;
                    ESP_LOGI(TAG, "WiFi reconnect attempt %d/%d", wifi_retry_count, MAX_WIFI_RETRIES);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "Max WiFi retries reached, staying disconnected");
                    // Don't call esp_wifi_connect() anymore, let GSM take over
                }
                break;
        }
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        wifi_retry_count = 0;  // Reset retry count on successful connection
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            current_conn_mode = CONN_MODE_WIFI;
            xSemaphoreGive(conn_mutex);
        }
        
        xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT);
    }
}

/* -------------------------------------------------------------------------- */
/* Sensor task (unchanged)                                                    */
/* -------------------------------------------------------------------------- */

static void sensor_task(void *arg)
{
    ESP_LOGI(TAG, "Sensor task started");

    while (1) {
        if (data_mutex && xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
            loop_counter++;

            /* Buzzer status vs stored zone value */
            if (zone_raw_value[TOTAL_ZONE-2] != gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN)) {
                alert_flg |= 0x0100;
                zone_alert_state[TOTAL_ZONE-2] = 0x01;
            } else {
                alert_flg &= ~0x0100;
            }

            /* Digital inputs */
            zone_raw_value[TOTAL_ZONE-1] = gpio_get_level((gpio_num_t)39);
            zone_raw_value[TOTAL_ZONE-2] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);

            /* Analog zones */
            for (uint8_t i = 0; i < (TOTAL_ZONE-2); i++) {
                zone_raw_value[i] = mcpReadData(&dev, i);

                uint16_t bitmask = 1 << i;
                if (zone_raw_value[i] < zone_lower_limit[i]) {
                    zone_alert_state[i] |= 0x01;
                    alert_flg |= bitmask;
                } else if (zone_raw_value[i] > zone_upper_limit[i]) {
                    zone_alert_state[i] |= 0x02;
                    alert_flg |= bitmask;
                } else {
                    alert_flg &= ~bitmask;
                }
            }

            snprintf(data_buff, sizeof(data_buff),
                "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
                zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3],
                zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
                zone_raw_value[8], zone_raw_value[9]);

            ESP_LOGI(TAG, "Sensor data: %s", data_buff);

            xSemaphoreGive(data_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // sensor period ~1s
    }
}

/* -------------------------------------------------------------------------- */
/* PPP/GSM helpers (modified for dual connectivity)                           */
/* -------------------------------------------------------------------------- */

static bool check_network_registration(esp_modem_dce_t *dce)
{
    int rssi, ber;
    esp_err_t err;
    int retry = 0;

    while (retry < 10) {
        err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GSM Signal quality - RSSI: %d, BER: %d", rssi, ber);
            if (rssi > -113) {
                return true;
            }
        }
        ESP_LOGW(TAG, "Waiting for GSM network registration. (attempt %d)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry++;
    }

    ESP_LOGE(TAG, "Failed to get proper GSM network signal after %d attempts", retry);
    return false;
}

static void perform_modem_reset(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Performing complete GSM modem reset...");

    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);

    ESP_LOGI(TAG, "Waiting for GSM modem to fully reboot (35 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(35000));
}

static bool restart_ppp_connection(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Attempting to restart GSM PPP connection...");

    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not switch to command mode, GSM modem may be unresponsive");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSM modem not responsive after command mode switch");
        return false;
    }
    ESP_LOGI(TAG, "GSM modem responsive, signal: rssi=%d, ber=%d", rssi, ber);

    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to GSM data mode: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "GSM PPP restart initiated, waiting for connection...");
    return true;
}

static void check_ppp_connection(esp_modem_dce_t *dce, esp_mqtt_client_handle_t mqtt_client)
{
    static uint32_t last_ppp_check = 0;
    static bool recovery_in_progress = false;

    uint32_t current_time = esp_timer_get_time() / 1000000;

    if (current_time - last_ppp_check > 30) {
        last_ppp_check = current_time;

        bool gsm_connected = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) == GSM_CONNECTED_BIT);

        if (!gsm_connected && !recovery_in_progress && current_conn_mode == CONN_MODE_GSM) {
            ESP_LOGW(TAG, "GSM PPP connection lost, attempting recovery. (fail count: %d)", ppp_fail_count);
            ppp_fail_count++;
            recovery_in_progress = true;

            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
                ESP_LOGI(TAG, "MQTT client stopped for GSM PPP recovery");
            }

            if (ppp_fail_count >= 2) {
                ESP_LOGE(TAG, "Multiple GSM PPP failures, performing modem reset.");
                perform_modem_reset(dce);
                ppp_fail_count = 0;
            } else {
                if (restart_ppp_connection(dce)) {
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
            }

            recovery_in_progress = false;
        } else if (gsm_connected && ppp_fail_count > 0) {
            ESP_LOGI(TAG, "GSM PPP connection restored, resetting fail count");
            ppp_fail_count = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* MQTT event handler (updated for dual connectivity)                         */
/* -------------------------------------------------------------------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "MQTT event base=%s, id=%" PRId32, base, event_id);
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED via %s", 
                (current_conn_mode == CONN_MODE_WIFI) ? "WiFi" : "GSM");
        
        xEventGroupSetBits(event_group, MQTT_CONNECTED_BIT);
        
        topic_buff[0] = 0;
        sprintf(topic_buff, "/ZIGRON/%s/CLEAR", mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe CLEAR, msg_id=%d", msg_id);

        topic_buff[0] = 0;
        sprintf(topic_buff, "/ZIGRON/%s/COMMAND", mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe COMMAND, msg_id=%d", msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        xEventGroupClearBits(event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        /* Clear alarm state on any data */
        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < TOTAL_ZONE; i++) {
                zone_alert_state[i] = 0;
            }
            alert_flg   = 0;
            loop_counter = PACKET_TIMEOUT - 1;
            xSemaphoreGive(data_mutex);
        }

        xEventGroupSetBits(event_group, GOT_DATA_BIT);

        /* OTA trigger: /ZIGRON/<MAC>/COMMAND topic with payload "OTA" */
        char cmd_topic[64];
        snprintf(cmd_topic, sizeof(cmd_topic), "/ZIGRON/%s/COMMAND", mac_string);

        if (event->topic_len == strlen(cmd_topic) &&
            strncmp(event->topic, cmd_topic, event->topic_len) == 0) {

            char payload[64] = {0};
            int len = event->data_len < (int)(sizeof(payload) - 1) ? event->data_len : (int)(sizeof(payload) - 1);
            memcpy(payload, event->data, len);
            payload[len] = '\0';

            ESP_LOGI(TAG, "COMMAND payload: '%s'", payload);

            /* Make case-insensitive "OTA" check */
            for (int i = 0; i < len; ++i) {
                payload[i] = (char)toupper((unsigned char)payload[i]);
            }

            if (strcmp(payload, "OTA") == 0) {
                ESP_LOGI(TAG, "OTA command received via MQTT, setting OTA trigger bit");
                xEventGroupSetBits(event_group, OTA_TRIGGER_BIT);
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        xEventGroupClearBits(event_group, MQTT_CONNECTED_BIT);
        break;

    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* IP / PPP event handlers (updated for dual connectivity)                    */
/* -------------------------------------------------------------------------- */

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %" PRIu32, event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
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

        ESP_LOGI(TAG, "GSM Modem connected to PPP server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        
        xEventGroupSetBits(event_group, GSM_CONNECTED_BIT);
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            current_conn_mode = CONN_MODE_GSM;
            ESP_LOGI(TAG, "GSM PPP connection active");
            xSemaphoreGive(conn_mutex);
        }
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "GSM Modem disconnected from PPP server");
        xEventGroupClearBits(event_group, GSM_CONNECTED_BIT);
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (current_conn_mode == CONN_MODE_GSM) {
                current_conn_mode = CONN_MODE_NONE;
                ESP_LOGI(TAG, "GSM PPP connection lost");
            }
            xSemaphoreGive(conn_mutex);
        }
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

/* -------------------------------------------------------------------------- */
/* HTTP client event handler (for OTA)                                        */
/* -------------------------------------------------------------------------- */

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:         ESP_LOGD(TAG, "HTTP_EVENT_ERROR"); break;
    case HTTP_EVENT_ON_CONNECTED:  ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED"); break;
    case HTTP_EVENT_HEADER_SENT:   ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT"); break;
    case HTTP_EVENT_ON_HEADER:     ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
                                            evt->header_key, evt->header_value); break;
    case HTTP_EVENT_ON_DATA:       ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len); break;
    case HTTP_EVENT_ON_FINISH:     ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH"); break;
    case HTTP_EVENT_DISCONNECTED:  ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED"); break;
    case HTTP_EVENT_REDIRECT:      ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT"); break;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* OTA task – works with both WiFi and GSM                                    */
/* -------------------------------------------------------------------------- */

static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "OTA task started, waiting for OTA_TRIGGER_BIT from MQTT...");

    while (1) {
        /* Wait until MQTT tells us to do OTA */
        xEventGroupWaitBits(event_group, OTA_TRIGGER_BIT,
                            pdTRUE,   // clear on exit
                            pdFALSE,
                            portMAX_DELAY);

        ESP_LOGI(TAG, "OTA trigger received");

        /* Ensure we have either WiFi or GSM connection before OTA */
        if ((xEventGroupGetBits(event_group) & (WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT)) == 0) {
            ESP_LOGW(TAG, "No network connection, skipping OTA attempt");
            continue;
        }

        esp_http_client_config_t config = {
            .url               = "http://54.194.219.149:45056/firmware/zigron_demo.bin",
            .event_handler     = _http_event_handler,
            .keep_alive_enable = true,
            .timeout_ms        = 30000,
        };

        esp_https_ota_config_t ota_config = {
            .http_config = &config,
        };

        ESP_LOGI(TAG, "Starting OTA via %s connection", 
                (current_conn_mode == CONN_MODE_WIFI) ? "WiFi" : "GSM");
        ESP_LOGI(TAG, "Free heap before OTA: %ld", esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(500));

        esp_err_t ret = esp_https_ota(&ota_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA success, restarting...");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA failed with error: %s", esp_err_to_name(ret));
            /* Stay alive and wait for next trigger */
        }
    }
}

/* -------------------------------------------------------------------------- */
/* FIXED: Connectivity Manager Task                                           */
/* -------------------------------------------------------------------------- */

static void connectivity_manager_task(void *arg)
{
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    static bool gsm_activated = false;
    static uint32_t last_gsm_activation_attempt = 0;
    
    ESP_LOGI(TAG, "Connectivity Manager task started");
    
    // First try WiFi
    ESP_LOGI(TAG, "Attempting WiFi connection first...");
    if (connect_wifi()) {
        ESP_LOGI(TAG, "WiFi connected successfully - using as primary");
        current_conn_mode = CONN_MODE_WIFI;
    } else {
        ESP_LOGW(TAG, "WiFi connection failed, will use GSM as backup");
        // WiFi failed at startup - GSM will be activated in the main loop
    }
    
    while (1) {
        bool wifi_connected = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0);
        bool gsm_connected = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0);
        uint32_t current_time = esp_timer_get_time() / 1000000;
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // CRITICAL FIX: If WiFi is not connected AND GSM is not connected AND GSM is not activated
            if (!wifi_connected && !gsm_connected && !gsm_activated) {
                // Check if enough time has passed since last attempt
                if (current_time - last_gsm_activation_attempt > 60) { // Wait 60 seconds between attempts
                    ESP_LOGI(TAG, "No network available - activating GSM PPP connection");
                    
                    // Switch GSM modem to data mode
                    esp_err_t ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "GSM data mode activated, waiting for PPP connection");
                        gsm_activated = true;
                        current_conn_mode = CONN_MODE_GSM;
                        
                        // Wait for PPP connection (up to 45 seconds)
                        EventBits_t bits = xEventGroupWaitBits(event_group,
                            GSM_CONNECTED_BIT | WIFI_CONNECTED_BIT,
                            pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(45000));
                        
                        if (bits & GSM_CONNECTED_BIT) {
                            ESP_LOGI(TAG, "GSM PPP connection established");
                        } else if (bits & WIFI_CONNECTED_BIT) {
                            ESP_LOGI(TAG, "WiFi reconnected while waiting for GSM");
                            gsm_activated = false;
                            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                        } else {
                            ESP_LOGW(TAG, "GSM PPP connection timeout");
                            gsm_activated = false;
                            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to activate GSM data mode: %s", esp_err_to_name(ret));
                    }
                    last_gsm_activation_attempt = current_time;
                }
            }
            // If WiFi connects and we're on GSM, switch back
            else if (wifi_connected && current_conn_mode == CONN_MODE_GSM) {
                ESP_LOGI(TAG, "WiFi reconnected, switching from GSM to WiFi");
                current_conn_mode = CONN_MODE_WIFI;
                
                // Switch GSM back to command mode to save power
                if (gsm_activated) {
                    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                    gsm_activated = false;
                }
            }
            // If GSM connects, update connection mode
            else if (gsm_connected && current_conn_mode != CONN_MODE_GSM) {
                current_conn_mode = CONN_MODE_GSM;
                ESP_LOGI(TAG, "GSM connection active");
            }
            
            xSemaphoreGive(conn_mutex);
        }
        
        // Periodically try to reconnect WiFi (if GSM is active)
        if (current_conn_mode == CONN_MODE_GSM) {
            static uint32_t last_wifi_attempt = 0;
            
            if (current_time - last_wifi_attempt > 300) { // Try every 5 minutes
                ESP_LOGI(TAG, "Attempting WiFi reconnection while on GSM...");
                
                esp_wifi_start();
                esp_wifi_connect();
                
                EventBits_t bits = xEventGroupWaitBits(event_group, 
                    WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, 
                    pdMS_TO_TICKS(30000));
                
                if (!(bits & WIFI_CONNECTED_BIT)) {
                    esp_wifi_disconnect();
                    esp_wifi_stop();
                }
                
                last_wifi_attempt = current_time;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
    }
}

/* -------------------------------------------------------------------------- */
/* MQTT task – works with both WiFi and GSM                                   */
/* -------------------------------------------------------------------------- */

typedef struct {
    esp_modem_dce_t *dce;
} mqtt_task_param_t;

static void mqtt_task(void *arg)
{
    mqtt_task_param_t *params = (mqtt_task_param_t *)arg;
    esp_modem_dce_t *dce = params->dce;
    vPortFree(params);

    ESP_LOGI(TAG, "MQTT task started");

    esp_mqtt_client_handle_t mqtt_client = NULL;
    bool mqtt_started           = false;
    uint32_t last_mqtt_publish  = 0;
    uint32_t last_connection_check = 0;
    uint32_t network_unavailable_start = 0;
    bool emergency_gsm_triggered = false;

    while (1) {
        bool network_connected = ((xEventGroupGetBits(event_group) & 
                                 (WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT)) != 0);
        
        // Emergency GSM activation if no network for too long
        if (!network_connected) {
            if (network_unavailable_start == 0) {
                network_unavailable_start = esp_timer_get_time() / 1000000;
                emergency_gsm_triggered = false;
            }
            
            uint32_t unavailable_duration = (esp_timer_get_time() / 1000000) - network_unavailable_start;
            
            // If no network for 2 minutes, trigger emergency GSM activation
            if (unavailable_duration > 120 && !emergency_gsm_triggered) {
                ESP_LOGW(TAG, "No network for %d seconds - emergency GSM activation", unavailable_duration);
                
                // Signal connectivity manager to activate GSM
                if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    current_conn_mode = CONN_MODE_NONE; // Reset to trigger GSM activation
                    xSemaphoreGive(conn_mutex);
                }
                emergency_gsm_triggered = true;
            }
        } else {
            network_unavailable_start = 0;
            emergency_gsm_triggered = false;
        }
        
        if (!network_connected) {
            if (mqtt_client && mqtt_started) {
                esp_mqtt_client_stop(mqtt_client);
                mqtt_started = false;
                ESP_LOGI(TAG, "MQTT client stopped due to network disconnect");
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        // Check GSM connection if we're using GSM
        if (current_conn_mode == CONN_MODE_GSM) {
            uint32_t current_time = esp_timer_get_time() / 1000000;
            if (current_time - last_connection_check > 30) {
                last_connection_check = current_time;
                check_ppp_connection(dce, mqtt_client);
            }
        }

        /* MQTT client init */
        if (!mqtt_client) {
            esp_mqtt_client_config_t mqtt_config = {
                .broker.address.uri         = "mqtt://zigron:zigron123@54.194.219.149:45055",
                .network.timeout_ms         = 20000,
                .session.keepalive          = 60,
                .network.disable_auto_reconnect = false,
                .network.reconnect_timeout_ms = 10000,
            };
            mqtt_client = esp_mqtt_client_init(&mqtt_config);
            esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        }

        if (!mqtt_started) {
            ESP_LOGI(TAG, "Starting MQTT client over %s...", 
                    (current_conn_mode == CONN_MODE_WIFI) ? "WiFi" : "GSM");
            if (esp_mqtt_client_start(mqtt_client) == ESP_OK) {
                mqtt_started = true;
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                ESP_LOGE(TAG, "Failed to start MQTT client");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
        }

        /* Publish every PACKET_TIMEOUT ticks from sensor_task */
        bool     should_publish = false;
        uint16_t local_loop_counter;
        uint16_t local_alert_flg;
        uint16_t local_zone_raw[TOTAL_ZONE];
        uint8_t  local_zone_alert[TOTAL_ZONE];

        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if ( (loop_counter >= PACKET_TIMEOUT) | alert_flg ) {
                should_publish     = true;
                local_loop_counter = loop_counter;
                local_alert_flg    = alert_flg;
                memcpy(local_zone_raw,   zone_raw_value,   sizeof(local_zone_raw));
                memcpy(local_zone_alert, zone_alert_state, sizeof(local_zone_alert));
                loop_counter = 0;
                ESP_LOGI(TAG, "SHOULD PUBLISH triggered at loop_counter= %d, alert_flg=%d", 
                        local_loop_counter, alert_flg);
            }
            xSemaphoreGive(data_mutex);
        }

        if (should_publish && mqtt_started && network_connected) {
            data_buff[0]  = 0;
            topic_buff[0] = 0;

            int data_len = snprintf(data_buff, sizeof(data_buff),
                "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"DNA\":[\"%s\",%lld],"
                "\"CONN\":\"%s\","
                "\"FW\":\"%s\"}",
                local_zone_raw[0], local_zone_raw[1], local_zone_raw[2], local_zone_raw[3],
                local_zone_raw[4], local_zone_raw[5], local_zone_raw[6], local_zone_raw[7],
                local_zone_raw[8], local_zone_raw[9],
                local_zone_alert[0], local_zone_alert[1], local_zone_alert[2], local_zone_alert[3],
                local_zone_alert[4], local_zone_alert[5], local_zone_alert[6], local_zone_alert[7],
                local_zone_alert[8], local_zone_alert[9],
                mac_string, (long long)(esp_timer_get_time() / 1000000),
                (current_conn_mode == CONN_MODE_WIFI) ? "WIFI" : "GSM",
                FW_VER);

            if (data_len > 0 && data_len < (int)sizeof(data_buff)) {
                if (prev_alert_flg != local_alert_flg) {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/ALERT", mac_string);
                } else {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/HB", mac_string);
                }

                int publish_response = esp_mqtt_client_publish(mqtt_client, topic_buff,
                                                               data_buff, 0, 0, 0);
                if (publish_response == -1) {
                    ESP_LOGE(TAG, "MQTT publish failed");
                } else {
                    ESP_LOGI(TAG, "Published via %s: %s -> %s", 
                            (current_conn_mode == CONN_MODE_WIFI) ? "WiFi" : "GSM",
                            topic_buff, data_buff);
                    last_mqtt_publish = esp_timer_get_time() / 1000000;
                    prev_alert_flg    = local_alert_flg;
                }
            }
        }

        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* -------------------------------------------------------------------------- */
/* app_main – initialises things and starts tasks                             */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_log_level_set("esp_http_client", ESP_LOG_DEBUG);
    esp_log_level_set("esp_https_ota",   ESP_LOG_DEBUG);
    esp_log_level_set("wifi",            ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO,
            CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);
    esp_read_mac(mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string, "%02X%02X%02X%02X%02X%02X",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC address %s", mac_string);

    event_group = xEventGroupCreate();
    data_mutex  = xSemaphoreCreateMutex();
    conn_mutex  = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    // Initialize WiFi first (priority connection)
    init_wifi();

    // GSM/PPP setup (backup connection)
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.tx_io_num = CONFIG_EXAMPLE_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_EXAMPLE_MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = CONFIG_EXAMPLE_MODEM_UART_RTS_PIN;
    dte_config.uart_config.cts_io_num = CONFIG_EXAMPLE_MODEM_UART_CTS_PIN;
    dte_config.uart_config.flow_control = EXAMPLE_FLOW_CONTROL;
    dte_config.uart_config.rx_buffer_size = 2048;
    dte_config.uart_config.tx_buffer_size = 1024;

    ESP_ERROR_CHECK(esp_task_wdt_deinit());
    ESP_LOGI(TAG, "Watchdog temporarily disabled for initialization");

    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN,   GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, 0);  vTaskDelay(pdMS_TO_TICKS(100));

    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_config, &dce_config, esp_netif);
    ESP_LOGI(TAG, "Waiting for GSM modem to boot (15 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    assert(dce);

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT | 
                       DISCONNECT_BIT | OTA_TRIGGER_BIT | WIFI_CONNECTED_BIT | 
                       GSM_CONNECTED_BIT | MQTT_CONNECTED_BIT);

    // Test GSM modem communication
    ESP_LOGI(TAG, "Testing basic GSM modem communication...");
    int retry_count = 0;
    bool gsm_available = false;
    while (retry_count < 3) {
        int rssi, ber;
        ret = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "GSM modem communication OK - Signal: rssi=%d, ber=%d", rssi, ber);
            gsm_available = true;
            break;
        }
        ESP_LOGW(TAG, "GSM communication test failed (attempt %d), retrying...", retry_count + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry_count++;
    }

    if (gsm_available && !check_network_registration(dce)) {
        ESP_LOGW(TAG, "GSM network registration failed - will use WiFi only");
        gsm_available = false;
    }

    // Set GSM modem to command mode (ready for fallback activation)
    if (gsm_available) {
        ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT);
        if (ret == ESP_OK) {
            esp_modem_dce_mode_t mode = esp_modem_get_mode(dce);
            ESP_LOGI(TAG, "GSM mode detection completed: current mode is: %d", mode);
            if (mode == ESP_MODEM_MODE_DATA) {
                ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "GSM command mode restored (ready for fallback)");
                }
            }
        }

#if CONFIG_EXAMPLE_NEED_SIM_PIN == 1
        {
            bool pin_ok = false;
            if (esp_modem_read_pin(dce, &pin_ok) == ESP_OK && pin_ok == false) {
                if (esp_modem_set_pin(dce, CONFIG_EXAMPLE_SIM_PIN) == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                } else {
                    ESP_LOGW(TAG, "Failed to set SIM PIN");
                }
            }
        }
#endif

        // Quick test of GSM PPP capability
        ESP_LOGI(TAG, "Testing GSM PPP capability...");
        ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000)); // Wait a bit
            EventBits_t bits = xEventGroupWaitBits(event_group, 
                GSM_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
            
            if (bits & GSM_CONNECTED_BIT) {
                ESP_LOGI(TAG, "GSM PPP test successful");
                // Go back to command mode for now
                esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
            } else {
                ESP_LOGW(TAG, "GSM PPP test timed out (will still try as fallback)");
                esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
            }
        } else {
            ESP_LOGE(TAG, "GSM data mode test failed: %s", esp_err_to_name(ret));
        }
    }

    /* Start tasks */
    BaseType_t xRet;

    // Start connectivity manager first
    xRet = xTaskCreate(connectivity_manager_task, "conn_mgr", 4096, dce, 4, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connectivity manager task");
    }

    // Start other tasks with delay to allow connectivity to establish
    vTaskDelay(pdMS_TO_TICKS(2000));

    xRet = xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 4, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
    }

    mqtt_task_param_t *params = pvPortMalloc(sizeof(mqtt_task_param_t));
    if (!params) {
        ESP_LOGE(TAG, "Failed to allocate MQTT task params");
        return;
    }
    params->dce = dce;

    xRet = xTaskCreate(mqtt_task, "mqtt_task", 8192, params, 5, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT task");
        vPortFree(params);
    }

    xRet = xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
    }

    ESP_LOGI(TAG, "app_main finished bootstrapping; tasks are running");
}