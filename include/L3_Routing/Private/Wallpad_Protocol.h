#pragma once

// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================

#include "L0_Base/System_Config.h"
#include "L0_Base/System_Platform.h"
#include "L3_Routing/Public/Device_Registry.h"
#include "L3_Routing/Public/ProtocolDiagnostics.h"
#include "L3_Routing/Private/AutoProbingEngine.h"
#include "L3_Routing/Private/PollingRegistry.h"
#include "L3_Routing/Private/Wallpad_Parser.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <freertos/FreeRTOS.h>

// ============================================================================
// DEVICE SPECIFICATION (HARDCODED PER VENDOR)
// ============================================================================

struct DeviceSpec {
  uint8_t dev_id;
  DeviceClass dev_class;
  char name[16];

  // 제어 패킷 템플릿 (CTL)
  uint8_t ctl_len;
  uint8_t ctl_payload_offset; // 제어 파라미터(Cmd/온도 등) 바이트 오프셋 (현대:
                              // Byte #7)
  uint8_t pwr_on_val;         // 전원 ON 토큰 (0x01)
  uint8_t pwr_off_val;        // 전원 OFF 토큰 (0x02, 0x04 등)
  uint8_t pwr_away_val;       // 외출 모드 토큰 (0x07, 미사용 시 0xFF)

  // 쿼리 응답 상태 슬롯 (QRY ACK)
  uint8_t qry_ack_len;      // 쿼리 응답 패킷 전체 길이
  uint8_t qry_power_offset; // 운전/전원 상태 슬롯 (현대: Byte #8)
  uint8_t
      qry_settemp_offset; // 설정 희망온도 슬롯 (난방: Byte #10, 미사용 시 0xFF)
  uint8_t
      qry_ambtemp_offset; // 현재 환경온도 슬롯 (난방: Byte #9, 미사용 시 0xFF)
  uint8_t qry_fanspeed_offset; // 풍량 상태 슬롯 (환기: Byte #9, 미사용 시 0xFF)
  bool qry_fanspeed_nibble;    // 풍량 하위 4비트(buf & 0x0F) 마스킹 필요 여부
  uint8_t
      qry_valve_offset; // 밸브 차단 상태 슬롯 (가스: Byte #8, 미사용 시 0xFF)
  uint8_t qry_watt_h_offset; // 소비전력(W) 상위 바이트 (콘센트: Byte #9, 미사용
                             // 시 0xFF)
  uint8_t qry_watt_l_offset; // 소비전력(W) 하위 바이트 (콘센트: Byte #10,
                             // 미사용 시 0xFF)

  // 제어 응답 상태 슬롯 (CTL ACK)
  uint8_t ctl_ack_len;          // 제어 응답 패킷 전체 길이
  uint8_t ctl_ack_echo_offset;  // 제어 명령 에코 바이트 오프셋 (현대: Byte #7)
  uint8_t ctl_ack_state_offset; // 제어 후 확정 상태 슬롯 (현대: Byte #8)
  uint8_t ctl_ack_ambtemp_offset; // 제어 응답 내 현재온도 슬롯 (난방: Byte #9,
                                  // 미사용 시 0xFF)
};

// ============================================================================
// WALLPAD PROFILE DEFINITION
// ============================================================================

enum class WallpadVendorId : uint8_t {
  UNKNOWN = 0,
  HYUNDAI,
  KOCOM,
  BESTIN,
  COMMAX,
  EZVILLE,
  SAMSUNG
};

struct DoorphoneSpec {
  uint32_t baud_rate;
  uint8_t stx;
  uint8_t etx;
  uint8_t len;
  const char *desc;
  uint8_t bell_front;
  uint8_t bell_lobby;
  uint8_t call_front;
  uint8_t call_lobby;
  uint8_t open_front;
  uint8_t open_lobby;
  uint8_t end_front;
  uint8_t end_lobby;
};

struct WallpadProfile {
  WallpadVendorId vendor_id;
  const char *vendor_name;
  uint8_t stx;
  uint8_t etx;
  ChecksumAlgo checksum_algo;
  uint8_t opcode_offset;
  uint8_t dev_id_offset;
  uint8_t sub1_offset;
  uint8_t sub2_offset;
  const DeviceSpec *devices;
  size_t device_count;
  DoorphoneSpec doorphone;
};

extern const WallpadProfile kHyundaiProfile;
extern const WallpadProfile *const kWallpadProfiles[];
extern const size_t kWallpadProfileCount;


// ============================================================================
// From include/ProfileMatcher.h
// ============================================================================

namespace ProfileMatcher {

// 현재 수렴/잠금된 AutoProbeDescriptor를 기반으로 일치하는 제조사 프로파일을
// 검색합니다.
const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad);

// 현재 활성화/매칭된 프로파일 반환 (기본값: kHyundaiProfile)
const WallpadProfile *getActiveProfile();

// 도어폰 패킷 헤더 매칭
const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len);

} // namespace ProfileMatcher

// ============================================================================
// WALLPAD PROTOCOL ORCHESTRATION INTERFACE (Canonical L3 Protocol Engine)
// ============================================================================

enum class ControlAction : uint8_t {
  DROP = 0,
  TRANSMIT_LOCAL,       // CH1 물리 버스 송신
  FORWARD_CH5,          // EW11 TCP 송신
  VIRTUAL_ACK_IMMEDIATE // 캐시 즉각 가상 응답
};

// 1. Polling & Bus Packet Handling
bool Wallpad_BuildNextPollPacket(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                                 uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept;
void Wallpad_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                             const StaticPacket *matching_query) noexcept;
void Wallpad_HandlePollTimeout(uint8_t poll_dev_id, uint8_t poll_sub1,
                               uint8_t poll_sub2) noexcept;

// 2. Control Evaluation (Validation, Safety Gate, Virtual ACK)
ControlAction Wallpad_EvaluateControl(StaticPacket &req, StaticPacket &virtual_ack_out,
                                      bool &out_unidir) noexcept;

// 3. Timing & Convergence
uint32_t Wallpad_GetPollIntervalMs() noexcept;
bool Wallpad_CheckConvergence(bool reset) noexcept;

// 4. Stream Framing & Parsing SPI
uint8_t Wallpad_GetStx() noexcept;
bool Wallpad_IsAutoUnlocked() noexcept;
void Wallpad_FeedAutoFrame(span<const uint8_t> frame) noexcept;
int Wallpad_ExtractLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept;
bool Wallpad_ValidatePacket(span<const uint8_t> frame) noexcept;

// 5. Sub-bus (CH2/CH3) Query & Control
bool Wallpad_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                               StaticPacket &virtual_ack_out) noexcept;
void Wallpad_FeedControlFrame(span<const uint8_t> frame) noexcept;

// 6. Doorphone (CH4) Handling & FSM (L3 Canonical)
// FramingStatus and FramingTracker are defined in ProtocolDiagnostics.h / Device_Registry.h


void Wallpad_DoorphoneInit() noexcept;
bool Wallpad_DoorphoneOpen(bool is_lobby = false) noexcept;
void Wallpad_InitDecoupledHooks() noexcept;
bool Wallpad_DoorphoneStartSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                    uint8_t op_open, uint8_t op_end) noexcept;
void Wallpad_DoorphoneCancel() noexcept;
[[nodiscard]] bool Wallpad_DoorphoneIsBusy() noexcept;
void Wallpad_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept;
void Wallpad_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                uint8_t &out_etx, uint8_t &out_len) noexcept;
bool Wallpad_DoorphoneGetLockedFraming(uint8_t &stx, uint8_t &etx, uint8_t &len) noexcept;
void Wallpad_DoorphoneFrameDetected(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
void Wallpad_DoorphoneCheckBellTimeout() noexcept;
void Wallpad_DoorphoneClearNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneRestoreNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneSaveNvs(const char *nvs_ns) noexcept;
void Wallpad_DoorphoneOnProfileChanged(uint8_t old_idx, uint8_t new_idx) noexcept;

using DoorphoneTxHandler = void (*)(const StaticPacket &pkt) noexcept;
void Wallpad_DoorphoneRegisterTxHandler(DoorphoneTxHandler handler) noexcept;

void Wallpad_HandleDoorphonePacket(const StaticPacket &packet) noexcept;
void Wallpad_ResetDoorphoneBellState() noexcept;
const DoorphoneSpec *Wallpad_MatchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) noexcept;
bool Wallpad_MatchDoorphoneLock(uint8_t stx, uint8_t etx, uint8_t len, uint8_t &out_fixed_len) noexcept;

// 7. Packet Type Inspector
bool Wallpad_IsQueryPacket(span<const uint8_t> frame) noexcept;
