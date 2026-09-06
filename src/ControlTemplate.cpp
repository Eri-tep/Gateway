#include "ControlTemplate.h"
#include "WallpadParser.h"
#include <Preferences.h>
#include <algorithm>

ControlTemplateRegistry g_control_registry;

// ============================================================================
// SLOT COVERAGE IMPLEMENTATION
// ============================================================================

DeviceClass SlotCoverage::classify(uint8_t dev_id, const AutoProbeDescriptor &ad) {
  if (dev_id == 0) return DeviceClass::UNKNOWN;

  // 1. [사용자 지침 유지] 전열교환기(ERV 0x2B, STX 0xF7, ETX 0xEE) 스마트싱스 호환 예외
  if (dev_id == Config::Devices::DEV_HEAT_EXCHANGER && ad.stx == 0xF7 && ad.etx == 0xEE) {
    return DeviceClass::VENT;
  }

  // 2. 2차 캐시(g_device_repo) 및 1차 캐시(g_polling_targets)에서 해당 dev_id의 ACK 패킷 페이로드 분석
  size_t matched_units = 0;
  bool has_temp_telemetry = false;
  bool has_speed_telemetry = false;
  bool has_gas_signature = false;
  bool has_power_telemetry = false;

  auto analyzeAckPayload = [&](const uint8_t *data, size_t len, uint8_t sub1) {
    if (len < 5) return;
    matched_units++;

    uint8_t payload_start = (ad.offsets_locked && ad.payload_offset < len) ? ad.payload_offset : 7;
    size_t payload_end = (len >= 2) ? (len - 2) : len;
    if (payload_start >= payload_end) return;

    // A. 온도(Thermostat) 시그니처 판별
    // 실내 인간 생활 온도 (현재온도, 설정온도): 14℃ ~ 36℃ (0x0E ~ 0x24)
    // 난방 패킷에는 실내온도와 희망온도 2개의 바이트가 상주
    size_t temp_byte_count = 0;
    for (size_t k = payload_start; k < payload_end; ++k) {
      uint8_t v = data[k];
      if (v >= 14 && v <= 36) {
        temp_byte_count++;
      }
    }
    if (temp_byte_count >= 2) {
      has_temp_telemetry = true;
    }

    // B. 풍량(Ventilation) 시그니처 판별
    // 풍량은 이산적인 단계(1, 2, 3)를 가지며, 전원 바이트(0/1/2)와 함께 나타남
    for (size_t k = payload_start; k < payload_end; ++k) {
      uint8_t v = data[k];
      if (k + 1 < payload_end) {
        uint8_t v2 = data[k + 1];
        if ((v == 1 || v == 2) && (v2 >= 1 && v2 <= 3) && temp_byte_count == 0) {
          has_speed_telemetry = true;
        }
      }
    }

    // C. 가스 밸브 등 단방향 차단 액추에이터 판별
    // 선지식(0x43, 0x04/0x01 등)을 전면 배제하고, 이미 관측된 슬롯 특성이 있을 때만 확정
    const auto *existing_grp = g_control_registry.findGroup(dev_id);
    if (existing_grp && (existing_grp->close_slot.discovered || existing_grp->coverage.valve_close_seen)) {
      has_gas_signature = true;
    }

    // D. 콘센트(Outlet) 시그니처 판별
    // 콘센트는 릴레이 상태 외에 전력량(W) 텔레메트리 바이트를 포함하므로
    // 기본 프레임 길이보다 길고(> base_len + 2) 15바이트 이상이며 온도가 아님
    uint8_t base_len = (ad.learned_query_len > 0) ? ad.learned_query_len : 11;
    if (len > (base_len + 2) && len >= 15 && temp_byte_count < 2) {
      has_power_telemetry = true;
    }
  };

  // 1) 2차 캐시(g_device_repo) 탐색
  size_t repo_cnt = g_device_repo.count();
  for (size_t i = 0; i < repo_cnt; ++i) {
    DeviceStateEntry snap{};
    if (g_device_repo.getSnapshot(i, snap)) {
      if (snap.dev_id == dev_id && snap.last_ack_len >= 5) {
        analyzeAckPayload(snap.last_ack_data.data(), snap.last_ack_len, snap.sub1);
      }
    }
  }

  // 2) 1차 캐시(g_polling_targets) 보완 탐색
  if (matched_units == 0) {
    size_t target_cnt = g_polling_targets.totalCount();
    for (size_t i = 0; i < target_cnt; ++i) {
      PollingTargetEntry target{};
      if (g_polling_targets.getEntry(i, target)) {
        if (target.dev_id == dev_id && target.raw_ack_len >= 5) {
          analyzeAckPayload(target.raw_ack_data.data(), target.raw_ack_len, target.sub1);
        }
      }
    }
  }

  // 우선순위 판정 (물리 역량 기반)
  if (has_temp_telemetry && has_speed_telemetry) {
    return DeviceClass::AIRCON;
  }
  if (has_temp_telemetry) {
    return DeviceClass::THERMOSTAT;
  }
  if (has_speed_telemetry) {
    return DeviceClass::VENT;
  }
  if (has_gas_signature) {
    return DeviceClass::MOMENTARY;
  }
  if (matched_units > 0) {
    return DeviceClass::SWITCH;
  }

  return DeviceClass::UNKNOWN;
}

bool SlotCoverage::isFullyCovered() const {
  switch (dev_class) {
  case DeviceClass::SWITCH:
    return (power_on_seen && power_off_seen) || valve_close_seen;

  case DeviceClass::MOMENTARY:
    return call_seen || valve_close_seen;

  case DeviceClass::THERMOSTAT:
    return (power_on_seen && power_off_seen && temp_set_seen && away_mode_seen &&
            temp_while_off_seen && temp_while_away_seen);

  case DeviceClass::VENT:
    return (power_on_seen && power_off_seen && speed_l1_seen && speed_l2_seen && speed_l3_seen);

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
  _session = ActiveLearningSession{};
  taskEXIT_CRITICAL(&_mux);
}

void ControlTemplateRegistry::autoAssignGroupName(GroupControlTemplate &group) {
  auto ad = g_auto_probing_engine.getDescriptor();
  group.coverage.dev_class = SlotCoverage::classify(group.dev_id, ad);

  // 사용자가 이미 이름을 커스텀 지정한 경우(Unknown/Dev_0x/기본이 아님) 보존
  if (strlen(group.group_name) > 0 &&
      strncmp(group.group_name, "Unknown", 7) != 0 &&
      strncmp(group.group_name, "Dev_0x", 6) != 0 &&
      strcmp(group.group_name, "Thermo") != 0 &&
      strcmp(group.group_name, "Vent") != 0 &&
      strcmp(group.group_name, "Aircon") != 0 &&
      strcmp(group.group_name, "Elevator") != 0) {
    return;
  }

  switch (group.coverage.dev_class) {
  case DeviceClass::SWITCH:
    snprintf(group.group_name, sizeof(group.group_name), "Dev_0x%02X", group.dev_id);
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
  default:
    snprintf(group.group_name, sizeof(group.group_name), "Dev_0x%02X", group.dev_id);
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

GroupControlTemplate *ControlTemplateRegistry::findGroup(uint8_t dev_id) {
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (_groups[i].coverage.dev_class == DeviceClass::UNKNOWN) {
        auto ad = g_auto_probing_engine.getDescriptor();
        DeviceClass dc = SlotCoverage::classify(dev_id, ad);
        if (dc != DeviceClass::UNKNOWN) {
          _groups[i].coverage.dev_class = dc;
          autoAssignGroupName(_groups[i]);
        }
      }
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

  // Phase 3 (offsets_locked) 수렴이 완료되었는데 템플릿이 아직 비어있다면 정밀 합성!
  if (cnt == 0 && g_auto_probing_engine.isOffsetsLocked() && g_polling_targets.activeCount() > 0) {
    const_cast<ControlTemplateRegistry*>(this)->synthesizeFromConvergedCache();
    taskENTER_CRITICAL(&_mux);
    cnt = _group_count;
    taskEXIT_CRITICAL(&_mux);
  }
  return cnt;
}

bool ControlTemplateRegistry::getGroupByIndex(size_t index, GroupControlTemplate &out) const {
  taskENTER_CRITICAL(&_mux);
  if (index < _group_count) {
    if (_groups[index].coverage.dev_class == DeviceClass::UNKNOWN) {
      auto ad = g_auto_probing_engine.getDescriptor();
      DeviceClass dc = SlotCoverage::classify(_groups[index].dev_id, ad);
      if (dc != DeviceClass::UNKNOWN) {
        auto *self = const_cast<ControlTemplateRegistry*>(this);
        self->_groups[index].coverage.dev_class = dc;
        self->autoAssignGroupName(self->_groups[index]);
      }
    }
    out = _groups[index];
    taskEXIT_CRITICAL(&_mux);
    return true;
  }
  taskEXIT_CRITICAL(&_mux);
  return false;
}

bool ControlTemplateRegistry::resetGroup(uint8_t dev_id) {
  taskENTER_CRITICAL(&_mux);
  if (dev_id == 0) {
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

      if (d_id == Config::Devices::DEV_HEAT_EXCHANGER && stx == 0xF7 && etx == 0xEE) {
        grp->ctl_sub1_override = Config::Devices::SUB_HEAT_EXCHANGER_QUERY; // 0x40
      }

      autoAssignGroupName(*grp);
      grp->coverage.dev_class = SlotCoverage::classify(d_id, ad);
      grp->status = GroupControlTemplate::Status::WAITING;
    }
    taskEXIT_CRITICAL(&_mux);
  }

  taskENTER_CRITICAL(&_mux);
  std::sort(_groups, _groups + _group_count, [](const GroupControlTemplate &a, const GroupControlTemplate &b) {
    return a.dev_id < b.dev_id;
  });
  taskEXIT_CRITICAL(&_mux);

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

  span<const uint8_t> ctl_span(ctl.data.data(), ctl.length);
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  if (!parser->extractDeviceKey(ctl_span, dev_id, sub1, sub2)) return;

  // 1. 상태 변화 감지: ack_before가 존재할 때만 diff 확인, 없으면 골격 부분 학습 허용
  bool has_before = (ack_before.length >= 5);
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

  // 2. 그룹 템플릿 확보
  GroupControlTemplate *grp = registerOrTouch(dev_id);
  if (!grp) return;

  auto ad = g_auto_probing_engine.getDescriptor();

  taskENTER_CRITICAL(&_mux);
  grp->frame_len = ctl.length;
  std::copy(ctl.data.begin(), ctl.data.begin() + std::min<size_t>(ctl.length, 32), grp->raw_template);
  grp->last_learned_ms = millis();
  grp->coverage.observation_count++;

  // 3. 주소 슬롯 마스킹 오프셋 감지
  if (ad.offsets_locked) {
    grp->sub1_offset = ad.sub1_offset;
    grp->sub2_offset = ad.sub2_offset;
  } else {
    for (size_t i = 0; i < ctl.length - 2; ++i) {
      if (ctl.data[i] == sub1 && grp->sub1_offset == 0xFF) grp->sub1_offset = i;
      if (ctl.data[i] == sub2 && grp->sub2_offset == 0xFF) grp->sub2_offset = i;
    }
  }

  // 전열교환기(dev_id 0x2B)는 현대통신(0xF7, 0xEE)인 경우에만 CTL 전송 시 sub1 = 0x40 강제
  if (dev_id == Config::Devices::DEV_HEAT_EXCHANGER && ad.stx == 0xF7 && ad.etx == 0xEE) {
    grp->ctl_sub1_override = Config::Devices::SUB_HEAT_EXCHANGER_QUERY;
  }

  // 4. 순수 패킷 차분(Differential) 분석 및 슬롯/토큰 자동 추출
  uint8_t payload_start = (ad.offsets_locked && ad.payload_offset < ctl.length) ? ad.payload_offset : 5;
  size_t end_idx = (ctl.length >= 2) ? (ctl.length - 2) : ctl.length; // CS, ETX 제외

  // ctl과 raw_template 사이에서 값이 달라진 바이트 수집
  uint8_t diff_offsets[8]{0};
  uint8_t diff_vals[8]{0};
  size_t diff_count = 0;

  for (size_t i = payload_start; i < end_idx && diff_count < 8; ++i) {
    if (i == grp->sub1_offset || i == grp->sub2_offset) continue;
    if (ad.offsets_locked && (i == ad.sub1_offset || i == ad.sub2_offset)) continue;

    if (ctl.data[i] != grp->raw_template[i]) {
      diff_offsets[diff_count] = static_cast<uint8_t>(i);
      diff_vals[diff_count] = ctl.data[i];
      diff_count++;
    }
  }

  // raw_template과 차이가 아직 없으면 payload_start 위치의 값을 후보로 채택
  if (diff_count == 0 && payload_start < end_idx) {
    diff_offsets[0] = payload_start;
    diff_vals[0] = ctl.data[payload_start];
    diff_count = 1;
  }

  // 만약 dev_class가 아직 미분류 상태라면 2차 캐시를 통해 즉시 분류 수행
  if (grp->coverage.dev_class == DeviceClass::UNKNOWN) {
    grp->coverage.dev_class = SlotCoverage::classify(dev_id, ad);
    autoAssignGroupName(*grp);
  }

  if (diff_count == 1) {
    uint8_t act_off = diff_offsets[0];
    uint8_t cmd_val = diff_vals[0];

    if (grp->coverage.dev_class == DeviceClass::MOMENTARY) {
      grp->power_slot.discovered = true;
      grp->power_slot.action_offset = act_off;
      grp->power_slot.on_val = cmd_val;
      grp->power_slot.sample_count++;
      grp->coverage.call_seen = true;
    } else {
      grp->power_slot.discovered = true;
      grp->power_slot.action_offset = act_off;
      grp->power_slot.sample_count++;

      if (!grp->coverage.power_on_seen && !grp->coverage.power_off_seen) {
        if (cmd_val == 1 || cmd_val == 0xFF) {
          grp->power_slot.on_val = cmd_val;
          grp->coverage.power_on_seen = true;
        } else {
          grp->power_slot.off_val = cmd_val;
          grp->coverage.power_off_seen = true;
        }
      } else if (grp->coverage.power_on_seen && cmd_val != grp->power_slot.on_val) {
        grp->power_slot.off_val = cmd_val;
        grp->coverage.power_off_seen = true;
      } else if (grp->coverage.power_off_seen && cmd_val != grp->power_slot.off_val) {
        grp->power_slot.on_val = cmd_val;
        grp->coverage.power_on_seen = true;
      } else {
        if (cmd_val == grp->power_slot.on_val) grp->coverage.power_on_seen = true;
        else if (cmd_val == grp->power_slot.off_val) grp->coverage.power_off_seen = true;
      }
    }
  } else if (diff_count >= 2) {
    uint8_t cat_off = diff_offsets[0];
    uint8_t cat_val = diff_vals[0];
    uint8_t act_off = diff_offsets[1];
    uint8_t cmd_val = diff_vals[1];

    if ((grp->coverage.dev_class == DeviceClass::VENT || grp->coverage.dev_class == DeviceClass::AIRCON) &&
        cmd_val >= 1 && cmd_val <= 3) {
      grp->speed_slot.discovered = true;
      grp->speed_slot.action_offset = act_off;
      grp->speed_slot.min_val = 1;
      grp->speed_slot.max_val = 3;
      grp->speed_slot.sample_count++;
      if (cmd_val == 1) grp->coverage.speed_l1_seen = true;
      else if (cmd_val == 2) grp->coverage.speed_l2_seen = true;
      else if (cmd_val == 3) grp->coverage.speed_l3_seen = true;
    } else if (cmd_val >= 10 && cmd_val <= 40) {
      // 연속 수치값 (온도 설정 등)
      grp->temp_slot.discovered = true;
      grp->temp_slot.category_offset = cat_off;
      grp->temp_slot.category_val = cat_val;
      grp->temp_slot.action_offset = act_off;
      if (grp->temp_slot.min_val == 0 || cmd_val < grp->temp_slot.min_val) grp->temp_slot.min_val = cmd_val;
      if (cmd_val > grp->temp_slot.max_val) grp->temp_slot.max_val = cmd_val;
      grp->temp_slot.sample_count++;
      grp->coverage.temp_set_seen = true;
    } else {
      // 카테고리/모드 + 전원
      grp->power_slot.discovered = true;
      grp->power_slot.category_offset = cat_off;
      grp->power_slot.category_val = cat_val;
      grp->power_slot.action_offset = act_off;
      grp->power_slot.sample_count++;
      if (!grp->coverage.power_on_seen) {
        grp->power_slot.on_val = cmd_val;
        grp->coverage.power_on_seen = true;
      } else if (cmd_val != grp->power_slot.on_val) {
        grp->power_slot.off_val = cmd_val;
        grp->coverage.power_off_seen = true;
      }
      if (cmd_val == 0x03) {
        grp->coverage.away_mode_seen = true;
      }
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

  // 주소 슬롯 주입 (전열교환기는 sub1 고정값 강제)
  uint8_t actual_sub1 = (grp->ctl_sub1_override != 0xFF) ? grp->ctl_sub1_override : sub1;
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
    if (grp->speed_slot.action_offset < grp->frame_len) {
      uint8_t min_s = (grp->speed_slot.min_val > 0) ? grp->speed_slot.min_val : 1;
      uint8_t max_s = (grp->speed_slot.max_val > 0) ? grp->speed_slot.max_val : 3;
      out.data[grp->speed_slot.action_offset] = static_cast<uint8_t>(constrain(value, min_s, max_s));
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
// ACTIVE PROBING DISCOVERY ENGINE (EXTENDED FSM)
// ============================================================================
bool ControlTemplateRegistry::setupTargetForProbing(uint8_t dev_id) {
  // 1. 타깃 탐색
  uint8_t t_sub1 = 0, t_sub2 = 0;
  bool found_target = false;
  for (size_t i = 0; i < g_polling_targets.totalCount(); ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry) && entry.dev_id == dev_id && entry.is_active) {
      t_sub1 = entry.sub1;
      t_sub2 = entry.sub2;
      found_target = true;
      break;
    }
  }

  if (!found_target) {
    snprintf(_session.last_log, sizeof(_session.last_log), "No active target for 0x%02X", dev_id);
    return false;
  }

  // 2. 기준 상태 스냅샷 확보
  const auto *cached = g_device_repo.find(dev_id, t_sub1, t_sub2);
  if (!cached && dev_id == Config::Devices::DEV_HEAT_EXCHANGER) {
    cached = g_device_repo.find(dev_id, Config::Devices::SUB_HEAT_EXCHANGER_CTRL_ACK, t_sub2);
  }

  if (!cached || cached->last_ack_len == 0) {
    snprintf(_session.last_log, sizeof(_session.last_log), "Target 0x%02X not cached yet", dev_id);
    return false;
  }

  taskENTER_CRITICAL(&_mux);
  _session.in_progress = true;
  _session.target_dev_id = dev_id;
  _session.target_sub1 = t_sub1;
  _session.target_sub2 = t_sub2;
  _session.current_step = ActiveProbingStep::PROBE_POWER_ON;
  _session.retry_count = 0;
  _session.candidate_offset = 0;
  _session.candidate_token = 0;
  _session.baseline_len = cached->last_ack_len;
  std::copy(cached->last_ack_data.begin(), cached->last_ack_data.begin() + cached->last_ack_len, _session.baseline_ack);
  _session.step_start_ms = millis();
  snprintf(_session.last_log, sizeof(_session.last_log), "Active probing started for 0x%02X (%02X:%02X)",
           dev_id, t_sub1, t_sub2);

  GroupControlTemplate *grp = registerOrTouch(dev_id);
  if (grp) {
    grp->status = GroupControlTemplate::Status::PROBING;
    if (dev_id == Config::Devices::DEV_HEAT_EXCHANGER) {
      grp->ctl_sub1_override = Config::Devices::SUB_HEAT_EXCHANGER_QUERY;
    }
  }
  taskEXIT_CRITICAL(&_mux);

  return true;
}

bool ControlTemplateRegistry::startActiveLearning(uint8_t dev_id) {
  // 1. 오프셋 락 확인 (수렴 완료 사전 안전 검사)
  auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.offsets_locked) {
    snprintf(_session.last_log, sizeof(_session.last_log), "Cannot start: offsets not locked yet");
    return false;
  }

  if (_group_count == 0) {
    synthesizeFromConvergedCache();
  }
  if (_group_count == 0) {
    snprintf(_session.last_log, sizeof(_session.last_log), "No device groups available to learn");
    return false;
  }

  if (dev_id == 0) {
    // 전체 기기 순차 능동 학습 모드 (Batch Mode)
    _session.learn_all = true;
    _session.current_all_idx = 0;
    while (_session.current_all_idx < _group_count) {
      uint8_t d_id = _groups[_session.current_all_idx].dev_id;
      if (d_id != 0 && setupTargetForProbing(d_id)) {
        return true;
      }
      _session.current_all_idx++;
    }
    _session.learn_all = false;
    snprintf(_session.last_log, sizeof(_session.last_log), "Failed to start active learning for any group");
    return false;
  } else {
    // 특정 기기 1개 단독 학습 모드
    _session.learn_all = false;
    _session.current_all_idx = 0;
    return setupTargetForProbing(dev_id);
  }
}

void ControlTemplateRegistry::abortActiveLearning() {
  taskENTER_CRITICAL(&_mux);
  if (_session.in_progress) {
    _session.learn_all = false;
    _session.current_step = ActiveProbingStep::RESTORE_BASELINE;
    snprintf(_session.last_log, sizeof(_session.last_log), "Aborted by user, restoring baseline...");
  }
  taskEXIT_CRITICAL(&_mux);
}

void ControlTemplateRegistry::processActiveLearning() {
  if (!_session.in_progress) return;

  uint32_t now = millis();
  if (now - _session.step_start_ms < 200) return; // 단계 간 인터벌

  GroupControlTemplate *grp = findGroup(_session.target_dev_id);
  if (!grp) {
    _session.in_progress = false;
    return;
  }

  auto sendProbe = [&](ControlActionType act, int val, ActiveProbingStep next_step, const char *log_msg) {
    StaticPacket pkt{};
    if (buildControlPacket(_session.target_dev_id, _session.target_sub1, _session.target_sub2, act, val, pkt)) {
      pkt.channel_id = 6;
      (void)Queue_EnqueueDropHead(g_ch1_vip_queue, pkt);
      _session.current_step = next_step;
      _session.step_start_ms = now;
      snprintf(_session.last_log, sizeof(_session.last_log), "%s", log_msg);
    } else {
      _session.current_step = ActiveProbingStep::FAILED;
      _session.in_progress = false;
    }
  };

  auto checkAckReceived = [&](bool &seen_flag, ActiveProbingStep next_step) {
    const auto *cached = g_device_repo.find(_session.target_dev_id, _session.target_sub1, _session.target_sub2);
    if (!cached && _session.target_dev_id == Config::Devices::DEV_HEAT_EXCHANGER) {
      cached = g_device_repo.find(_session.target_dev_id, Config::Devices::SUB_HEAT_EXCHANGER_CTRL_ACK, _session.target_sub2);
    }

    if (cached && cached->last_updated_ms >= _session.step_start_ms) {
      seen_flag = true;
      grp->power_slot.discovered = true;
      grp->power_slot.sample_count++;
      _session.retry_count = 0;
      _session.current_step = next_step;
      _session.step_start_ms = now;
      return true;
    } else if (now - _session.step_start_ms > 1000) {
      _session.retry_count++;
      if (_session.retry_count >= 3) {
        _session.current_step = ActiveProbingStep::RESTORE_BASELINE;
      } else {
        _session.step_start_ms = now; // 재시도
      }
    }
    return false;
  };

  switch (_session.current_step) {
  case ActiveProbingStep::PROBE_POWER_ON:
    sendProbe(ControlActionType::POWER, 1, ActiveProbingStep::VERIFY_POWER_ON_ACK, "Probing Power ON...");
    break;

  case ActiveProbingStep::VERIFY_POWER_ON_ACK: {
    bool ok = checkAckReceived(grp->coverage.power_on_seen, ActiveProbingStep::PROBE_POWER_OFF);
    if (ok && grp->temp_slot.discovered) {
      // 난방 켜기 시 기존 저장된 설정온도 복원 여부 검증
      const auto *cached = g_device_repo.find(_session.target_dev_id, _session.target_sub1, _session.target_sub2);
      if (cached && cached->last_ack_len >= 8) {
        uint8_t restored_t = cached->last_ack_data[7];
        if (restored_t >= 15 && restored_t <= 35) {
          grp->temp_slot.discovered = true;
          grp->temp_slot.min_val = 15;
          grp->temp_slot.max_val = 30;
          snprintf(_session.last_log, sizeof(_session.last_log), "Power ON ACK verified: Restored Temp = %u C", restored_t);
        }
      }
    }
    break;
  }

  case ActiveProbingStep::PROBE_POWER_OFF:
    sendProbe(ControlActionType::POWER, 0, ActiveProbingStep::VERIFY_POWER_OFF_ACK, "Probing Power OFF...");
    break;

  case ActiveProbingStep::VERIFY_POWER_OFF_ACK: {
    ActiveProbingStep next_branch = ActiveProbingStep::RESTORE_BASELINE;
    if (grp->coverage.dev_class == DeviceClass::THERMOSTAT || grp->temp_slot.discovered) next_branch = ActiveProbingStep::PROBE_TEMP_L1;
    else if (grp->coverage.dev_class == DeviceClass::VENT || grp->speed_slot.discovered) next_branch = ActiveProbingStep::PROBE_SPEED_L1;
    else if (grp->close_slot.discovered) next_branch = ActiveProbingStep::PROBE_VALVE_CLOSE;

    checkAckReceived(grp->coverage.power_off_seen, next_branch);
    break;
  }

  // ── 난방 단계
  case ActiveProbingStep::PROBE_TEMP_L1:
    sendProbe(ControlActionType::SET_TEMP, 15, ActiveProbingStep::VERIFY_TEMP_L1_ACK, "Probing Temp L1 (15C)...");
    break;

  case ActiveProbingStep::VERIFY_TEMP_L1_ACK:
    checkAckReceived(grp->coverage.temp_set_seen, ActiveProbingStep::PROBE_TEMP_L2);
    break;

  case ActiveProbingStep::PROBE_TEMP_L2:
    sendProbe(ControlActionType::SET_TEMP, 25, ActiveProbingStep::VERIFY_TEMP_L2_ACK, "Probing Baseline Temp (25C)...");
    break;

  case ActiveProbingStep::VERIFY_TEMP_L2_ACK:
    checkAckReceived(grp->coverage.temp_set_seen, ActiveProbingStep::PROBE_AWAY_MODE);
    break;

  case ActiveProbingStep::PROBE_AWAY_MODE:
    sendProbe(ControlActionType::POWER, 3, ActiveProbingStep::VERIFY_AWAY_ACK, "Probing Away Mode...");
    break;

  case ActiveProbingStep::VERIFY_AWAY_ACK: {
    bool ok = checkAckReceived(grp->coverage.away_mode_seen, ActiveProbingStep::PROBE_RECALL_CHECK);
    if (ok) {
      // 외출 진입 시 특정 온도로 고정되는지 판별
      const auto *cached = g_device_repo.find(_session.target_dev_id, _session.target_sub1, _session.target_sub2);
      if (cached && cached->last_ack_len >= 8) {
        uint8_t away_t = cached->last_ack_data[7];
        if (away_t != 25 && away_t >= 5 && away_t <= 20) {
          grp->away_has_dedicated_temp = true;
          grp->away_fixed_temp = away_t;
          snprintf(_session.last_log, sizeof(_session.last_log), "Away mode fixed temp detected: %u C", away_t);
        }
      }
    }
    break;
  }

  case ActiveProbingStep::PROBE_RECALL_CHECK:
    // 외출 상태에서 순수 전원 켜기(cmd=0x01) 송출하여 원래 25C로 복원되는지 검증
    sendProbe(ControlActionType::POWER, 1, ActiveProbingStep::VERIFY_RECALL_ACK, "Testing Temp Recall from Away (cmd=0x01)...");
    break;

  case ActiveProbingStep::VERIFY_RECALL_ACK: {
    bool dummy = false;
    bool ok = checkAckReceived(dummy, ActiveProbingStep::PROBE_TEMP_WHILE_OFF);
    if (ok) {
      const auto *cached = g_device_repo.find(_session.target_dev_id, _session.target_sub1, _session.target_sub2);
      if (cached && cached->last_ack_len >= 8) {
        uint8_t recalled_t = cached->last_ack_data[7];
        if (recalled_t == 25) {
          grp->temp_recall_verified = true;
          snprintf(_session.last_log, sizeof(_session.last_log), "Temp Recall VERIFIED! Successfully restored 25 C from Away");
        }
      }
    }
    break;
  }

  case ActiveProbingStep::PROBE_TEMP_WHILE_OFF:
    // 전원 OFF 상태에서 온도 설정 송출
    sendProbe(ControlActionType::SET_TEMP, 22, ActiveProbingStep::VERIFY_TEMP_WHILE_OFF_ACK, "Probing Temp-while-OFF compound...");
    break;

  case ActiveProbingStep::VERIFY_TEMP_WHILE_OFF_ACK:
    checkAckReceived(grp->coverage.temp_while_off_seen, ActiveProbingStep::PROBE_TEMP_WHILE_AWAY);
    break;

  case ActiveProbingStep::PROBE_TEMP_WHILE_AWAY:
    // 외출 상태에서 온도 설정 송출
    sendProbe(ControlActionType::SET_TEMP, 24, ActiveProbingStep::VERIFY_TEMP_WHILE_AWAY_ACK, "Probing Temp-while-AWAY compound...");
    break;

  case ActiveProbingStep::VERIFY_TEMP_WHILE_AWAY_ACK:
    checkAckReceived(grp->coverage.temp_while_away_seen, ActiveProbingStep::RESTORE_BASELINE);
    break;

  // ── 환기 단계
  case ActiveProbingStep::PROBE_SPEED_L1:
    sendProbe(ControlActionType::FAN_SPEED, 1, ActiveProbingStep::VERIFY_SPEED_L1_ACK, "Probing Fan Speed 1...");
    break;

  case ActiveProbingStep::VERIFY_SPEED_L1_ACK:
    checkAckReceived(grp->coverage.speed_l1_seen, ActiveProbingStep::PROBE_SPEED_L2);
    break;

  case ActiveProbingStep::PROBE_SPEED_L2:
    sendProbe(ControlActionType::FAN_SPEED, 2, ActiveProbingStep::VERIFY_SPEED_L2_ACK, "Probing Fan Speed 2...");
    break;

  case ActiveProbingStep::VERIFY_SPEED_L2_ACK:
    checkAckReceived(grp->coverage.speed_l2_seen, ActiveProbingStep::PROBE_SPEED_L3);
    break;

  case ActiveProbingStep::PROBE_SPEED_L3:
    sendProbe(ControlActionType::FAN_SPEED, 3, ActiveProbingStep::VERIFY_SPEED_L3_ACK, "Probing Fan Speed 3...");
    break;

  case ActiveProbingStep::VERIFY_SPEED_L3_ACK:
    checkAckReceived(grp->coverage.speed_l3_seen, ActiveProbingStep::RESTORE_BASELINE);
    break;

  // ── 가스 단계
  case ActiveProbingStep::PROBE_VALVE_CLOSE:
    sendProbe(ControlActionType::VALVE_CLOSE, 1, ActiveProbingStep::VERIFY_VALVE_CLOSE_ACK, "Probing Gas Valve Close...");
    break;

  case ActiveProbingStep::VERIFY_VALVE_CLOSE_ACK:
    checkAckReceived(grp->coverage.valve_close_seen, ActiveProbingStep::RESTORE_BASELINE);
    break;

  // ── 복원 및 완료
  case ActiveProbingStep::RESTORE_BASELINE: {
    StaticPacket restore_pkt{};
    if (buildControlPacket(_session.target_dev_id, _session.target_sub1, _session.target_sub2,
                           ControlActionType::POWER, 0, restore_pkt)) {
      restore_pkt.channel_id = 6;
      (void)Queue_EnqueueDropHead(g_ch1_vip_queue, restore_pkt);
    }

    if (grp->coverage.isFullyCovered()) {
      grp->status = GroupControlTemplate::Status::VERIFIED;
    } else {
      grp->status = GroupControlTemplate::Status::CAPTURING;
    }
    saveToNvs();

    if (_session.learn_all) {
      _session.current_all_idx++;
      bool started_next = false;
      while (_session.current_all_idx < _group_count) {
        uint8_t next_id = _groups[_session.current_all_idx].dev_id;
        if (next_id != 0 && setupTargetForProbing(next_id)) {
          started_next = true;
          break;
        }
        _session.current_all_idx++;
      }
      if (!started_next) {
        _session.in_progress = false;
        _session.learn_all = false;
        _session.current_step = ActiveProbingStep::COMPLETED;
        snprintf(_session.last_log, sizeof(_session.last_log), "Batch active learning COMPLETED for all %zu groups", _group_count);
      }
    } else {
      _session.in_progress = false;
      if (strstr(_session.last_log, "Aborted") || strstr(_session.last_log, "aborted")) {
        _session.current_step = ActiveProbingStep::FAILED;
        snprintf(_session.last_log, sizeof(_session.last_log), "Active probing aborted by user (Baseline restored)");
      } else {
        _session.current_step = ActiveProbingStep::COMPLETED;
        snprintf(_session.last_log, sizeof(_session.last_log), "Active probing COMPLETED for 0x%02X", _session.target_dev_id);
      }
    }
    break;
  }

  default:
    break;
  }
}

// ============================================================================
// NVS
// ============================================================================
void ControlTemplateRegistry::saveToNvs() {
  GroupControlTemplate local_groups[MAX_GROUPS];
  size_t local_count = 0;

  // 1. Critical section에서는 오직 메모리 복사만 신속히 완료 (유효한 dev_id만)
  taskENTER_CRITICAL(&_mux);
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id != 0) {
      local_groups[local_count++] = _groups[i];
    }
  }
  taskEXIT_CRITICAL(&_mux);

  // dev_id 오름차순 정렬 저장
  std::sort(local_groups, local_groups + local_count, [](const GroupControlTemplate &a, const GroupControlTemplate &b) {
    return a.dev_id < b.dev_id;
  });

  // 2. Flash I/O (NVS)는 락이 완전히 풀린 상태에서 안전하게 수행 (Panic 방지)
  Preferences prefs;
  if (!prefs.begin("ctl_tmpls", false)) return;

  prefs.putUChar("cnt", static_cast<uint8_t>(local_count));
  for (size_t i = 0; i < local_count; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    prefs.putBytes(key, &local_groups[i], sizeof(GroupControlTemplate));
  }
  prefs.end();
}

void ControlTemplateRegistry::loadFromNvs() {
  Preferences prefs;
  if (!prefs.begin("ctl_tmpls", true)) return;

  uint8_t cnt = prefs.getUChar("cnt", 0);
  if (cnt > MAX_GROUPS) cnt = MAX_GROUPS;

  GroupControlTemplate local_groups[MAX_GROUPS];
  size_t valid_cnt = 0;
  for (size_t i = 0; i < cnt; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    GroupControlTemplate temp{};
    if (prefs.getBytes(key, &temp, sizeof(GroupControlTemplate)) > 0) {
      if (temp.dev_id != 0) {
        local_groups[valid_cnt++] = temp;
      }
    }
  }
  prefs.end();

  // dev_id 오름차순 정렬
  std::sort(local_groups, local_groups + valid_cnt, [](const GroupControlTemplate &a, const GroupControlTemplate &b) {
    return a.dev_id < b.dev_id;
  });

  taskENTER_CRITICAL(&_mux);
  _group_count = valid_cnt;
  for (size_t i = 0; i < valid_cnt; ++i) {
    _groups[i] = local_groups[i];
  }
  for (size_t i = valid_cnt; i < MAX_GROUPS; ++i) {
    _groups[i] = GroupControlTemplate{};
  }
  taskEXIT_CRITICAL(&_mux);
}
