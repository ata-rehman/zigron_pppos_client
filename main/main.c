/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* WiFi-first MQTT + OTA with GSM/PPP as backup
   Web server and hotspot functionality removed
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
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
// WiFi headers for ESP-IDF v5.5.1
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

#define PACKET_TIMEOUT      300          // 30 seconds
#define FW_VER              "0.10"      // Updated version with fixes
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
#define WIFI_CONNECT_TIMEOUT_MS 30000   // 30 seconds WiFi timeout
#define MAX_WIFI_RETRIES   3
#define WIFI_STATE_CHECK_DELAY_MS 500   // Delay between WiFi state checks
#define GSM_RECOVERY_DELAY_MS 60000     // 60 seconds between GSM recovery attempts
#define MIN_GSM_SIGNAL_THRESHOLD -105   // Minimum acceptable RSSI for GSM

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
static const int WIFI_RECONNECT_BIT   = BIT5;   // WiFi reconnection requested

/* Connectivity mode */
typedef enum {
    CONN_MODE_NONE = 0,
    CONN_MODE_WIFI,
    CONN_MODE_GSM,
    CONN_MODE_MAX
} conn_mode_t;

static conn_mode_t current_conn_mode = CONN_MODE_NONE;

/* WiFi state machine */
typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_RECONNECTING
} wifi_state_t;

static wifi_state_t wifi_state = WIFI_STATE_IDLE;
static SemaphoreHandle_t wifi_state_mutex = NULL;

static bool sim_select_flag = 1;

/* Shared data */
MCP_t   dev;
uint8_t mac_addr[6] = {0};
char    mac_string[13] = "0123456789AB";

char data_buff[512];
char topic_buff[128];

static uint8_t  zone_alert_state[TOTAL_ZONE];
static uint16_t zone_raw_value[TOTAL_ZONE];

static uint16_t zone_lower_limit[TOTAL_ZONE] = {0,0,0,0,0,0,0,0,0,0};
static uint16_t zone_upper_limit[TOTAL_ZONE] = {2000,2000,2000,2000,2000,2000,2000,2000,0,0};

static uint16_t alert_flg       = 0;
static uint16_t prev_alert_flg  = 0;
static uint16_t loop_counter    = 0;

/* PPP recovery state */
static int      ppp_fail_count  = 0;
static uint32_t last_gsm_recovery_time = 0;

/* WiFi connection attempts */
static int      wifi_retry_count = 0;
static uint32_t last_wifi_connect_time = 0;

uint8_t ota_state = 0;  

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
    .wifi_ssid = "Piffers Control Room",
    .wifi_password = "Pak@12345",
    .mqtt_broker_url = "mqtt://zigron:zigron123@54.194.219.149:45055",
    .publish_interval_ms = 5000,
    .enable_ota = true,
};

// Task handles
static TaskHandle_t wifi_task_handle = NULL;

esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_started           = false;

/* Forward declarations */
static void init_wifi(void);
static bool connect_wifi_safe(void);
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

/* Safe WiFi configuration application with state checking */
static esp_err_t safe_wifi_set_config(wifi_config_t *wifi_config)
{
    esp_err_t ret;
    wifi_mode_t mode;
    wifi_state_t current_state;
    
    if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    current_state = wifi_state;
    xSemaphoreGive(wifi_state_mutex);
    
    // Check current WiFi state
    ret = esp_wifi_get_mode(&mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // If WiFi is connecting, wait for it to finish or timeout
    if (current_state == WIFI_STATE_CONNECTING) {
        ESP_LOGW(TAG, "WiFi is in connecting state, waiting...");
        
        EventBits_t bits = xEventGroupWaitBits(event_group, 
                                              WIFI_CONNECTED_BIT,
                                              pdFALSE, pdFALSE,
                                              pdMS_TO_TICKS(10000));
        
        if (!(bits & WIFI_CONNECTED_BIT)) {
            // Force disconnect if connection timeout
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    // Now safe to set config
    ret = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

/* Apply WiFi configuration and reconnect safely */
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
    
    strncpy((char*)wifi_config_sta.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config_sta.sta.ssid) - 1);
    strncpy((char*)wifi_config_sta.sta.password, g_config.wifi_password, sizeof(wifi_config_sta.sta.password) - 1);
    
    if (safe_wifi_set_config(&wifi_config_sta) == ESP_OK) {
        // Signal reconnection
        xEventGroupSetBits(event_group, WIFI_RECONNECT_BIT);
    }
}

/* Handle MQTT configuration command */
static void handle_mqtt_config_command(const char *payload, int payload_len)
{
    char payload_copy[1024];
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
                    cJSON *low = cJSON_GetObjectItem(threshold, "l");
                    cJSON *high = cJSON_GetObjectItem(threshold, "h");
                    
                    if (cJSON_IsNumber(low) && cJSON_IsNumber(high)) {
                        uint16_t new_low = low->valueint;
                        uint16_t new_high = high->valueint;
                        
                        if (new_low != g_config.zone_lower[i] || new_high != g_config.zone_upper[i]) {
                            g_config.zone_lower[i] = new_low;
                            g_config.zone_upper[i] = new_high;
                            config_changed = true;                           
                            zone_lower_limit[i] = new_low;
                            zone_upper_limit[i] = new_high;
                            
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

/* WiFi Functions - Station mode only (no AP) */
static void init_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi in station mode only...");
    
    esp_netif_create_default_wifi_sta();
    // No AP netif created
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Set WiFi to station mode only
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));
    
    ESP_LOGI(TAG, "WiFi initialization complete in station mode");
}

/* Safe WiFi connection function with proper state management */
static bool connect_wifi_safe(void)
{
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", g_config.wifi_ssid);
    
    if (strlen(g_config.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID not configured");
        return false;
    }
    
    // Update state
    if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        wifi_state = WIFI_STATE_CONNECTING;
        xSemaphoreGive(wifi_state_mutex);
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
    
    strncpy((char*)wifi_config_sta.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config_sta.sta.ssid) - 1);
    strncpy((char*)wifi_config_sta.sta.password, g_config.wifi_password, sizeof(wifi_config_sta.sta.password) - 1);
    
    // Disconnect if already connected or connecting
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Set configuration
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            wifi_state = WIFI_STATE_IDLE;
            xSemaphoreGive(wifi_state_mutex);
        }
        return false;
    }
    
    // Start WiFi if not already started
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            wifi_state = WIFI_STATE_IDLE;
            xSemaphoreGive(wifi_state_mutex);
        }
        return false;
    }
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initiate WiFi connection: %s", esp_err_to_name(ret));
        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            wifi_state = WIFI_STATE_IDLE;
            xSemaphoreGive(wifi_state_mutex);
        }
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
        last_wifi_connect_time = esp_timer_get_time() / 1000000;
        
        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            wifi_state = WIFI_STATE_CONNECTED;
            xSemaphoreGive(wifi_state_mutex);
        }
        return true;
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout");
        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            wifi_state = WIFI_STATE_DISCONNECTED;
            xSemaphoreGive(wifi_state_mutex);
        }
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
                {
                    wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t*) event_data;
                    ESP_LOGW(TAG, "WiFi station disconnected, reason: %d", disconnected->reason);
                    
                    xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT);
                    
                    if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        wifi_state = WIFI_STATE_DISCONNECTED;
                        xSemaphoreGive(wifi_state_mutex);
                    }
                    
                    if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                        if (current_conn_mode == CONN_MODE_WIFI) {
                            ESP_LOGI(TAG, "WiFi disconnected, will attempt GSM fallback");
                        }
                        xSemaphoreGive(conn_mutex);
                    }
                    
                    // Only retry if we haven't exceeded max retries
                    if (wifi_retry_count < MAX_WIFI_RETRIES) {
                        wifi_retry_count++;
                        ESP_LOGI(TAG, "WiFi reconnect attempt %d/%d", wifi_retry_count, MAX_WIFI_RETRIES);
                        
                        // Add delay before reconnecting
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        
                        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            wifi_state = WIFI_STATE_RECONNECTING;
                            xSemaphoreGive(wifi_state_mutex);
                        }
                        
                        esp_wifi_connect();
                    } else {
                        ESP_LOGW(TAG, "Max WiFi retries reached, staying disconnected");
                        if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            wifi_state = WIFI_STATE_IDLE;
                            xSemaphoreGive(wifi_state_mutex);
                        }
                    }
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
            
            if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                wifi_state = WIFI_STATE_CONNECTED;
                xSemaphoreGive(wifi_state_mutex);
            }
            
            if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                current_conn_mode = CONN_MODE_WIFI;
                xSemaphoreGive(conn_mutex);
            }
            
            xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT);
            
            // Initialize SNTP for WiFi connection
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            esp_sntp_init();
            
            // Wait for time synchronization
            time_t now = 0;
            struct tm timeinfo = {0};
            int retry = 0;
            
            while (timeinfo.tm_year < (2020 - 1900) && ++retry < 10) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                time(&now);
                localtime_r(&now, &timeinfo);
            }
            
            if (timeinfo.tm_year > (2020 - 1900)) {
                char strftime_buf[64];
                strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
                ESP_LOGI(TAG, "Time synchronized via WiFi: %s", strftime_buf);
            } else {
                ESP_LOGW(TAG, "Time synchronization via WiFi failed");
            }
        }
    }
}

/* WiFi Connection Task - No hotspot */
static void wifi_connection_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi connection task started");
    
    // Main WiFi connection loop
    while (1) {
        // Check if reconfiguration was requested
        EventBits_t bits = xEventGroupGetBits(event_group);
        if (bits & RECONFIG_TRIGGER_BIT) {
            xEventGroupClearBits(event_group, RECONFIG_TRIGGER_BIT);
            ESP_LOGI(TAG, "Reconfiguration triggered, re-evaluating WiFi connection");
        }
        
        // Check if reconnection was requested
        if (bits & WIFI_RECONNECT_BIT) {
            xEventGroupClearBits(event_group, WIFI_RECONNECT_BIT);
            ESP_LOGI(TAG, "Reconnection requested");
            
            // Force disconnect and reconnect
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        
        // Check if WiFi credentials are configured
        if (strlen(g_config.wifi_ssid) == 0) {
            ESP_LOGW(TAG, "WiFi SSID not configured. Waiting for configuration via MQTT...");
            
            // Wait for 30 seconds before checking again
            for (int i = 0; i < 30; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (strlen(g_config.wifi_ssid) > 0) {
                    break;  // SSID configured, break out of wait loop
                }
            }
            continue;
        }
        
        // Check if we're already connected
        if ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) {
            // Already connected, just monitor
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        
        // Check if we're in cooldown period after failed connection
        uint32_t current_time = esp_timer_get_time() / 1000000;
        if (current_time - last_wifi_connect_time < 60 && wifi_retry_count >= MAX_WIFI_RETRIES) {
            // In cooldown, wait
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        
        ESP_LOGI(TAG, "Attempting to connect to WiFi: %s", g_config.wifi_ssid);
        
        // Try to connect to WiFi
        if (connect_wifi_safe()) {
            ESP_LOGI(TAG, "Successfully connected to WiFi: %s", g_config.wifi_ssid);
            
            // Monitor connection while connected
            while ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0) {
                vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
                
                // Check connection status
                wifi_ap_record_t ap_info;
                if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi connection lost");
                    break;
                }
            }
            
        } else {
            ESP_LOGW(TAG, "Failed to connect to WiFi: %s", g_config.wifi_ssid);
            
            last_wifi_connect_time = current_time;
            
            // Exponential backoff for retries
            int wait_time = 60; // seconds
            if (wifi_retry_count < MAX_WIFI_RETRIES) {
                wait_time = 30 * (wifi_retry_count + 1);
            }
            
            ESP_LOGI(TAG, "Will retry WiFi connection in %d seconds...", wait_time);
            
            for (int i = 0; i < wait_time; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
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
                zone_alert_state[TOTAL_ZONE-2] |= 0x01;
            } else {
                alert_flg &= ~0x0100;
                zone_alert_state[TOTAL_ZONE-2] &= ~0x01;
            }

            /* ARM/DisARM status vs stored zone value */
            if (zone_raw_value[TOTAL_ZONE-1] != gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_ARM_STATUS_PIN)) {
                alert_flg |= 0x0200;
                zone_alert_state[TOTAL_ZONE-1] |= 0x01;
            } else {
                alert_flg &= ~0x0200;
                zone_alert_state[TOTAL_ZONE-1] &= ~0x01;
            }

            /* Digital inputs */
            zone_raw_value[TOTAL_ZONE-1] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_ARM_STATUS_PIN);
            zone_raw_value[TOTAL_ZONE-2] = gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);

            /* Analog zones */
            for (uint8_t i = 0; i < (TOTAL_ZONE-2); i++) {
                zone_raw_value[i] = mcpReadData(&dev, i);

                uint16_t bitmask = 1 << i;
                
                // Check if we need to set an alert
                if (zone_raw_value[i] < zone_lower_limit[i]) {
                    zone_alert_state[i] |= 0x01;  // Low alert
                    alert_flg |= bitmask;
                } else if (zone_raw_value[i] > zone_upper_limit[i]) {
                    zone_alert_state[i] |= 0x02;  // High alert
                    alert_flg |= bitmask;
                } else {
                    // Value is within range, clear the alert flag
                    alert_flg &= ~bitmask;
                    // zone_alert_state[i] = 0;  // Clear alert state
                }
            }
            xSemaphoreGive(data_mutex);
            
            if( ((loop_counter >= PACKET_TIMEOUT) || (prev_alert_flg != alert_flg)) ) {
                time_t now;
                time(&now);
                topic_buff[0] = 0;
                snprintf(data_buff, sizeof(data_buff),
                "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"DNA\":[\"%s\",%lld],"
                "\"CONN\":\"%s%d\","
                "\"FW\":\"%s\","
                "\"OTA_STATE\":%d}",
                zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3],
                zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
                zone_raw_value[8], zone_raw_value[9],
                zone_alert_state[0], zone_alert_state[1], zone_alert_state[2], zone_alert_state[3],
                zone_alert_state[4], zone_alert_state[5], zone_alert_state[6], zone_alert_state[7],
                zone_alert_state[8], zone_alert_state[9],
                mac_string, (long long)now,
                (current_conn_mode == CONN_MODE_WIFI) ? "WIFI" : "GSM",sim_select_flag,
                FW_VER, ota_state);

                if(loop_counter >= PACKET_TIMEOUT) 
                {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/HB", mac_string);
                }
                else
                {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/ALERT", mac_string);
                }

                if( (mqtt_client) && (mqtt_started) )
                {
                    int publish_response = esp_mqtt_client_publish(mqtt_client, topic_buff,
                                                               data_buff, 0, 0, 0);
                    if (publish_response < 0) {
                        ESP_LOGW(TAG, "MQTT publish failed: %d", publish_response);
                    }
                }
               
                ESP_LOGI(TAG, "%s %s, Alert flags:0x%03X 0x%03X, LC: %d", 
                         topic_buff, data_buff, alert_flg, prev_alert_flg, loop_counter);
                
                loop_counter = 0;
                prev_alert_flg = alert_flg;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));  // sensor period ~1s
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
            if (rssi > MIN_GSM_SIGNAL_THRESHOLD) {
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

    if(sim_select_flag == 0) sim_select_flag = 1;
    else sim_select_flag = 0;

    ESP_LOGI(TAG, "SIM flag %X", sim_select_flag);
    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, sim_select_flag);
    vTaskDelay(10);

    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);

    ESP_LOGI(TAG, "Waiting for GSM modem to fully reboot (15 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(15000));
}

static bool restart_ppp_connection(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Attempting to restart GSM PPP connection...");
    
    // Check if enough time has passed since last recovery
    uint32_t current_time = esp_timer_get_time() / 1000000;
    if (current_time - last_gsm_recovery_time < 60) {
        ESP_LOGW(TAG, "GSM recovery cooldown active, skipping");
        // return false;
    }
    
    perform_modem_reset(dce);

    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not switch to command mode, GSM modem may be unresponsive");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(5000));

    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSM modem not responsive after command mode switch");
        return false;
    }
    
    if (rssi <= MIN_GSM_SIGNAL_THRESHOLD) {
        ESP_LOGW(TAG, "GSM signal too weak: %d", rssi);
        return false;
    }
    
    ESP_LOGI(TAG, "GSM modem responsive, signal: rssi=%d, ber=%d", rssi, ber);

    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch to GSM data mode: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "GSM PPP restart initiated, waiting for connection...");
    last_gsm_recovery_time = current_time;
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
                mqtt_started = false;
                ESP_LOGI(TAG, "MQTT client stopped for GSM PPP recovery");
            }

            if (ppp_fail_count >= 2) {
                ESP_LOGE(TAG, "Multiple GSM PPP failures, performing modem reset.");
                perform_modem_reset(dce);
                ppp_fail_count = 0;
            } else {
                if (restart_ppp_connection(dce)) {
                    vTaskDelay(pdMS_TO_TICKS(45000));
                }
            }

            recovery_in_progress = false;
        } else if (gsm_connected && ppp_fail_count > 0) {
            ESP_LOGI(TAG, "GSM PPP connection restored, resetting fail count");
            ppp_fail_count = 0;
        }
    }
}

/* MQTT Event Handler */
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
                xSemaphoreGive(data_mutex);
                ESP_LOGI(TAG, "Alerts cleared via MQTT CLEAR");
            }
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
                    "{\"ssid\":\"%s\","
                    "\"broker\":\"%s\","
                    "\"intrvl\":%lu,"
                    "\"thresh\":[",
                    g_config.wifi_ssid,
                    g_config.mqtt_broker_url,
                    g_config.publish_interval_ms);
                
                // Add thresholds
                char thresholds_str[512] = "";
                for (int i = 0; i < TOTAL_ZONE; i++) {
                    char zone_str[64];
                    snprintf(zone_str, sizeof(zone_str), 
                        "{\"z\":%d,\"l\":%d,\"h\":%d}%s",
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

        // Initialize SNTP for ESP-IDF v5.5.1
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();

        time_t now = 0;
        struct tm timeinfo = {0};
        int retry = 0;

        // Wait for time to be set
        while (timeinfo.tm_year < (2020 - 1900) && ++retry < 10) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            time(&now);
            localtime_r(&now, &timeinfo);
        }
        
        // Log time synchronization status
        if (timeinfo.tm_year > (2020 - 1900)) {
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "Time synchronized: %s", strftime_buf);
        } else {
            ESP_LOGW(TAG, "Time synchronization failed or took too long");
        }

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
        ota_state = 1;    

        esp_err_t ret = esp_https_ota(&ota_config);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA success, restarting...");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA failed with error: %s", esp_err_to_name(ret));
            ota_state = 2; 
        }
    }
}

/* Connectivity Manager Task */
static void connectivity_manager_task(void *arg)
{
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    static bool gsm_activated = false;
    static uint32_t last_gsm_activation_attempt = 0;
    static uint32_t last_mode_switch = 0;
    const uint32_t MIN_SWITCH_INTERVAL = 60; // Minimum 60 seconds between mode switches
    
    ESP_LOGI(TAG, "Connectivity Manager task started");
    
    while (1) {
        bool wifi_connected = ((xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT) != 0);
        bool gsm_connected = ((xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) != 0);
        uint32_t current_time = esp_timer_get_time() / 1000000;
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // If WiFi is not connected AND GSM is not connected
            if (!wifi_connected && !gsm_connected) {
                // Check if enough time has passed since last GSM attempt
                if (current_time - last_gsm_activation_attempt > GSM_RECOVERY_DELAY_MS / 1000) {
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
                            if (restart_ppp_connection(dce)) {
                                vTaskDelay(pdMS_TO_TICKS(30000));
                            }
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to activate GSM data mode: %s", esp_err_to_name(ret));
                        if (restart_ppp_connection(dce)) {
                            vTaskDelay(pdMS_TO_TICKS(30000));
                        }
                    }
                    last_gsm_activation_attempt = current_time;
                }
            }
            // If WiFi connects and we're on GSM, switch back (but with minimum interval)
            else if (wifi_connected && current_conn_mode == CONN_MODE_GSM && 
                     (current_time - last_mode_switch > MIN_SWITCH_INTERVAL)) {
                ESP_LOGI(TAG, "WiFi reconnected, switching from GSM to WiFi");
                current_conn_mode = CONN_MODE_WIFI;
                last_mode_switch = current_time;
                
                // Switch GSM back to command mode to save power
                if (gsm_activated) {
                    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                    gsm_activated = false;
                }
            }
            // If GSM connects, update connection mode (but only if WiFi not connected)
            else if (gsm_connected && !wifi_connected && current_conn_mode != CONN_MODE_GSM) {
                current_conn_mode = CONN_MODE_GSM;
                last_mode_switch = current_time;
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
            if (!mqtt_client) {
                ESP_LOGE(TAG, "Failed to initialize MQTT client");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
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
                // Don't keep retrying immediately
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
        }

        // Status LED toggle
        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* app_main - No web server or hotspot */
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
    wifi_state_mutex = xSemaphoreCreateMutex();

    if (!event_group || !data_mutex || !conn_mutex || !wifi_state_mutex) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives");
        return;
    }

    // Initialize network stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Register IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    // Initialize WiFi in station mode only (no AP)
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

    // Setup GPIOs
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_ARM_STATUS_PIN, GPIO_MODE_INPUT);

    // Reset modem
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1); 
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    gpio_set_level( (gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, sim_select_flag);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize GSM modem
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_config, &dce_config, esp_netif);
    if (!dce) {
        ESP_LOGE(TAG, "Failed to create GSM modem device");
        // Continue without GSM - WiFi only mode
    } else {
        ESP_LOGI(TAG, "Waiting for GSM modem to boot (15 seconds)...");
        vTaskDelay(pdMS_TO_TICKS(15000));

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
    }

    // Clear all event bits
    xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT | 
                                    MQTT_CONNECTED_BIT | OTA_TRIGGER_BIT | 
                                    RECONFIG_TRIGGER_BIT | WIFI_RECONNECT_BIT);

    /* Start tasks */
    BaseType_t xRet;

    // Start WiFi connection task
    xRet = xTaskCreate(wifi_connection_task, "wifi_conn_task", 12288, NULL, 4, &wifi_task_handle);
    if (xRet != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi connection task");
    }

    // Start connectivity manager only if GSM is available
    if (dce) {
        xRet = xTaskCreate(connectivity_manager_task, "conn_mgr", 8192, dce, 4, NULL);
        if (xRet != pdPASS) {
            ESP_LOGE(TAG, "Failed to create connectivity manager task");
        }
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
    ESP_LOGI(TAG, "Configuration can be updated via MQTT topic: /ZIGRON/%s/CONFIG", mac_string);
}