#pragma once

// ============================================================================
// SystemOta: Level 1 System-Level Cloud & HTTP(S) Firmware OTA Engine
// ============================================================================

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct HttpOtaState {
  std::atomic<bool> in_progress{false};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

extern HttpOtaState g_http_ota_state;

static constexpr const char *DEFAULT_CLOUD_OTA_URL =
    "https://raw.githubusercontent.com/Eri-tep/Gateway/main/bin/firmware.bin";

// Cloud/HTTP(S) OTA 비동기 시작 함수
void System_StartHttpOta(const char *url);
