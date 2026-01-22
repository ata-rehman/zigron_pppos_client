/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* WiFi-first MQTT + OTA with GSM/PPP as backup + Web Server + Permanent Hotspot
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include <cJSON.h>

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
#define FW_VER              "0.05"      // Updated version with MQTT config
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
#define WIFI_CONNECT_TIMEOUT_MS 30000   // 30 seconds WiFi timeout
#define MAX_WIFI_RETRIES   3

// Webserver and Hotspot constants
#define AP_SSID "Zigron-Config"
#define AP_PASSWORD "config1234"
#define AP_CHANNEL 1
#define AP_MAX_CONN 4
#define NVS_CONFIG_NAMESPACE "zigron_config"
#define TOTAL_ZONE 10

static const char *TAG = "Zigron_Dual_MQTT";

/* Event group for connectivity status */
static EventGroupHandle_t event_group = NULL;
static const int WIFI_CONNECTED_BIT   = BIT0;   // WiFi is connected
static const int GSM_CONNECTED_BIT    = BIT1;   // GSM/PPP is connected
static const int MQTT_CONNECTED_BIT   = BIT2;   // MQTT is connected
static const int OTA_TRIGGER_BIT      = BIT3;   // MQTT-triggered OTA
static const int RECONFIG_TRIGGER_BIT = BIT4;   // Reconfiguration triggered

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

/* Configuration structure */
typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_broker_url[128];
    uint16_t zone_lower[TOTAL_ZONE];
    uint16_t zone_upper[TOTAL_ZONE];
    uint32_t publish_interval_ms;
    bool enable_ota;
} device_config_t;

// Default configuration
static device_config_t g_config = {
    .wifi_ssid = "",
    .wifi_password = "",
    .mqtt_broker_url = "mqtt://zigron:zigron123@54.194.219.149:45055",
    .publish_interval_ms = 5000,
    .enable_ota = true,
};

// HTTP Server
static httpd_handle_t server = NULL;
static bool ap_mode_active = true;

// Task handles
static TaskHandle_t wifi_task_handle = NULL;
static TaskHandle_t webserver_task_handle = NULL;

/* Forward declarations */
static void init_wifi(void);
static bool connect_wifi(void);
static void start_hotspot(void);
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
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);

static void sensor_task(void *arg);
static void mqtt_task(void *arg);
static void ota_task(void *arg);
static void connectivity_manager_task(void *arg);
static void webserver_task(void *arg);
static void wifi_connection_task(void *arg);
static void handle_mqtt_config_command(const char *payload, int payload_len);

/* NVS Configuration Functions */
static esp_err_t save_config_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_blob(handle, "config", &g_config, sizeof(g_config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    
    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    size_t required_size = 0;
    
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    
    // Check if config exists
    err = nvs_get_blob(handle, "config", NULL, &required_size);
    if (err == ESP_OK && required_size == sizeof(g_config)) {
        err = nvs_get_blob(handle, "config", &g_config, &required_size);
        ESP_LOGI(TAG, "Loaded config from NVS");
        ESP_LOGI(TAG, "WiFi SSID: %s", g_config.wifi_ssid);
        ESP_LOGI(TAG, "MQTT Broker: %s", g_config.mqtt_broker_url);
        
        // Apply loaded thresholds
        memcpy(zone_lower_limit, g_config.zone_lower, sizeof(g_config.zone_lower));
        memcpy(zone_upper_limit, g_config.zone_upper, sizeof(g_config.zone_upper));
        
        // Check if MQTT broker is empty and set a default
        if (strlen(g_config.mqtt_broker_url) == 0) {
            ESP_LOGW(TAG, "MQTT broker URL is empty, setting default");
            strcpy(g_config.mqtt_broker_url, "mqtt://zigron:zigron123@54.194.219.149:45055");
        }
    } else {
        // If no config saved, use defaults
        memcpy(g_config.zone_lower, zone_lower_limit, sizeof(zone_lower_limit));
        memcpy(g_config.zone_upper, zone_upper_limit, sizeof(zone_upper_limit));
        ESP_LOGW(TAG, "No saved config in NVS, using defaults");
        ESP_LOGI(TAG, "Default WiFi SSID: %s", g_config.wifi_ssid);
        ESP_LOGI(TAG, "Default MQTT Broker: %s", g_config.mqtt_broker_url);
        err = ESP_ERR_NOT_FOUND;
    }
    
    nvs_close(handle);
    return err;
}

/* Apply WiFi configuration and reconnect */
static void apply_wifi_configuration(void)
{
    ESP_LOGI(TAG, "Applying new WiFi configuration");
    
    // Configure WiFi station
    wifi_config_t wifi_config_sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strncpy((char*)wifi_config_sta.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config_sta.sta.ssid));
    strncpy((char*)wifi_config_sta.sta.password, g_config.wifi_password, sizeof(wifi_config_sta.sta.password));
    
    // Update WiFi configuration
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta));
    
    // If currently connected, disconnect and reconnect with new settings
    if ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) {
        ESP_LOGI(TAG, "Disconnecting from current WiFi to apply new settings...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Trigger reconnection
    esp_wifi_connect();
}

/* Handle MQTT configuration command */
static void handle_mqtt_config_command(const char *payload, int payload_len)
{
    char payload_copy[256];
    if (payload_len >= sizeof(payload_copy)) {
        ESP_LOGE(TAG, "MQTT config payload too large");
        return;
    }
    
    memcpy(payload_copy, payload, payload_len);
    payload_copy[payload_len] = '\0';
    
    ESP_LOGI(TAG, "Processing MQTT config command: %s", payload_copy);
    
    // Parse JSON
    cJSON *root = cJSON_Parse(payload_copy);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON config");
        return;
    }
    
    bool config_changed = false;
    
    // Parse WiFi SSID
    cJSON *wifi_ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(wifi_ssid) && wifi_ssid->valuestring != NULL) {
        if (strlen(wifi_ssid->valuestring) > 0 && 
            strcmp(wifi_ssid->valuestring, g_config.wifi_ssid) != 0) {
            strncpy(g_config.wifi_ssid, wifi_ssid->valuestring, sizeof(g_config.wifi_ssid)-1);
            g_config.wifi_ssid[sizeof(g_config.wifi_ssid)-1] = '\0';
            config_changed = true;
            ESP_LOGI(TAG, "MQTT: New WiFi SSID: %s", g_config.wifi_ssid);
        }
    }
    
    // Parse WiFi Password
    cJSON *wifi_password = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(wifi_password) && wifi_password->valuestring != NULL) {
        if (strcmp(wifi_password->valuestring, g_config.wifi_password) != 0) {
            strncpy(g_config.wifi_password, wifi_password->valuestring, sizeof(g_config.wifi_password)-1);
            g_config.wifi_password[sizeof(g_config.wifi_password)-1] = '\0';
            config_changed = true;
            ESP_LOGI(TAG, "MQTT: WiFi Password updated");
        }
    }
    
    // Parse MQTT Broker URL
    cJSON *mqtt_broker = cJSON_GetObjectItem(root, "mqtt_broker");
    if (cJSON_IsString(mqtt_broker) && mqtt_broker->valuestring != NULL) {
        if (strlen(mqtt_broker->valuestring) > 0 && 
            strcmp(mqtt_broker->valuestring, g_config.mqtt_broker_url) != 0) {
            strncpy(g_config.mqtt_broker_url, mqtt_broker->valuestring, sizeof(g_config.mqtt_broker_url)-1);
            g_config.mqtt_broker_url[sizeof(g_config.mqtt_broker_url)-1] = '\0';
            config_changed = true;
            ESP_LOGI(TAG, "MQTT: New Broker URL: %s", g_config.mqtt_broker_url);
        }
    }
    
    // Parse Publish Interval
    cJSON *publish_interval = cJSON_GetObjectItem(root, "publish_interval");
    if (cJSON_IsNumber(publish_interval)) {
        uint32_t new_interval = publish_interval->valueint;
        if (new_interval >= 1000 && new_interval <= 30000 && 
            new_interval != g_config.publish_interval_ms) {
            g_config.publish_interval_ms = new_interval;
            config_changed = true;
            ESP_LOGI(TAG, "MQTT: New publish interval: %d ms", g_config.publish_interval_ms);
        }
    }
    
    // Parse Thresholds
    cJSON *thresholds = cJSON_GetObjectItem(root, "thresholds");
    if (cJSON_IsArray(thresholds)) {
        int array_size = cJSON_GetArraySize(thresholds);
        if (array_size == TOTAL_ZONE) {
            for (int i = 0; i < TOTAL_ZONE && i < array_size; i++) {
                cJSON *threshold = cJSON_GetArrayItem(thresholds, i);
                if (cJSON_IsObject(threshold)) {
                    cJSON *low = cJSON_GetObjectItem(threshold, "low");
                    cJSON *high = cJSON_GetObjectItem(threshold, "high");
                    
                    if (cJSON_IsNumber(low) && cJSON_IsNumber(high)) {
                        uint16_t new_low = low->valueint;
                        uint16_t new_high = high->valueint;
                        
                        if (new_low != g_config.zone_lower[i] || new_high != g_config.zone_upper[i]) {
                            g_config.zone_lower[i] = new_low;
                            g_config.zone_upper[i] = new_high;
                            config_changed = true;
                            
                            // Update runtime thresholds
                            if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                                zone_lower_limit[i] = new_low;
                                zone_upper_limit[i] = new_high;
                                xSemaphoreGive(data_mutex);
                            }
                            
                            ESP_LOGI(TAG, "MQTT: Zone %d thresholds: low=%d, high=%d", i, new_low, new_high);
                        }
                    }
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    if (config_changed) {
        // Save to NVS
        esp_err_t err = save_config_to_nvs();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Configuration saved to NVS via MQTT");
            
            // Apply WiFi configuration if changed
            if (strlen(g_config.wifi_ssid) > 0) {
                apply_wifi_configuration();
            }
            
            // Signal reconfiguration
            xEventGroupSetBits(event_group, RECONFIG_TRIGGER_BIT);
        } else {
            ESP_LOGE(TAG, "Failed to save configuration to NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "No configuration changes detected in MQTT command");
    }
}

/* Hotspot Functions */
static void start_hotspot(void)
{
    ESP_LOGI(TAG, "Starting hotspot (AP mode)...");
    
    // Configure AP settings
    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .password = AP_PASSWORD,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = true,
            },
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config_ap));
    
    // Get and log AP IP address
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        ESP_LOGI(TAG, "Hotspot started:");
        ESP_LOGI(TAG, "  SSID: %s", AP_SSID);
        ESP_LOGI(TAG, "  Password: %s", AP_PASSWORD);
        ESP_LOGI(TAG, "  IP Address: " IPSTR, IP2STR(&ip_info.ip));
        ESP_LOGI(TAG, "Connect to hotspot and visit: http://" IPSTR, IP2STR(&ip_info.ip));
    }
    
    ap_mode_active = true;
}

/* WiFi Functions */
static void init_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set WiFi to AP+STA mode (simultaneous)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));
    
    ESP_LOGI(TAG, "WiFi initialization complete in AP+STA mode");
}

static bool connect_wifi(void)
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", g_config.wifi_ssid);
    
    if (strlen(g_config.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID not configured");
        return false;
    }
    
    // Configure WiFi station
    wifi_config_t wifi_config_sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    
    strncpy((char*)wifi_config_sta.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config_sta.sta.ssid));
    strncpy((char*)wifi_config_sta.sta.password, g_config.wifi_password, sizeof(wifi_config_sta.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta));
    
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
        wifi_retry_count = 0;
        return true;
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
        return false;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_STACONNECTED:
                ESP_LOGI(TAG, "Station connected to AP");
                break;
                
            case WIFI_EVENT_AP_STADISCONNECTED:
                ESP_LOGI(TAG, "Station disconnected from AP");
                break;
                
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi station connected to AP");
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi station disconnected");
                xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
                
                if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (current_conn_mode == CONN_MODE_WIFI) {
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
                }
                break;
        }
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            
            wifi_retry_count = 0;
            
            if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                current_conn_mode = CONN_MODE_WIFI;
                xSemaphoreGive(conn_mutex);
            }
            
            xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT);
            
        } else if (event_id == IP_EVENT_AP_STAIPASSIGNED) {
            ip_event_ap_staipassigned_t* event = (ip_event_ap_staipassigned_t*) event_data;
            ESP_LOGI(TAG, "AP assigned IP to station: " IPSTR, IP2STR(&event->ip));
        }
    }
}

/* WiFi Connection Task */
static void wifi_connection_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi connection task started");
    
    // Always start hotspot first
    start_hotspot();
    
    // Main WiFi connection loop
    while (1) {
        // Check if reconfiguration was requested
        EventBits_t bits = xEventGroupGetBits(event_group);
        if (bits & RECONFIG_TRIGGER_BIT) {
            xEventGroupClearBits(event_group, RECONFIG_TRIGGER_BIT);
            ESP_LOGI(TAG, "Reconfiguration triggered, re-evaluating WiFi connection");
        }
        
        // Check if WiFi credentials are configured
        if (strlen(g_config.wifi_ssid) == 0) {
            ESP_LOGW(TAG, "WiFi SSID not configured. Hotspot is active for configuration.");
            
            // Wait for 30 seconds before checking again
            for (int i = 0; i < 30; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (strlen(g_config.wifi_ssid) > 0) {
                    break;  // SSID configured, break out of wait loop
                }
            }
            continue;
        }
        
        ESP_LOGI(TAG, "Attempting to connect to WiFi: %s", g_config.wifi_ssid);
        
        // Try to connect to WiFi (hotspot remains active)
        if (connect_wifi()) {
            ESP_LOGI(TAG, "Successfully connected to WiFi: %s", g_config.wifi_ssid);
            ESP_LOGI(TAG, "Hotspot remains active at SSID: %s", AP_SSID);
            
            // Monitor connection while connected
            while ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) {
                vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
                
                // Check connection status
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi connection lost");
                    xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
                    
                    // Break to outer loop to retry connection
                    break;
                }
            }
            
        } else {
            ESP_LOGW(TAG, "Failed to connect to WiFi: %s", g_config.wifi_ssid);
            ESP_LOGI(TAG, "Hotspot remains active for configuration");
            
            ESP_LOGI(TAG, "Will retry WiFi connection in 1 minute...");
            vTaskDelay(pdMS_TO_TICKS(60 * 1000));  // Wait 1 minute before retrying
        }
        
        // Reset retry count for next attempt
        wifi_retry_count = 0;
    }
}

/* Sensor Task */
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
                zone_alert_state[TOTAL_ZONE-2] = 0x00;  // Clear if no longer different
            }

            /* Digital inputs */
            zone_raw_value[TOTAL_ZONE-1] = gpio_get_level((gpio_num_t)39);
            zone_raw_value[TOTAL_ZONE-2] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);

            /* Analog zones */
            for (uint8_t i = 0; i < (TOTAL_ZONE-2); i++) {
                zone_raw_value[i] = mcpReadData(&dev, i);

                uint16_t bitmask = 1 << i;
                
                // First, clear any existing alert state for this zone
                zone_alert_state[i] = 0x00;
                
                // Then check if we need to set an alert
                if (zone_raw_value[i] < zone_lower_limit[i]) {
                    zone_alert_state[i] = 0x01;  // Low alert
                    alert_flg |= bitmask;
                } else if (zone_raw_value[i] > zone_upper_limit[i]) {
                    zone_alert_state[i] = 0x02;  // High alert
                    alert_flg |= bitmask;
                } else {
                    // Value is within range, clear the alert flag
                    alert_flg &= ~bitmask;
                }
            }

            snprintf(data_buff, sizeof(data_buff),
                "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
                zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3],
                zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
                zone_raw_value[8], zone_raw_value[9]);

            ESP_LOGI(TAG, "Sensor data: %s, Alert flags: 0x%03X", data_buff, alert_flg);

            xSemaphoreGive(data_mutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));  // sensor period ~1s
    }
}

/* GSM/PPP Functions */
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

/* MQTT Event Handler - UPDATED with config commands */
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
        
        // Subscribe to standard topics
        topic_buff[0] = 0;
        sprintf(topic_buff, "/ZIGRON/%s/CLEAR", mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe CLEAR, msg_id=%d", msg_id);

        topic_buff[0] = 0;
        sprintf(topic_buff, "/ZIGRON/%s/COMMAND", mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe COMMAND, msg_id=%d", msg_id);
        
        // Subscribe to configuration topic
        topic_buff[0] = 0;
        sprintf(topic_buff, "/ZIGRON/%s/CONFIG", mac_string);
        msg_id = esp_mqtt_client_subscribe(client, topic_buff, 0);
        ESP_LOGI(TAG, "sent subscribe CONFIG, msg_id=%d", msg_id);
        
        // Publish configuration status
        char config_status[256];
        snprintf(config_status, sizeof(config_status),
            "{\"wifi_ssid\":\"%s\",\"mqtt_broker\":\"%s\",\"status\":\"connected\"}",
            g_config.wifi_ssid, g_config.mqtt_broker_url);
        
        sprintf(topic_buff, "/ZIGRON/%s/STATUS", mac_string);
        esp_mqtt_client_publish(client, topic_buff, config_status, 0, 0, 0);
        ESP_LOGI(TAG, "Published configuration status");
        
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

        /* Check if this is a CLEAR topic */
        char topic[64];
        int topic_len = event->topic_len < 63 ? event->topic_len : 63;
        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';
        
        char clear_topic[64];
        snprintf(clear_topic, sizeof(clear_topic), "/ZIGRON/%s/CLEAR", mac_string);
        
        /* Clear alarm state only on CLEAR topics */
        if (strncmp(topic, clear_topic, strlen(clear_topic)) == 0) {
            if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < TOTAL_ZONE; i++) {
                    zone_alert_state[i] = 0;
                }
                alert_flg   = 0;
                loop_counter = PACKET_TIMEOUT - 1;
                xSemaphoreGive(data_mutex);
            }
        }

        /* Clear alarm state on any data */
        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < TOTAL_ZONE; i++) {
                zone_alert_state[i] = 0;
            }
            alert_flg   = 0;
            loop_counter = PACKET_TIMEOUT - 1;
            xSemaphoreGive(data_mutex);
        }

        memcpy(topic, event->topic, topic_len);
        topic[topic_len] = '\0';
        
        char config_topic[64];
        snprintf(config_topic, sizeof(config_topic), "/ZIGRON/%s/CONFIG", mac_string);
        
        char cmd_topic[64];
        snprintf(cmd_topic, sizeof(cmd_topic), "/ZIGRON/%s/COMMAND", mac_string);
        
        /* Handle CONFIG topic */
        if (strncmp(topic, config_topic, strlen(config_topic)) == 0) {
            ESP_LOGI(TAG, "Configuration command received via MQTT");
            handle_mqtt_config_command(event->data, event->data_len);
        }
        /* Handle COMMAND topic */
        else if (strncmp(topic, cmd_topic, strlen(cmd_topic)) == 0) {
            char payload[64] = {0};
            int len = event->data_len < (int)(sizeof(payload) - 1) ? event->data_len : (int)(sizeof(payload) - 1);
            memcpy(payload, event->data, len);
            payload[len] = '\0';

            ESP_LOGI(TAG, "COMMAND payload: '%s'", payload);

            /* Make case-insensitive check */
            for (int i = 0; i < len; ++i) {
                payload[i] = (char)toupper((unsigned char)payload[i]);
            }

            if (strcmp(payload, "OTA") == 0) {
                ESP_LOGI(TAG, "OTA command received via MQTT, setting OTA trigger bit");
                xEventGroupSetBits(event_group, OTA_TRIGGER_BIT);
            }
            else if (strcmp(payload, "GET_CONFIG") == 0) {
                ESP_LOGI(TAG, "GET_CONFIG command received via MQTT");
                // Publish current configuration
                char config_json[1024];
                snprintf(config_json, sizeof(config_json),
                    "{\"wifi_ssid\":\"%s\","
                    "\"wifi_password\":\"%s\","
                    "\"mqtt_broker\":\"%s\","
                    "\"publish_interval\":%lu,"
                    "\"thresholds\":[",
                    g_config.wifi_ssid,
                    g_config.wifi_password,
                    g_config.mqtt_broker_url,
                    g_config.publish_interval_ms);
                
                // Add thresholds
                char thresholds_str[256];
                thresholds_str[0] = '\0';
                for (int i = 0; i < TOTAL_ZONE; i++) {
                    char zone_str[64];
                    snprintf(zone_str, sizeof(zone_str), 
                        "{\"zone\":%d,\"low\":%d,\"high\":%d}%s",
                        i, g_config.zone_lower[i], g_config.zone_upper[i],
                        (i < TOTAL_ZONE - 1) ? "," : "");
                    strcat(thresholds_str, zone_str);
                }
                
                strcat(config_json, thresholds_str);
                strcat(config_json, "]}");
                
                // Publish to config response topic
                char response_topic[64];
                snprintf(response_topic, sizeof(response_topic), "/ZIGRON/%s/CONFIG_RESPONSE", mac_string);
                esp_mqtt_client_publish(client, response_topic, config_json, 0, 0, 0);
                ESP_LOGI(TAG, "Published configuration to %s", response_topic);
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

/* IP / PPP Event Handlers */
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

/* HTTP Client Event Handler (for OTA) */
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

/* OTA Task */
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

/* HTTP Server Handlers (remain mostly the same, but updated for consistency) */

// Helper function to parse form data
static char* urldecode(char *dst, const char *src, size_t dstsize) {
    char a, b;
    size_t i = 0;
    while (*src && i < dstsize - 1) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
    return dst;
}

// Handler for status information
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[512];
    
    const char* device_mode = "AP+STA Mode";
    const char* wifi_status = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* wifi_class = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    const char* mqtt_status = ((xEventGroupGetBits(event_group) & MQTT_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* mqtt_class = ((xEventGroupGetBits(event_group) & MQTT_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    const char* gsm_status = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* gsm_class = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    
    char ip_address[16] = "Not Available";
    esp_netif_t* netif = NULL;
    
    // For web interface, show hotspot IP (AP interface)
    netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(netif, &ip_info);
        snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
    }
    
    snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"wifi\":\"%s\",\"mqtt\":\"%s\",\"gsm\":\"%s\","
        "\"ip\":\"%s\",\"wifi_class\":\"%s\",\"mqtt_class\":\"%s\",\"gsm_class\":\"%s\","
        "\"wifi_ssid\":\"%s\",\"mqtt_broker\":\"%s\"}",
        device_mode, wifi_status, mqtt_status, gsm_status, ip_address, 
        wifi_class, mqtt_class, gsm_class,
        g_config.wifi_ssid, g_config.mqtt_broker_url);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

// Handler for sensor data
static esp_err_t sensor_data_get_handler(httpd_req_t *req)
{
    char json[800];
    
    if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        snprintf(json, sizeof(json),
            "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
            "\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
            "\"LOW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
            "\"HIGH\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d]}",
            zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3],
            zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
            zone_raw_value[8], zone_raw_value[9],
            zone_alert_state[0], zone_alert_state[1], zone_alert_state[2], zone_alert_state[3],
            zone_alert_state[4], zone_alert_state[5], zone_alert_state[6], zone_alert_state[7],
            zone_alert_state[8], zone_alert_state[9],
            zone_lower_limit[0], zone_lower_limit[1], zone_lower_limit[2], zone_lower_limit[3],
            zone_lower_limit[4], zone_lower_limit[5], zone_lower_limit[6], zone_lower_limit[7],
            zone_lower_limit[8], zone_lower_limit[9],
            zone_upper_limit[0], zone_upper_limit[1], zone_upper_limit[2], zone_upper_limit[3],
            zone_upper_limit[4], zone_upper_limit[5], zone_upper_limit[6], zone_upper_limit[7],
            zone_upper_limit[8], zone_upper_limit[9]);
        
        xSemaphoreGive(data_mutex);
    } else {
        strcpy(json, "{\"error\":\"Unable to read sensor data\"}");
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

// Handler for Wi-Fi configuration - FIXED VERSION
static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret;
    
    if ((ret = httpd_req_recv(req, buf, sizeof(buf)-1)) <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Parse form data
    char ssid[32] = "";
    char password[64] = "";
    
    char *token = strtok(buf, "&");
    while (token != NULL) {
        if (strncmp(token, "ssid=", 5) == 0) {
            urldecode(ssid, token + 5, sizeof(ssid));
        } else if (strncmp(token, "password=", 9) == 0) {
            urldecode(password, token + 9, sizeof(password));
        }
        token = strtok(NULL, "&");
    }
    
    // Validate input
    if (strlen(ssid) == 0) {
        httpd_resp_send(req, "Error: WiFi SSID cannot be empty", -1);
        return ESP_OK;
    }
    
    // Save to configuration
    strncpy(g_config.wifi_ssid, ssid, sizeof(g_config.wifi_ssid)-1);
    g_config.wifi_ssid[sizeof(g_config.wifi_ssid)-1] = '\0';
    
    if (strlen(password) > 0) {
        strncpy(g_config.wifi_password, password, sizeof(g_config.wifi_password)-1);
        g_config.wifi_password[sizeof(g_config.wifi_password)-1] = '\0';
    } else {
        // Clear password if empty
        memset(g_config.wifi_password, 0, sizeof(g_config.wifi_password));
    }
    
    // Save to NVS
    esp_err_t err = save_config_to_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(err));
        httpd_resp_send(req, "Error: Failed to save configuration to storage", -1);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "WiFi credentials updated via web interface:");
    ESP_LOGI(TAG, "  SSID: %s", g_config.wifi_ssid);
    ESP_LOGI(TAG, "  Password: %s", g_config.wifi_password);
    
    httpd_resp_send(req, "Wi-Fi settings saved successfully! Device will attempt to connect to the new network...", -1);
    
    // Apply WiFi configuration
    apply_wifi_configuration();
    
    return ESP_OK;
}

// Handler for MQTT configuration
static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret;
    
    if ((ret = httpd_req_recv(req, buf, sizeof(buf)-1)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Parse form data
    char broker[128] = "";
    
    if (strncmp(buf, "broker=", 7) == 0) {
        urldecode(broker, buf + 7, sizeof(broker));
    }
    
    // Save to configuration
    if (strlen(broker) > 0) {
        strncpy(g_config.mqtt_broker_url, broker, sizeof(g_config.mqtt_broker_url)-1);
        g_config.mqtt_broker_url[sizeof(g_config.mqtt_broker_url)-1] = '\0';
        
        esp_err_t err = save_config_to_nvs();
        if (err == ESP_OK) {
            httpd_resp_send(req, "MQTT settings saved. Device will reconnect to the new broker...", -1);
            xEventGroupSetBits(event_group, RECONFIG_TRIGGER_BIT);
        } else {
            httpd_resp_send(req, "Error: Failed to save MQTT settings", -1);
        }
    } else {
        httpd_resp_send(req, "Invalid broker URL", -1);
    }
    
    return ESP_OK;
}

// Handler for threshold configuration
static esp_err_t thresholds_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret;
    
    if ((ret = httpd_req_recv(req, buf, sizeof(buf)-1)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Parse thresholds
    for (int i = 0; i < TOTAL_ZONE; i++) {
        char low_str[8] = "0";
        char high_str[8] = "1023";
        
        // Find low value
        char search[16];
        snprintf(search, sizeof(search), "low%d=", i);
        char *pos = strstr(buf, search);
        if (pos) {
            char *end = strchr(pos + strlen(search), '&');
            if (end) {
                strncpy(low_str, pos + strlen(search), end - (pos + strlen(search)));
                low_str[end - (pos + strlen(search))] = '\0';
            } else {
                strcpy(low_str, pos + strlen(search));
            }
            urldecode(low_str, low_str, sizeof(low_str));
        }
        
        // Find high value
        snprintf(search, sizeof(search), "high%d=", i);
        pos = strstr(buf, search);
        if (pos) {
            char *end = strchr(pos + strlen(search), '&');
            if (end) {
                strncpy(high_str, pos + strlen(search), end - (pos + strlen(search)));
                high_str[end - (pos + strlen(search))] = '\0';
            } else {
                strcpy(high_str, pos + strlen(search));
            }
            urldecode(high_str, high_str, sizeof(high_str));
        }
        
        // Update thresholds
        g_config.zone_lower[i] = atoi(low_str);
        g_config.zone_upper[i] = atoi(high_str);
        
        if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            zone_lower_limit[i] = g_config.zone_lower[i];
            zone_upper_limit[i] = g_config.zone_upper[i];
            xSemaphoreGive(data_mutex);
        }
    }
    
    save_config_to_nvs();
    httpd_resp_send(req, "Thresholds saved successfully.", -1);
    
    return ESP_OK;
}

// Handler for interval configuration
static esp_err_t interval_post_handler(httpd_req_t *req)
{
    char buf[64];
    int ret;
    
    if ((ret = httpd_req_recv(req, buf, sizeof(buf)-1)) <= 0) {
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    
    // Parse interval
    if (strncmp(buf, "interval=", 9) == 0) {
        char interval_str[16];
        urldecode(interval_str, buf + 9, sizeof(interval_str));
        uint32_t interval = atoi(interval_str);
        
        if (interval >= 1000 && interval <= 30000) {
            g_config.publish_interval_ms = interval;
            save_config_to_nvs();
            httpd_resp_send(req, "Publish interval updated.", -1);
        } else {
            httpd_resp_send(req, "Invalid interval (1000-30000 ms)", -1);
        }
    }
    
    return ESP_OK;
}

// Handler for command execution
static esp_err_t command_get_handler(httpd_req_t *req)
{
    char cmd[64] = {0};
    
    // Get command parameter
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char* buf = malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            httpd_query_key_value(buf, "cmd", cmd, sizeof(cmd)-1);
        }
        free(buf);
    }
    
    if (strlen(cmd) > 0) {
        // Handle command
        if (strcmp(cmd, "HOTSPOT") == 0) {
            ESP_LOGI(TAG, "Hotspot refresh command received");
            // Hotspot is always active, just log
            httpd_resp_send(req, "Hotspot is always active", -1);
        } else if (strcmp(cmd, "OTA") == 0) {
            ESP_LOGI(TAG, "OTA command received via web interface");
            xEventGroupSetBits(event_group, OTA_TRIGGER_BIT);
            httpd_resp_send(req, "OTA update triggered", -1);
        } else if (strcmp(cmd, "RESET") == 0) {
            ESP_LOGI(TAG, "Reset command received, restarting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        } else if (strcmp(cmd, "CLEAR") == 0) {
            ESP_LOGI(TAG, "Clear alerts command received");
            if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                for (int i = 0; i < TOTAL_ZONE; i++) {
                    zone_alert_state[i] = 0;
                }
                alert_flg = 0;
                prev_alert_flg = 0;
                xSemaphoreGive(data_mutex);
            }
            httpd_resp_send(req, "Alerts cleared", -1);
        } else if (strcmp(cmd, "GET_CONFIG") == 0) {
            ESP_LOGI(TAG, "Get config command received via web interface");
            char config_json[1024];
            snprintf(config_json, sizeof(config_json),
                "{\"wifi_ssid\":\"%s\","
                "\"wifi_password\":\"%s\","
                "\"mqtt_broker\":\"%s\","
                "\"publish_interval\":%lu}",
                g_config.wifi_ssid,
                g_config.wifi_password,
                g_config.mqtt_broker_url,
                g_config.publish_interval_ms);
            
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, config_json, strlen(config_json));
        } else {
            httpd_resp_send(req, "Unknown command", -1);
        }
    } else {
        httpd_resp_send(req, "No command specified", -1);
    }
    
    return ESP_OK;
}

/* HTML for web interface - UPDATED with better config display */
static const char* config_html = 
"<!DOCTYPE html>"
"<html><head><title>Zigron Device</title>"
"<meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"body{font-family:Arial;margin:10px;background:#f5f5f5;}"
".container{max-width:600px;margin:auto;background:white;padding:15px;border-radius:5px;}"
"h1{color:#333;font-size:20px;}"
"h2{color:#555;margin-top:20px;font-size:16px;}"
".section{margin:15px 0;padding:10px;border-left:3px solid #4CAF50;background:#f9f9f9;}"
"input{width:100%;padding:8px;margin:5px 0 10px;border:1px solid #ddd;border-radius:3px;box-sizing:border-box;}"
"button{background:#4CAF50;color:white;padding:10px 15px;border:none;border-radius:3px;cursor:pointer;margin:5px;}"
"button:hover{background:#45a049;}"
".zone-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:5px;margin-bottom:10px;}"
".zone-item{padding:5px;border:1px solid #ddd;border-radius:3px;font-size:12px;}"
".alert-low{background:#fff3cd;}"
".alert-high{background:#f8d7da;}"
".alert-normal{background:#d4edda;}"
".form-section{margin-bottom:15px;}"
".status-connected{color:green;}"
".status-disconnected{color:red;}"
".status-box{margin:5px 0;padding:5px;border-radius:3px;}"
".wifi-status{background:#e3f2fd;}"
".mqtt-status{background:#f3e5f5;}"
".gsm-status{background:#e8f5e8;}"
".config-info{background:#fff3cd;padding:10px;border-radius:5px;margin:10px 0;}"
"</style></head>"
"<body>"
"<div class='container'>"
"<h1>Zigron Device (MAC: %s)</h1>"
"<div class='section'>"
"<h2>Device Status</h2>"
"<div id='device-status'>"
"<p>Device Mode: <span id='mode-status'>%s</span></p>"
"<div class='status-box wifi-status'>WiFi Status: <span id='wifi-status' class='%s'>%s</span></div>"
"<div class='status-box gsm-status'>GSM Status: <span id='gsm-status' class='%s'>%s</span></div>"
"<div class='status-box mqtt-status'>MQTT Status: <span id='mqtt-status' class='%s'>%s</span></div>"
"<p>Hotspot IP: <span id='ip-address'>%s</span></p>"
"<p>Current Connection: <span id='current-conn'>%s</span></p>"
"</div>"
"<div class='config-info'>"
"<p><strong>Current Configuration:</strong></p>"
"<p>WiFi SSID: <span id='config-wifi-ssid'>%s</span></p>"
"<p>MQTT Broker: <span id='config-mqtt-broker'>%s</span></p>"
"</div>"
"</div>"
"<div class='section'>"
"<h2>Live Sensor Data</h2>"
"<div id='sensor-data' class='zone-grid'>Loading...</div>"
"</div>"
"<div class='section'>"
"<h2>Configuration</h2>"
"<div class='form-section'>"
"<h3>Wi-Fi Settings</h3>"
"<form action='/save-wifi' method='post'>"
"<input type='text' name='ssid' placeholder='WiFi SSID' value='%s'>"
"<input type='password' name='password' placeholder='WiFi Password' value='%s'>"
"<button type='submit'>Save WiFi Settings</button>"
"</form>"
"<p><small><i>Note: After saving WiFi settings, device will attempt to connect to the network. Hotspot remains active.</i></small></p>"
"</div>"
"<div class='form-section'>"
"<h3>MQTT Settings</h3>"
"<form action='/save-mqtt' method='post'>"
"<input type='text' name='broker' placeholder='Broker URL (e.g., mqtt://broker.example.com)' value='%s'>"
"<button type='submit'>Save MQTT</button>"
"</form>"
"<p><small><i>Note: You can also update configuration via MQTT topic: /ZIGRON/%s/CONFIG</i></small></p>"
"</div>"
"<div class='form-section'>"
"<h3>Thresholds</h3>"
"<form action='/save-thresholds' method='post'>"
"<div id='threshold-inputs' class='zone-grid'>%s</div>"
"<button type='submit'>Save All Thresholds</button>"
"</form>"
"</div>"
"<div class='form-section'>"
"<h3>Publish Interval (ms)</h3>"
"<form action='/save-interval' method='post'>"
"<input type='number' name='interval' value='%d' min='1000' max='30000'>"
"<button type='submit'>Save Interval</button>"
"</form>"
"</div>"
"</div>"
"<div class='section'>"
"<h2>Device Control</h2>"
"<button onclick='sendCmd(\"OTA\")'>OTA Update</button>"
"<button onclick='sendCmd(\"RESET\")'>Restart Device</button>"
"<button onclick='sendCmd(\"CLEAR\")'>Clear Alerts</button>"
"<button onclick='sendCmd(\"GET_CONFIG\")'>Get Config</button>"
"<button onclick='window.location.reload()'>Refresh Page</button>"
"</div>"
"<div class='section'>"
"<h2>Hotspot Information</h2>"
"<p><strong>Hotspot SSID:</strong> %s</p>"
"<p><strong>Hotspot Password:</strong> %s</p>"
"<p><strong>Hotspot IP:</strong> %s</p>"
"<p><small><i>Hotspot is ALWAYS active. Connect to configure device anytime.</i></small></p>"
"</div>"
"<div class='section'>"
"<h2>MQTT Configuration</h2>"
"<p><strong>Configuration Topic:</strong> /ZIGRON/%s/CONFIG</p>"
"<p><strong>Configuration JSON Format:</strong></p>"
"<pre style='background:#f0f0f0;padding:10px;border-radius:5px;font-size:12px;'>"
"{\n"
"  \"wifi_ssid\": \"YourSSID\",\n"
"  \"wifi_password\": \"YourPassword\",\n"
"  \"mqtt_broker\": \"mqtt://broker.example.com\",\n"
"  \"publish_interval\": 5000,\n"
"  \"thresholds\": [\n"
"    {\"zone\": 0, \"low\": 0, \"high\": 900},\n"
"    {\"zone\": 1, \"low\": 0, \"high\": 900}\n"
"  ]\n"
"}"
"</pre>"
"<p><strong>Get Configuration:</strong> Send \"GET_CONFIG\" to /ZIGRON/%s/COMMAND topic</p>"
"</div>"
"</div>"
"<script>"
"function sendCmd(cmd){"
"fetch('/command?cmd='+cmd).then(r=>r.text()).then(data=>{"
"if(cmd === 'GET_CONFIG'){"
"try{"
"let config = JSON.parse(data);"
"alert('Current Configuration:\\n' + "
"'WiFi SSID: ' + config.wifi_ssid + '\\n' + "
"'MQTT Broker: ' + config.mqtt_broker + '\\n' + "
"'Publish Interval: ' + config.publish_interval + 'ms');"
"}catch(e){"
"alert('Response: ' + data);"
"}"
"}else{"
"alert('Command Sent: ' + cmd + '\\nResponse: ' + data);"
"}"
"});"
"}"
"function updateStatus(){"
"fetch('/status').then(r=>r.json()).then(data=>{"
"document.getElementById('mode-status').textContent = data.mode;"
"document.getElementById('wifi-status').textContent = data.wifi;"
"document.getElementById('gsm-status').textContent = data.gsm;"
"document.getElementById('mqtt-status').textContent = data.mqtt;"
"document.getElementById('ip-address').textContent = data.ip;"
"document.getElementById('config-wifi-ssid').textContent = data.wifi_ssid;"
"document.getElementById('config-mqtt-broker').textContent = data.mqtt_broker;"
"document.getElementById('wifi-status').className = data.wifi === 'Connected' ? 'status-connected' : 'status-disconnected';"
"document.getElementById('gsm-status').className = data.gsm === 'Connected' ? 'status-connected' : 'status-disconnected';"
"document.getElementById('mqtt-status').className = data.mqtt === 'Connected' ? 'status-connected' : 'status-disconnected';"
"let currentConn = 'None';"
"if(data.wifi === 'Connected') currentConn = 'WiFi';"
"else if(data.gsm === 'Connected') currentConn = 'GSM';"
"document.getElementById('current-conn').textContent = currentConn;"
"});"
"}"
"function updateSensorData(){"
"fetch('/sensor-data').then(r=>r.json()).then(data=>{"
"let html='';"
"for(let i=0;i<data.RAW.length;i++){"
"let cls='alert-normal';"
"if(data.ALERT[i]==1)cls='alert-low';"
"if(data.ALERT[i]==2)cls='alert-high';"
"html+='<div class=\"zone-item '+cls+'\">Zone '+i+': '+data.RAW[i]+'</div>';"
"}"
"document.getElementById('sensor-data').innerHTML=html;"
"});"
"}"
"setInterval(updateSensorData,2000);"
"setInterval(updateStatus,5000);"
"updateSensorData();"
"updateStatus();"
"</script>"
"</body></html>";

// Handler for root page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET request received for root page");
    
    char zone_inputs[2000] = "";
    
    // Generate zone threshold inputs
    for (int i = 0; i < TOTAL_ZONE; i++) {
        char zone_html[300];
        snprintf(zone_html, sizeof(zone_html),
            "<div>"
            "<small>Zone %d:</small><br>"
            "<input type='number' name='low%d' value='%d' style='width:48%%;' placeholder='Low'>"
            "<input type='number' name='high%d' value='%d' style='width:48%%;' placeholder='High'>"
            "</div>",
            i, i, g_config.zone_lower[i], i, g_config.zone_upper[i]);
        strcat(zone_inputs, zone_html);
    }
    
    // Determine status strings
    const char* device_mode = "AP+STA Mode";
    const char* wifi_status = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* wifi_class = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    const char* gsm_status = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* gsm_class = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    const char* mqtt_status = ((xEventGroupGetBits(event_group) & MQTT_CONNECTED_BIT) != 0) ? "Connected" : "Disconnected";
    const char* mqtt_class = ((xEventGroupGetBits(event_group) & MQTT_CONNECTED_BIT) != 0) ? "status-connected" : "status-disconnected";
    
    char ip_address[16] = "Not Available";
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    
    if (netif) {
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(netif, &ip_info);
        snprintf(ip_address, sizeof(ip_address), IPSTR, IP2STR(&ip_info.ip));
    }
    
    // Get hotspot IP
    char hotspot_ip[16] = "192.168.4.1";
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_ip_info_t ap_ip_info;
        esp_netif_get_ip_info(ap_netif, &ap_ip_info);
        snprintf(hotspot_ip, sizeof(hotspot_ip), IPSTR, IP2STR(&ap_ip_info.ip));
    }
    
    // Determine current connection
    const char* current_conn = "None";
    if ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) {
        current_conn = "WiFi";
    } else if ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0) {
        current_conn = "GSM";
    }
    
    // Response buffer
    char response[5000];
    snprintf(response, sizeof(response), config_html,
             mac_string,
             device_mode,
             wifi_class, wifi_status,
             gsm_class, gsm_status,
             mqtt_class, mqtt_status,
             ip_address,
             current_conn,
             g_config.wifi_ssid,
             g_config.mqtt_broker_url,
             g_config.wifi_ssid,
             g_config.wifi_password,
             g_config.mqtt_broker_url,
             mac_string,
             zone_inputs,
             g_config.publish_interval_ms,
             AP_SSID, AP_PASSWORD, hotspot_ip,
             mac_string, mac_string);
    
    ESP_LOGI(TAG, "Sending HTML response (%d bytes)", strlen(response));
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Start HTTP server
static void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;
    config.stack_size = 12288;
    
    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);
        
        httpd_uri_t sensor = {
            .uri = "/sensor-data",
            .method = HTTP_GET,
            .handler = sensor_data_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &sensor);
        
        httpd_uri_t status = {
            .uri = "/status",
            .method = HTTP_GET,
            .handler = status_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status);
        
        httpd_uri_t wifi = {
            .uri = "/save-wifi",
            .method = HTTP_POST,
            .handler = wifi_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi);
        
        httpd_uri_t mqtt = {
            .uri = "/save-mqtt",
            .method = HTTP_POST,
            .handler = mqtt_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &mqtt);
        
        httpd_uri_t thresholds = {
            .uri = "/save-thresholds",
            .method = HTTP_POST,
            .handler = thresholds_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &thresholds);
        
        httpd_uri_t interval = {
            .uri = "/save-interval",
            .method = HTTP_POST,
            .handler = interval_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &interval);
        
        httpd_uri_t cmd = {
            .uri = "/command",
            .method = HTTP_GET,
            .handler = command_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &cmd);
        
        ESP_LOGI(TAG, "HTTP server started successfully");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

/* Web Server Task */
static void webserver_task(void *arg)
{
    ESP_LOGI(TAG, "Web server task started");
    
    // Wait a bit for network initialization
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // Start HTTP server
    start_webserver();
    
    // Keep task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* Connectivity Manager Task */
static void connectivity_manager_task(void *arg)
{
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    static bool gsm_activated = false;
    static uint32_t last_gsm_activation_attempt = 0;
    
    ESP_LOGI(TAG, "Connectivity Manager task started");
    
    while (1) {
        bool wifi_connected = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0);
        bool gsm_connected = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0);
        uint32_t current_time = esp_timer_get_time() / 1000000;
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // If WiFi is not connected AND GSM is not connected
            if (!wifi_connected && !gsm_connected) {
                // Check if enough time has passed since last GSM attempt
                if (current_time - last_gsm_activation_attempt > 60) { // Wait 60 seconds between attempts
                    ESP_LOGI(TAG, "No WiFi connection - activating GSM PPP connection");
                    
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
        
        vTaskDelay(pdMS_TO_TICKS(10000)); // Check every 10 seconds
    }
}

/* MQTT Task */
static void mqtt_task(void *arg)
{
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    ESP_LOGI(TAG, "MQTT task started");

    esp_mqtt_client_handle_t mqtt_client = NULL;
    bool mqtt_started           = false;
    // uint32_t last_mqtt_publish  = 0;
    uint32_t last_connection_check = 0;

    while (1) {
        bool network_connected = ((xEventGroupGetBits(event_group) & 
                                 (WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT)) != 0);
        
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
                .broker.address.uri         = g_config.mqtt_broker_url,
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
            if ( (loop_counter >= PACKET_TIMEOUT) || prev_alert_flg != alert_flg ) {
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
                    // last_mqtt_publish = esp_timer_get_time() / 1000000;
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

/* app_main */
void app_main(void)
{
    esp_log_level_set("esp_http_client", ESP_LOG_DEBUG);
    esp_log_level_set("esp_https_ota",   ESP_LOG_DEBUG);
    esp_log_level_set("wifi",            ESP_LOG_WARN);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    // Load configuration from NVS
    ret = load_config_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Using default configuration");
        // Save defaults to NVS
        memcpy(g_config.zone_lower, zone_lower_limit, sizeof(zone_lower_limit));
        memcpy(g_config.zone_upper, zone_upper_limit, sizeof(zone_upper_limit));
        save_config_to_nvs();
    }

    // Initialize MCP3002
    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO,
            CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);
    
    // Get MAC address
    esp_read_mac(mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string, "%02X%02X%02X%02X%02X%02X",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC address %s", mac_string);

    // Create synchronization primitives
    event_group = xEventGroupCreate();
    data_mutex  = xSemaphoreCreateMutex();
    conn_mutex  = xSemaphoreCreateMutex();

    // Initialize network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    // Initialize WiFi in AP+STA mode for permanent hotspot
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

    // Temporarily disable watchdog for initialization
    ESP_ERROR_CHECK(esp_task_wdt_deinit());
    ESP_LOGI(TAG, "Watchdog temporarily disabled for initialization");

    // Setup GPIOs
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)39, GPIO_MODE_INPUT); // Digital input pin

    // Reset modem
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize GSM modem
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_config, &dce_config, esp_netif);
    ESP_LOGI(TAG, "Waiting for GSM modem to boot (15 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    assert(dce);

    // Clear all event bits
    xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT | MQTT_CONNECTED_BIT | OTA_TRIGGER_BIT | RECONFIG_TRIGGER_BIT);

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
        ESP_LOGW(TAG, "GSM network registration failed - will use as fallback only");
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
    }

    /* Start tasks */
    BaseType_t xRet;

    // Start web server task
    xRet = xTaskCreate(webserver_task, "webserver_task", 12288, NULL, 4, &webserver_task_handle);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create webserver task");
    }

    // Start WiFi connection task
    xRet = xTaskCreate(wifi_connection_task, "wifi_conn_task", 12288, NULL, 4, &wifi_task_handle);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi connection task");
    }

    // Start connectivity manager
    xRet = xTaskCreate(connectivity_manager_task, "conn_mgr", 8192, dce, 4, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connectivity manager task");
    }

    // Start other tasks with delay to allow connectivity to establish
    vTaskDelay(pdMS_TO_TICKS(2000));

    xRet = xTaskCreate(sensor_task, "sensor_task", 8192, NULL, 4, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sensor task");
    }

    xRet = xTaskCreate(mqtt_task, "mqtt_task", 12288, dce, 5, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create MQTT task");
    }

    xRet = xTaskCreate(ota_task, "ota_task", 12288, NULL, 5, NULL);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create OTA task");
    }

    ESP_LOGI(TAG, "System fully started with MQTT configuration support");
    ESP_LOGI(TAG, "Hotspot is ALWAYS active at SSID: %s, Password: %s", AP_SSID, AP_PASSWORD);
    ESP_LOGI(TAG, "Connect to hotspot anytime at: http://192.168.4.1 to configure device");
    ESP_LOGI(TAG, "Configuration can also be updated via MQTT topic: /ZIGRON/%s/CONFIG", mac_string);
}