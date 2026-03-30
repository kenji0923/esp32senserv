#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
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
  uint32_t sleep_sec;
  uint16_t work_delay_ms;
  uint8_t batt_avg;
  uint8_t sht_avg;
  uint16_t sht_read_wait_time_ms;
  uint8_t dps_osr; 
  uint8_t dps_avg; 
  uint16_t dps_read_wait_time_ms;
  uint32_t config_hash;
} config_data_t;

typedef struct {
  float temperature;
  float humidity;
  float pressure;
  float v_batt;
  float dps_temp;
  uint16_t temphumid_validcount;
  uint16_t temphumid_trial;
  uint16_t pressure_validcount;
  uint16_t pressure_trial;
  uint32_t timestamp;
  uint32_t config_hash;
} sensor_data_t;

typedef struct {
  uint8_t mac[6];
  char name[32];
  config_data_t config;
} client_record_t;

#define DB_SCHEMA_VERSION 2
#define MAX_CLIENTS 20
#define SLEEP_MIN 0UL
#define SLEEP_MAX 86400UL
#define AVG_MIN 1UL
#define AVG_MAX 255UL
#define WAIT_MIN 1UL
#define WAIT_MAX 65535UL
#define DPS_OSR_MIN 0UL
#define DPS_OSR_MAX 7UL
static client_record_t db[MAX_CLIENTS];
static int32_t client_count = 0;

static config_data_t default_config = {
  .sleep_sec = 5, .work_delay_ms = 50, .batt_avg = 1, .sht_avg = 4, .sht_read_wait_time_ms = 30, .dps_osr = 4, .dps_avg = 1, .dps_read_wait_time_ms = 120, .config_hash = 0
};

static uint32_t calculate_hash(config_data_t *c) {
  uint32_t h = 0; uint8_t *p = (uint8_t *)c;
  for (size_t i = 0; i < sizeof(config_data_t) - 4; i++) h += p[i] * (i + 1);
  if (h == 0) h = 1; // Prevent 0 hash
  return h;
}

static void save_db() {
  nvs_handle_t h;
  if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_i32(h, "db_schema", DB_SCHEMA_VERSION);
    nvs_set_blob(h, "db", db, sizeof(db));
    nvs_set_i32(h, "count", client_count);
    nvs_commit(h); nvs_close(h);
  }
}

static void load_db() {
  default_config.config_hash = calculate_hash(&default_config);
  nvs_handle_t h;
  if (nvs_open("storage", NVS_READONLY, &h) == ESP_OK) {
    int32_t schema = 0;
    size_t sz = sizeof(db);
    if (nvs_get_i32(h, "db_schema", &schema) != ESP_OK || schema != DB_SCHEMA_VERSION) {
      ESP_LOGW(TAG, "DB schema mismatch, starting fresh");
      client_count = 0;
      memset(db, 0, sizeof(db));
    } else if (nvs_get_blob(h, "db", db, &sz) != ESP_OK || sz != sizeof(db)) {
      ESP_LOGW(TAG, "No valid DB found, starting fresh");
      client_count = 0;
      memset(db, 0, sizeof(db));
    } else {
      nvs_get_i32(h, "count", &client_count);
      ESP_LOGI(TAG, "Loaded %d clients", (int)client_count);
    }
    nvs_close(h);
  }

  save_db();

  if (nvs_open("storage", NVS_READWRITE, &h) == ESP_OK) {
    nvs_commit(h);
    nvs_close(h);
  }
}

static int find_client(const uint8_t *mac) {
  for (int i = 0; i < client_count; i++) if (memcmp(db[i].mac, mac, 6) == 0) return i;
  return -1;
}

static bool validate_config_value(const char *key, unsigned long val, const char **error_msg) {
  if (strcmp(key, "sleep") == 0) {
    if (val >= SLEEP_MIN && val <= SLEEP_MAX) return true;
    *error_msg = "sleep must be 0..86400";
  } else if (strcmp(key, "work_delay_ms") == 0 || strcmp(key, "sht_read_wait_time_ms") == 0 || strcmp(key, "dps_read_wait_time_ms") == 0) {
    if (val >= WAIT_MIN && val <= WAIT_MAX) return true;
    *error_msg = "wait time must be 1..65535 ms";
  } else if (strcmp(key, "batt_avg") == 0 || strcmp(key, "sht_avg") == 0 || strcmp(key, "dps_avg") == 0) {
    if (val >= AVG_MIN && val <= AVG_MAX) return true;
    *error_msg = "average count must be 1..255";
  } else if (strcmp(key, "dps_osr") == 0) {
    if (val >= DPS_OSR_MIN && val <= DPS_OSR_MAX) return true;
    *error_msg = "dps_osr must be 0..7";
  } else {
    *error_msg = "unknown config key";
  }
  return false;
}

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
  int idx = find_client(recv_info->src_addr);
  config_data_t *target_cfg = (idx >= 0) ? &db[idx].config : &default_config;

  if (type == MSG_CLIENT_READY) {
    uint8_t buf[sizeof(config_data_t) + 1]; buf[0] = MSG_SERVER_CONFIG;
    memcpy(buf + 1, target_cfg, sizeof(config_data_t));
    esp_now_send(recv_info->src_addr, buf, sizeof(buf));
  } 
  else if (type == MSG_CLIENT_DATA) {
    sensor_data_t s; memcpy(&s, data + 1, sizeof(sensor_data_t));
    const char *name = (idx >= 0 && db[idx].name[0]) ? db[idx].name : "Unknown";
    ESP_LOGI(TAG, "DATA [%s (" MACSTR ")] V:%.2f T:%.2f H:%.2f P:%.1f TV:%u TT:%u PV:%u PT:%u Hash:%08X",
        name, MAC2STR(recv_info->src_addr), s.v_batt, s.temperature, s.humidity, s.pressure,
        (unsigned int)s.temphumid_validcount, (unsigned int)s.temphumid_trial,
        (unsigned int)s.pressure_validcount, (unsigned int)s.pressure_trial,
        (unsigned int)s.config_hash);

    uint8_t ack_buf[2] = { MSG_SERVER_ACK, (s.config_hash != target_cfg->config_hash) ? 1 : 0 };
    esp_now_send(recv_info->src_addr, ack_buf, 2);
  }
}

void console_task(void *p) {
  char line[128]; int idx = 0;
  printf("\nCMD> "); fflush(stdout);
  while (1) {
    int c = getchar();
    if (c == EOF) { vTaskDelay(1); continue; }
    if (c == '\r' || c == '\n') {
      line[idx] = 0;
      if (idx > 0) {
        printf("\n");
        uint8_t m[6]; char key[32]; unsigned long val; char name[32];
        if (sscanf(line, "name %hhx:%hhx:%hhx:%hhx:%hhx:%hhx %31s", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5], name) == 7) {
          int i = find_client(m);
          if (i < 0 && client_count < MAX_CLIENTS) {
            i = client_count++; memcpy(db[i].mac, m, 6); db[i].config = default_config;
          }
          if (i >= 0) {
            strncpy(db[i].name, name, 31); db[i].name[31] = 0;
            save_db(); printf("Named " MACSTR " as %s\n", MAC2STR(m), name);
          }
        } else if (sscanf(line, "set %hhx:%hhx:%hhx:%hhx:%hhx:%hhx %31s %lu", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5], key, &val) == 8) {
          int i = find_client(m);
          if (i < 0 && client_count < MAX_CLIENTS) {
            i = client_count++; memcpy(db[i].mac, m, 6); db[i].config = default_config; db[i].name[0] = 0;
          }
          if (i >= 0) {
            const char *error_msg = NULL;
            if (validate_config_value(key, val, &error_msg)) {
              if (strcmp(key, "sleep") == 0) db[i].config.sleep_sec = (uint32_t)val;
              else if (strcmp(key, "work_delay_ms") == 0) db[i].config.work_delay_ms = (uint16_t)val;
              else if (strcmp(key, "batt_avg") == 0) db[i].config.batt_avg = (uint8_t)val;
              else if (strcmp(key, "sht_avg") == 0) db[i].config.sht_avg = (uint8_t)val;
              else if (strcmp(key, "sht_read_wait_time_ms") == 0) db[i].config.sht_read_wait_time_ms = (uint16_t)val;
              else if (strcmp(key, "dps_osr") == 0) db[i].config.dps_osr = (uint8_t)val;
              else if (strcmp(key, "dps_avg") == 0) db[i].config.dps_avg = (uint8_t)val;
              else if (strcmp(key, "dps_read_wait_time_ms") == 0) db[i].config.dps_read_wait_time_ms = (uint16_t)val;
              db[i].config.config_hash = calculate_hash(&db[i].config);
              save_db(); printf("Updated " MACSTR ". Hash: %08X\n", MAC2STR(m), (unsigned int)db[i].config.config_hash);
            } else {
              printf("Invalid value for %s: %lu (%s)\n", key, val, error_msg);
            }
          }
        } else if (sscanf(line, "show %hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0],&m[1],&m[2],&m[3],&m[4],&m[5]) == 6) {
          int i = find_client(m);
          config_data_t *cf = (i >= 0) ? &db[i].config : &default_config;
          printf("Config for " MACSTR " (%s):\n", MAC2STR(m), (i>=0)?"Custom":"Default");
          printf("  Name:     %s\n", (i>=0)?db[i].name:"N/A");
          printf("  Sleep:    %u s\n", (unsigned int)cf->sleep_sec);
          printf("  Work Delay: %u ms\n", (unsigned int)cf->work_delay_ms);
          printf("  Batt Avg: %u\n", (unsigned int)cf->batt_avg);
          printf("  SHT Avg:  %u\n", (unsigned int)cf->sht_avg);
          printf("  SHT Wait: %u ms\n", (unsigned int)cf->sht_read_wait_time_ms);
          printf("  DPS OSR:  %u\n", (unsigned int)cf->dps_osr);
          printf("  DPS Avg:  %u\n", (unsigned int)cf->dps_avg);
          printf("  DPS Wait: %u ms\n", (unsigned int)cf->dps_read_wait_time_ms);
          printf("  Hash:     %08X\n", (unsigned int)cf->config_hash);
        } else if (strcmp(line, "ls") == 0) {
          printf("Clients (%d):\n", (int)client_count);
          for (int i=0; i<client_count; i++) {
            printf(" [%s] " MACSTR " | %s\n",
                db[i].name[0] ? db[i].name : "(unnamed)",
                MAC2STR(db[i].mac),
                memcmp(&db[i].config, &default_config, sizeof(config_data_t)) == 0 ? "Default" : "Custom");
          }
        } else if (strcmp(line, "clear") == 0) {
          client_count = 0; save_db(); printf("DB Cleared\n");
        }
        printf("CMD> "); fflush(stdout);
      } else { printf("\nCMD> "); fflush(stdout); }
      idx = 0;
    } else if (idx < sizeof(line) - 1) { putchar(c); fflush(stdout); line[idx++] = c; }
  }
}

static uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void app_main(void) {
  esp_err_t r = nvs_flash_init();
  if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
  load_db();
  esp_netif_init(); esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg); esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_start();
  esp_now_init(); esp_now_register_recv_cb(esp_now_recv_cb);
  add_peer(broadcast_mac);
  xTaskCreate(console_task, "console", 4096, NULL, 5, NULL);
  uint8_t mac[6]; esp_wifi_get_mac(WIFI_IF_STA, mac);
  ESP_LOGI(TAG, "Server Started: " MACSTR, MAC2STR(mac));
  while (1) {
    uint8_t msg = MSG_SERVER_HELLO; esp_now_send(broadcast_mac, &msg, 1);
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}
