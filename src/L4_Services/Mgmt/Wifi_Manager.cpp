#include "L4_Services/Mgmt/Mgmt_Internal.h"
#include "L4_Services/Mgmt_Service.h"
#include "L0_Foundation/System_Config.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

EventGroupHandle_t g_wifi_event_group = nullptr;
static StaticEventGroup_t s_wifi_event_group_buf;
static uint32_t s_wifi_disconnect_count = 0;

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("[WIFI EVENT] STA Started"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WIFI EVENT] STA Connected to AP"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    s_wifi_disconnect_count = 0;
    Serial.printf("[WIFI EVENT] STA Got IP: %s\r\n",
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
    }
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_GOT_IP);
    }
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Serial.printf(
        "[WIFI EVENT] AP Station Connected! MAC: "
        "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
        info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
        info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
        info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5],
        info.wifi_ap_staconnected.aid);
    break;
  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Serial.printf("[WIFI EVENT] AP Station Disconnected! MAC: "
                  "%02X:%02X:%02X:%02X:%02X:%02X, AID: %d\r\n",
                  info.wifi_ap_stadisconnected.mac[0],
                  info.wifi_ap_stadisconnected.mac[1],
                  info.wifi_ap_stadisconnected.mac[2],
                  info.wifi_ap_stadisconnected.mac[3],
                  info.wifi_ap_stadisconnected.mac[4],
                  info.wifi_ap_stadisconnected.mac[5],
                  info.wifi_ap_stadisconnected.aid);
    break;
  default:
    break;
  }
}

void Wifi_Init() {
  if (!g_wifi_event_group) {
    g_wifi_event_group = xEventGroupCreateStatic(&s_wifi_event_group_buf);
  }
  WiFi.onEvent(onWifiEvent);

  Serial.printf("[WIFI] Connecting to '%s' (Timeout: %us)...\r\n",
                g_config.wifi_ssid, g_config.wifi_connect_timeout_s);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_config_t w_conf;
  memset(&w_conf, 0, sizeof(w_conf));
  strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), g_config.wifi_ssid,
          sizeof(w_conf.sta.ssid) - 1);
  strncpy(reinterpret_cast<char *>(w_conf.sta.password),
          g_config.wifi_password, sizeof(w_conf.sta.password) - 1);
  w_conf.sta.scan_method = WIFI_FAST_SCAN;
  w_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  w_conf.sta.pmf_cfg.capable = true;
  w_conf.sta.pmf_cfg.required = false;
  w_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  esp_wifi_set_config(WIFI_IF_STA, &w_conf);
  esp_wifi_connect();

  uint32_t t_start = millis();
  uint32_t max_wait =
      (g_config.wifi_connect_timeout_s ? g_config.wifi_connect_timeout_s
                                       : 30) *
      1000;
  bool connected = false;

  while (millis() - t_start < max_wait) {
    if ((connected = (WiFi.status() == WL_CONNECTED)))
      break;
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (connected) {
    WiFi.setSleep(false);
    Serial.printf("[WIFI] Connected successfully! IP: %s, RSSI: %d dBm\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
    setenv("TZ", "KST-9", 1);
    tzset();
  } else {
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(100));

    WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                      IPAddress(255, 255, 255, 0));
    bool ap_ok = WiFi.softAP(g_config.ap_ssid, g_config.ap_password, 1, 0, 4);

    WiFi.setSleep(false);
    esp_wifi_set_max_tx_power(78);
    Serial.printf("[WIFI] STA connect failed. Fallback SoftAP '%s' started: "
                  "%s (IP: %s)\r\n",
                  g_config.ap_ssid, ap_ok ? "SUCCESS" : "FAILED",
                  WiFi.softAPIP().toString().c_str());
  }
}

