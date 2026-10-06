#pragma once

// ============================================================================
// ControlTemplate: Level 3 Device Capability Blueprint & Group Control Engine
// ============================================================================

#include "L0_Foundation/System_Config.h"
#include "L0_Foundation/System_Platform.h"
#include "L3_Protocol/Public/Device_Registry.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============================================================================
// GROUP CONTROL TEMPLATE (CONTROL BLUEPRINT)
// ============================================================================

struct PacketSnapshot {
  uint8_t len{0};
  uint8_t raw[32]{0};
};

struct AckStateSlots {
  bool discovered{false};
  uint8_t power_offset{0xFF};        // ACK 내 전원/가동 상태 위치 [AS]
  uint8_t target_temp_offset{0xFF};  // ACK 내 설정 희망온도 위치 [TT]
  uint8_t current_temp_offset{0xFF}; // ACK 내 현재 환경온도 위치 [AT]
  uint8_t fan_speed_offset{0xFF};    // ACK 내 풍량 상태 위치 [FS]
  uint8_t valve_state_offset{0xFF};  // ACK 내 밸브 차단 상태 위치 [VS]
  uint8_t sample_count{0};
  uint32_t power_changed_mask{0};
};

struct QueryStateSlots {
  bool discovered{false};
  uint8_t expected_len{0}; // 쿼리 응답 패킷 길이 (예: 콘센트 18B, 난방 18B 등)
  uint8_t power_offset{0xFF};        // 평상시 전원/가동 상태 바이트 오프셋 [AS]
  uint8_t target_temp_offset{0xFF};  // 희망 설정온도 오프셋 [TT]
  uint8_t current_temp_offset{0xFF}; // 현재 환경온도 오프셋 [AT]
  uint8_t fan_speed_offset{0xFF};    // 환기 풍량 오프셋 [FS]
  uint8_t power_w_offset{0xFF};      // 콘센트 실시간 소비전력(W) 오프셋
  uint8_t valve_state_offset{0xFF};  // 가스 차단 상태 오프셋 [VS]
  bool is_bitmap_power{false};       // 다채널 비트맵 전원 여부
};

struct GroupControlTemplate {
  uint8_t dev_id{0x00}; // 기기 그룹 코드 (예: 0x19 조명, 0x18 난방 등)
  char group_name[16]{"Unknown"}; // 그룹 명칭 ("Light", "Thermo", "Vent" 등)
  uint8_t frame_len{0};           // 제어 패킷 프레임 길이 (11, 13 등)
  uint8_t raw_template[32]{0};    // 기본 제어 프레임 골격

  // 주소 마스킹 오프셋
  uint8_t sub1_offset{0xFF};       // 방 번호(Sub1) 주입 오프셋
  uint8_t sub2_offset{0xFF};       // 기기 번호(Sub2) 주입 오프셋
  uint8_t ctl_sub1_override{0xFF}; // 전열교환기 등 특수 sub1 고정값 (0x40 등)

  // 기능별 액션 슬롯
  ActionSlot power_slot; // 전원 제어 슬롯
  ActionSlot temp_slot;  // 온도 제어 슬롯 (난방)
  ActionSlot speed_slot; // 풍량 제어 슬롯 (환기)
  ActionSlot mode_slot;  // 운전 모드 슬롯
  ActionSlot close_slot; // 닫기 제어 슬롯 (가스)

  SlotCoverage coverage;         // 슬롯 매핑 정보
  uint8_t away_mode_token{0xFF}; // 외출 시 전원/모드 바이트 코드 (현대: 0x07)

  // ACK 상태 슬롯 (제어 트랜잭션 응답)
  AckStateSlots ack_slots;

  // 쿼리 상태 슬롯 (수기/명세 주입)
  QueryStateSlots query_slots;

  // ── 통합 슬롯 접근자 (Pure Declarations) ──
  uint8_t getPowerOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getTargetTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getCurrentTempOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getFanSpeedOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t decodeFanSpeed(uint8_t raw_token) const noexcept;
  uint8_t decodeVentMode(uint8_t raw_byte) const noexcept;
  uint8_t getValveStateOffset(uint8_t pkt_len = 0) const noexcept;
  uint8_t getWattageOffset(uint8_t pkt_len = 0) const noexcept;
  bool isUnidirectional() const noexcept;
};

static_assert(sizeof(GroupControlTemplate) == 180,
              "NVS ABI break: GroupControlTemplate size changed");

// ============================================================================
// CONTROL TEMPLATE REGISTRY
// ============================================================================

class ControlTemplateRegistry {
public:
  static constexpr size_t MAX_GROUPS = 8;
  static constexpr TickType_t kQueryLockTimeout = pdMS_TO_TICKS(5);
  static constexpr TickType_t kManageLockTimeout = pdMS_TO_TICKS(50);

  ControlTemplateRegistry();

  void init();
  void clear();

  // 수렴 완료 시점 자동 골격 합성 (ProfileMatcher 연계)
  void synthesizeFromConvergedCache();

  // 그룹 등록 및 조회
  bool findGroup(uint8_t dev_id, GroupControlTemplate &out,
                 TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupsSnapshot(GroupControlTemplate *out_buf, size_t max_count,
                           TickType_t timeout = kQueryLockTimeout) const;
  size_t getGroupCount() const;
  bool getGroupByIndex(size_t index, GroupControlTemplate &out) const;
  bool resetGroup(uint8_t dev_id, bool full_reset = false);
  bool setGroupName(uint8_t dev_id, const char *name);
  bool setGroupClass(uint8_t dev_id, DeviceClass cls,
                     const char *name = nullptr);

  template <typename Func>
  bool modifyOrCreateGroup(uint8_t dev_id, Func &&mutator,
                           const char *initial_name = nullptr,
                           TickType_t timeout = kManageLockTimeout) {
    if (dev_id == 0)
      return false;
    MutexLocker lock(_mutex, timeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *grp = registerOrTouchUnlocked(dev_id, initial_name);
    if (!grp)
      return false;
    mutator(*grp);
    return true;
  }

  // 제어 패킷 조립 (스마트싱스 및 외부 연동 공용)
  bool buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          ControlActionType action, int value,
                          StaticPacket &out) const;

  // L2.3 DeviceRepository decoupled unit count hook
  using DeviceUnitCountFn = size_t (*)(uint8_t dev_id);
  static void setDeviceUnitCountProvider(DeviceUnitCountFn fn);

  // 제조사 프로파일 명세 기반 슬롯 주입
  void applyProfile(const struct WallpadProfile *profile);
  void matchAndInject(const struct AutoProbeDescriptor &ad);

  // NVS 저장 / 복원 (프로파일 격리 지원)
  bool saveToNvs();
  void loadFromNvs();
  bool saveToNvsForProfile(uint8_t prof_idx);
  void loadFromNvsForProfile(uint8_t prof_idx);
  void onProfileChanged(uint8_t old_prof_idx, uint8_t new_prof_idx);

private:
  GroupControlTemplate _groups[MAX_GROUPS];
  size_t _group_count{0};
  mutable StaticSemaphore_t _mutex_storage{};
  mutable SemaphoreHandle_t _mutex{nullptr};
  mutable StaticSemaphore_t _nvs_mutex_storage{};
  mutable SemaphoreHandle_t _nvs_mutex{nullptr};

  void autoAssignGroupName(GroupControlTemplate &group);
  GroupControlTemplate *registerOrTouchUnlocked(uint8_t dev_id,
                                                const char *name = nullptr);
};

extern ControlTemplateRegistry g_control_registry;

namespace ControlTemplateUtils {
inline void getControlNamespace(char *out_ns, size_t max_len,
                                uint8_t prof_idx) {
  snprintf(out_ns, max_len, "ctl_p%u", prof_idx);
}

inline uint8_t getCurrentProfileIndex() {
  return Config_GetWallpadProfile();
}
} // namespace ControlTemplateUtils

void ControlTemplate_DecodeDeviceState(const GroupControlTemplate &grp,
                                       const StaticPacket &ack,
                                       const DeviceStateEntry *dev,
                                       DecodedDeviceState &out) noexcept;

bool ControlTemplate_DecodeByDevId(uint8_t dev_id,
                                   const StaticPacket &ack,
                                   const DeviceStateEntry *dev,
                                   DecodedDeviceState &out) noexcept;

uint8_t ControlTemplate_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept;


