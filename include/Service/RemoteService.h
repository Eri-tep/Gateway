#pragma once

// ============================================================================
// RemoteService: Level 4 Network Remote Services (SmartThings JSON-RPC & EW11 Hub)
// ============================================================================

#include "Service/EngineTask.h"
#include <lwip/sockets.h>

// ============================================================================
// 1. Core Runtime Timing Configuration (NVS Stored)
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
// 2. HTTP(S) Cloud OTA Status
// ============================================================================
struct HttpOtaState {
  std::atomic<bool> in_progress{false};
  char status[64]{"Idle"};
  uint8_t progress_pct{0};
  char last_error[64]{""};
};

extern HttpOtaState g_http_ota_state;

static constexpr const char *DEFAULT_CLOUD_OTA_URL =
    "https://raw.githubusercontent.com/Eri-tep/Gateway/main/bin/firmware.bin";
void Mgmt_StartHttpOta(const char *url);

// ============================================================================
// 3. Port 8900 Management TCP Session Structure
// ============================================================================
struct MgmtSession {
  int sock{-1};
  uint8_t buffer[Config::TCP::MGMT_BUFFER_SIZE];
  size_t len{0};
  uint32_t connected_at_ms{0};
};

extern MgmtSession g_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];

// ============================================================================
// 4. Management JSON-RPC Functions
// ============================================================================
void Mgmt_Init();
void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len);
void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id = -1);
void Mgmt_SerializeDevices(AppendBuf &out, long req_id = -1);
void Mgmt_DispatchJsonRpc(int sock, const char *json_str);

void Mgmt_BroadcastDoorphoneEvent(bool front_bell, bool lobby_bell);
void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp = 0, int current_temp = 0,
                               int speed = 0, const char *valve_state = nullptr,
                               float power_w = 0.0f, int floor = 0,
                               int direction = 0, int ho = 0,
                               int vent_mode = 1);
void Mgmt_BroadcastDevicesUpdated();
void Mgmt_BroadcastRawJson(const char *json_payload);

// ============================================================================
// 5. EW11 TCP Bridge & Air Conditioner (FCU) Subsystem (from Bridge)
// ============================================================================
namespace Fcu {

enum class Mode : uint16_t { Cool = 1, Heat = 2, FanOnly = 3 };

enum class FanSpeed : uint16_t {
  Off = 0, // 정지 (30~40초 지연 정지 트리거)
  Low = 1,
  Mid = 2,
  High = 3,
  Auto = 4
};

enum class Swing : uint16_t {
  Off = 0,
  On = 2 // 값 1은 예약/미사용, 스윙ON은 반드시 2
};

struct Snapshot {
  Mode mode{Mode::Cool};
  FanSpeed fan_speed{FanSpeed::Off};
  Swing swing{Swing::Off};
  uint8_t error_code{0};
  uint8_t target_temp{24}; // 희망 설정 온도 (℃)
  uint8_t room_temp{0};    // 실내 측정 온도 (℃)
  bool power{false};       // fan_speed != FanSpeed::Off
};

struct SlotRuntime {
  Snapshot snap{};
  uint32_t last_poll_ms{0};
  uint32_t query_sent_ms{0};
  uint8_t timeout_count{0};
  bool waiting_response{false};
  bool is_online{false};
  Mode last_active_mode{Mode::Cool};       // 기록 없을 시 안전 기본 냉방
  FanSpeed last_active_fan{FanSpeed::Low}; // 기록 없을 시 기본 약풍
  Swing last_active_swing{Swing::Off};     // 기록 없을 시 기본 고정
  bool has_active_record{false};           // 냉방/난방 운전 이력 여부
  uint32_t next_tx_ms{0};                  // 120ms 논블로킹 가드타임 만료 시각
  uint8_t pending_temp{0};                 // 120ms 후 전송할 대기 목표온도
  bool has_pending_temp{false};            // 온도 패킷 전송 대기 여부
  uint8_t pending_cmd_buf[16]{}; // RS-485 Stop-and-Wait 대기 명령 버퍼
  uint8_t pending_cmd_len{0};    // 대기 중인 명령 패킷 길이
  uint8_t pending_restore_swing{0}; // 전원 켜기 복원 시 스윙값
};

// ── 외부 공개 제어 API ──
bool SetPower(uint8_t slot_idx, bool on);
bool RestorePower(uint8_t slot_idx, uint16_t mode, uint16_t fan, uint16_t swing,
                  uint8_t temp);
bool SetMode(uint8_t slot_idx, Mode m);
bool SetFanSpeed(uint8_t slot_idx, FanSpeed f);
bool SetSwing(uint8_t slot_idx, Swing s);
bool SetTargetTemp(uint8_t slot_idx, uint8_t temp_c);
bool GetSlotRuntime(uint8_t slot_idx, SlotRuntime &out_rt);

void handleSlotLoop(uint8_t slot_idx, struct HubClientSlot *slot, uint32_t now);
void handleSlotRx(uint8_t slot_idx, const uint8_t *data, size_t len);

} // namespace Fcu

namespace Ew11Manager {
void init();
void processPacket(int slot_idx, const uint8_t *pkt_data, size_t pkt_len);
void processStream(int slot_idx, struct HubClientSlot *slot);
bool sendBurstPacket(uint8_t slot_idx, const StaticPacket &pkt,
                     uint8_t count = 2, uint32_t silence_ms = 20);
} // namespace Ew11Manager

struct HubClientSlot;
int Hub_AcceptClient(int slot_idx, int server_fd);
void Hub_ProcessPacket(HubClientSlot *slot, const uint8_t *pkt_data,
                       size_t pkt_len);
void Hub_Data(HubClientSlot *slot, const uint8_t *data, size_t len);

// ── Network Subsystem Entry Points ──
void Task_Network(void *pvParameters);
extern EventGroupHandle_t g_wifi_event_group;
