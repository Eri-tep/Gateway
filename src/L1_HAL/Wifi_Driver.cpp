// ============================================================================
// Wifi_Driver.cpp — Level 1 Physical HAL Wi-Fi Driver Implementation
// Encapsulated ESP32 RF/PHY Hardware Driver & Lifecycle Manager
// ============================================================================

#include "L1_HAL/Wifi_Driver.h"
#include "L0_Foundation/System_Platform.h"
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

static EventGroupHandle_t s_wifi_event_group = nullptr;
static StaticEventGroup_t s_wifi_event_group_buf;
static uint32_t s_wifi_disconnect_count = 0;

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println(F("[WIFI EVENT] STA Started"));
    break;
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println(F("[WIFI EVENT] STA Connected to AP"));
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_CONNECTED);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    s_wifi_disconnect_count = 0;
    Serial.printf("[WIFI EVENT] STA Got IP: %s\r\n",
                  IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_GOT_IP);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
    }
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_GOT_IP);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_GOT_IP);
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

void Wifi_Driver_Init(const WifiHwConfig &cfg) {
  if (!s_wifi_event_group) {
    s_wifi_event_group = xEventGroupCreateStatic(&s_wifi_event_group_buf);
  }
  WiFi.onEvent(onWifiEvent);

  const char *ssid = (cfg.sta_ssid && cfg.sta_ssid[0]) ? cfg.sta_ssid : "";
  const char *pass = cfg.sta_password ? cfg.sta_password : "";
  uint16_t tout = cfg.timeout_s ? cfg.timeout_s : 30;

  Serial.printf("[WIFI] Connecting to '%s' (Timeout: %us)...\r\n", ssid, tout);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);

  wifi_config_t w_conf;
  memset(&w_conf, 0, sizeof(w_conf));
  strncpy(reinterpret_cast<char *>(w_conf.sta.ssid), ssid,
          sizeof(w_conf.sta.ssid) - 1);
  w_conf.sta.ssid[sizeof(w_conf.sta.ssid) - 1] = 0;
  strncpy(reinterpret_cast<char *>(w_conf.sta.password), pass,
          sizeof(w_conf.sta.password) - 1);
  w_conf.sta.password[sizeof(w_conf.sta.password) - 1] = 0;
  w_conf.sta.scan_method = WIFI_FAST_SCAN;
  w_conf.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  w_conf.sta.pmf_cfg.capable = true;
  w_conf.sta.pmf_cfg.required = false;
  w_conf.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  esp_wifi_set_config(WIFI_IF_STA, &w_conf);
  esp_wifi_connect();

  uint32_t t_start = millis();
  uint32_t max_wait = tout * 1000;
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
    const char *fallback_ap =
        (cfg.ap_ssid && cfg.ap_ssid[0]) ? cfg.ap_ssid : "Sweet_Home_Rescue";
    const char *fallback_pass =
        (cfg.ap_password && cfg.ap_password[0]) ? cfg.ap_password : "";
    bool ap_ok = WiFi.softAP(fallback_ap, fallback_pass, 1, 0, 4);

    WiFi.setSleep(false);
    esp_wifi_set_max_tx_power(78);
    Serial.printf("[WIFI] STA connect failed. Fallback SoftAP '%s' started: "
                  "%s (IP: %s)\r\n",
                  fallback_ap, ap_ok ? "SUCCESS" : "FAILED",
                  WiFi.softAPIP().toString().c_str());
  }
}

[[nodiscard]] bool Wifi_Driver_IsConnected() noexcept {
  return (WiFi.status() == WL_CONNECTED);
}

[[nodiscard]] IPAddress Wifi_Driver_GetIp() noexcept {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP();
  }
  return WiFi.softAPIP();
}

[[nodiscard]] int8_t Wifi_Driver_GetRssi() noexcept {
  return static_cast<int8_t>(WiFi.RSSI());
}

void Wifi_Driver_Reconnect() noexcept {
  esp_wifi_connect();
}

// ── L0 Foundation Universal Contract Implementations ──
void System_WifiInit() noexcept {
  // Wifi is initialized explicitly at boot via Wifi_Driver_Init() with config
}

bool System_WifiIsConnected() noexcept {
  return Wifi_Driver_IsConnected();
}

IPAddress System_WifiGetIp() noexcept {
  return Wifi_Driver_GetIp();
}

IPAddress System_WifiGetSubnetMask() noexcept {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.subnetMask();
  }
  return IPAddress(0, 0, 0, 0);
}

IPAddress System_WifiGetApIp() noexcept {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
    return WiFi.softAPIP();
  }
  return IPAddress(0, 0, 0, 0);
}

IPAddress System_WifiGetApSubnetMask() noexcept {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
    return WiFi.softAPSubnetMask();
  }
  return IPAddress(0, 0, 0, 0);
}

int8_t System_WifiGetRssi() noexcept {
  return Wifi_Driver_GetRssi();
}

void System_WifiReconnect() noexcept {
  Wifi_Driver_Reconnect();
}

[[nodiscard]] EventBits_t Wifi_Driver_GetEventBits() noexcept {
  return s_wifi_event_group ? xEventGroupGetBits(s_wifi_event_group) : 0;
}

[[nodiscard]] EventBits_t System_WifiGetEventBits() noexcept {
  return Wifi_Driver_GetEventBits();
}
