#pragma once

// ============================================================================
// SystemOta: Level 1 System-Level Cloud & HTTP(S) Firmware OTA Engine
// ============================================================================

#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

enum class OtaStage : uint8_t {
  Idle = 0,
  Connecting,
  Downloading,
  Flashing,
  Success,
  Failed
};

struct HttpOtaState {
  std::atomic<bool> in_progress{false};
  std::atomic<OtaStage> stage{OtaStage::Idle};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

static constexpr const char *DEFAULT_CLOUD_OTA_URL =
    "https://raw.githubusercontent.com/Eri-tep/Gateway/main/bin/firmware.bin";

// Cloud/HTTP(S) OTA 비동기 시작 함수
void System_StartHttpOta(const char *url);

// OTA 시작 전 네트워크/소켓 정리 훅 등록
using PreOtaHookFn = void (*)() noexcept;
void SystemOta_RegisterPreOtaHook(PreOtaHookFn hook) noexcept;

// ArduinoOTA 포트 및 콜백 수명주기 초기화 함수
void SystemOta_InitArduinoOta(const char *hostname, const char *password);

// ArduinoOTA 백그라운드 패킷 핸들러 폴링 함수
void SystemOta_Handle() noexcept;
