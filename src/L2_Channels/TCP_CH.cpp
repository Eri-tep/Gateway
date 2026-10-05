// ============================================================================
// TcpReactor: Level 2 Transport Network Event Reactor Implementation
// ============================================================================

#include "L2_Channels/TCP_CH.h"
#include "L0_Base/System_Config.h"
#include "L0_Base/System_Platform.h"
#include "L1_Drivers/Diagnostics_Driver.h"
#include "L1_Drivers/SystemOta.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

// ── IP Subnet Whitelist Filters ───────────────────────────────────────────────

bool Tcp_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

bool Telnet_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}

namespace Transport {

static ReactorParticipant s_participants[TcpReactor::MAX_PARTICIPANTS]{};
static size_t s_participant_count = 0;

bool TcpReactor::registerParticipant(const ReactorParticipant &p) noexcept {
  if (s_participant_count >= MAX_PARTICIPANTS || !p.populateFds || !p.processEvents) {
    return false;
  }
  s_participants[s_participant_count++] = p;
  return true;
}

void TcpReactor::shutdownAll() noexcept {
  // Graceful notification / participants cleanup can be called if needed
}

void TcpReactor::runTask(void * /*pvParameters*/) {
  esp_task_wdt_add(nullptr);

  for (;;) {
    esp_task_wdt_reset();
    g_wdt_monitor.feed(4);
    ArduinoOTA.handle();

    const bool ota_now = g_ota_in_progress.load(std::memory_order_relaxed);
    const uint32_t now_ms = millis();

    System_CheckOtaHealth();

    fd_set readfds, errorfds;
    FD_ZERO(&readfds);
    FD_ZERO(&errorfds);
    int max_fd = -1;

    for (size_t i = 0; i < s_participant_count; ++i) {
      if (s_participants[i].populateFds) {
        s_participants[i].populateFds(readfds, errorfds, max_fd);
      }
    }

    int act = 0;
    struct timeval tv = {0, 10000}; // 10ms
    if (max_fd >= 0) {
      act = select(max_fd + 1, &readfds, nullptr, &errorfds, &tv);
      if (ota_now) {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_task_wdt_reset();
    g_wdt_monitor.feed(4);

    if (act > 0) {
      for (size_t i = 0; i < s_participant_count; ++i) {
        if (s_participants[i].processEvents) {
          s_participants[i].processEvents(readfds, errorfds, ota_now);
        }
      }
    }

    for (size_t i = 0; i < s_participant_count; ++i) {
      if (s_participants[i].tick) {
        s_participants[i].tick(ota_now, now_ms);
      }
    }
  }
}

} // namespace Transport
