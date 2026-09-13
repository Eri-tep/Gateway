#include "ControlTemplate.h"
#include "WallpadParser.h"
#include <Preferences.h>
#include <algorithm>

ControlTemplateRegistry g_control_registry;

// ============================================================================
// SLOT COVERAGE IMPLEMENTATION
// ============================================================================

DeviceClass SlotCoverage::classify(uint8_t dev_id, const AutoProbeDescriptor &ad) {
  (void)dev_id;
  (void)ad;
  // 1차/2차 캐시에서 사전 기기 종류 추측을 완전히 배제합니다.
  // 모든 기기는 UNKNOWN(-)으로 시작하며, 'ctl learn' 대화형 러닝을 통해서만 확정합니다.
  return DeviceClass::UNKNOWN;
}

bool SlotCoverage::isFullyCovered() const {
  switch (dev_class) {
  case DeviceClass::GAS:
    return valve_close_seen || power_off_seen;

  case DeviceClass::SWITCH:
    return (power_on_seen && power_off_seen) || valve_close_seen;

  case DeviceClass::MOMENTARY:
    return call_seen || valve_close_seen;

  case DeviceClass::THERMOSTAT:
    // 필수 제어 슬롯(ON, OFF, 온도설정, 재가동 복원 검증) 100% 충족 시에만 완전 검증 수렴
    return (power_on_seen && power_off_seen && temp_set_seen && temp_recall_seen);

  case DeviceClass::VENT:
    // 전원 ON, OFF 뿐만 아니라 최소 2개 이상의 풍량 레벨(L1, L2) 관측 완료되어야 완전 검증 수렴
    return (power_on_seen && power_off_seen && speed_l1_seen && speed_l2_seen);

  case DeviceClass::AIRCON:
    return (power_on_seen && power_off_seen && temp_set_seen &&
            speed_l1_seen && speed_l2_seen);

  case DeviceClass::UNKNOWN:
  default:
    return (power_on_seen && power_off_seen);
  }
}

// ============================================================================
// CONSTRUCTOR & INITIALIZATION
// ============================================================================

ControlTemplateRegistry::ControlTemplateRegistry() {
  clear();
}

void ControlTemplateRegistry::init() {
  loadFromNvs();
}

void ControlTemplateRegistry::clear() {
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < MAX_GROUPS; ++i) {
    _groups[i] = GroupControlTemplate{};
  }
  _group_count = 0;
  taskEXIT_CRITICAL(&_mux);
}

void ControlTemplateRegistry::autoAssignGroupName(GroupControlTemplate &group) {
  // 사용자가 이미 이름을 커스텀 지정한 경우(Unknown/Dev_0x/기본이 아님) 보존
  if (strlen(group.group_name) > 0 &&
      strncmp(group.group_name, "Unknown", 7) != 0 &&
      strncmp(group.group_name, "Dev_0x", 6) != 0 &&
      strcmp(group.group_name, "-") != 0 &&
      strcmp(group.group_name, "Gas") != 0 &&
      strcmp(group.group_name, "Thermo") != 0 &&
      strcmp(group.group_name, "Vent") != 0 &&
      strcmp(group.group_name, "Aircon") != 0 &&
      strcmp(group.group_name, "Elevator") != 0) {
    return;
  }

  switch (group.coverage.dev_class) {
  case DeviceClass::GAS:
    snprintf(group.group_name, sizeof(group.group_name), "Gas");
    break;
  case DeviceClass::SWITCH:
    snprintf(group.group_name, sizeof(group.group_name), "Light");
    break;
  case DeviceClass::MOMENTARY:
    snprintf(group.group_name, sizeof(group.group_name), "Elevator");
    break;
  case DeviceClass::THERMOSTAT:
    snprintf(group.group_name, sizeof(group.group_name), "Thermo");
    break;
  case DeviceClass::VENT:
    snprintf(group.group_name, sizeof(group.group_name), "Vent");
    break;
  case DeviceClass::AIRCON:
    snprintf(group.group_name, sizeof(group.group_name), "Aircon");
    break;
  case DeviceClass::UNKNOWN:
  default:
    snprintf(group.group_name, sizeof(group.group_name), "-");
    break;
  }
}

bool ControlTemplateRegistry::setGroupName(uint8_t dev_id, const char *name) {
  if (dev_id == 0 || !name || strlen(name) == 0) return false;

  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      strncpy(_groups[i].group_name, name, sizeof(_groups[i].group_name) - 1);
      _groups[i].group_name[sizeof(_groups[i].group_name) - 1] = '\0';
      if (strcasecmp(name, "Elevator") == 0 || strcasecmp(name, "EV") == 0) {
        _groups[i].coverage.dev_class = DeviceClass::MOMENTARY;
      }
      taskEXIT_CRITICAL(&_mux);
      saveToNvs();
      return true;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return false;
}

bool ControlTemplateRegistry::setGroupClass(uint8_t dev_id, DeviceClass cls, const char *name) {
  if (dev_id == 0) return false;

  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (_groups[i].coverage.dev_class != cls) {
        // UNKNOWN에서 구체 클래스로 변경되는 경우는 진행 중인 위자드 학습 데이터를 보존
        if (_groups[i].coverage.dev_class == DeviceClass::UNKNOWN) {
          _groups[i].coverage.dev_class = cls;
        } else {
          _groups[i].coverage = SlotCoverage{};
          _groups[i].coverage.dev_class = cls;
          _groups[i].power_slot = ActionSlot{};
          _groups[i].temp_slot = ActionSlot{};
          _groups[i].speed_slot = ActionSlot{};
          _groups[i].close_slot = ActionSlot{};
        }
      } else {
        _groups[i].coverage.dev_class = cls;
      }
      if (name && strlen(name) > 0) {
        strncpy(_groups[i].group_name, name, sizeof(_groups[i].group_name) - 1);
        _groups[i].group_name[sizeof(_groups[i].group_name) - 1] = '\0';
      } else {
        autoAssignGroupName(_groups[i]);
      }
      taskEXIT_CRITICAL(&_mux);
      saveToNvs();
      return true;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return false;
}

GroupControlTemplate *ControlTemplateRegistry::findGroup(uint8_t dev_id) {
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      taskEXIT_CRITICAL(&_mux);
      return &_groups[i];
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return nullptr;
}

const GroupControlTemplate *ControlTemplateRegistry::findGroup(uint8_t dev_id) const {
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      taskEXIT_CRITICAL(&_mux);
      return &_groups[i];
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return nullptr;
}

GroupControlTemplate *ControlTemplateRegistry::registerOrTouch(uint8_t dev_id, const char *name) {
  if (dev_id == 0) return nullptr;

  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (name && strlen(name) > 0) {
        strncpy(_groups[i].group_name, name, sizeof(_groups[i].group_name) - 1);
        _groups[i].group_name[sizeof(_groups[i].group_name) - 1] = '\0';
      }
      taskEXIT_CRITICAL(&_mux);
      return &_groups[i];
    }
  }

  if (_group_count < MAX_GROUPS) {
    // dev_id 오름차순으로 삽입 위치 탐색 및 정렬 유지
    size_t insert_idx = _group_count;
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id > dev_id) {
        insert_idx = i;
        break;
      }
    }
    for (size_t i = _group_count; i > insert_idx; --i) {
      _groups[i] = _groups[i - 1];
    }
    _group_count++;
    GroupControlTemplate &new_grp = _groups[insert_idx];
    new_grp = GroupControlTemplate{};
    new_grp.dev_id = dev_id;
    if (name && strlen(name) > 0) {
      strncpy(new_grp.group_name, name, sizeof(new_grp.group_name) - 1);
      new_grp.group_name[sizeof(new_grp.group_name) - 1] = '\0';
    } else {
      autoAssignGroupName(new_grp);
    }
    new_grp.status = GroupControlTemplate::Status::WAITING;
    taskEXIT_CRITICAL(&_mux);
    return &new_grp;
  }
  taskEXIT_CRITICAL(&_mux);
  return nullptr;
}

size_t ControlTemplateRegistry::getGroupCount() const {
  taskENTER_CRITICAL(&_mux);
  size_t cnt = _group_count;
  taskEXIT_CRITICAL(&_mux);
  return cnt;
}

bool ControlTemplateRegistry::getGroupByIndex(size_t index, GroupControlTemplate &out) const {
  taskENTER_CRITICAL(&_mux);
  if (index < _group_count) {
    out = _groups[index];
    taskEXIT_CRITICAL(&_mux);
    return true;
  }
  taskEXIT_CRITICAL(&_mux);
  return false;
}

bool ControlTemplateRegistry::lockGroup(uint8_t dev_id, bool lock_all) {
  taskENTER_CRITICAL(&_mux);
  bool modified = false;
  for (size_t i = 0; i < _group_count; ++i) {
    if (lock_all || _groups[i].dev_id == dev_id) {
      _groups[i].status = GroupControlTemplate::Status::LOCKED;
      // [무손실 보존] 사용자가 락(LOCKED)을 걸더라도 마지막 학습된 트랜잭션 히스토리를 영구 보존!
      modified = true;
      if (!lock_all) break;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  if (modified) {
    saveToNvs();
    return true;
  }
  return false;
}

bool ControlTemplateRegistry::unlockGroup(uint8_t dev_id, bool unlock_all) {
  taskENTER_CRITICAL(&_mux);
  bool modified = false;
  for (size_t i = 0; i < _group_count; ++i) {
    if (unlock_all || _groups[i].dev_id == dev_id) {
      _groups[i].status = _groups[i].coverage.isFullyCovered()
                            ? GroupControlTemplate::Status::VERIFIED
                            : GroupControlTemplate::Status::CAPTURING;
      modified = true;
      if (!unlock_all) break;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  if (modified) {
    saveToNvs();
    return true;
  }
  return false;
}

bool ControlTemplateRegistry::resetGroup(uint8_t dev_id) {
  if (dev_id == 0) {
    taskENTER_CRITICAL(&_mux);
    for (size_t i = 0; i < MAX_GROUPS; ++i) {
      _groups[i] = GroupControlTemplate{};
    }
    _group_count = 0;
    taskEXIT_CRITICAL(&_mux);

    Preferences prefs;
    if (prefs.begin("ctl_tmpls", false)) {
      prefs.clear();
      prefs.end();
    }
    return true;
  }

  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      for (size_t j = i; j + 1 < _group_count; ++j) {
        _groups[j] = _groups[j + 1];
      }
      _groups[_group_count - 1] = GroupControlTemplate{};
      _group_count--;
      taskEXIT_CRITICAL(&_mux);
      saveToNvs();
      return true;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return false;
}

// ============================================================================
// SYNTHESIZE TEMPLATES ON CACHE CONVERGENCE
// ============================================================================
void ControlTemplateRegistry::synthesizeFromConvergedCache() {
  // [엄격한 안전 규칙] 버스 오토프로빙이 Phase 3 (offsets_locked)에 도달하기 전에는 절대 사전 추측/생성 금지!
  auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.offsets_locked) return;

  uint8_t opcode_offset = ad.opcode_offset;
  uint8_t ctrl_opcode = (ad.control_opcode != 0) ? ad.control_opcode : 0x02;
  uint8_t sub1_offset = ad.sub1_offset;
  uint8_t sub2_offset = ad.sub2_offset;
  uint8_t stx = ad.stx;
  uint8_t etx = ad.etx;

  size_t total = g_polling_targets.totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry entry{};
    if (!g_polling_targets.getEntry(i, entry) || !entry.is_active || entry.raw_query_len < 5) {
      continue;
    }

    // raw_query에서 DevType 추출 (reindex 전이라도 offset 위치에서 직접 읽기)
    uint8_t d_id = (entry.dev_id != 0) ? entry.dev_id :
                   (ad.dev_id_offset < entry.raw_query_len ? entry.raw_query_data[ad.dev_id_offset] : 0);
    if (d_id == 0) continue;

    DeviceClass dc = SlotCoverage::classify(d_id, ad);

    GroupControlTemplate *grp = registerOrTouch(d_id);
    if (!grp) continue;

    taskENTER_CRITICAL(&_mux);
    if (grp->frame_len == 0 || grp->status == GroupControlTemplate::Status::WAITING) {
      grp->frame_len = entry.raw_query_len;
      std::copy(entry.raw_query_data.begin(),
                entry.raw_query_data.begin() + std::min<size_t>(entry.raw_query_len, 32),
                grp->raw_template);

      if (opcode_offset < grp->frame_len && ctrl_opcode != 0) {
        grp->raw_template[opcode_offset] = ctrl_opcode;
      }
      grp->sub1_offset = sub1_offset;
      grp->sub2_offset = sub2_offset;

      // 쿼리 패킷에서 관측된 sub1을 제어 서브주소의 초기값으로 기록
      grp->ctl_sub1_override = entry.sub1;

      grp->coverage.dev_class = dc;
      autoAssignGroupName(*grp);
      grp->status = GroupControlTemplate::Status::WAITING;
    }
    taskEXIT_CRITICAL(&_mux);
  }

  saveToNvs();
}

// ============================================================================
// TRIPLET DIFFERENTIAL SNIFFER
// ============================================================================
void ControlTemplateRegistry::onControlTransaction(const StaticPacket &ctl,
                                                   const StaticPacket &ack_before,
                                                   const StaticPacket &ack_after,
                                                   AckSlotHint hint) {
  if (ctl.length < 5 || ack_after.length < 5) return;

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser) return;

  auto ad = g_auto_probing_engine.getDescriptor();

  // --------------------------------------------------------------------------
  // 1. ACK 및 CTL 프레임 4중 방화벽 검증 (STX -> ETX -> Checksum -> Length)
  // --------------------------------------------------------------------------
  auto validateFramePure = [&](const StaticPacket &pkt, bool is_ack) -> bool {
    if (pkt.length < 4 || pkt.length > 64) return false;
    // (1) STX 확인
    if (pkt.data[0] != ad.stx) return false;
    // (2) ETX 확인
    if (pkt.data[pkt.length - 1] != ad.etx) return false;
    // (3) Checksum 검증
    if (ad.checksum_algo != ChecksumAlgo::NONE) {
      uint8_t calc_cs = parser->calculateChecksum(pkt.data.data(), pkt.length);
      if (calc_cs != pkt.data[pkt.length - 2]) return false;
    }
    // (4) Length 필드 검증 (가변 길이 프로토콜인 경우)
    if (ad.has_len_field && ad.len_offset < pkt.length) {
      if (pkt.data[ad.len_offset] != pkt.length) return false;
    }
    return true;
  };

  // ACK 패킷의 4중 방화벽 검증 (노이즈, 훼손된 패킷 차단)
  if (!validateFramePure(ack_after, true)) return;
  bool has_before = (ack_before.length >= 5 && validateFramePure(ack_before, true));

  // CTL 패킷 유효성 검증
  if (!validateFramePure(ctl, false)) return;

  // [G-1 Feedback] 실제 관측된 제어 트랜잭션을 Engine 1(AutoProbingEngine)에 역주입하여 제어 Opcode 확정 가속
  g_auto_probing_engine.feedControlPair(
      span<const uint8_t>(ctl.data.data(), ctl.length),
      span<const uint8_t>(ack_after.data.data(), ack_after.length)
  );

  span<const uint8_t> ctl_span(ctl.data.data(), ctl.length);
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!parser->extractDeviceKey(ctl_span, dev_id, sub1, sub2)) return;

  // --------------------------------------------------------------------------
  // 2. 상태 변화 감지: ack_before가 존재할 때 diff 확인
  // --------------------------------------------------------------------------
  bool state_changed = false;
  if (has_before) {
    if (ack_before.length == ack_after.length) {
      for (size_t i = 0; i < ack_after.length; ++i) {
        if (ack_before.data[i] != ack_after.data[i]) {
          state_changed = true;
          break;
        }
      }
    } else {
      state_changed = true;
    }
    if (!state_changed) return; // 무의미한 동일 상태 중복 응답은 배제
  }

  // --------------------------------------------------------------------------
  // 3. 그룹 템플릿 확보
  // --------------------------------------------------------------------------
  GroupControlTemplate *grp = registerOrTouch(dev_id);
  if (!grp) return;

  // ★ LOCKED 상태인 기기는 사용자 승인 불변 잠금 상태이므로 외부 패킷에 의한 변형 차단!
  if (grp->status == GroupControlTemplate::Status::LOCKED) return;

  taskENTER_CRITICAL(&_mux);
  // 이전 raw_template을 diff 비교용으로 보존한 뒤 새 패킷 복사
  uint8_t prev_raw[32]{0};
  uint8_t prev_len = grp->frame_len;
  if (prev_len > 0) {
    std::copy(grp->raw_template, grp->raw_template + std::min<size_t>(prev_len, 32), prev_raw);
  }

  if (grp->last_ctl_len > 0) {
    grp->ctl_before_len = grp->last_ctl_len;
    std::copy(grp->last_ctl_raw, grp->last_ctl_raw + grp->ctl_before_len, grp->ctl_before_raw);
  }

  grp->frame_len = ctl.length;
  std::copy(ctl.data.begin(), ctl.data.begin() + std::min<size_t>(ctl.length, 32), grp->raw_template);
  grp->last_ctl_len = std::min<size_t>(ctl.length, 32);
  std::copy(ctl.data.begin(), ctl.data.begin() + grp->last_ctl_len, grp->last_ctl_raw);
  grp->ctl_after_len = grp->last_ctl_len;
  std::copy(grp->last_ctl_raw, grp->last_ctl_raw + grp->ctl_after_len, grp->ctl_after_raw);

  grp->last_ack_before_len = std::min<size_t>(ack_before.length, 32);
  if (grp->last_ack_before_len > 0) {
    std::copy(ack_before.data.begin(), ack_before.data.begin() + grp->last_ack_before_len, grp->last_ack_before_raw);
  }
  grp->last_ack_after_len = std::min<size_t>(ack_after.length, 32);
  if (grp->last_ack_after_len > 0) {
    std::copy(ack_after.data.begin(), ack_after.data.begin() + grp->last_ack_after_len, grp->last_ack_after_raw);
  }

  // [3-단계 순환 트랜잭션 캡처 링 버퍼: T-2 -> T-1 -> T]
  // 0: T-2 (가장 오래됨), 1: T-1 (직전), 2: T_current (최신)
  if (grp->hist_count < 3) {
    uint8_t slot = grp->hist_count;
    grp->ctl_hist[slot].len = std::min<size_t>(ctl.length, 32);
    std::copy(ctl.data.begin(), ctl.data.begin() + grp->ctl_hist[slot].len, grp->ctl_hist[slot].raw);
    grp->ack_hist[slot].len = std::min<size_t>(ack_after.length, 32);
    std::copy(ack_after.data.begin(), ack_after.data.begin() + grp->ack_hist[slot].len, grp->ack_hist[slot].raw);
    grp->hist_count++;
  } else {
    // 링 시프트 (0 <- 1, 1 <- 2, 2 <- new)
    grp->ctl_hist[0] = grp->ctl_hist[1];
    grp->ctl_hist[1] = grp->ctl_hist[2];
    grp->ctl_hist[2].len = std::min<size_t>(ctl.length, 32);
    std::copy(ctl.data.begin(), ctl.data.begin() + grp->ctl_hist[2].len, grp->ctl_hist[2].raw);

    grp->ack_hist[0] = grp->ack_hist[1];
    grp->ack_hist[1] = grp->ack_hist[2];
    grp->ack_hist[2].len = std::min<size_t>(ack_after.length, 32);
    std::copy(ack_after.data.begin(), ack_after.data.begin() + grp->ack_hist[2].len, grp->ack_hist[2].raw);
  }

  grp->last_learned_ms = millis();
  grp->coverage.observation_count++;

  // --------------------------------------------------------------------------
  // 4. 주소 슬롯 마스킹 오프셋 감지 및 SUB 주소 동적 학습 (수학적 불변량 헤더 방어)
  // --------------------------------------------------------------------------
  if (ad.offsets_locked) {
    grp->sub1_offset = ad.sub1_offset;
    grp->sub2_offset = ad.sub2_offset;
  } else {
    // 프로토콜 헤더 영역(payload_offset 이전)은 어떤 경우에도 주소 슬롯(sub1, sub2)이 될 수 없음!
    size_t addr_search_start = (ad.payload_offset > 0) ? ad.payload_offset : 5;
    for (size_t i = addr_search_start; i < ctl.length - 2; ++i) {
      if (ad.gw_addr_offset != 0xFF && i == ad.gw_addr_offset) continue;
      if (i == ad.opcode_offset || i == ad.dev_id_offset) continue;
      if (ctl.data[i] == sub1 && grp->sub1_offset == 0xFF) grp->sub1_offset = i;
      if (ctl.data[i] == sub2 && grp->sub2_offset == 0xFF) grp->sub2_offset = i;
    }
  }

  // 월패드가 실제로 송신한 제어 패킷(ctl)의 sub1 주소를 동적 학습
  if (grp->sub1_offset < ctl.length) {
    grp->ctl_sub1_override = ctl.data[grp->sub1_offset];
  }

  // --------------------------------------------------------------------------
  // 5. 무사전지식 Full-Spectrum 차분 분석 (Full Differential Scan)
  // 고정 프레임 필드(STX, LEN, DevID, OP, CS, ETX) 및 주소(SUB1, SUB2)를 엄격히 분리
  // --------------------------------------------------------------------------
  size_t start_idx = (ad.offsets_locked && ad.payload_offset > 0) 
                       ? ad.payload_offset 
                       : std::max({(size_t)1, (size_t)ad.opcode_offset + 1, (size_t)ad.dev_id_offset + 1});
  size_t end_idx = (ctl.length >= 2) ? (ctl.length - 2) : ctl.length;

  auto isFixedField = [&](size_t idx, size_t frame_len) -> bool {
    if (idx == 0) return true;
    if (frame_len >= 2 && idx >= frame_len - 2) return true;
    if (ad.has_len_field && idx == ad.len_offset) return true;
    if (idx == ad.dev_id_offset) return true;
    if (idx == ad.opcode_offset) return true;
    if (ad.is_swapped_addr && idx == ad.gw_addr_offset) return true;
    // Sub1/Sub2 주소 위치라도 프레임 간 값이 변한다면 고정 장치 주소가 아니라 명령 컨텍스트(Category)로 취급
    if (idx == grp->sub1_offset || (ad.offsets_locked && idx == ad.sub1_offset)) {
      if (prev_len > 0 && ctl.data[idx] != prev_raw[idx]) return false;
      return true;
    }
    if (idx == grp->sub2_offset || (ad.offsets_locked && idx == ad.sub2_offset)) {
      if (prev_len > 0 && ctl.data[idx] != prev_raw[idx]) return false;
      return true;
    }
    if (ad.offsets_locked && idx < ad.payload_offset) return true;
    return false;
  };

  auto isFixedCtlField = [&](size_t idx) -> bool {
    return isFixedField(idx, ctl.length);
  };

  auto isFixedAckField = [&](size_t idx, size_t ack_len = 0) -> bool {
    size_t l = (ack_len > 0) ? ack_len : ack_after.length;
    return isFixedField(idx, l);
  };

  // CTL 프레임에서 실제로 값이 변한 오프셋 수집 (명령 트리거 바이트)
  uint8_t ctl_diff_offsets[8]{0};
  uint8_t ctl_diff_vals[8]{0};
  size_t ctl_diff_count = 0;

  for (size_t i = start_idx; i < end_idx; ++i) {
    if (isFixedCtlField(i)) continue;
    if (prev_len > 0 && ctl.data[i] != prev_raw[i]) {
      if (ctl_diff_count < 8) {
        ctl_diff_offsets[ctl_diff_count] = static_cast<uint8_t>(i);
        ctl_diff_vals[ctl_diff_count] = ctl.data[i];
        ctl_diff_count++;
      }
    }
  }

  // ACK 응답에서 변한 오프셋 수집 (장치 상태 바이트)
  uint8_t ack_diff_offsets[8]{0};
  size_t ack_diff_count = 0;
  if (has_before) {
    size_t ack_b_end = (ack_before.length >= 2) ? (ack_before.length - 2) : ack_before.length;
    size_t ack_a_end = (ack_after.length >= 2) ? (ack_after.length - 2) : ack_after.length;
    size_t common_ack_end = std::min(ack_b_end, ack_a_end);
    for (size_t k = start_idx; k < common_ack_end; ++k) {
      if (isFixedAckField(k, common_ack_end + 2)) continue;
      if (ack_before.data[k] != ack_after.data[k]) {
        if (ack_diff_count < 8) {
          ack_diff_offsets[ack_diff_count++] = static_cast<uint8_t>(k);
        }
      }
    }
  }

  // 제어 오프셋 선정:
  // 1순위: CTL 패킷에서 실제 변화가 발생한 바이트 (예: Byte #7 0x01 vs 0x02)
  // 2순위: ACK에서 상태 변화가 발생한 바이트
  // 3순위: 페이로드의 첫 번째 가변 바이트
  uint8_t act_off = 0xFF;
  uint8_t cmd_val = 0;
  uint8_t cat_off = 0xFF;
  uint8_t cat_val = 0;
  bool is_temp_ctx = false;
  bool is_pwr_mode_token = false;

  if (ctl_diff_count > 0) {
    act_off = ctl_diff_offsets[0];
    cmd_val = ctl_diff_vals[0];
    if (ctl_diff_count > 1) {
      cat_off = ctl_diff_offsets[0];
      cat_val = ctl_diff_vals[0];
      act_off = ctl_diff_offsets[1];
      cmd_val = ctl_diff_vals[1];
    }
  } else if (ack_diff_count > 0) {
    act_off = ack_diff_offsets[0];
    cmd_val = ctl.data[act_off];
  } else {
    for (size_t i = start_idx; i < end_idx; ++i) {
      if (!isFixedCtlField(i)) {
        act_off = static_cast<uint8_t>(i);
        cmd_val = ctl.data[i];
        break;
      }
    }
  }

  // 컨텍스트(서브명령) 바이트 위치 및 값 확정:
  // 현대통신(0x40/0x42, 0x45/0x46) 등에서 Opcode2/SubCmd 위치(주소가 아닌 서브명령) 추출
  // 주소 오프셋(sub1_offset, sub2_offset)은 장치 주소 번호이므로 절대 컨텍스트 채널로 오인하지 않음!
  if (cat_off == 0xFF) {
    // 1) 기존 학습된 슬롯에서 category_offset이 유효하면 재사용
    if (grp->power_slot.category_offset != 0xFF && grp->power_slot.category_offset < ctl.length &&
        grp->power_slot.category_offset != act_off) {
      cat_off = grp->power_slot.category_offset;
      cat_val = ctl.data[cat_off];
    } else if (grp->temp_slot.category_offset != 0xFF && grp->temp_slot.category_offset < ctl.length &&
               grp->temp_slot.category_offset != act_off) {
      cat_off = grp->temp_slot.category_offset;
      cat_val = ctl.data[cat_off];
    } else {
      // 2) 패킷 헤더(opcode_offset 이후)와 act_off 사이에서 컨텍스트 바이트 탐색
      size_t scan_start = (ad.opcode_offset > 0) ? (ad.opcode_offset + 1) : 1;
      for (size_t k = scan_start; k < act_off && k < ctl.length - 2; ++k) {
        if (k == ad.dev_id_offset || k == ad.gw_addr_offset) continue;
        if (k == grp->sub1_offset || k == grp->sub2_offset) continue;
        if (ad.offsets_locked && (k == ad.sub1_offset || k == ad.sub2_offset)) continue;
        // 서브명령 컨텍스트는 통상 0x20 이상의 커맨드 코드 (0x45, 0x46 등)
        if (ctl.data[k] >= 0x20) {
          cat_off = static_cast<uint8_t>(k);
          cat_val = ctl.data[k];
          break;
        }
      }
    }
  }

  if (act_off == 0xFF || act_off >= end_idx) {
    act_off = static_cast<uint8_t>(start_idx);
    cmd_val = ctl.data[act_off];
  }

  // 만약 dev_class가 아직 미분류 상태라면 2차 캐시를 통해 즉시 분류 수행
  if (grp->coverage.dev_class == DeviceClass::UNKNOWN) {
    DeviceClass auto_cls = SlotCoverage::classify(dev_id, ad);
    if (auto_cls != DeviceClass::UNKNOWN) {
      grp->coverage.dev_class = auto_cls;
      autoAssignGroupName(*grp);
    }
  }

  // --------------------------------------------------------------------------
  // 보편적 슬롯 매핑 (Universal Slot Mapping)
  // --------------------------------------------------------------------------
  if (grp->coverage.dev_class == DeviceClass::MOMENTARY) {
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    grp->power_slot.on_val = cmd_val;
    grp->power_slot.sample_count++;
    grp->coverage.call_seen = true;
  } else if (grp->coverage.dev_class == DeviceClass::GAS) {
    grp->close_slot.discovered = true;
    grp->close_slot.action_offset = act_off;
    grp->close_slot.off_val = cmd_val;
    grp->close_slot.sample_count++;
    grp->coverage.valve_close_seen = true;
    grp->coverage.power_off_seen = true;
  } else if (grp->coverage.dev_class == DeviceClass::THERMOSTAT) {
    // Context(cat_val) 엄격 격리:
    // cat_val == 0x46 (또는 power_slot.category_val과 일치): 전원 및 모드(ON, OFF, AWAY)
    // cat_val == 0x45 (또는 temp_slot.category_val): 희망온도 (SET_TEMP)
    is_pwr_mode_token = (cmd_val == 0x01 || cmd_val == 0x02 || cmd_val == 0x03 || cmd_val == 0x04 || cmd_val == 0x07);
    is_temp_ctx = false;

    if (hint == AckSlotHint::TEMP) {
      // 위저드가 명시적으로 "이번은 온도 변경" 이라고 알려줌 → 하드코딩 없이 확정
      is_temp_ctx = true;
    } else if (hint == AckSlotHint::POWER) {
      // 위저드가 명시적으로 "이번은 전원/모드" 이라고 알려줌
      is_temp_ctx = false;
    } else {
      // hint==NONE: 패시브 스니핑 — category_val로 추론 (이미 학습된 카테고리 기준)
      if (grp->power_slot.category_offset != 0xFF && cat_val == grp->power_slot.category_val) {
        is_temp_ctx = false;
      } else if (grp->temp_slot.category_offset != 0xFF && cat_val == grp->temp_slot.category_val) {
        is_temp_ctx = true;
      } else if (is_pwr_mode_token) {
        is_temp_ctx = false;
      }
      // cat_val이 변했고 이미 학습된 카테고리와 다를 때:
      // 전원 카테고리가 이미 알려져 있고 다른 cat_val이 왔다면 온도 컨텍스트로 가정
      else if (grp->power_slot.category_offset != 0xFF && cat_val != grp->power_slot.category_val && cat_val != 0) {
        is_temp_ctx = true;
      }
    }

    if (!is_temp_ctx) {
      // [전원 / 모드 컨텍스트 (0x46 등)]
      grp->power_slot.discovered = true;
      grp->power_slot.action_offset = act_off;
      if (cat_off != 0xFF) {
        grp->power_slot.category_offset = cat_off;
        grp->power_slot.category_val = cat_val;
      }
      grp->power_slot.sample_count++;

      if (!grp->coverage.power_on_seen) {
        if (cmd_val == 0x01) {
          grp->power_slot.on_val = cmd_val;
          grp->coverage.power_on_seen = true;
        } else if (cmd_val == 0x07 || cmd_val == 0x02 || cmd_val == 0x03) {
          grp->away_mode_token = cmd_val;
          grp->coverage.away_mode_seen = true;
        }
      } else {
        if (cmd_val == 0x07 || cmd_val == 0x02 || cmd_val == 0x03) {
          // 외출(Away) 모드 토큰으로 등록 (전원 OFF나 희망온도와 절대 혼동하지 않음!)
          grp->away_mode_token = cmd_val;
          grp->coverage.away_mode_seen = true;

          // 외출 패킷 유입 시 ACK에서 특정 온도로 고정되는지 확인 (예: 10℃ 고정, 토큰값 0x07 배제)
          if (has_before && ack_after.length >= 5) {
            // target_temp_offset이 이미 알려져 있다면 해당 위치 우선 확인
            if (grp->ack_slots.target_temp_offset != 0xFF && grp->ack_slots.target_temp_offset < ack_after.length) {
              uint8_t t_val = ack_after.data[grp->ack_slots.target_temp_offset];
              if (t_val >= 5 && t_val <= 25 && t_val != cmd_val) {
                grp->away_fixed_temp = t_val;
                grp->away_has_dedicated_temp = true;
              }
            } else {
              // 아직 target_temp_offset 미확정 시 페이로드 뒤쪽의 온도 바이트(토큰 0x07 제외) 탐색
              for (size_t k = (start_idx > 8 ? start_idx : 8); k < ack_after.length - 2; ++k) {
                if (isFixedAckField(k)) continue;
                uint8_t t_val = ack_after.data[k];
                if (t_val >= 5 && t_val <= 25 && t_val != cmd_val) {
                  grp->away_fixed_temp = t_val;
                  grp->away_has_dedicated_temp = true;
                  break;
                }
              }
            }
          }
        } else if (cmd_val == 0x04 || (!grp->coverage.power_off_seen && cmd_val != grp->power_slot.on_val && cmd_val <= 0x0F)) {
          grp->power_slot.off_val = cmd_val;
          grp->coverage.power_off_seen = true;
        } else if (grp->coverage.power_off_seen && cmd_val == grp->power_slot.on_val) {
          if (!grp->coverage.temp_ready_on) {
            // [3단계] 전원 OFF 후 온도 조절을 위해 다시 켠 상태
            grp->coverage.temp_ready_on = true;
          } else if (grp->coverage.temp_set_seen) {
            // [6단계] 온도 조절 및 외출 후 다시 켜기 -> 직전 설정온도 복원(Recall) 성공 검증!
            grp->temp_recall_verified = true;
            grp->coverage.temp_recall_seen = true;
          }
        }
      }
    } else {
      // 전원이 꺼진 상태(power_off_seen)에서 온도 조작이 들어온 경우
      if (grp->coverage.power_off_seen) {
        grp->coverage.temp_while_off_seen = true;
        if (grp->off_temp_behavior == ThermoOffTempBehavior::UNKNOWN ||
            grp->off_temp_behavior == ThermoOffTempBehavior::PASSIVE_MEMORY) {
          if (has_before) {
            bool power_turned_on = false;
            bool power_remained_off = false;
            bool temp_reflected_in_ack = false;

            // 1) ACK 차분 오프셋들 검사 (무사전지식 / 제로 하드코딩)
            for (size_t d = 0; d < ack_diff_count; ++d) {
              uint8_t off_idx = ack_diff_offsets[d];
              if (off_idx < ack_before.length && off_idx < ack_after.length) {
                uint8_t b_val = ack_before.data[off_idx];
                uint8_t a_val = ack_after.data[off_idx];

                // 조작하여 보낸 희망온도(cmd_val)가 ACK 차분에 정확히 반영/수렴되었는가?
                if (a_val == cmd_val) {
                  temp_reflected_in_ack = true;
                }

                // 전원 토큰 전이 확인 (OFF -> ON)
                if (b_val == grp->power_slot.off_val && a_val == grp->power_slot.on_val) {
                  power_turned_on = true;
                }
              }
            }

            // 2) 가변 길이 ACK 대응: ack_after 페이로드에서 온도가 반영되고 전원 ON 토큰이 존재하는지 보조 전수 검사
            if (!temp_reflected_in_ack) {
              for (size_t k = start_idx; k < ack_after.length - 2; ++k) {
                if (!isFixedAckField(k) && ack_after.data[k] == cmd_val) {
                  temp_reflected_in_ack = true;
                  break;
                }
              }
            }

            if (!power_turned_on) {
              for (size_t k = start_idx; k < ack_after.length - 2; ++k) {
                if (!isFixedAckField(k) && ack_after.data[k] == grp->power_slot.on_val) {
                  power_turned_on = true;
                  break;
                }
              }
            }

            // 3) 만약 여전히 OFF 토큰만 존재한다면 power_remained_off
            if (!power_turned_on) {
              for (size_t k = start_idx; k < ack_after.length - 2; ++k) {
                if (!isFixedAckField(k) && ack_after.data[k] == grp->power_slot.off_val) {
                  power_remained_off = true;
                  break;
                }
              }
            }

            // [판정] 조작한 온도가 실제로 반영되면서 전원이 켜진 경우에만 확실한 AUTO_POWER_ON!
            if (power_turned_on && temp_reflected_in_ack) {
              grp->off_temp_behavior = ThermoOffTempBehavior::AUTO_POWER_ON;
              grp->off_temp_unchanged_count = 0;
            } else if (temp_reflected_in_ack && (power_remained_off || !power_turned_on)) {
              // 전원은 켜지지 않았으나 설정온도는 메모리에 정상 반영됨
              grp->off_temp_behavior = ThermoOffTempBehavior::PASSIVE_MEMORY;
              grp->off_temp_unchanged_count = 0;
            } else if (power_remained_off && !temp_reflected_in_ack && ack_diff_count == 0) {
              // 온도가 전혀 반영되지 않고 무시됨
              grp->off_temp_unchanged_count++;
              if (grp->off_temp_unchanged_count >= 3) {
                grp->off_temp_behavior = ThermoOffTempBehavior::LOCKED_IGNORE;
              }
            }
          }
        }
      }

      // 설정온도(SET_TEMP) 슬롯으로 독립 분리
      grp->temp_slot.discovered = true;
      grp->temp_slot.action_offset = act_off;
      if (cat_off != 0xFF) {
        grp->temp_slot.category_offset = cat_off;
        grp->temp_slot.category_val = cat_val;
      }
      if (cmd_val >= 5 && cmd_val <= 40) {
        if (grp->temp_slot.min_val == 0 || cmd_val < grp->temp_slot.min_val) grp->temp_slot.min_val = cmd_val;
        if (cmd_val > grp->temp_slot.max_val) grp->temp_slot.max_val = cmd_val;
        grp->temp_slot.sample_count++;
        grp->coverage.temp_set_seen = true;
      }
    }
  } else if (grp->coverage.dev_class == DeviceClass::VENT) {
    // 환기 동적 슬롯 학습 (사전지식/하드코딩 배제)
    // 조작값(cmd_val)과 컨텍스트(cat_val)의 인과관계에 따라 전원 및 풍량 단계 자동 수렴
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    if (cat_off != 0xFF) {
      grp->power_slot.category_offset = cat_off;
      grp->power_slot.category_val = (cat_val > 0) ? cat_val : 0x40;
    }
    grp->power_slot.sample_count++;

    // 1) 전원 OFF 토큰 판별: 0x02 또는 0x00 또는 0x04
    bool is_off_val = (cmd_val == 0x02 || cmd_val == 0x00 || cmd_val == 0x04);

    if (is_off_val) {
      grp->power_slot.off_val = cmd_val;
      grp->coverage.power_off_seen = true;
    } else if (cmd_val > 0) {
      if (!grp->coverage.power_on_seen) {
        grp->power_slot.on_val = cmd_val;
        grp->coverage.power_on_seen = true;
      } else if (grp->coverage.power_off_seen && !grp->coverage.speed_ready_on) {
        // [3단계] 전원 OFF 후 풍량 조작을 위해 다시 켠 상태
        grp->coverage.speed_ready_on = true;
      }

      // 2) 풍량 슬롯(speed_slot) 동적 등록:
      // 전원 OFF가 아닌 모든 유효 조작값(0x01, 0x03, 0x07 등)을 풍량 단계로 등록
      grp->speed_slot.discovered = true;
      grp->speed_slot.action_offset = act_off;
      if (cat_off != 0xFF) {
        grp->speed_slot.category_offset = cat_off;
        grp->speed_slot.category_val = cat_val;
      }
      grp->speed_slot.sample_count++;

      bool exists = false;
      for (uint8_t k = 0; k < grp->speed_slot.level_count; ++k) {
        if (grp->speed_slot.level_tokens[k] == cmd_val) { exists = true; break; }
      }
      if (!exists && grp->speed_slot.level_count < 4) {
        grp->speed_slot.level_tokens[grp->speed_slot.level_count++] = cmd_val;
        grp->speed_slot.min_val = 1;
        grp->speed_slot.max_val = grp->speed_slot.level_count;
      }

      if (grp->speed_slot.level_count >= 1) grp->coverage.speed_l1_seen = true;
      if (grp->speed_slot.level_count >= 2) grp->coverage.speed_l2_seen = true;
      if (grp->speed_slot.level_count >= 3) grp->coverage.speed_l3_seen = true;
    }
  } else {
    // SWITCH (조명, 콘센트 등 ON/OFF 기기 공통)
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    if (cat_off != 0xFF) {
      grp->power_slot.category_offset = cat_off;
      grp->power_slot.category_val = cat_val;
    }
    grp->power_slot.sample_count++;

    if (!grp->coverage.power_on_seen) {
      grp->power_slot.on_val = cmd_val;
      grp->coverage.power_on_seen = true;
    } else if (!grp->coverage.power_off_seen) {
      // 1차 제어값(ON)과 다른 새로운 제어값이 들어왔을 때만 2차 제어값(OFF)으로 확정 (중복 재전송 패킷 완벽 무시)
      if (cmd_val != grp->power_slot.on_val) {
        grp->power_slot.off_val = cmd_val;
        grp->coverage.power_off_seen = true;
      }
    } else {
      if (cmd_val == grp->power_slot.on_val) grp->coverage.power_on_seen = true;
      else if (cmd_val == grp->power_slot.off_val) grp->coverage.power_off_seen = true;
    }
  }

  // --------------------------------------------------------------------------
  // 6. 무사전지식 인과관계 차분(Causal Differential) 기반 Universal ACK 상태 슬롯 학습
  // ★ hint(위저드 semantic)로 "이번 트랜잭션이 무슨 목적인가"를 알고 있으면
  //   해당 슬롯만 탐색 → if-if-if 없이 완전한 1:1 의미론적 바인딩.
  // hint==NONE이면 기존 추론 로직(패시브 스니핑)으로 fallback.
  // --------------------------------------------------------------------------
  if (has_before && ack_after.length >= 5) {
    size_t ack_payload_start = (ad.offsets_locked && ad.payload_offset > 0) ? ad.payload_offset : 5;
    size_t ack_payload_end = (ack_after.length >= 2) ? (ack_after.length - 2) : ack_after.length;

    if (hint == AckSlotHint::POWER) {
      // ──────────────────────────────────────────────────────────────────────
      // [POWER hint] 이번 트랜잭션은 전원/외출/모드 조작.
      // cmd_val 수렴 또는 on/off 전이 바이트 → power_offset 확정
      // ──────────────────────────────────────────────────────────────────────
      for (size_t d = 0; d < ack_diff_count; ++d) {
        uint8_t k = ack_diff_offsets[d];
        if (k < ack_payload_start || k >= ack_payload_end || k >= ack_before.length) continue;
        uint8_t b_val = ack_before.data[k];
        uint8_t a_val = ack_after.data[k];
        if (a_val == cmd_val) {
          grp->ack_slots.power_offset = k;
          grp->ack_slots.discovered = true;
          // ★ 상호배제: 같은 위치에 잘못 바인딩된 슬롯 즉시 무효화
          if (grp->ack_slots.target_temp_offset == k) grp->ack_slots.target_temp_offset = 0xFF;
          if (grp->ack_slots.fan_speed_offset   == k) grp->ack_slots.fan_speed_offset   = 0xFF;
          break;
        }
        if (grp->power_slot.discovered) {
          uint8_t on_t = grp->power_slot.on_val;
          uint8_t off_t = grp->power_slot.off_val;
          if ((b_val == off_t && a_val == on_t) || (b_val == on_t && a_val == off_t)) {
            grp->ack_slots.power_offset = k;
            grp->ack_slots.discovered = true;
            if (grp->ack_slots.target_temp_offset == k) grp->ack_slots.target_temp_offset = 0xFF;
            if (grp->ack_slots.fan_speed_offset   == k) grp->ack_slots.fan_speed_offset   = 0xFF;
            break;
          }
        }
      }

    } else if (hint == AckSlotHint::TEMP) {
      // ──────────────────────────────────────────────────────────────────────
      // [TEMP hint] 이번 트랜잭션은 희망온도 변경.
      // ACK 차분 중 cmd_val과 수렴하는 바이트 → target_temp_offset
      // 상호배제: power_offset과 다른 위치여야 함
      // ──────────────────────────────────────────────────────────────────────
      for (size_t d = 0; d < ack_diff_count; ++d) {
        uint8_t k = ack_diff_offsets[d];
        if (k < ack_payload_start || k >= ack_payload_end) continue;
        if (k == grp->ack_slots.power_offset) continue; // 상호배제 (전원/에코 슬롯 배제)
        if (ack_after.data[k] == cmd_val) {
          grp->ack_slots.target_temp_offset = k;
          grp->ack_slots.discovered = true;
          grp->coverage.temp_set_seen = true;
          break;
        }
      }

      // [AT] 현재온도: TEMP 트랜잭션에서 불변인 환경온도 바이트 탐색
      if (grp->ack_slots.current_temp_offset == 0xFF) {
        for (size_t k = ack_payload_start; k < ack_payload_end; ++k) {
          if (isFixedAckField(k)) continue;
          if (k == grp->ack_slots.power_offset || k == grp->ack_slots.target_temp_offset) continue;
          if (k == grp->sub1_offset || k == grp->sub2_offset || k == cat_off || k == act_off) continue;
          if (k < ack_before.length && ack_before.data[k] == ack_after.data[k]) {
            uint8_t env_val = ack_after.data[k];
            // ★ 상호배제: 전원/외출 모드 토큰값과 동일한 바이트는 현재온도 센서값이 될 수 없음
            if (grp->power_slot.discovered) {
              if (env_val == grp->power_slot.on_val || env_val == grp->power_slot.off_val) continue;
              if (grp->coverage.away_mode_seen && env_val == grp->away_mode_token) continue;
            }
            grp->ack_slots.current_temp_offset = k;
            grp->temp_slot.telemetry_offset = k;
            grp->ack_slots.discovered = true;
            break;
          }
        }
      }

    } else if (hint == AckSlotHint::SPEED) {
      // ──────────────────────────────────────────────────────────────────────
      // [SPEED hint] 이번 트랜잭션은 풍량 변경.
      // ACK 차분 중 cmd_val과 수렴하는 바이트 → fan_speed_offset
      // 상호배제: power_offset과 다른 위치여야 함
      // ──────────────────────────────────────────────────────────────────────
      for (size_t d = 0; d < ack_diff_count; ++d) {
        uint8_t k = ack_diff_offsets[d];
        if (k < ack_payload_start || k >= ack_payload_end) continue;
        if (k == grp->ack_slots.power_offset) continue; // 상호배제
        if (ack_after.data[k] == cmd_val) {
          grp->ack_slots.fan_speed_offset = k;
          grp->ack_slots.discovered = true;
          break;
        }
      }

    } else {
      // ──────────────────────────────────────────────────────────────────────
      // [NONE hint] 위저드 밖 패시브 스니핑 — 기존 추론 로직 유지
      // ──────────────────────────────────────────────────────────────────────

      // (1) 전원 상태 슬롯 [AS]: off_val <-> on_val 전이 바이트 탐색
      if (grp->power_slot.discovered) {
        uint8_t on_t = grp->power_slot.on_val;
        uint8_t off_t = grp->power_slot.off_val;
        for (size_t d = 0; d < ack_diff_count; ++d) {
          uint8_t k = ack_diff_offsets[d];
          if (k >= ack_payload_start && k < ack_payload_end && k < ack_before.length) {
            uint8_t b_val = ack_before.data[k];
            uint8_t a_val = ack_after.data[k];
            if ((b_val == off_t && a_val == on_t) || (b_val == on_t && a_val == off_t)) {
              grp->ack_slots.power_offset = k;
              grp->ack_slots.discovered = true;
              if (grp->ack_slots.target_temp_offset == k) grp->ack_slots.target_temp_offset = 0xFF;
              break;
            }
          }
        }
      }

      // (2) 설정온도 슬롯 [TT]: THERMOSTAT/AIRCON 전용, power/away 토큰이 아닐 때만
      if (grp->coverage.dev_class == DeviceClass::THERMOSTAT ||
          grp->coverage.dev_class == DeviceClass::AIRCON) {
        bool is_pwr_cmd = (cmd_val == grp->power_slot.on_val || cmd_val == grp->power_slot.off_val ||
                           (grp->coverage.away_mode_seen && cmd_val == grp->away_mode_token));
        if (!is_pwr_cmd) {
          for (size_t d = 0; d < ack_diff_count; ++d) {
            uint8_t k = ack_diff_offsets[d];
            if (k >= ack_payload_start && k < ack_payload_end && k != grp->ack_slots.power_offset) {
              if (ack_after.data[k] == cmd_val) {
                grp->ack_slots.target_temp_offset = k;
                grp->ack_slots.discovered = true;
                break;
              }
            }
          }
        }
        // 상호배제 불변량: power_offset과 target_temp_offset이 겹치면 무효화
        if (grp->ack_slots.target_temp_offset != 0xFF &&
            grp->ack_slots.target_temp_offset == grp->ack_slots.power_offset) {
          grp->ack_slots.target_temp_offset = 0xFF;
        }

        // (3) 현재온도 텔레메트리 슬롯 [AT]
        if (grp->ack_slots.current_temp_offset == 0xFF) {
          for (size_t k = ack_payload_start; k < ack_payload_end; ++k) {
            if (isFixedAckField(k)) continue;
            if (k == grp->ack_slots.power_offset || k == grp->ack_slots.target_temp_offset) continue;
            if (k == grp->sub1_offset || k == grp->sub2_offset || k == cat_off) continue;
            if (k < ack_before.length && ack_before.data[k] == ack_after.data[k]) {
              uint8_t env_val = ack_after.data[k];
              if (grp->power_slot.discovered) {
                if (env_val == grp->power_slot.on_val || env_val == grp->power_slot.off_val) continue;
                if (grp->coverage.away_mode_seen && env_val == grp->away_mode_token) continue;
              }
              if (env_val >= 10 && env_val <= 35) {
                grp->ack_slots.current_temp_offset = k;
                grp->temp_slot.telemetry_offset = k;
                grp->ack_slots.discovered = true;
                break;
              }
            }
          }
        }
      }

      // (4) 환기(VENT) 풍량 슬롯 탐색
      if (grp->coverage.dev_class == DeviceClass::VENT) {
        bool is_speed_cmd = (cmd_val != grp->power_slot.off_val && cmd_val > 0);
        if (is_speed_cmd) {
          for (size_t d = 0; d < ack_diff_count; ++d) {
            uint8_t k = ack_diff_offsets[d];
            if (k >= ack_payload_start && k < ack_payload_end && k != grp->ack_slots.power_offset) {
              if (ack_after.data[k] == cmd_val) {
                grp->ack_slots.fan_speed_offset = k;
                grp->ack_slots.discovered = true;
                break;
              }
            }
          }
        }
      }

      // (5) 에어컨 풍량 슬롯
      if (grp->coverage.dev_class == DeviceClass::AIRCON) {
        bool is_speed_cmd = (cmd_val != grp->power_slot.off_val && cmd_val > 0);
        if (is_speed_cmd) {
          for (size_t d = 0; d < ack_diff_count; ++d) {
            uint8_t k = ack_diff_offsets[d];
            if (k >= ack_payload_start && k < ack_payload_end && k != grp->ack_slots.power_offset) {
              if (ack_after.data[k] == cmd_val) {
                grp->ack_slots.fan_speed_offset = k;
                grp->ack_slots.discovered = true;
                break;
              }
            }
          }
        }
      }

      // (6) 가스 밸브 차단 상태 슬롯 [VS]
      if (grp->coverage.dev_class == DeviceClass::GAS && grp->close_slot.discovered) {
        for (size_t d = 0; d < ack_diff_count; ++d) {
          uint8_t k = ack_diff_offsets[d];
          if (k >= ack_payload_start && k < ack_payload_end) {
            if (ack_after.data[k] == grp->close_slot.off_val) {
              grp->ack_slots.valve_state_offset = k;
              grp->ack_slots.discovered = true;
              break;
            }
          }
        }
      }
    } // end hint == NONE

    if (grp->ack_slots.discovered) {
      grp->ack_slots.sample_count++;
    }
  }

  // 7. 학습 상태 전이 (SlotCoverage 완전성 기반)
  if (!has_before) {
    if (grp->status == GroupControlTemplate::Status::WAITING) {
      grp->status = GroupControlTemplate::Status::PARTIAL;
    }
  } else {
    if (grp->coverage.isFullyCovered()) {
      grp->status = GroupControlTemplate::Status::VERIFIED;
    } else {
      grp->status = GroupControlTemplate::Status::CAPTURING;
    }
  }
  taskEXIT_CRITICAL(&_mux);

  saveToNvs();
}

// ============================================================================
// BUILD CONTROL PACKET
// ============================================================================
bool ControlTemplateRegistry::buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                                 ControlActionType action, int value,
                                                 StaticPacket &out) const {
  const GroupControlTemplate *grp = findGroup(dev_id);
  if (!grp || grp->frame_len < 5) return false;

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser) return false;

  out.channel_id = 1;
  out.length = grp->frame_len;
  out.data.fill(0);
  std::copy(grp->raw_template, grp->raw_template + grp->frame_len, out.data.begin());

  // 단일 유닛 기기 판별: 해당 dev_id로 등록된 기기가 1대 이하인지 동적 확인
  size_t unit_count = 0;
  for (size_t i = 0; i < g_device_repo.count(); ++i) {
    DeviceStateEntry snap{};
    if (g_device_repo.getSnapshot(i, snap) && snap.dev_id == dev_id) {
      unit_count++;
      if (unit_count > 1) break;
    }
  }

  // 주소 슬롯 주입:
  // - 다중 유닛 기기(조명, 각 방 난방 등): 요청된 방 번호(sub1)를 동적으로 주입
  // - 단일 유닛 기기(전열교환기, 가스 등): 월패드가 실제 쏜 제어 주소(ctl_sub1_override)를 고정 주입
  uint8_t actual_sub1 = (unit_count <= 1 && grp->ctl_sub1_override != 0xFF) ? grp->ctl_sub1_override : sub1;
  if (grp->sub1_offset < grp->frame_len) out.data[grp->sub1_offset] = actual_sub1;
  if (grp->sub2_offset < grp->frame_len) out.data[grp->sub2_offset] = sub2;

  // 액션 슬롯 주입 (오직 학습/발견된 슬롯만 주입)
  if (action == ControlActionType::POWER) {
    // [SAFETY] 가스 밸브는 안전상 절대 열기(ON) 명령을 허용하지 않음 (단방향 닫기만 허용)
    if (grp->coverage.dev_class == DeviceClass::GAS && value > 0) {
      return false;
    }
    if (!grp->power_slot.discovered) return false;
    if (grp->power_slot.category_offset < grp->frame_len) {
      out.data[grp->power_slot.category_offset] = grp->power_slot.category_val;
    }
    if (grp->power_slot.action_offset < grp->frame_len) {
      out.data[grp->power_slot.action_offset] = (value > 0) ? grp->power_slot.on_val : grp->power_slot.off_val;
    }
  } else if (action == ControlActionType::SET_TEMP) {
    if (!grp->temp_slot.discovered) return false;
    if (grp->temp_slot.category_offset < grp->frame_len) {
      out.data[grp->temp_slot.category_offset] = grp->temp_slot.category_val;
    }
    if (grp->temp_slot.action_offset < grp->frame_len) {
      // 주거 난방의 보편적 물리 온도 범위(5℃~35℃)로 안전 제어
      uint8_t t_val = static_cast<uint8_t>(constrain(value, 5, 35));
      out.data[grp->temp_slot.action_offset] = t_val;
    }
  } else if (action == ControlActionType::FAN_SPEED) {
    if (!grp->speed_slot.discovered) return false;
    if (grp->speed_slot.category_offset < grp->frame_len) {
      out.data[grp->speed_slot.category_offset] = grp->speed_slot.category_val;
    }
    if (grp->speed_slot.action_offset < grp->frame_len) {
      uint8_t speed_token = 0;
      if (grp->speed_slot.level_count > 0) {
        // 동적 학습된 토큰 테이블 참조 (1-based -> 0-based 인덱스)
        int idx = constrain(value - 1, 0, grp->speed_slot.level_count - 1);
        speed_token = grp->speed_slot.level_tokens[idx];
      } else {
        uint8_t min_s = (grp->speed_slot.min_val > 0) ? grp->speed_slot.min_val : 1;
        uint8_t max_s = (grp->speed_slot.max_val > 0) ? grp->speed_slot.max_val : 3;
        speed_token = static_cast<uint8_t>(constrain(value, min_s, max_s));
      }
      out.data[grp->speed_slot.action_offset] = speed_token;
    }
  } else if (action == ControlActionType::VALVE_CLOSE) {
    if (!grp->close_slot.discovered) return false;
    if (grp->close_slot.action_offset < grp->frame_len) {
      out.data[grp->close_slot.action_offset] = grp->close_slot.off_val;
    }
  }

  // 체크섬 및 ETX 종단
  if (out.length >= 3) {
    out.data[out.length - 2] = parser->calculateChecksum(out.data.data(), out.length);
    out.data[out.length - 1] = parser->getEtx();
  }
  return true;
}

// ============================================================================
// NVS
// ============================================================================
void ControlTemplateRegistry::saveToNvs() {
  // 스택 오버플로우 원천 방지:
  // _groups는 registerOrTouch()에 의해 항상 dev_id 오름차순으로 정렬 유지됩니다.
  // 따라서 2.7KB의 대형 배열을 스택에 복사하여 std::sort를 수행할 필요가 전혀 없으며,
  // 유효한 개수를 확인한 뒤 락을 풀고 1건씩 안전하게 NVS에 기록합니다. (스택 소모: 수십 바이트)
  uint8_t save_count = 0;
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id != 0) {
      save_count++;
    }
  }
  taskEXIT_CRITICAL(&_mux);

  Preferences prefs;
  if (!prefs.begin("ctl_tmpls", false)) return;

  prefs.putUChar("cnt", save_count);
  uint8_t saved_idx = 0;
  for (size_t i = 0; i < MAX_GROUPS && saved_idx < save_count; ++i) {
    GroupControlTemplate temp{};
    bool has_item = false;

    taskENTER_CRITICAL(&_mux);
    if (i < _group_count && _groups[i].dev_id != 0) {
      temp = _groups[i];
      has_item = true;
    }
    taskEXIT_CRITICAL(&_mux);

    if (has_item) {
      char key[16];
      snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(saved_idx));
      NvsEnvelope<GroupControlTemplate> env{};
      env.payload = temp;
      env.seal();
      prefs.putBytes(key, &env, sizeof(env));
      saved_idx++;
    }
  }
  prefs.end();
}

void ControlTemplateRegistry::loadFromNvs() {
  Preferences prefs;
  if (!prefs.begin("ctl_tmpls", true)) return;

  uint8_t cnt = prefs.getUChar("cnt", 0);
  if (cnt > MAX_GROUPS) cnt = MAX_GROUPS;

  // 단일 템플릿 단위로 읽어서 레지스트리에 순차 적재 (스택 소모 최소화)
  taskENTER_CRITICAL(&_mux);
  _group_count = 0;
  for (size_t i = 0; i < MAX_GROUPS; ++i) {
    _groups[i] = GroupControlTemplate{};
  }
  taskEXIT_CRITICAL(&_mux);

  for (size_t i = 0; i < cnt; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    GroupControlTemplate temp{};
    bool loaded = false;

    // 1) NvsEnvelope 무결성 검증 로드 시도
    NvsEnvelope<GroupControlTemplate> env{};
    size_t rlen = prefs.getBytes(key, &env, sizeof(env));
    if (rlen == sizeof(env) && env.verify()) {
      temp = env.payload;
      loaded = true;
    } else if (rlen == sizeof(GroupControlTemplate)) {
      // 2) 레거시 비-래핑 템플릿 하위 호환 로드
      memcpy(&temp, &env, sizeof(GroupControlTemplate));
      loaded = true;
    }

    if (loaded && temp.dev_id != 0) {
      taskENTER_CRITICAL(&_mux);
      if (_group_count < MAX_GROUPS) {
        // dev_id 오름차순 삽입 유지
        size_t insert_idx = _group_count;
        for (size_t j = 0; j < _group_count; ++j) {
          if (_groups[j].dev_id > temp.dev_id) {
            insert_idx = j;
            break;
          }
        }
        // ★ 템플릿 셀프 힐링: sub1_offset이 마스터 주소(2)로 오염된 경우 방 주소(sub2)로 교정
        if (temp.sub1_offset == 2 && temp.sub2_offset > 2) {
          temp.sub1_offset = temp.sub2_offset;
        }
        // ★ 난방 템플릿: 0x07(외출)로 인해 최저 설정온도가 7도로 오염된 경우 복구
        if (temp.coverage.dev_class == DeviceClass::THERMOSTAT && temp.temp_slot.min_val == 7) {
          temp.temp_slot.min_val = 0; // 재학습 유도
        }
        _groups[insert_idx] = temp;
        _group_count++;
      }
      taskEXIT_CRITICAL(&_mux);
    }
  }
  prefs.end();
}
