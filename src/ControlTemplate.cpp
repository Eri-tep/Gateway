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
      _groups[i].coverage.dev_class = cls;
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

  // Phase 3 (offsets_locked) 상태에서 활성 타깃이 있는데 등록된 그룹이 적거나 비어있다면 자동 동기화
  if (g_auto_probing_engine.isOffsetsLocked() && g_polling_targets.activeCount() > 0) {
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
  // 이전 raw_template을 diff 비교용으로 보존한 뒤 새 패킷 복사
  uint8_t prev_raw[32]{0};
  uint8_t prev_len = grp->frame_len;
  if (prev_len > 0) {
    std::copy(grp->raw_template, grp->raw_template + std::min<size_t>(prev_len, 32), prev_raw);
  }

  grp->frame_len = ctl.length;
  std::copy(ctl.data.begin(), ctl.data.begin() + std::min<size_t>(ctl.length, 32), grp->raw_template);
  grp->last_ctl_len = std::min<size_t>(ctl.length, 32);
  std::copy(ctl.data.begin(), ctl.data.begin() + grp->last_ctl_len, grp->last_ctl_raw);

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

  // 월패드가 실제로 송신한 제어 패킷(ctl)의 sub1 주소를 동적 학습
  if (grp->sub1_offset < ctl.length) {
    grp->ctl_sub1_override = ctl.data[grp->sub1_offset];
  }

  // 4. 순수 패킷 차분(Differential) 분석 및 슬롯/토큰 자동 추출
  uint8_t payload_start = (ad.offsets_locked && ad.payload_offset < ctl.length) ? ad.payload_offset : 5;
  size_t end_idx = (ctl.length >= 2) ? (ctl.length - 2) : ctl.length; // CS, ETX 제외

  // ctl과 이전 raw_template(또는 이전 ctl) 사이에서 값이 달라진 바이트 수집
  uint8_t diff_offsets[8]{0};
  uint8_t diff_vals[8]{0};
  size_t diff_count = 0;

  for (size_t i = payload_start; i < end_idx && diff_count < 8; ++i) {
    if (i == grp->sub1_offset || i == grp->sub2_offset) continue;
    if (ad.offsets_locked && (i == ad.sub1_offset || i == ad.sub2_offset)) continue;

    if (prev_len > 0 && ctl.data[i] != prev_raw[i]) {
      diff_offsets[diff_count] = static_cast<uint8_t>(i);
      diff_vals[diff_count] = ctl.data[i];
      diff_count++;
    }
  }

  // 이전 골격과의 차이가 아직 발견되지 않았다면 payload_start 위치의 값을 후보로 채택
  if (diff_count == 0 && payload_start < end_idx) {
    diff_offsets[0] = payload_start;
    diff_vals[0] = ctl.data[payload_start];
    diff_count = 1;
  }

  // 만약 dev_class가 아직 미분류 상태라면 2차 캐시를 통해 즉시 분류 수행 (Thermostat 감지 시)
  if (grp->coverage.dev_class == DeviceClass::UNKNOWN) {
    DeviceClass auto_cls = SlotCoverage::classify(dev_id, ad);
    if (auto_cls != DeviceClass::UNKNOWN) {
      grp->coverage.dev_class = auto_cls;
      autoAssignGroupName(*grp);
    }
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
    } else if (grp->coverage.dev_class == DeviceClass::GAS) {
      grp->close_slot.discovered = true;
      grp->close_slot.action_offset = act_off;
      grp->close_slot.off_val = cmd_val;
      grp->close_slot.sample_count++;
      grp->coverage.valve_close_seen = true;
      grp->coverage.power_off_seen = true;
    } else if (grp->coverage.dev_class == DeviceClass::VENT) {
      // 전열교환기(환기): 단일 바이트 제어 슬롯에서 전원 및 풍량 이산 토큰을 동적 관측
      // 하드코딩 없이 버스에서 관측되는 값을 level_tokens 배열에 순차적으로 동적 등록
      grp->speed_slot.discovered = true;
      grp->speed_slot.action_offset = act_off;
      grp->speed_slot.sample_count++;

      // 기존 등록 여부 확인
      bool token_exists = false;
      for (uint8_t k = 0; k < grp->speed_slot.level_count; ++k) {
        if (grp->speed_slot.level_tokens[k] == cmd_val) {
          token_exists = true;
          break;
        }
      }
      if (!token_exists && grp->speed_slot.level_count < 4) {
        grp->speed_slot.level_tokens[grp->speed_slot.level_count++] = cmd_val;
        grp->speed_slot.min_val = 1;
        grp->speed_slot.max_val = grp->speed_slot.level_count;
      }

      // 관측된 개수에 맞춰 순차적으로 레벨 로드맵 활성화
      if (grp->speed_slot.level_count >= 1) grp->coverage.speed_l1_seen = true;
      if (grp->speed_slot.level_count >= 2) grp->coverage.speed_l2_seen = true;
      if (grp->speed_slot.level_count >= 3) grp->coverage.speed_l3_seen = true;

      // 전원 슬롯과의 연동: 최초 토큰을 ON 토큰으로 채택
      grp->power_slot.discovered = true;
      grp->power_slot.action_offset = act_off;
      if (!grp->coverage.power_on_seen) {
        grp->power_slot.on_val = cmd_val;
        grp->coverage.power_on_seen = true;
      }
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

    if (grp->coverage.dev_class == DeviceClass::VENT || grp->coverage.dev_class == DeviceClass::AIRCON) {
      grp->speed_slot.discovered = true;
      grp->speed_slot.category_offset = cat_off;
      grp->speed_slot.category_val = cat_val;
      grp->speed_slot.action_offset = act_off;
      grp->speed_slot.sample_count++;

      bool token_exists = false;
      for (uint8_t k = 0; k < grp->speed_slot.level_count; ++k) {
        if (grp->speed_slot.level_tokens[k] == cmd_val) {
          token_exists = true;
          break;
        }
      }
      if (!token_exists && grp->speed_slot.level_count < 4) {
        grp->speed_slot.level_tokens[grp->speed_slot.level_count++] = cmd_val;
        grp->speed_slot.min_val = 1;
        grp->speed_slot.max_val = grp->speed_slot.level_count;
      }

      if (grp->speed_slot.level_count >= 1) grp->coverage.speed_l1_seen = true;
      if (grp->speed_slot.level_count >= 2) grp->coverage.speed_l2_seen = true;
      if (grp->speed_slot.level_count >= 3) grp->coverage.speed_l3_seen = true;
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

      // [동적 ENV 학습] 운용자가 온도를 설정했을 때, 응답 패킷(ack_after)에서
      // 설정온도와 인접하며 실내 기온 구간(12~38℃)에 머무는 바이트를 현재온도(ENV) 슬롯으로 동적 확정
      if (ack_after.length >= 5) {
        uint8_t best_env = 0xFF;
        int best_score = -1;
        size_t ack_end = (ack_after.length >= 2) ? (ack_after.length - 2) : ack_after.length;

        for (size_t k = 0; k < ack_end; ++k) {
          if (k == 0 || k == ad.dev_id_offset || k == ad.sub1_offset || k == ad.sub2_offset || k == ad.opcode_offset) continue;
          if (k == act_off || k == cat_off) continue;

          uint8_t v = ack_after.data[k];
          if (v >= 12 && v <= 38) {
            int score = 10;
            int dist = std::abs(static_cast<int>(k) - static_cast<int>(act_off));
            if (dist == 1) score += 30; // 설정온도 인접 최우선
            else if (dist == 2) score += 10;
            if (v >= 15 && v <= 33) score += 15; // 한국 실내 생활 기온 구간

            if (score > best_score) {
              best_score = score;
              best_env = static_cast<uint8_t>(k);
            }
          }
        }
        if (best_env != 0xFF) {
          grp->temp_slot.telemetry_offset = best_env;
        }
      }
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
