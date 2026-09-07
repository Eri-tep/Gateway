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
    return (power_on_seen && power_off_seen && temp_set_seen && away_mode_seen &&
            temp_while_off_seen && temp_while_away_seen);

  case DeviceClass::VENT:
    return (power_on_seen && power_off_seen && speed_l1_seen);

  case DeviceClass::AIRCON:
    return (power_on_seen && power_off_seen && temp_set_seen &&
            speed_l1_seen && speed_l2_seen && speed_l3_seen);

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
                                                   const StaticPacket &ack_after) {
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

  grp->last_learned_ms = millis();
  grp->coverage.observation_count++;

  // --------------------------------------------------------------------------
  // 4. 주소 슬롯 마스킹 오프셋 감지 및 SUB 주소 동적 학습 (전열교환기 등 가변 SUB 대응)
  // --------------------------------------------------------------------------
  if (ad.offsets_locked) {
    grp->sub1_offset = ad.sub1_offset;
    grp->sub2_offset = ad.sub2_offset;
  } else {
    for (size_t i = 0; i < ctl.length - 2; ++i) {
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
  size_t start_idx = (ad.offsets_locked && ad.payload_offset > 0) ? ad.payload_offset : 1;
  size_t end_idx = (ctl.length >= 2) ? (ctl.length - 2) : ctl.length;

  auto isFixedFrameField = [&](size_t idx) -> bool {
    if (idx == 0) return true;
    if (idx >= ctl.length - 2) return true;
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

  // CTL 프레임에서 실제로 값이 변한 오프셋 수집 (명령 트리거 바이트)
  uint8_t ctl_diff_offsets[8]{0};
  uint8_t ctl_diff_vals[8]{0};
  size_t ctl_diff_count = 0;

  for (size_t i = start_idx; i < end_idx; ++i) {
    if (isFixedFrameField(i)) continue;
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
    size_t common_payload_end = std::min({ack_b_end, ack_a_end, end_idx});
    for (size_t k = start_idx; k < common_payload_end; ++k) {
      if (isFixedFrameField(k)) continue;
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
      if (!isFixedFrameField(i)) {
        act_off = static_cast<uint8_t>(i);
        cmd_val = ctl.data[i];
        break;
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
    } else if (!grp->coverage.away_mode_seen) {
      if (cmd_val != grp->power_slot.on_val && cmd_val != grp->power_slot.off_val) {
        grp->coverage.away_mode_seen = true;
      }
    } else {
      grp->temp_slot.discovered = true;
      grp->temp_slot.action_offset = act_off;
      if (grp->temp_slot.min_val == 0 || cmd_val < grp->temp_slot.min_val) grp->temp_slot.min_val = cmd_val;
      if (cmd_val > grp->temp_slot.max_val) grp->temp_slot.max_val = cmd_val;
      grp->temp_slot.sample_count++;
      grp->coverage.temp_set_seen = true;
    }
  } else if (grp->coverage.dev_class == DeviceClass::VENT) {
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    grp->power_slot.sample_count++;

    if (!grp->coverage.power_on_seen) {
      grp->power_slot.on_val = cmd_val;
      if (cat_off != 0xFF) {
        grp->power_slot.category_offset = cat_off;
        grp->power_slot.category_val = cat_val;
      }
      grp->coverage.power_on_seen = true;
      // 1단 풍량으로도 동시 등록
      if (!grp->speed_slot.discovered) {
        grp->speed_slot.discovered = true;
        grp->speed_slot.action_offset = act_off;
        if (cat_off != 0xFF) {
          grp->speed_slot.category_offset = cat_off;
          grp->speed_slot.category_val = cat_val;
        }
        grp->speed_slot.level_tokens[0] = cmd_val;
        grp->speed_slot.level_count = 1;
        grp->speed_slot.min_val = 1;
        grp->speed_slot.max_val = 1;
        grp->coverage.speed_l1_seen = true;
      }
    } else if (!grp->coverage.power_off_seen) {
      // 1차 제어값(ON)과 다른 새로운 제어값이 들어왔을 때만 2차 제어값(OFF)으로 확정 (사전지식/특정값 하드코딩 완전 배제)
      if (cmd_val != grp->power_slot.on_val) {
        grp->power_slot.off_val = cmd_val;
        grp->coverage.power_off_seen = true;
      }
    } else {
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

  // 5. 학습 상태 전이 (SlotCoverage 완전성 기반)
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
      uint8_t min_t = (grp->temp_slot.min_val > 0) ? grp->temp_slot.min_val : 15;
      uint8_t max_t = (grp->temp_slot.max_val > 0) ? grp->temp_slot.max_val : 30;
      uint8_t t_val = static_cast<uint8_t>(constrain(value, min_t, max_t));
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
      prefs.putBytes(key, &temp, sizeof(GroupControlTemplate));
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

  // 단일 템플릿(340B) 단위로 읽어서 레지스트리에 순차 적재 (스택 소모 최소화)
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
    if (prefs.getBytes(key, &temp, sizeof(GroupControlTemplate)) > 0) {
      if (temp.dev_id != 0) {
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
          for (size_t j = _group_count; j > insert_idx; --j) {
            _groups[j] = _groups[j - 1];
          }
          _groups[insert_idx] = temp;
          _group_count++;
        }
        taskEXIT_CRITICAL(&_mux);
      }
    }
  }
  prefs.end();
}
