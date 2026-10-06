// ============================================================================
// TcpReactor: Level 2 Transport Network Event Reactor Implementation
// ============================================================================

#include "L2_Transport/TCP_CH.h"
#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"

#include <ArduinoOTA.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <sys/time.h>

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
    System_FeedWdt(Config::Task::WDT_ID_NET);
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
    System_FeedWdt(Config::Task::WDT_ID_NET);

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
