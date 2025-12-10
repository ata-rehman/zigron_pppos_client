/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* PPPoS Client Example + Zigron custom logic (refactored with tasks + MQTT-triggered OTA)
*/

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

#include "esp_mac.h"
#include "mcp3002.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "wifi_app.h"
#include "sntp_time_sync.h"

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#define PACKET_TIMEOUT      30          // 30 seconds
#define FW_VER              "0.03"
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE

static const char *TAG = "pppos_example";

/* Event group for PPP + app status */
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT          = BIT0;
static const int DISCONNECT_BIT       = BIT1;
static const int GOT_DATA_BIT         = BIT2;
static const int USB_DISCONNECTED_BIT = BIT3;
static const int OTA_TRIGGER_BIT      = BIT4;   // new: MQTT-triggered OTA

/* Shared data */
MCP_t   dev;
uint8_t mac_addr[6] = {0};
char    mac_string[13] = "0123456789AB";

char data_buff[255];
char topic_buff[255];

#define TOTAL_ZONE 10
static uint8_t  zone_alert_state[TOTAL_ZONE];
static uint16_t zone_raw_value[TOTAL_ZONE];

// static uint16_t zone_lower_limit[TOTAL_ZONE] = {500,500,500,500,500,500,500,500,0,0};
// static uint16_t zone_upper_limit[TOTAL_ZONE] = {900,900,900,900,900,900,900,900,0,0};
static uint16_t zone_lower_limit[TOTAL_ZONE] = {0,0,0,0,0,0,0,0,0,0};
static uint16_t zone_upper_limit[TOTAL_ZONE] = {900,900,900,900,900,900,900,900,0,0};

static uint16_t alert_flg       = 0;
static uint16_t prev_alert_flg  = 0;
static uint16_t loop_counter    = 0;

/* PPP recovery state */
static int      ppp_fail_count  = 0;

/* Protect shared sensor + alert data between tasks & MQTT callback */
static SemaphoreHandle_t data_mutex = NULL;

/* Forward declarations */
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

/* Optional custom modem time function */
#ifdef CONFIG_EXAMPLE_MODEM_DEVICE_CUSTOM
esp_err_t esp_modem_get_time(esp_modem_dce_t *dce_wrap, char *p_time);
#endif

/* USB specific (unchanged) */
#if defined(CONFIG_EXAMPLE_SERIAL_CONFIG_USB)
#include "esp_modem_usb_c_api.h"
#include "esp_modem_usb_config.h"
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
    ESP_LOGE(TAG, "USB_DISCONNECTED_BIT destroying modem dce"); \
    esp_modem_destroy(dce); \
    continue; \
}
#else
#define CHECK_USB_DISCONNECTION(event_group)
#endif

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[]   asm("_binary_ca_cert_pem_end");

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

uint8_t get_temperature(void)
{
    return 10;
}

uint8_t get_humidity(void)
{
    return 90;
}

/* -------------------------------------------------------------------------- */
/* Sensor / port-reading task (was periodic_timer_callback + part of main)    */
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
                    // keep previous alert state or clear? up to you
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
/* PPP/network helpers (unchanged logic)                                      */
/* -------------------------------------------------------------------------- */

static bool check_network_registration(esp_modem_dce_t *dce)
{
    int rssi, ber;
    esp_err_t err;
    int retry = 0;

    while (retry < 10) {
        err = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Signal quality - RSSI: %d, BER: %d", rssi, ber);
            if (rssi > -113) {
                return true;
            }
        }
        ESP_LOGW(TAG, "Waiting for network registration. (attempt %d)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry++;
    }

    ESP_LOGE(TAG, "Failed to get proper network signal after %d attempts", retry);
    return false;
}

static void perform_modem_reset(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Performing complete modem reset...");

    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);

    ESP_LOGI(TAG, "Waiting for modem to fully reboot (35 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(35000));
}

static bool restart_ppp_connection(esp_modem_dce_t *dce)
{
    ESP_LOGI(TAG, "Attempting to restart PPP connection...");

    esp_err_t err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not switch to command mode, modem may be unresponsive");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));

    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modem not responsive after command mode switch");
        return false;
    }
    ESP_LOGI(TAG, "Modem responsive, signal: rssi=%d, ber=%d", rssi, ber);

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

    if (current_time - last_ppp_check > 30) {
        last_ppp_check = current_time;

        bool ppp_connected = ((xEventGroupGetBits(event_group) & CONNECT_BIT) == CONNECT_BIT);

        if (!ppp_connected && !recovery_in_progress) {
            ESP_LOGW(TAG, "PPP connection lost, attempting recovery. (fail count: %d)", ppp_fail_count);
            ppp_fail_count++;
            recovery_in_progress = true;

            if (mqtt_client) {
                esp_mqtt_client_stop(mqtt_client);
                ESP_LOGI(TAG, "MQTT client stopped for PPP recovery");
            }

            if (ppp_fail_count >= 2) {
                ESP_LOGE(TAG, "Multiple PPP failures, performing modem reset.");
                perform_modem_reset(dce);
                ppp_fail_count = 0;
            } else {
                if (restart_ppp_connection(dce)) {
                    vTaskDelay(pdMS_TO_TICKS(30000));
                }
            }

            recovery_in_progress = false;
        } else if (ppp_connected && ppp_fail_count > 0) {
            ESP_LOGI(TAG, "PPP connection restored, resetting fail count");
            ppp_fail_count = 0;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* MQTT event handler (now also triggers OTA via COMMAND topic)               */
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
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
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
        xEventGroupClearBits(event_group, CONNECT_BIT);
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

        /* Clear alarm state on any data (your original behaviour) */
        if (data_mutex && xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < TOTAL_ZONE; i++) {
                zone_alert_state[i] = 0;
            }
            alert_flg   = 0;
            loop_counter = PACKET_TIMEOUT - 1;
            xSemaphoreGive(data_mutex);
        }

        xEventGroupSetBits(event_group, GOT_DATA_BIT);

        /* --- OTA trigger: /ZIGRON/<MAC>/COMMAND topic with payload "OTA" --- */
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
        xEventGroupClearBits(event_group, CONNECT_BIT);
        break;

    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* IP / PPP event handlers (unchanged)                                        */
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

        ESP_LOGI(TAG, "Modem connected to PPP server");
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
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Modem disconnected from PPP server");
        xEventGroupSetBits(event_group, DISCONNECT_BIT);
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
/* OTA task – waits for MQTT trigger (OTA_TRIGGER_BIT)                        */
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

        /* Ensure PPP is connected before OTA */
        if ((xEventGroupGetBits(event_group) & CONNECT_BIT) == 0) {
            ESP_LOGW(TAG, "PPP not connected, skipping OTA attempt");
            continue;
        }

        esp_http_client_config_t config = {
            .url               = "http://54.194.219.149:45056/firmware/zigron_demo.bin",
            .event_handler     = _http_event_handler,
            .keep_alive_enable = true,
        };

        esp_https_ota_config_t ota_config = {
            .http_config = &config,
        };

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
/* MQTT + PPP management task (was main while(1) loop)                        */
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
    bool ppp_was_connected      = true;
    uint32_t last_connection_check = 0;
    uint32_t disconnect_start_time  = 0;

    while (1) {
        bool ppp_connected = ((xEventGroupGetBits(event_group) & CONNECT_BIT) == CONNECT_BIT);

        if (ppp_connected != ppp_was_connected) {
            if (ppp_connected) {
                ESP_LOGI(TAG, "PPP connection established");
                disconnect_start_time = 0;
            } else {
                ESP_LOGW(TAG, "PPP connection lost");
                if (disconnect_start_time == 0) {
                    disconnect_start_time = esp_timer_get_time() / 1000000;
                }
            }
            ppp_was_connected = ppp_connected;
        }

        uint32_t current_time = esp_timer_get_time() / 1000000;

        if (current_time - last_connection_check > 30) {
            last_connection_check = current_time;
            check_ppp_connection(dce, mqtt_client);
        }

        if (!ppp_connected && disconnect_start_time > 0) {
            uint32_t disconnect_duration = current_time - disconnect_start_time;
            if (disconnect_duration > 300) {
                ESP_LOGW(TAG, "PPP disconnected for 5 minutes, forcing emergency modem reset");
                perform_modem_reset(dce);
                disconnect_start_time = 0;
                ppp_fail_count = 0;
            }
        }

        if (!ppp_connected) {
            if (mqtt_client && mqtt_started) {
                esp_mqtt_client_stop(mqtt_client);
                mqtt_started = false;
                ESP_LOGI(TAG, "MQTT client stopped due to PPP disconnect");
            }
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        /* MQTT client init (PPP already up here) */
        if (!mqtt_client) {
            esp_mqtt_client_config_t mqtt_config = {
                .broker.address.uri         = "mqtt://zigron:zigron123@54.194.219.149:45055",
                .network.timeout_ms         = 20000,
                .session.keepalive          = 60,
                .network.disable_auto_reconnect = false,
            };
            mqtt_client = esp_mqtt_client_init(&mqtt_config);
            esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
        }

        if (!mqtt_started) {
            ESP_LOGI(TAG, "Starting MQTT client...");
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
            // ESP_LOGI(TAG, "SHOULD PUBLISH triggered at loop_counter=%d", loop_counter);
            if ( (loop_counter >= PACKET_TIMEOUT) | alert_flg ) {
                should_publish     = true;
                local_loop_counter = loop_counter;
                local_alert_flg    = alert_flg;
                memcpy(local_zone_raw,   zone_raw_value,   sizeof(local_zone_raw));
                memcpy(local_zone_alert, zone_alert_state, sizeof(local_zone_alert));
                loop_counter = 0;
                ESP_LOGI(TAG, "SHOULD PUBLISH triggered at loop_counter= %d, alert_flg=%d", local_loop_counter, alert_flg);
            }
            xSemaphoreGive(data_mutex);
        }

        if (should_publish && mqtt_started && ppp_connected) {
            data_buff[0]  = 0;
            topic_buff[0] = 0;

            int data_len = snprintf(data_buff, sizeof(data_buff),
                "{\"RAW\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"ALERT\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
                "\"DNA\":[\"%s\",%lld],"
                "\"FW\":\"%s\"}",
                local_zone_raw[0], local_zone_raw[1], local_zone_raw[2], local_zone_raw[3],
                local_zone_raw[4], local_zone_raw[5], local_zone_raw[6], local_zone_raw[7],
                local_zone_raw[8], local_zone_raw[9],
                local_zone_alert[0], local_zone_alert[1], local_zone_alert[2], local_zone_alert[3],
                local_zone_alert[4], local_zone_alert[5], local_zone_alert[6], local_zone_alert[7],
                local_zone_alert[8], local_zone_alert[9],
                mac_string, (long long)(esp_timer_get_time() / 1000000),
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
                    if (ppp_fail_count < 2) {
                        ppp_fail_count++;
                    }
                } else {
                    ESP_LOGI(TAG, "Published: %s -> %s", topic_buff, data_buff);
                    last_mqtt_publish = current_time;
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

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mcpInit(&dev, MCP3008, CONFIG_MISO_GPIO, CONFIG_MOSI_GPIO,
            CONFIG_SCLK_GPIO, CONFIG_CS_GPIO, MCP_SINGLE);
    esp_read_mac(mac_addr, ESP_MAC_EFUSE_FACTORY);
    sprintf(mac_string, "%02X%02X%02X%02X%02X%02X",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]);
    ESP_LOGI(TAG, "MAC address %s", mac_string);

    event_group = xEventGroupCreate();
    data_mutex  = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    // esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("jazzconnect.mobilinkworld.com");
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
    ESP_LOGI(TAG, "Watchdog temporarily disabled for modem initialization");

    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_LED_STATUS_PIN,   GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN,  GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN,   GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 1); vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_MODEM_RESET_PIN, 0);
    gpio_set_level((gpio_num_t)CONFIG_EXAMPLE_SIM_SELECT_PIN, 0);  vTaskDelay(pdMS_TO_TICKS(100));

    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_EC20, &dte_config, &dce_config, esp_netif);
    ESP_LOGI(TAG, "Waiting for modem to boot (15 seconds)...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    assert(dce);

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT | OTA_TRIGGER_BIT);

    ESP_LOGI(TAG, "Testing basic modem communication...");
    int retry_count = 0;
    while (retry_count < 5) {
        int rssi, ber;
        ret = esp_modem_get_signal_quality(dce, &rssi, &ber);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Basic communication OK - Signal: rssi=%d, ber=%d", rssi, ber);
            break;
        }
        ESP_LOGW(TAG, "Communication test failed (attempt %d), retrying...", retry_count + 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
        retry_count++;
    }

    if (!check_network_registration(dce)) {
        ESP_LOGE(TAG, "Cannot proceed without network registration");
        return;
    }

    ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DETECT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DETECT) failed with %d", ret);
        return;
    }
    esp_modem_dce_mode_t mode = esp_modem_get_mode(dce);
    ESP_LOGI(TAG, "Mode detection completed: current mode is: %d", mode);
    if (mode == ESP_MODEM_MODE_DATA) {
        ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_COMMAND) failed with %d", ret);
            return;
        }
        ESP_LOGI(TAG, "Command mode restored");
    }

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT | OTA_TRIGGER_BIT);

#if CONFIG_EXAMPLE_NEED_SIM_PIN == 1
    {
        bool pin_ok = false;
        if (esp_modem_read_pin(dce, &pin_ok) == ESP_OK && pin_ok == false) {
            if (esp_modem_set_pin(dce, CONFIG_EXAMPLE_SIM_PIN) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            } else {
                abort();
            }
        }
    }
#endif

    int rssi, ber;
    ret = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_get_signal_quality failed with %d %s", ret, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "Signal quality: rssi=%d, ber=%d", rssi, ber);

    ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DATA) failed with %d", ret);
        return;
    }

    ESP_LOGI(TAG, "Waiting for IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT | USB_DISCONNECTED_BIT | DISCONNECT_BIT,
                        pdFALSE, pdFALSE, pdMS_TO_TICKS(60000));
    CHECK_USB_DISCONNECTION(event_group);
    if ((xEventGroupGetBits(event_group) & CONNECT_BIT) != CONNECT_BIT) {
        ESP_LOGW(TAG, "Modem not connected, switching back to command mode");
        ret = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_COMMAND) failed with %d", ret);
            return;
        }
        ESP_LOGI(TAG, "Command mode restored");
        return;
    }

    /* Start tasks: sensor, MQTT, and OTA (OTA waits for MQTT trigger) */
    BaseType_t xRet;

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
    /* app_main can return; tasks keep running */
}
