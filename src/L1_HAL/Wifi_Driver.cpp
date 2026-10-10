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
static std::atomic<uint32_t> s_wifi_disconnect_count{0};
static WifiHwConfig s_hw_cfg;
static std::atomic<bool> s_ap_active{false};

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
    s_wifi_disconnect_count.store(0, std::memory_order_relaxed);
    {
      IPAddress sta_ip(info.got_ip.ip_info.ip.addr);
      Serial.printf("[WIFI EVENT] STA Got IP: %u.%u.%u.%u\r\n",
                    sta_ip[0], sta_ip[1], sta_ip[2], sta_ip[3]);
    }
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_GOT_IP);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
    }
    if (g_system_event_group) {
      xEventGroupSetBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    WiFi.setSleep(false);
    configTime(0, 0, "pool.ntp.org", "asia.pool.ntp.org");
    setenv("TZ", "KST-9", 1);
    tzset();
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    {
      uint32_t disc_cnt =
          s_wifi_disconnect_count.fetch_add(1, std::memory_order_relaxed) + 1;
      Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                    info.wifi_sta_disconnected.reason, disc_cnt);
    }
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(s_wifi_event_group, WIFI_BIT_CONNECTED | WIFI_BIT_GOT_IP);
    }
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_NETWORK_READY);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    s_ap_active.store(true, std::memory_order_relaxed);
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
    s_ap_active.store(false, std::memory_order_relaxed);
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
  s_hw_cfg = cfg;
  if (!s_wifi_event_group) {
    s_wifi_event_group = xEventGroupCreateStatic(&s_wifi_event_group_buf);
  }
  WiFi.onEvent(onWifiEvent);

  const char *ssid = (cfg.sta_ssid && cfg.sta_ssid[0]) ? cfg.sta_ssid : "";
  const char *pass = cfg.sta_password ? cfg.sta_password : "";

  Serial.printf("[WIFI] Connecting to '%s' (Async Fast-Boot)...\r\n", ssid);
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
  constexpr uint32_t kBootFastGraceMs = 1500;
  bool connected = false;

  while (millis() - t_start < kBootFastGraceMs) {
    if ((connected = (WiFi.status() == WL_CONNECTED)))
      break;
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  if (connected) {
    WiFi.setSleep(false);
    IPAddress sta_ip = WiFi.localIP();
    Serial.printf("[WIFI] Fast-boot connected successfully! IP: %u.%u.%u.%u, RSSI: %d dBm\r\n",
                  sta_ip[0], sta_ip[1], sta_ip[2], sta_ip[3], WiFi.RSSI());
  } else {
    Serial.println(F("[WIFI] Fast-boot: STA association in progress asynchronously. Local buses starting immediately..."));
  }
}

void Wifi_Driver_StartFallbackAp() noexcept {
  bool expected = false;
  if (!s_ap_active.compare_exchange_strong(expected, true)) {
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  vTaskDelay(pdMS_TO_TICKS(100));

  WiFi.softAPConfig(IPAddress(172, 30, 2, 1), IPAddress(172, 30, 2, 1),
                    IPAddress(255, 255, 255, 0));
  const char *fallback_ap =
      (s_hw_cfg.ap_ssid && s_hw_cfg.ap_ssid[0]) ? s_hw_cfg.ap_ssid : "Sweet_Home_Rescue";
  const char *fallback_pass =
      (s_hw_cfg.ap_password && s_hw_cfg.ap_password[0]) ? s_hw_cfg.ap_password : "";
  bool ap_ok = WiFi.softAP(fallback_ap, fallback_pass, 1, 0, 4);

  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);
  IPAddress ap_ip = WiFi.softAPIP();
  Serial.printf("[WIFI] Fallback SoftAP '%s' started: %s (IP: %u.%u.%u.%u)\r\n",
                fallback_ap, ap_ok ? "SUCCESS" : "FAILED",
                ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]);
}

[[nodiscard]] bool Wifi_Driver_IsApActive() noexcept {
  return s_ap_active.load(std::memory_order_relaxed);
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

[[nodiscard]] uint32_t Wifi_Driver_GetDisconnectCount() noexcept {
  return s_wifi_disconnect_count.load(std::memory_order_relaxed);
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

void System_WifiStartFallbackAp() noexcept {
  Wifi_Driver_StartFallbackAp();
}

bool System_WifiIsApActive() noexcept {
  return Wifi_Driver_IsApActive();
}

// ── IP Subnet & Management Whitelist Filters (L1 Physical HAL) ────────────────
[[nodiscard]] bool Tcp_IsAllowedIP(IPAddress ip) noexcept {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (System_WifiIsConnected()) {
    IPAddress sta_ip = System_WifiGetIp();
    IPAddress sta_mask = System_WifiGetSubnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  IPAddress ap_ip = System_WifiGetApIp();
  if (ap_ip != IPAddress(0, 0, 0, 0)) {
    IPAddress ap_mask = System_WifiGetApSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

[[nodiscard]] bool Telnet_IsAllowedIP(IPAddress ip) noexcept {
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}
