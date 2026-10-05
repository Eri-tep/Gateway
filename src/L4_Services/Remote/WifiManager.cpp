// ============================================================================
// RemoteService: Level 4 Network Remote Services
// ============================================================================

#include "L4_Services/Remote/RemoteInternal.h"
#include "L4_Services/EW11_Service.h"
#include "L4_Services/ST_Service.h"
#include "L3_Routing/Wallpad_Protocol.h"
#include "L1_Drivers/Diagnostics_Driver.h"
#include "L1_Drivers/NVS_Driver.h"
#include "L2_Channels/TCP_CH.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lwip/ip.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

EventGroupHandle_t g_wifi_event_group = nullptr;
static uint32_t s_wifi_disconnect_count = 0;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
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
    }
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    s_wifi_disconnect_count++;
    Serial.printf("[WIFI EVENT] STA Disconnected (Reason: %d, Count: %u)\r\n",
                  info.wifi_sta_disconnected.reason, s_wifi_disconnect_count);
    if (g_wifi_event_group) {
      xEventGroupSetBits(g_wifi_event_group, WIFI_BIT_DISCONNECTED);
      xEventGroupClearBits(g_wifi_event_group, WIFI_BIT_CONNECTED);
    }
    break;
  case ARDUINO_EVENT_WIFI_AP_START:
    Serial.println(F("[WIFI EVENT] SoftAP Started"));
    break;
  case ARDUINO_EVENT_WIFI_AP_STOP:
    Serial.println(F("[WIFI EVENT] SoftAP Stopped"));
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
