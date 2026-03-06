#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "esp32senserv";

typedef enum {
    MSG_SERVER_HELLO = 0x01,
    MSG_CLIENT_READY = 0x02,
    MSG_SERVER_CONFIG = 0x03,
    MSG_CLIENT_DATA = 0x04,
    MSG_SERVER_ACK = 0x05
} msg_type_t;

typedef struct {
    uint8_t server_mac[6];
    uint32_t sleep_sec;
    uint16_t batt_cyc;
    uint16_t sht_cyc;
    uint16_t dps_cyc;
    uint16_t reconfig_cyc;
    uint32_t reconfig_to_ms;
    uint32_t config_ver;
    uint8_t batt_avg;
    uint8_t sht_avg;
    uint8_t dps_osr; 
    uint8_t dps_avg; 
} config_data_t;

typedef struct {
    float temperature;
    float humidity;
    float pressure;
    float v_batt;
    float dps_temp; // New field
    uint32_t timestamp;
    uint32_t config_ver;
} sensor_data_t;

static config_data_t global_config = {
    .sleep_sec = 5,
    .batt_cyc = 1,
    .sht_cyc = 1,
    .dps_cyc = 1,
    .reconfig_cyc = 10,
    .reconfig_to_ms = 1000,
    .config_ver = 107,
    .batt_avg = 16,
    .sht_avg = 4,
    .dps_osr = 4, // x16 Pressure
    .dps_avg = 1  
};

static uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void add_peer(const uint8_t *mac) {
    if (!esp_now_is_peer_exist(mac)) {
        esp_now_peer_info_t peer = { .channel = 0, .encrypt = false };
        memcpy(peer.peer_addr, mac, 6);
        esp_now_add_peer(&peer);
    }
}

static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    uint8_t type = data[0];
    add_peer(recv_info->src_addr);

    if (type == MSG_CLIENT_READY) {
        ESP_LOGI(TAG, "Config Request from " MACSTR, MAC2STR(recv_info->src_addr));
        uint8_t buf[sizeof(config_data_t) + 1];
        buf[0] = MSG_SERVER_CONFIG;
        esp_wifi_get_mac(WIFI_IF_STA, global_config.server_mac);
        memcpy(buf + 1, &global_config, sizeof(config_data_t));
        esp_now_send(recv_info->src_addr, buf, sizeof(buf));
    } 
    else if (type == MSG_CLIENT_DATA) {
        if (len >= sizeof(sensor_data_t) + 1) {
            sensor_data_t s;
            memcpy(&s, data + 1, sizeof(sensor_data_t));
            ESP_LOGI(TAG, "DATA [" MACSTR "] Vbat:%.3fV, SHT_T:%.2fC, SHT_H:%.2f%%, DPS_P:%.1f, DPS_T:%.2fC, Ver:%u, TS:%u", 
                     MAC2STR(recv_info->src_addr), s.v_batt, s.temperature, s.humidity, s.pressure, s.dps_temp,
                     (unsigned int)s.config_ver, (unsigned int)s.timestamp);
            
            uint8_t ack_buf[2] = { MSG_SERVER_ACK, (s.config_ver != global_config.config_ver) ? 1 : 0 };
            esp_now_send(recv_info->src_addr, ack_buf, 2);
        }
    }
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_now_init();
    esp_now_register_recv_cb(esp_now_recv_cb);

    add_peer(broadcast_mac);
    uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
    ESP_LOGI(TAG, "Server Active: " MACSTR " Ver: %u", MAC2STR(mac), (unsigned int)global_config.config_ver);

    while (1) {
        uint8_t msg = MSG_SERVER_HELLO;
        esp_now_send(broadcast_mac, &msg, 1);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
