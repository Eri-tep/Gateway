#pragma once

#include <Arduino.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "Common.h"
#include "WallpadParser.h"

// ============================================================================
// CONTROL ACTION TYPES & SLOTS
// ============================================================================

enum class ControlActionType : uint8_t {
  POWER = 0,    // 전원 ON / OFF
  SET_TEMP,     // 설정 온도 변경 (난방/에어컨)
  FAN_SPEED,    // 풍량 변경 (환기/에어컨)
  VALVE_CLOSE,  // 밸브 닫기 (가스)
  MOMENTARY_TRIGGER, // 순간 호출 (엘리베이터 등)
  UNKNOWN = 0xFF
};

struct ActionSlot {
  bool discovered{false};
  uint8_t category_offset{0xFF}; // 카테고리/서브1 위치 (현대 0x45, 0x46 등)
  uint8_t category_val{0x00};    // 해당 액션의 카테고리 바이트 값
  uint8_t action_offset{0xFF};   // 제어 파라미터가 위치하는 바이트 오프셋
  uint8_t on_val{0x01};          // ON / Active 토큰
  uint8_t off_val{0x02};         // OFF / Inactive 토큰
  uint8_t min_val{0};            // 최소값 (온도 15℃, 풍량 1 등)
  uint8_t max_val{0};            // 최대값 (온도 30℃, 풍량 3 등)
  uint16_t sample_count{0};      // 관측/검증 횟수
};

// ============================================================================
// DEVICE CAPABILITY CLASSIFICATION & SLOT COVERAGE
// ============================================================================

enum class DeviceClass : uint8_t {
  UNKNOWN = 0,
  SWITCH,     // 지속 릴레이 (ON/OFF) - 조명, 콘센트, 일괄소등
  GAS,        // 차단 밸브 (단방향 닫기 / 차단) - 가스 밸브
  MOMENTARY,  // 단방향 순간 펄스 트리거 (호출) - 엘리베이터 호출, 현관문 열림
  THERMOSTAT, // 연속 희망온도 파라미터 (14~36℃ 2개 슬롯) - 난방
  VENT,       // 이산 다단계 풍량 파라미터 (1~3단) - 환기
  AIRCON      // 온도 + 풍량 복합 파라미터 - 에어컨
};

struct SlotCoverage {
  DeviceClass dev_class{DeviceClass::UNKNOWN};
  bool power_on_seen{false};
  bool power_off_seen{false};
  bool temp_set_seen{false};
  bool away_mode_seen{false};
  // ── 복합 상태 슬롯 (THERMOSTAT 전용, 필수)
  bool temp_while_off_seen{false};   // 꺼진 상태 온도 변경 → 켜기+온도 복합 패턴
  bool temp_while_away_seen{false};  // 외출 모드 온도 변경 → 제조사별 상이한 응답 패턴
  // ── 환기 / 에어컨 풍량
  bool speed_l1_seen{false};
  bool speed_l2_seen{false};
  bool speed_l3_seen{false};
  // ── 순간 펄스 (엘리베이터)
  bool call_seen{false};
  // ── 가스 / 차단
  bool valve_close_seen{false};
  // ── 콘센트
  bool telemetry_masked{false};    // 전력량 오프셋 마스킹 완료
  uint8_t observation_count{0};    // 총 관측 횟수

  static DeviceClass classify(uint8_t dev_id, const AutoProbeDescriptor &ad);
  bool isFullyCovered() const;
};

// ============================================================================
// GROUP CONTROL TEMPLATE (CONTROL BLUEPRINT)
// ============================================================================

struct GroupControlTemplate {
  uint8_t dev_id{0x00};          // 기기 그룹 코드 (예: 0x19 조명, 0x18 난방 등)
  char group_name[16]{"Unknown"};// 그룹 명칭 ("Light", "Thermo", "Vent" 등)
  uint8_t frame_len{0};          // 제어 패킷 프레임 길이 (11, 12, 21 등)
  uint8_t raw_template[32]{0};   // 기본 제어 프레임 골격
  
  // 주소 마스킹 오프셋
  uint8_t sub1_offset{0xFF};     // 방 번호(Sub1) 주입 오프셋
  uint8_t sub2_offset{0xFF};     // 기기 번호(Sub2) 주입 오프셋
  uint8_t ctl_sub1_override{0xFF};// 전열교환기 등 특수 sub1 고정값 (0x40 등)
  
  // 기능별 액션 슬롯
  ActionSlot power_slot;         // 전원 제어 슬롯
  ActionSlot temp_slot;          // 온도 제어 슬롯 (난방)
  ActionSlot speed_slot;         // 풍량 제어 슬롯 (환기)
  ActionSlot close_slot;         // 닫기 제어 슬롯 (가스)
  
  SlotCoverage coverage;         // 슬롯 완전성 매트릭스
  uint8_t volatile_mask[32]{0};  // 콘센트 텔레메트리 마스킹 비트맵
  uint8_t away_temp_behavior{0}; // 0: 미정, 1: 해제+온도 복합, 2: 외출유지 예약, 3: 무시
  uint8_t away_fixed_temp{0xFF}; // 외출 시 고정되는 설정온도 (예: 10℃)
  bool away_has_dedicated_temp{false}; // 외출 시 특정 온도로 고정 여부
  bool temp_recall_verified{false};    // 켜기/외출해제 시 저장된 온도로 자동 복원 검증 완료

  // 학습 진행 상태
  enum class Status : uint8_t {
    EMPTY = 0,     // 기기 미등록
    WAITING,       // 기기 그룹 등록됨, 제어 패킷 대기 중
    CAPTURING,     // 제어 패킷 관측 시작
    PARTIAL,       // ACK_before 없어 골격만 부분 학습
    PROBING,       // 능동 검증(Active Probing) 진행 중
    VERIFIED       // 슬롯 완전 검증 완료 (LOCKED)
  } status{Status::EMPTY};

  uint32_t last_learned_ms{0};   // 마지막 학습 시각
};

// ============================================================================
// ACTIVE PROBING LEARNING SESSION (EXTENDED FSM)
// ============================================================================

enum class ActiveProbingStep : uint8_t {
  IDLE = 0,
  PREFLIGHT_CHECK,        // 수렴 완료·캐시 유효성 확인 (offsets_locked 필수)
  SNAPSHOT_BASELINE,      // 2차 캐시에서 기준 상태 스냅샷

  // ── 전원 검증 루프
  PROBE_POWER_ON,         // 전원 ON 패킷 송신
  VERIFY_POWER_ON_ACK,    // ACK 수신 → power_on_seen 마킹
  PROBE_POWER_OFF,        // 전원 OFF 패킷 송신
  VERIFY_POWER_OFF_ACK,   // ACK 수신 → power_off_seen 마킹

  // ── 온도 검증 루프 (THERMOSTAT)
  PROBE_TEMP_L1,          // 온도 최솟값(15℃) 설정
  VERIFY_TEMP_L1_ACK,     // ACK → temp_set_seen 마킹
  PROBE_TEMP_L2,          // 온도 테스트값(25℃) 설정 (기준 타깃온도)
  VERIFY_TEMP_L2_ACK,     // ACK → 25℃ 확인 (기준온도 주입)
  PROBE_AWAY_MODE,        // 외출 모드 진입
  VERIFY_AWAY_ACK,        // ACK → away_mode_seen & 외출 고정온도(예: 10℃) 감지
  PROBE_RECALL_CHECK,     // 순수 켜기 송출 (외출 고정온도에서 원래 25℃로 복원되는지 확인)
  VERIFY_RECALL_ACK,      // ACK → 25℃ 자동 복원 확인 (temp_recall_verified)
  PROBE_TEMP_WHILE_OFF,   // 꺼진 상태에서 온도 변경 → "켜기+온도 복합" 검출
  VERIFY_TEMP_WHILE_OFF_ACK, // ACK → temp_while_off_seen 마킹
  PROBE_TEMP_WHILE_AWAY,     // 외출 상태에서 온도 변경 → 제조사별 복합 응답 검출
  VERIFY_TEMP_WHILE_AWAY_ACK,// ACK → temp_while_away_seen 마킹

  // ── 풍량 검증 루프 (VENT)
  PROBE_SPEED_L1,         // 풍량 1단
  VERIFY_SPEED_L1_ACK,    // ACK → speed_l1_seen
  PROBE_SPEED_L2,         // 풍량 2단
  VERIFY_SPEED_L2_ACK,
  PROBE_SPEED_L3,         // 풍량 3단
  VERIFY_SPEED_L3_ACK,

  // ── 가스 검증 (GAS)
  PROBE_VALVE_CLOSE,      // 밸브 닫기
  VERIFY_VALVE_CLOSE_ACK,

  // ── 공통 종료 루프
  RESTORE_BASELINE,       // 기준 상태 복원
  VERIFY_RESTORE_ACK,     // 복원 ACK 확인
  COMPLETED,              // SlotCoverage 통과 → VERIFIED
  FAILED                  // 실패
};

struct ActiveLearningSession {
  bool in_progress{false};
  bool learn_all{false};
  size_t current_all_idx{0};
  uint8_t target_dev_id{0};
  uint8_t target_sub1{0};
  uint8_t target_sub2{0};
  ActiveProbingStep current_step{ActiveProbingStep::IDLE};
  
  uint8_t retry_count{0};
  uint8_t candidate_offset{0};
  uint8_t candidate_token{0};
  
  uint8_t baseline_ack[32]{0};
  uint8_t baseline_len{0};
  
  uint32_t step_start_ms{0};
  char last_log[80]{"Idle"};
};

// ============================================================================
// CONTROL TEMPLATE REGISTRY
// ============================================================================

class ControlTemplateRegistry {
public:
  static constexpr size_t MAX_GROUPS = 8;

  ControlTemplateRegistry();

  void init();
  void clear();

  // 수렴 완료 시점 자동 골격 합성
  void synthesizeFromConvergedCache();

  // 그룹 등록 및 조회
  GroupControlTemplate *findGroup(uint8_t dev_id);
  const GroupControlTemplate *findGroup(uint8_t dev_id) const;
  GroupControlTemplate *registerOrTouch(uint8_t dev_id, const char *name = nullptr);
  size_t getGroupCount() const;
  bool getGroupByIndex(size_t index, GroupControlTemplate &out) const;
  bool resetGroup(uint8_t dev_id);
  bool setGroupName(uint8_t dev_id, const char *name);

  // 패시브 삼각 차분 분석 (Triplet Differential Sniffer)
  void onControlTransaction(const StaticPacket &ctl,
                            const StaticPacket &ack_before,
                            const StaticPacket &ack_after);

  // 제어 패킷 조립 (가설 시험 및 추후 연동 공용)
  bool buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          ControlActionType action, int value,
                          StaticPacket &out) const;

  // 능동 학습 세션 제어
  bool startActiveLearning(uint8_t dev_id);
  void processActiveLearning();
  void abortActiveLearning();
  const ActiveLearningSession &getSession() const { return _session; }

  // NVS 저장 / 복원
  void saveToNvs();
  void loadFromNvs();

private:
  GroupControlTemplate _groups[MAX_GROUPS];
  size_t _group_count{0};
  ActiveLearningSession _session;
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

  void autoAssignGroupName(GroupControlTemplate &group);
  bool setupTargetForProbing(uint8_t dev_id);
};

extern ControlTemplateRegistry g_control_registry;
