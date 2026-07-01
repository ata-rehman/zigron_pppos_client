/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Optimized WiFi-first MQTT + OTA with GSM/PPP as backup
   Fast switching, reduced memory footprint, improved reliability
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
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_pm.h"
#include "esp_sleep.h"

// ============= CONFIGURATION =============
#define PACKET_TIMEOUT          600          // 60 seconds
#define ALERT_TIMEOUT           50           // 5 seconds
#define FW_VER                  "0.13"       // Updated version
#define EXAMPLE_FLOW_CONTROL    ESP_MODEM_FLOW_CONTROL_NONE

// Optimized timing parameters
#define WIFI_CONNECT_TIMEOUT_MS     15000   // Reduced from 30s to 15s
#define GSM_CONNECT_TIMEOUT_MS      20000   // GSM connection timeout
#define MAX_WIFI_RETRIES            2       // Reduced from 3
#define GSM_RECOVERY_DELAY_MS       10000   // Reduced from 60s to 10s
#define MIN_SWITCH_INTERVAL         5       // Reduced from 60s to 5s
#define NETWORK_CHECK_INTERVAL_MS   3000    // Check every 3s instead of 10s
#define MIN_GSM_SIGNAL_THRESHOLD    -105

#define NVS_CONFIG_NAMESPACE        "zigron_config"
#define TOTAL_ZONE                  10
#define MQTT_PUBLISH_RETRY_INTERVAL 5000    // 5s retry interval

static const char *TAG = "Zigron_Opt";

// ============= EVENT BITS =============
static EventGroupHandle_t event_group = NULL;
static const int WIFI_CONNECTED_BIT   = BIT0;
static const int GSM_CONNECTED_BIT    = BIT1;
static const int MQTT_CONNECTED_BIT   = BIT2;
static const int OTA_TRIGGER_BIT      = BIT3;
static const int RECONFIG_TRIGGER_BIT = BIT4;
static const int WIFI_RECONNECT_BIT   = BIT5;
static const int NETWORK_AVAILABLE_BIT = BIT6;  // New: any network available

// ============= STATE MANAGEMENT =============
typedef enum {
    CONN_MODE_NONE = 0,
    CONN_MODE_WIFI,
    CONN_MODE_GSM,
} conn_mode_t;

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
} wifi_state_t;

static conn_mode_t current_conn_mode = CONN_MODE_NONE;
static wifi_state_t wifi_state = WIFI_STATE_IDLE;
static SemaphoreHandle_t wifi_state_mutex = NULL;
static SemaphoreHandle_t data_mutex = NULL;
static SemaphoreHandle_t conn_mutex = NULL;
static SemaphoreHandle_t ota_mutex = NULL;

static bool sim_select_flag = 1;
static uint8_t ota_state = 0;

// ============= OTA Dynamic Configuration =============
static char ota_url[256] = "http://54.194.219.149:45056/firmware/zigron_demo.bin";
static char ota_version[64] = "";

// ============= SENSOR DATA =============
MCP_t dev;
uint8_t mac_addr[6] = {0};
char mac_string[13] = "0123456789AB";

char data_buff[512];
char topic_buff[128];

static uint8_t zone_alert_state[TOTAL_ZONE];
static uint16_t zone_raw_value[TOTAL_ZONE];
static uint16_t zone_lower_limit[TOTAL_ZONE] = {0};
static uint16_t zone_upper_limit[TOTAL_ZONE] = {2000,2000,2000,2000,2000,2000,2000,2000,0,0};

// ============= CONFIGURATION =============
typedef struct {
    char wifi_ssid[32];
    char wifi_password[64];
    char mqtt_broker_url[128];
    uint16_t zone_lower[TOTAL_ZONE];
    uint16_t zone_upper[TOTAL_ZONE];
    uint32_t publish_interval_ms;
    bool enable_ota;
} device_config_t;

static device_config_t g_config = {
    .wifi_ssid = "Piffers Control Room",
    .wifi_password = "Pak@12345",
    .mqtt_broker_url = "mqtt://zigron:zigron123@54.194.219.149:45055",
    .publish_interval_ms = 5000,
    .enable_ota = true,
};

// ============= TASK HANDLES =============
static TaskHandle_t wifi_task_handle = NULL;
static TaskHandle_t sensor_task_handle = NULL;
static TaskHandle_t mqtt_task_handle = NULL;
static TaskHandle_t ota_task_handle = NULL;

esp_mqtt_client_handle_t mqtt_client = NULL;
bool mqtt_started = false;

// ============= FORWARD DECLARATIONS =============
static void init_wifi(void);
static bool connect_wifi_safe(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static bool check_network_registration(esp_modem_dce_t *dce);
static void perform_modem_reset(esp_modem_dce_t *dce);
static bool restart_ppp_connection(esp_modem_dce_t *dce);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static esp_err_t _http_event_handler(esp_http_client_event_t *evt);
static void sensor_task(void *arg);
static void mqtt_task(void *arg);
static void ota_task(void *arg);
static void connectivity_manager_task(void *arg);
static void wifi_connection_task(void *arg);
static void handle_mqtt_config_command(const char *payload, int payload_len);
static bool validate_ota_request(const char *url, const char *version);
static int compare_firmware_version(const char *new_version, const char *current_version);
static esp_err_t save_config_to_nvs(void);
static esp_err_t load_config_from_nvs(void);

// ============= UTILITY FUNCTIONS =============
static inline void set_wifi_state(wifi_state_t state) {
    if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        wifi_state = state;
        xSemaphoreGive(wifi_state_mutex);
    }
}

static inline wifi_state_t get_wifi_state(void) {
    wifi_state_t state = WIFI_STATE_IDLE;
    if (xSemaphoreTake(wifi_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = wifi_state;
        xSemaphoreGive(wifi_state_mutex);
    }
    return state;
}

// ============= VERSION COMPARISON =============
static int compare_firmware_version(const char *new_version, const char *current_version)
{
    if (!new_version || !current_version || strlen(new_version) == 0 || strlen(current_version) == 0) {
        return 1;
    }
    
    int nm=0, nn=0, np=0, cm=0, cn=0, cp=0;
    sscanf(new_version, "%d.%d.%d", &nm, &nn, &np);
    sscanf(current_version, "%d.%d.%d", &cm, &cn, &cp);
    
    if (nm != cm) return (nm > cm) ? 1 : -1;
    if (nn != cn) return (nn > cn) ? 1 : -1;
    if (np != cp) return (np > cp) ? 1 : -1;
    return 0;
}

static bool validate_ota_request(const char *url, const char *version)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "OTA URL is empty");
        return false;
    }
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "Invalid OTA URL protocol: %s", url);
        return false;
    }
    return true;
}

// ============= NVS CONFIGURATION =============
static esp_err_t save_config_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_blob(handle, "config", &g_config, sizeof(g_config));
    if (err == ESP_OK) err = nvs_commit(handle);
    
    nvs_close(handle);
    return err;
}

static esp_err_t load_config_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    
    size_t required_size = 0;
    err = nvs_get_blob(handle, "config", NULL, &required_size);
    if (err == ESP_OK && required_size == sizeof(g_config)) {
        err = nvs_get_blob(handle, "config", &g_config, &required_size);
        if (err == ESP_OK) {
            memcpy(zone_lower_limit, g_config.zone_lower, sizeof(g_config.zone_lower));
            memcpy(zone_upper_limit, g_config.zone_upper, sizeof(g_config.zone_upper));
            ESP_LOGI(TAG, "Config loaded: SSID=%s", g_config.wifi_ssid);
        }
    } else {
        memcpy(g_config.zone_lower, zone_lower_limit, sizeof(zone_lower_limit));
        memcpy(g_config.zone_upper, zone_upper_limit, sizeof(zone_upper_limit));
        err = ESP_ERR_NOT_FOUND;
    }
    
    nvs_close(handle);
    return err;
}

// ============= WIFI FUNCTIONS (OPTIMIZED) =============
static void init_wifi(void)
{
    ESP_LOGI(TAG, "Initializing WiFi...");
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 1;  // Enable NVS for faster connection
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));
    
    // Set WiFi power save mode for faster response
    esp_wifi_set_ps(WIFI_PS_NONE);  // No power save for faster connection
}

static bool connect_wifi_safe(void)
{
    if (strlen(g_config.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "WiFi SSID not configured");
        return false;
    }
    
    ESP_LOGI(TAG, "Connecting to WiFi: %s", g_config.wifi_ssid);
    set_wifi_state(WIFI_STATE_CONNECTING);
    
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    strncpy((char*)wifi_config.sta.ssid, g_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, g_config.wifi_password, sizeof(wifi_config.sta.password) - 1);
    
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set config failed: %s", esp_err_to_name(ret));
        set_wifi_state(WIFI_STATE_IDLE);
        return false;
    }
    
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(ret));
        set_wifi_state(WIFI_STATE_IDLE);
        return false;
    }
    
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connect failed: %s", esp_err_to_name(ret));
        set_wifi_state(WIFI_STATE_IDLE);
        return false;
    }
    
    // Wait for connection with reduced timeout
    EventBits_t bits = xEventGroupWaitBits(event_group, WIFI_CONNECTED_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected!");
        set_wifi_state(WIFI_STATE_CONNECTED);
        xEventGroupSetBits(event_group, NETWORK_AVAILABLE_BIT);
        return true;
    }
    
    ESP_LOGW(TAG, "WiFi connection timeout");
    set_wifi_state(WIFI_STATE_DISCONNECTED);
    return false;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t*)event_data;
                ESP_LOGW(TAG, "WiFi disconnected, reason: %d", dis->reason);
                xEventGroupClearBits(event_group, WIFI_CONNECTED_BIT | NETWORK_AVAILABLE_BIT);
                set_wifi_state(WIFI_STATE_DISCONNECTED);
                break;
            }
            default:
                break;
        }
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        set_wifi_state(WIFI_STATE_CONNECTED);
        xEventGroupSetBits(event_group, WIFI_CONNECTED_BIT | NETWORK_AVAILABLE_BIT);
        
        // Fast time sync
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
    }
}

// ============= WIFI CONNECTION TASK =============
static void wifi_connection_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi connection task started");
    int retry_delay = 5;  // Start with 5 seconds
    
    while (1) {
        EventBits_t bits = xEventGroupGetBits(event_group);
        
        // Handle reconfiguration
        if (bits & RECONFIG_TRIGGER_BIT) {
            xEventGroupClearBits(event_group, RECONFIG_TRIGGER_BIT);
            ESP_LOGI(TAG, "Reconfiguration triggered");
        }
        
        if (bits & WIFI_RECONNECT_BIT) {
            xEventGroupClearBits(event_group, WIFI_RECONNECT_BIT);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        
        // Check if already connected
        if (bits & WIFI_CONNECTED_BIT) {
            vTaskDelay(pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS));
            continue;
        }
        
        // Check SSID configured
        if (strlen(g_config.wifi_ssid) == 0) {
            ESP_LOGW(TAG, "WiFi SSID not configured");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        
        // Attempt connection
        if (connect_wifi_safe()) {
            retry_delay = 5;  // Reset delay on success
        } else {
            // Exponential backoff with cap
            retry_delay = (retry_delay < 60) ? retry_delay * 2 : 60;
            ESP_LOGI(TAG, "Retry WiFi in %d seconds", retry_delay);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                // Check if GSM took over
                if (xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT) {
                    ESP_LOGI(TAG, "GSM active, pausing WiFi retry");
                    xEventGroupWaitBits(event_group, WIFI_RECONNECT_BIT | RECONFIG_TRIGGER_BIT,
                                      pdTRUE, pdFALSE, portMAX_DELAY);
                }
            }
        }
    }
}

// ============= GSM/PPP FUNCTIONS (OPTIMIZED) =============
static bool check_network_registration(esp_modem_dce_t *dce)
{
    int rssi, ber;
    for (int retry = 0; retry < 5; retry++) {  // Reduced from 10 to 5
        if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
            ESP_LOGI(TAG, "GSM RSSI: %d", rssi);
            if (rssi > MIN_GSM_SIGNAL_THRESHOLD) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));  // Reduced from 5s to 2s
    }
    return false;
}

static void perform_modem_reset(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Modem reset...");
    sim_select_flag = !sim_select_flag;
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, sim_select_flag);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(500));  // Reduced from 1s to 500ms
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    
    vTaskDelay(pdMS_TO_TICKS(8000));  // Reduced from 15s to 8s
}

static bool restart_ppp_connection(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Restarting PPP...");
    perform_modem_reset(dce);
    
    if (esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));  // Reduced from 5s to 2s
    
    int rssi, ber;
    if (esp_modem_get_signal_quality(dce, &rssi, &ber) != ESP_OK || 
        rssi <= MIN_GSM_SIGNAL_THRESHOLD) {
        ESP_LOGW(TAG, "GSM signal weak: %d", rssi);
        return false;
    }
    
    if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        return false;
    }
    
    ESP_LOGI(TAG, "PPP restart initiated");
    return true;
}

// ============= CONNECTIVITY MANAGER (FAST SWITCHING) =============
static void connectivity_manager_task(void *arg)
{
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    static uint32_t last_switch_time = 0;
    static uint32_t last_gsm_attempt = 0;
    static bool gsm_activated = false;
    const uint32_t MIN_SWITCH_INTERVAL_MS = MIN_SWITCH_INTERVAL * 1000;
    
    ESP_LOGI(TAG, "Connectivity Manager started");
    
    while (1) {
        bool wifi_connected = (xEventGroupGetBits(event_group) & WIFI_CONNECTED_BIT);
        bool gsm_connected = (xEventGroupGetBits(event_group) & GSM_CONNECTED_BIT);
        uint32_t now = esp_timer_get_time() / 1000;  // ms
        
        if (xSemaphoreTake(conn_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            // Priority: WiFi > GSM
            if (!wifi_connected && !gsm_connected) {
                // Try GSM if enough time passed
                if (now - last_gsm_attempt > GSM_RECOVERY_DELAY_MS) {
                    ESP_LOGI(TAG, "Activating GSM...");
                    last_gsm_attempt = now;
                    
                    if (esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA) == ESP_OK) {
                        gsm_activated = true;
                        current_conn_mode = CONN_MODE_GSM;
                        
                        // Fast wait for PPP connection (15s instead of 45s)
                        EventBits_t bits = xEventGroupWaitBits(event_group,
                            GSM_CONNECTED_BIT | WIFI_CONNECTED_BIT,
                            pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(GSM_CONNECT_TIMEOUT_MS));
                        
                        if (bits & GSM_CONNECTED_BIT) {
                            ESP_LOGI(TAG, "GSM connected!");
                            xEventGroupSetBits(event_group, NETWORK_AVAILABLE_BIT);
                        } else if (bits & WIFI_CONNECTED_BIT) {
                            ESP_LOGI(TAG, "WiFi returned, switching back");
                            gsm_activated = false;
                            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                            current_conn_mode = CONN_MODE_WIFI;
                        } else {
                            ESP_LOGW(TAG, "GSM timeout");
                            gsm_activated = false;
                            esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                            restart_ppp_connection(dce);
                        }
                    }
                }
            }
            // Fast switch: WiFi reconnected, switch from GSM immediately
            else if (wifi_connected && current_conn_mode == CONN_MODE_GSM) {
                // Reduced minimum interval from 60s to 5s
                if (now - last_switch_time > MIN_SWITCH_INTERVAL_MS) {
                    ESP_LOGI(TAG, "Switching GSM->WiFi");
                    current_conn_mode = CONN_MODE_WIFI;
                    last_switch_time = now;
                    
                    if (gsm_activated) {
                        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
                        gsm_activated = false;
                    }
                    xEventGroupSetBits(event_group, NETWORK_AVAILABLE_BIT);
                }
            }
            // GSM connected and WiFi not available
            else if (gsm_connected && !wifi_connected && current_conn_mode != CONN_MODE_GSM) {
                current_conn_mode = CONN_MODE_GSM;
                last_switch_time = now;
                xEventGroupSetBits(event_group, NETWORK_AVAILABLE_BIT);
                ESP_LOGI(TAG, "GSM active");
            }
            
            xSemaphoreGive(conn_mutex);
        }
        
        // Check more frequently for faster switching
        vTaskDelay(pdMS_TO_TICKS(NETWORK_CHECK_INTERVAL_MS));
    }
}

// ============= SENSOR TASK (OPTIMIZED) =============
static void sensor_task(void *arg)
{
    ESP_LOGI(TAG, "Sensor task started");
    static uint16_t alert_flg = 0;
    static uint16_t prev_alert_flg = 0;
    static uint16_t loop_counter = 0;
    static uint16_t alert_counter = 0;
    static uint8_t alert_ready = 0;
    
    uint32_t last_publish_time = 0;
    
    while (1) {
        if (data_mutex && xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
            loop_counter++;
            alert_counter++;
            
            // Read analog zones - optimized with direct reads
            for (uint8_t i = 0; i < (TOTAL_ZONE - 2); i++) {
                zone_raw_value[i] = mcpReadData(&dev, i);
                uint16_t bitmask = 1 << i;
                
                // Check if we need to set an alert
                if (zone_raw_value[i] < zone_lower_limit[i]) {
                    zone_alert_state[i] |= 0x01;    // Low alert
                    // zone_alert_state[i] |= 0x10;    // Alert active
                    alert_flg |= bitmask;
                } else if (zone_raw_value[i] > zone_upper_limit[i]) {
                    zone_alert_state[i] |= 0x02;    // High alert
                    // zone_alert_state[i] |= 0x10;    // Alert active
                    alert_flg |= bitmask;
                } else {
                    // Value is within range, clear the alert flag
                    zone_alert_state[i] &= ~0x10;   // Clear alert state
                    alert_flg &= ~bitmask;
                }
            }
            
            // Digital inputs - optimized
            zone_raw_value[TOTAL_ZONE-1] = 0;
            zone_raw_value[TOTAL_ZONE-2] = 0;
            for (uint8_t i = 0; i < 10; i++) {  // Reduced from 20 to 10
                zone_raw_value[TOTAL_ZONE-1] += gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_ARM_STATUS_PIN);
                zone_raw_value[TOTAL_ZONE-2] += gpio_get_level((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN);
            }
            
            // Process digital alerts
            for (int i = TOTAL_ZONE - 2; i < TOTAL_ZONE; i++) {
                uint16_t bitmask = 1 << i;
                if (zone_raw_value[i] > 8) {  // Reduced threshold
                    zone_raw_value[i] = 1;
                    zone_alert_state[i] |= 0x02;
                    // zone_alert_state[i] |= 0x82;
                    alert_flg |= bitmask;
                } else {
                    // zone_alert_state[i] &= ~0x80;
                    alert_flg &= ~bitmask;
                    zone_raw_value[i] = 0;
                }
            }
            
            if (alert_flg != prev_alert_flg) {
                ESP_LOGI(TAG, "Alert: 0x%03X", alert_flg);
                prev_alert_flg = alert_flg;
                alert_ready = 1;
                alert_counter = 80;
            }
            
            xSemaphoreGive(data_mutex);
            
            // Publish condition - optimized
            uint32_t now = esp_timer_get_time() / 1000;  // ms
            bool publish_hb = (loop_counter >= PACKET_TIMEOUT);
            bool publish_alert = (alert_counter >= ALERT_TIMEOUT && alert_ready);
            
            if (publish_hb || publish_alert) {
                time_t now_ts;
                time(&now_ts);
                
                snprintf(data_buff, sizeof(data_buff),
                    "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                    "\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                    "\"DNA\":[\"%s\",%lld],\"TS\":\"%lld\","
                    "\"CONN\":\"%s%d\",\"FW\":\"%s\","
                    "\"THRESH\":[%d,%d],\"OS\":%d}%c",
                    zone_raw_value[0], zone_raw_value[1], zone_raw_value[2], zone_raw_value[3],
                    zone_raw_value[4], zone_raw_value[5], zone_raw_value[6], zone_raw_value[7],
                    zone_raw_value[8], zone_raw_value[9],
                    zone_alert_state[0], zone_alert_state[1], zone_alert_state[2], zone_alert_state[3],
                    zone_alert_state[4], zone_alert_state[5], zone_alert_state[6], zone_alert_state[7],
                    zone_alert_state[8], zone_alert_state[9],
                    mac_string, (long long)(esp_timer_get_time() / 1000000),
                    (long long)now_ts,
                    (current_conn_mode == CONN_MODE_WIFI) ? "WIFI" : "GSM", sim_select_flag,
                    FW_VER, zone_lower_limit[0], zone_upper_limit[0], ota_state, 0);
                
                if (publish_hb) {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/HB", mac_string);
                    loop_counter = 0;
                } else {
                    snprintf(topic_buff, sizeof(topic_buff), "/ZIGRON/%s/ALERT", mac_string);
                    alert_counter = 0;
                    alert_ready = 0;
                }
                
                if (mqtt_client && mqtt_started) {
                    int pub = esp_mqtt_client_publish(mqtt_client, topic_buff, data_buff, 0, 0, 0);
                    if (pub >= 0) {
                        // memset(zone_alert_state, 0, sizeof(zone_alert_state));
                        ESP_LOGI(TAG, "Published to: %s", topic_buff);
                    }
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms loop = faster response
    }
}

// ============= MQTT EVENT HANDLER =============
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected via %s", 
                    (current_conn_mode == CONN_MODE_WIFI) ? "WiFi" : "GSM");
            xEventGroupSetBits(event_group, MQTT_CONNECTED_BIT);
            
            // Subscribe to topics
            char sub_topic[64];
            snprintf(sub_topic, sizeof(sub_topic), "/ZIGRON/%s/#", mac_string);
            esp_mqtt_client_subscribe(client, sub_topic, 0);
            ESP_LOGI(TAG, "Subscribed to: %s", sub_topic);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            xEventGroupClearBits(event_group, MQTT_CONNECTED_BIT);
            break;
            
        case MQTT_EVENT_DATA: {
            char topic[64];
            int topic_len = event->topic_len < 63 ? event->topic_len : 63;
            memcpy(topic, event->topic, topic_len);
            topic[topic_len] = '\0';
            
            // Parse topics efficiently
            char config_topic[64], ota_topic[64], clear_topic[64], cmd_topic[64];
            snprintf(config_topic, sizeof(config_topic), "/ZIGRON/%s/CONFIG", mac_string);
            snprintf(ota_topic, sizeof(ota_topic), "/ZIGRON/%s/OTA", mac_string);
            snprintf(clear_topic, sizeof(clear_topic), "/ZIGRON/%s/CLEAR", mac_string);
            snprintf(cmd_topic, sizeof(cmd_topic), "/ZIGRON/%s/COMMAND", mac_string);
            
            // Handle CLEAR
            if (strcmp(topic, clear_topic) == 0) {
                if (data_mutex && xSemaphoreTake(data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    memset(zone_alert_state, 0, sizeof(zone_alert_state));
                    xSemaphoreGive(data_mutex);
                    ESP_LOGI(TAG, "Alerts cleared");
                }
            }
            // Handle CONFIG
            else if (strcmp(topic, config_topic) == 0) {
                handle_mqtt_config_command(event->data, event->data_len);
            }
            // Handle OTA
            else if (strcmp(topic, ota_topic) == 0) {
                ESP_LOGI(TAG, "OTA command received");
                char payload[512] = {0};
                int len = event->data_len < sizeof(payload)-1 ? event->data_len : sizeof(payload)-1;
                memcpy(payload, event->data, len);
                
                cJSON *root = cJSON_Parse(payload);
                if (root) {
                    cJSON *url = cJSON_GetObjectItem(root, "url");
                    cJSON *ver = cJSON_GetObjectItem(root, "version");
                    
                    if (url && cJSON_IsString(url)) {
                        char new_url[256], new_ver[64] = {0};
                        strncpy(new_url, url->valuestring, sizeof(new_url)-1);
                        if (ver && cJSON_IsString(ver)) {
                            strncpy(new_ver, ver->valuestring, sizeof(new_ver)-1);
                        }
                        
                        if (validate_ota_request(new_url, new_ver)) {
                            if (strlen(new_ver) > 0) {
                                int cmp = compare_firmware_version(new_ver, FW_VER);
                                if (cmp < 0) {
                                    ESP_LOGW(TAG, "Version %s < current %s, skipping", new_ver, FW_VER);
                                    cJSON_Delete(root);
                                    break;
                                }
                            }
                            
                            if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                                strcpy(ota_url, new_url);
                                strcpy(ota_version, new_ver);
                                xSemaphoreGive(ota_mutex);
                                xEventGroupSetBits(event_group, OTA_TRIGGER_BIT);
                                ESP_LOGI(TAG, "OTA triggered: %s", new_url);
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            // Handle COMMAND
            else if (strcmp(topic, cmd_topic) == 0) {
                char cmd[32] = {0};
                int len = event->data_len < sizeof(cmd)-1 ? event->data_len : sizeof(cmd)-1;
                memcpy(cmd, event->data, len);
                for (int i = 0; i < len; i++) cmd[i] = toupper((unsigned char)cmd[i]);
                
                if (strcmp(cmd, "RESET") == 0) {
                    ESP_LOGI(TAG, "RESET command received via MQTT");
                    esp_restart();
                }
                else if (strcmp(cmd, "GET_CONFIG") == 0) {
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
        
        default:
            break;
    }
}

// ============= MQTT TASK =============
static void mqtt_task(void *arg)
{
    ESP_LOGI(TAG, "MQTT task started");
    esp_modem_dce_t *dce = (esp_modem_dce_t *)arg;
    
    while (1) {
        // Wait for network
        xEventGroupWaitBits(event_group, NETWORK_AVAILABLE_BIT,
                          pdFALSE, pdFALSE, portMAX_DELAY);
        
        // Initialize MQTT if needed
        if (!mqtt_client) {
            esp_mqtt_client_config_t cfg = {
                .broker.address.uri = g_config.mqtt_broker_url,
                .network.timeout_ms = 10000,
                .session.keepalive = 30,
                .network.reconnect_timeout_ms = 5000,
            };
            mqtt_client = esp_mqtt_client_init(&cfg);
            if (mqtt_client) {
                esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            }
        }
        
        // Start MQTT if not started
        if (mqtt_client && !mqtt_started) {
            if (esp_mqtt_client_start(mqtt_client) == ESP_OK) {
                mqtt_started = true;
                ESP_LOGI(TAG, "MQTT started");
            }
        }
        
        // Check connection health
        if (mqtt_client && mqtt_started) {
            // Check if MQTT is still connected
            if (!(xEventGroupGetBits(event_group) & MQTT_CONNECTED_BIT)) {
                ESP_LOGW(TAG, "MQTT not connected, restarting...");
                esp_mqtt_client_stop(mqtt_client);
                mqtt_started = false;
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============= OTA TASK =============
static void ota_task(void *arg)
{
    ESP_LOGI(TAG, "OTA task started");
    
    while (1) {
        xEventGroupWaitBits(event_group, OTA_TRIGGER_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (!(xEventGroupGetBits(event_group) & NETWORK_AVAILABLE_BIT)) {
            ESP_LOGW(TAG, "No network for OTA");
            continue;
        }
        
        char url[256], ver[64];
        if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            strcpy(url, ota_url);
            strcpy(ver, ota_version);
            xSemaphoreGive(ota_mutex);
        } else {
            continue;
        }
        
        ESP_LOGI(TAG, "Starting OTA from: %s", url);
        ota_state = 1;
        
        esp_http_client_config_t http_cfg = {
            .url = url,
            .event_handler = _http_event_handler,
            .timeout_ms = 30000,
        };
        
        esp_https_ota_config_t ota_cfg = {
            .http_config = &http_cfg,
        };
        
        esp_err_t ret = esp_https_ota(&ota_cfg);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "OTA success, restarting...");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
            ota_state = 2;
        }
    }
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

// ============= HANDLE MQTT CONFIG =============
static void handle_mqtt_config_command(const char *payload, int payload_len)
{
    char copy[512];
    if (payload_len >= sizeof(copy)) return;
    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';
    
    cJSON *root = cJSON_Parse(copy);
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON config");
        return;
    }
    
    bool changed = false;
    
    cJSON *ssid = cJSON_GetObjectItem(root, "wifi_ssid");
    if (ssid && cJSON_IsString(ssid)) {
        if (strcmp(ssid->valuestring, g_config.wifi_ssid) != 0) {
            strncpy(g_config.wifi_ssid, ssid->valuestring, sizeof(g_config.wifi_ssid)-1);
            changed = true;
        }
    }
    
    cJSON *pass = cJSON_GetObjectItem(root, "wifi_password");
    if (pass && cJSON_IsString(pass)) {
        if (strcmp(pass->valuestring, g_config.wifi_password) != 0) {
            strncpy(g_config.wifi_password, pass->valuestring, sizeof(g_config.wifi_password)-1);
            changed = true;
        }
    }
    
    cJSON *broker = cJSON_GetObjectItem(root, "mqtt_broker");
    if (broker && cJSON_IsString(broker)) {
        if (strcmp(broker->valuestring, g_config.mqtt_broker_url) != 0) {
            strncpy(g_config.mqtt_broker_url, broker->valuestring, sizeof(g_config.mqtt_broker_url)-1);
            changed = true;
        }
    }
    
    cJSON *interval = cJSON_GetObjectItem(root, "publish_interval");
    if (interval && cJSON_IsNumber(interval)) {
        uint32_t val = interval->valueint;
        if (val >= 1000 && val <= 30000 && val != g_config.publish_interval_ms) {
            g_config.publish_interval_ms = val;
            changed = true;
        }
    }
    
    cJSON *thresh = cJSON_GetObjectItem(root, "thresholds");
    if (thresh && cJSON_IsArray(thresh)) {
        int size = cJSON_GetArraySize(thresh);
        for (int i = 0; i < size && i < TOTAL_ZONE; i++) {
            cJSON *item = cJSON_GetArrayItem(thresh, i);
            if (!cJSON_IsObject(item)) continue;
            
            cJSON *low = cJSON_GetObjectItem(item, "l");
            cJSON *high = cJSON_GetObjectItem(item, "h");
            if (low && high && cJSON_IsNumber(low) && cJSON_IsNumber(high)) {
                uint16_t nl = low->valueint;
                uint16_t nh = high->valueint;
                if (nl != g_config.zone_lower[i] || nh != g_config.zone_upper[i]) {
                    g_config.zone_lower[i] = nl;
                    g_config.zone_upper[i] = nh;
                    zone_lower_limit[i] = nl;
                    zone_upper_limit[i] = nh;
                    changed = true;
                }
            }
        }
    }
    
    cJSON_Delete(root);
    
    if (changed) {
        if (save_config_to_nvs() == ESP_OK) {
            ESP_LOGI(TAG, "Config saved");
            // Trigger WiFi reconnection if SSID changed
            if (strlen(g_config.wifi_ssid) > 0) {
                xEventGroupSetBits(event_group, WIFI_RECONNECT_BIT);
            }
        }
    }
}

// ============= APP MAIN =============
void app_main(void)
{
    // Set log levels
    esp_log_level_set("esp_http_client", ESP_LOG_WARN);
    esp_log_level_set("esp_https_ota", ESP_LOG_WARN);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set(TAG, ESP_LOG_INFO);
    
    ESP_LOGI(TAG, "=== ZIGRON OPTIMIZED STARTING ===");
    ESP_LOGI(TAG, "FW Version: %s", FW_VER);
    
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // Load config
    ret = load_config_from_nvs();
    if (ret != ESP_OK) {
        memcpy(g_config.zone_lower, zone_lower_limit, sizeof(zone_lower_limit));
        memcpy(g_config.zone_upper, zone_upper_limit, sizeof(zone_upper_limit));
        save_config_to_nvs();
    }
    
    // Initialize hardware
    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO,
            CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);
    
    esp_read_mac(mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string, "%02X%02X%02X%02X%02X%02X",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC: %s", mac_string);
    
    // Create mutexes
    event_group = xEventGroupCreate();
    data_mutex = xSemaphoreCreateMutex();
    conn_mutex = xSemaphoreCreateMutex();
    wifi_state_mutex = xSemaphoreCreateMutex();
    ota_mutex = xSemaphoreCreateMutex();
    
    if (!event_group || !data_mutex || !conn_mutex || !wifi_state_mutex || !ota_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return;
    }
    
    // Network
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // WiFi
    init_wifi();
    
    // GPIO setup
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_BUZZER_STATUS_PIN, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_ARM_STATUS_PIN, GPIO_MODE_INPUT);
    
    // Reset modem
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, sim_select_flag);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // GSM modem
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG("internet");
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_cfg);
    assert(esp_netif);
    
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.tx_io_num = CONFIG_EXAMPLE_MODEM_UART_TX_PIN;
    dte_cfg.uart_config.rx_io_num = CONFIG_EXAMPLE_MODEM_UART_RX_PIN;
    dte_cfg.uart_config.rts_io_num = CONFIG_EXAMPLE_MODEM_UART_RTS_PIN;
    dte_cfg.uart_config.cts_io_num = CONFIG_EXAMPLE_MODEM_UART_CTS_PIN;
    dte_cfg.uart_config.flow_control = EXAMPLE_FLOW_CONTROL;
    
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_cfg, &dce_cfg, esp_netif);
    if (dce) {
        ESP_LOGI(TAG, "GSM modem initialized");
        // Test communication
        int rssi, ber;
        for (int i = 0; i < 3; i++) {
            if (esp_modem_get_signal_quality(dce, &rssi, &ber) == ESP_OK) {
                ESP_LOGI(TAG, "GSM signal: %d", rssi);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    } else {
        ESP_LOGW(TAG, "GSM modem not available");
    }
    
    // Register IP event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));
    
    // ===== FIX: Clear only valid event bits =====
    // Define the valid bits mask (bits 0-23 are valid, 24-31 are reserved)
    #define VALID_EVENT_BITS (WIFI_CONNECTED_BIT | GSM_CONNECTED_BIT | \
                             MQTT_CONNECTED_BIT | OTA_TRIGGER_BIT | \
                             RECONFIG_TRIGGER_BIT | WIFI_RECONNECT_BIT | \
                             NETWORK_AVAILABLE_BIT)
    
    xEventGroupClearBits(event_group, VALID_EVENT_BITS);
    
    // Create tasks with optimized stack sizes
    xTaskCreate(wifi_connection_task, "wifi", 8192, NULL, 4, &wifi_task_handle);
    xTaskCreate(sensor_task, "sensor", 6144, NULL, 3, &sensor_task_handle);
    xTaskCreate(mqtt_task, "mqtt", 8192, dce, 5, &mqtt_task_handle);
    xTaskCreate(ota_task, "ota", 8192, NULL, 5, &ota_task_handle);
    
    if (dce) {
        xTaskCreate(connectivity_manager_task, "conn_mgr", 4096, dce, 4, NULL);
    }
    
    ESP_LOGI(TAG, "System ready!");
    ESP_LOGI(TAG, "OTA topic: /ZIGRON/%s/OTA", mac_string);
    ESP_LOGI(TAG, "Config topic: /ZIGRON/%s/CONFIG", mac_string);
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {}
static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "PPP IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(event_group, GSM_CONNECTED_BIT | NETWORK_AVAILABLE_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "PPP lost");
        xEventGroupClearBits(event_group, GSM_CONNECTED_BIT | NETWORK_AVAILABLE_BIT);
    }
}