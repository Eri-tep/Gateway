#pragma once

#include "Common.h"

// ============================================================================
// 3대 핵심 런타임 타이밍 구조체 (NVS 영구 보관)
// ============================================================================
struct RuntimeTimingConfig {
  uint16_t ch1_poll_interval_ms{1000}; // CH1 폴링 주기 (200~3000ms, 기본 1000ms)
  uint16_t ch2_cache_delay_ms{30};     // CH2 메인 월패드 Virtual ACK 딜레이 (10~150ms, 기본 30ms)
  uint16_t ch3_cache_delay_ms{240};    // CH3 서브 월패드 Virtual ACK 딜레이 (50~500ms, 기본 240ms)
};

extern RuntimeTimingConfig g_timing_config;

void TimingConfig_Load();
void TimingConfig_Save();

// ============================================================================
// HTTP(S) Cloud OTA 상태 구조체
// ============================================================================
struct HttpOtaState {
  std::atomic<bool> in_progress{false};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

extern HttpOtaState g_http_ota_state;

void Mgmt_StartHttpOta(const char *url);

// ============================================================================
// 포트 8900 관리 TCP 세션 구조체
// ============================================================================
struct MgmtSession {
  int sock{-1};
  uint8_t buffer[512];
  size_t len{0};
  uint32_t connected_at_ms{0};
};

extern MgmtSession g_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];

// ============================================================================
// 관리 JSON-RPC 함수 인터페이스
// ============================================================================
void Mgmt_Init();
void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len);
void Mgmt_SerializeTelemetry(AppendBuf &out);
void Mgmt_DispatchJsonRpc(int sock, const char *json_str);
