#include "ControlTemplate.h"
#include "WallpadParser.h"
#include <Preferences.h>
#include <algorithm>

ControlTemplateRegistry g_control_registry;

// ============================================================================
// SLOT COVERAGE IMPLEMENTATION
// ============================================================================

DeviceClass SlotCoverage::classify(uint8_t dev_id, const AutoProbeDescriptor &ad) {
  // 전열교환기(ERV, 0x2B)는 현대통신(STX 0xF7, ETX 0xEE) 규격의 특수 서브주소 체계
  if ((dev_id == Config::Devices::DEV_HEAT_EXCHANGER && ad.stx == 0xF7 && ad.etx == 0xEE) || dev_id == 0x48) {
    return DeviceClass::VENT;
  }
  if (dev_id == Config::Devices::DEV_THERMOSTAT || dev_id == 0x36) {
    return DeviceClass::THERMOSTAT;
  }
  if (dev_id == 0x1B || dev_id == 0x2C) {
    return DeviceClass::GAS;
  }
  if (dev_id == 0x1F || dev_id == 0x39) {
    return DeviceClass::OUTLET;
  }
  if (dev_id == 0x19 || dev_id == 0x0E || dev_id == 0x31) {
    return DeviceClass::LIGHT;
  }
  return DeviceClass::UNKNOWN;
}

bool SlotCoverage::isFullyCovered() const {
  switch (dev_class) {
  case DeviceClass::LIGHT:
    return power_on_seen && power_off_seen;
  case DeviceClass::OUTLET:
    return power_on_seen && power_off_seen;
  case DeviceClass::THERMOSTAT:
    return power_on_seen && power_off_seen && temp_set_seen && away_mode_seen &&
           temp_while_off_seen && temp_while_away_seen;
  case DeviceClass::VENT:
    return power_on_seen && power_off_seen && speed_l1_seen && speed_l2_seen && speed_l3_seen;
  case DeviceClass::GAS:
    return valve_close_seen;
  case DeviceClass::UNKNOWN:
  default:
    return (observation_count >= 3);
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

  switch (group.coverage.dev_class) {
  case DeviceClass::LIGHT:
    snprintf(group.group_name, sizeof(group.group_name), "Light");
    break;
  case DeviceClass::THERMOSTAT:
    snprintf(group.group_name, sizeof(group.group_name), "Thermo");
    break;
  case DeviceClass::VENT:
    snprintf(group.group_name, sizeof(group.group_name), "Vent");
    break;
  case DeviceClass::GAS:
    snprintf(group.group_name, sizeof(group.group_name), "Gas");
    break;
  case DeviceClass::OUTLET:
    snprintf(group.group_name, sizeof(group.group_name), "Outlet");
    break;
  default:
    snprintf(group.group_name, sizeof(group.group_name), "Dev_0x%02X", group.dev_id);
    break;
  }
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
    GroupControlTemplate &new_grp = _groups[_group_count++];
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
    saveToNvs();
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

    GroupControlTemplate *grp = registerOrTouch(entry.dev_id);
    if (!grp) continue;

    taskENTER_CRITICAL(&_mux);
    // 아직 프레임 골격이 없거나 WAITING 상태이면 1차 캐시(raw_query) 기반 골격 구축
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

      // 전열교환기(ERV) 특수 sub1 고정 오버라이드: 현대통신(0xF7, 0xEE) 규격인 경우에만 0x40 강제
      if (entry.dev_id == Config::Devices::DEV_HEAT_EXCHANGER && stx == 0xF7 && etx == 0xEE) {
        grp->ctl_sub1_override = Config::Devices::SUB_HEAT_EXCHANGER_QUERY; // 0x40
      }

      autoAssignGroupName(*grp);
      if (grp->status == GroupControlTemplate::Status::EMPTY) {
        grp->status = GroupControlTemplate::Status::WAITING;
      }
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

  // 4. 액션 슬롯 차분 분석 및 매핑
  uint8_t act_off = (ad.offsets_locked && ad.payload_offset < ctl.length) ? ad.payload_offset : 7;
  uint8_t ctl_cmd = (act_off < ctl.length) ? ctl.data[act_off] : 0;
  uint8_t cat_off = (ad.offsets_locked && ad.sub1_offset < ctl.length) ? ad.sub1_offset : 5;
  uint8_t ctl_cat = (cat_off < ctl.length) ? ctl.data[cat_off] : 0;

  DeviceClass dclass = grp->coverage.dev_class;
  if (dclass == DeviceClass::UNKNOWN) {
    dclass = SlotCoverage::classify(dev_id, ad);
    grp->coverage.dev_class = dclass;
  }

  if (dclass == DeviceClass::THERMOSTAT) {
    if (ctl_cat == 0x45) { // 온도 설정
      grp->temp_slot.discovered = true;
      grp->temp_slot.category_offset = cat_off;
      grp->temp_slot.category_val = 0x45;
      grp->temp_slot.action_offset = act_off;
      grp->temp_slot.min_val = 15;
      grp->temp_slot.max_val = 30;
      grp->temp_slot.sample_count++;
      grp->coverage.temp_set_seen = true;
    } else { // 전원/외출
      grp->power_slot.discovered = true;
      grp->power_slot.category_offset = cat_off;
      grp->power_slot.category_val = 0x46;
      grp->power_slot.action_offset = act_off;
      if (ctl_cmd == 0x01) {
        grp->power_slot.on_val = 0x01;
        grp->coverage.power_on_seen = true;
      } else if (ctl_cmd == 0x04 || ctl_cmd == 0x02) {
        grp->power_slot.off_val = ctl_cmd;
        grp->coverage.power_off_seen = true;
      } else if (ctl_cmd == 0x03) {
        grp->coverage.away_mode_seen = true;
      }
      grp->power_slot.sample_count++;
    }
  } else if (dclass == DeviceClass::VENT) {
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    if (ctl_cmd == 0x01) {
      grp->power_slot.on_val = 0x01;
      grp->coverage.power_on_seen = true;
    } else if (ctl_cmd == 0x02) {
      grp->power_slot.off_val = 0x02;
      grp->coverage.power_off_seen = true;
    }
    grp->power_slot.sample_count++;

    // 풍량 슬롯 (보통 act_off+1 또는 8)
    uint8_t spd_off = act_off + 1;
    if (spd_off < ctl.length) {
      grp->speed_slot.discovered = true;
      grp->speed_slot.action_offset = spd_off;
      grp->speed_slot.min_val = 1;
      grp->speed_slot.max_val = 3;
      grp->speed_slot.sample_count++;

      uint8_t spd_val = ctl.data[spd_off];
      if (spd_val == 1) grp->coverage.speed_l1_seen = true;
      else if (spd_val == 2) grp->coverage.speed_l2_seen = true;
      else if (spd_val == 3) grp->coverage.speed_l3_seen = true;
    }
  } else if (dclass == DeviceClass::GAS) {
    grp->close_slot.discovered = true;
    grp->close_slot.action_offset = act_off;
    grp->close_slot.off_val = ctl_cmd;
    grp->close_slot.sample_count++;
    grp->coverage.valve_close_seen = true;
  } else {
    // LIGHT or OUTLET
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = act_off;
    if (ctl_cmd == 0x01) {
      grp->power_slot.on_val = 0x01;
      grp->coverage.power_on_seen = true;
    } else if (ctl_cmd == 0x02 || ctl_cmd == 0x00) {
      grp->power_slot.off_val = ctl_cmd;
      grp->coverage.power_off_seen = true;
    }
    grp->power_slot.sample_count++;
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

  // 액션 슬롯 주입
  if (action == ControlActionType::POWER) {
    if (grp->power_slot.category_offset < grp->frame_len) {
      out.data[grp->power_slot.category_offset] = grp->power_slot.category_val;
    }
    if (grp->power_slot.action_offset < grp->frame_len) {
      out.data[grp->power_slot.action_offset] = (value > 0) ? grp->power_slot.on_val : grp->power_slot.off_val;
    }
  } else if (action == ControlActionType::SET_TEMP) {
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
    if (grp->speed_slot.action_offset < grp->frame_len) {
      uint8_t min_s = (grp->speed_slot.min_val > 0) ? grp->speed_slot.min_val : 1;
      uint8_t max_s = (grp->speed_slot.max_val > 0) ? grp->speed_slot.max_val : 3;
      out.data[grp->speed_slot.action_offset] = static_cast<uint8_t>(constrain(value, min_s, max_s));
    }
  } else if (action == ControlActionType::VALVE_CLOSE) {
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
bool ControlTemplateRegistry::startActiveLearning(uint8_t dev_id) {
  // 1. 오프셋 락 확인 (수렴 완료 사전 안전 검사)
  auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.offsets_locked) {
    snprintf(_session.last_log, sizeof(_session.last_log), "Cannot start: offsets not locked yet");
    return false;
  }

  // 2. 타깃 탐색
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

  // 3. 기준 상태 스냅샷 확보
  // 전열교환기는 ACK 조회 시 sub1이 0x42일 수도 있으므로 보정 검색
  const auto *cached = g_device_repo.find(dev_id, t_sub1, t_sub2);
  if (!cached && dev_id == Config::Devices::DEV_HEAT_EXCHANGER) {
    cached = g_device_repo.find(dev_id, Config::Devices::SUB_HEAT_EXCHANGER_CTRL_ACK, t_sub2);
  }

  if (!cached || cached->last_ack_len == 0) {
    snprintf(_session.last_log, sizeof(_session.last_log), "Target not cached yet");
    return false;
  }

  taskENTER_CRITICAL(&_mux);
  _session.in_progress = true;
  _session.target_dev_id = dev_id;
  _session.target_sub1 = t_sub1;
  _session.target_sub2 = t_sub2;
  _session.current_step = ActiveProbingStep::PROBE_POWER_ON;
  _session.retry_count = 0;
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

void ControlTemplateRegistry::abortActiveLearning() {
  taskENTER_CRITICAL(&_mux);
  if (_session.in_progress) {
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
    if (ok && grp->coverage.dev_class == DeviceClass::THERMOSTAT) {
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
    if (grp->coverage.dev_class == DeviceClass::THERMOSTAT) next_branch = ActiveProbingStep::PROBE_TEMP_L1;
    else if (grp->coverage.dev_class == DeviceClass::VENT) next_branch = ActiveProbingStep::PROBE_SPEED_L1;
    else if (grp->coverage.dev_class == DeviceClass::GAS) next_branch = ActiveProbingStep::PROBE_VALVE_CLOSE;

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
      _session.current_step = ActiveProbingStep::COMPLETED;
      snprintf(_session.last_log, sizeof(_session.last_log), "Active probing COMPLETED (Fully Covered)");
    } else {
      grp->status = GroupControlTemplate::Status::CAPTURING;
      _session.current_step = ActiveProbingStep::COMPLETED;
      snprintf(_session.last_log, sizeof(_session.last_log), "Active probing finished (Partial coverage)");
    }
    _session.in_progress = false;
    saveToNvs();
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

  // 1. Critical section에서는 오직 메모리 복사만 신속히 완료
  taskENTER_CRITICAL(&_mux);
  local_count = _group_count;
  for (size_t i = 0; i < local_count; ++i) {
    local_groups[i] = _groups[i];
  }
  taskEXIT_CRITICAL(&_mux);

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
  for (size_t i = 0; i < cnt; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    prefs.getBytes(key, &local_groups[i], sizeof(GroupControlTemplate));
  }
  prefs.end();

  taskENTER_CRITICAL(&_mux);
  _group_count = cnt;
  for (size_t i = 0; i < cnt; ++i) {
    _groups[i] = local_groups[i];
  }
  taskEXIT_CRITICAL(&_mux);
}
