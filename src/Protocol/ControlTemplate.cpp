// ============================================================================
// ControlTemplate: Level 3 Device Capability Blueprint Implementation
// ============================================================================

#include "Protocol/ControlTemplate.h"
#include "Protocol/WallpadProtocol.h"
#include "Service/ConsoleCli.h"
#include "Service/EngineTask.h"
#include "Service/RemoteService.h"
#include "Transport/NetworkRouter.h"

#include <Preferences.h>
#include <algorithm>

using namespace ControlTemplateUtils;

// ============================================================================
// Control: ControlDispatcher
// ============================================================================

bool ControlDispatcher::dispatch(StaticPacket &req,
                                 StaticPacket &virtual_ack_out) {
  if (UNLIKELY(req.length < 5))
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  span<const uint8_t> frame(req.data.data(), req.length);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  const bool has_key = parser->extractDeviceKey(frame, dev_id, sub1, sub2);

  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    return has_key &&
           g_device_repo.copyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
  }

  GroupControlTemplate grp{};
  const bool has_grp =
      (has_key && dev_id != 0) && g_control_registry.findGroup(dev_id, grp);

  bool is_ctl = parser->isControlPacket(frame);
  if (!is_ctl && has_grp && grp.frame_len > 4 &&
      frame.size() >= grp.frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    const uint8_t op_off =
        (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    is_ctl = (frame[op_off] == grp.raw_template[op_off]);
  }
  if (!is_ctl)
    return false;

  auto drop = [&]() {
    g_telnet_tracer.trace(req.channel_id, false, TraceType::DRP, req);
    return false;
  };

  if (has_grp) {
    // 가스: 원격 '열림' 차단 (닫힘 값만 허용)
    if (grp.coverage.dev_class == DeviceClass::GAS &&
        grp.close_slot.discovered &&
        grp.close_slot.action_offset < req.length &&
        req.data[grp.close_slot.action_offset] != grp.close_slot.off_val)
      return drop();

    // 난방: 온도 설정 범위 검증
    const auto &ts = grp.temp_slot;
    if (grp.coverage.dev_class == DeviceClass::THERMOSTAT && ts.discovered &&
        ts.action_offset < req.length && ts.category_offset != 0xFF &&
        ts.category_offset < req.length &&
        req.data[ts.category_offset] == ts.category_val) {
      const uint8_t t = req.data[ts.action_offset];
      if (t < 5 || t > 35)
        return drop();
    }
  }

  RouteEndpoint ep{1, -1, 0};
  const bool route_known =
      has_key && g_route_registry.lookupRoute(dev_id, sub1, sub2, ep);

  if (route_known && ep.channel_id == 5 && ep.slot_idx >= 0 &&
      ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
    const bool unidir = (has_grp && grp.isUnidirectional()) || dev_id == 0x34;
    if (unidir || ep.slot_idx == 0) {
      Ew11Manager::sendBurstPacket(static_cast<uint8_t>(ep.slot_idx), req, 2,
                                   20);
    } else {
      const bool sent = Hub_SendPacket(static_cast<uint8_t>(ep.slot_idx), req);
      g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP,
                            req);
    }
    return false;
  }

  QueueHandle_t q =
      (req.channel_id == 6) ? g_ch1_vip_queue : g_ch1_control_queue;
  if (Queue_EnqueueDropHead(q, req))
    g_telnet_tracer.trace(1, true, TraceType::CTL, req);
  return false;
}

// ============================================================================
// Control: ControlTemplateRegistry
// ============================================================================

ControlTemplateRegistry g_control_registry;
static GroupControlTemplate
    s_nvs_transfer_buf[ControlTemplateRegistry::MAX_GROUPS];

namespace {

// 슬롯에 category/값을 기록하는 공통 로직 (모든 액션 빌더가 공유)
bool applySlot(const ActionSlot &s, uint8_t val, const GroupControlTemplate &g,
               StaticPacket &out) {
  if (!s.discovered)
    return false;
  if (s.category_offset < g.frame_len)
    out.data[s.category_offset] = s.category_val;
  if (s.action_offset < g.frame_len)
    out.data[s.action_offset] = val;
  return true;
}

bool buildActionPower(const GroupControlTemplate &g, int value,
                      StaticPacket &out) {
  if (value > 0 && g.coverage.dev_class == DeviceClass::GAS)
    return false; // 가스 원격 열림 방지
  const uint8_t val = (value == 2 && g.away_mode_token != 0) ? g.away_mode_token
                      : (value > 0) ? g.power_slot.on_val
                                    : g.power_slot.off_val;
  return applySlot(g.power_slot, val, g, out);
}

bool buildActionSetTemp(const GroupControlTemplate &g, int value,
                        StaticPacket &out) {
  return applySlot(g.temp_slot, static_cast<uint8_t>(constrain(value, 5, 35)),
                   g, out);
}

bool buildActionFanSpeed(const GroupControlTemplate &g, int value,
                         StaticPacket &out) {
  const ActionSlot &s = g.speed_slot;
  uint8_t token;
  if (s.level_count > 0) {
    token = s.level_tokens[constrain(value - 1, 0, s.level_count - 1)];
  } else {
    const uint8_t mn = s.min_val ? s.min_val : 1,
                  mx = s.max_val ? s.max_val : 3;
    token = static_cast<uint8_t>(constrain(value, mn, mx));
  }
  return applySlot(s, token, g, out);
}

bool buildActionValveClose(const GroupControlTemplate &g, int,
                           StaticPacket &out) {
  return applySlot(g.close_slot, g.close_slot.off_val, g, out);
}

bool buildActionVentMode(const GroupControlTemplate &g, int value,
                         StaticPacket &out) {
  const uint8_t mx = g.mode_slot.max_val ? g.mode_slot.max_val : 5;
  return applySlot(g.mode_slot, static_cast<uint8_t>(constrain(value, 1, mx)),
                   g, out);
}

using ActionBuilderFn = bool (*)(const GroupControlTemplate &, int,
                                 StaticPacket &);
constexpr ActionBuilderFn kActionBuilders[] = {
    buildActionPower,      // POWER
    buildActionSetTemp,    // SET_TEMP
    buildActionFanSpeed,   // FAN_SPEED
    buildActionValveClose, // VALVE_CLOSE
    buildActionPower,      // MOMENTARY_TRIGGER
    buildActionVentMode    // VENT_MODE
};

void clearActionSlots(GroupControlTemplate &g) {
  g.power_slot = ActionSlot{};
  g.temp_slot = ActionSlot{};
  g.speed_slot = ActionSlot{};
  g.close_slot = ActionSlot{};
  g.mode_slot = ActionSlot{};
}

// dev_id 오름차순 정렬 삽입 (registerOrTouch / NVS 로드 공통)
GroupControlTemplate *insertSorted(GroupControlTemplate *arr, size_t &count,
                                   size_t cap, const GroupControlTemplate &g) {
  if (count >= cap)
    return nullptr;
  GroupControlTemplate *pos =
      std::find_if(arr, arr + count, [&](const GroupControlTemplate &x) {
        return x.dev_id > g.dev_id;
      });
  std::move_backward(pos, arr + count, arr + count + 1);
  *pos = g;
  ++count;
  return pos;
}

} // namespace

ControlTemplateRegistry::ControlTemplateRegistry() {
  _mutex = xSemaphoreCreateMutexStatic(&_mutex_storage);
  _nvs_mutex = xSemaphoreCreateMutexStatic(&_nvs_mutex_storage);
  clear();
}

void ControlTemplateRegistry::init() { loadFromNvs(); }

void ControlTemplateRegistry::clear() {
  MutexLocker lock(_mutex, kManageLockTimeout);
  if (!lock.isLocked())
    return;
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
}

void ControlTemplateRegistry::autoAssignGroupName(GroupControlTemplate &group) {
  static const char *const kPlaceholders[] = {"-",    "Gas",    "Thermo",
                                              "Vent", "Aircon", "Elevator"};
  const char *n = group.group_name;
  bool placeholder = (*n == '\0') || strncmp(n, "Unknown", 7) == 0 ||
                     strncmp(n, "Dev_0x", 6) == 0;
  for (const char *p : kPlaceholders)
    placeholder |= (strcmp(n, p) == 0);
  if (!placeholder)
    return; // 사용자 지정 이름 유지

  setStr(group.group_name, group.coverage.dev_class == DeviceClass::UNKNOWN
                               ? "-"
                               : DeviceClassToName(group.coverage.dev_class));
}

bool ControlTemplateRegistry::setGroupName(uint8_t dev_id, const char *name) {
  if (dev_id == 0 || !name || !*name)
    return false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *g = std::find_if(
        &_groups[0], &_groups[0] + _group_count,
        [&](const GroupControlTemplate &x) { return x.dev_id == dev_id; });
    if (g == &_groups[0] + _group_count)
      return false;
    setStr(g->group_name, name);
    if (strcasecmp(name, "Elevator") == 0 || strcasecmp(name, "EV") == 0)
      g->coverage.dev_class = DeviceClass::MOMENTARY;
    else if (strcasestr(name, "Outlet"))
      g->coverage.dev_class = DeviceClass::OUTLET;
  }
  saveToNvs();
  return true;
}

bool ControlTemplateRegistry::setGroupClass(uint8_t dev_id, DeviceClass cls,
                                            const char *name) {
  if (dev_id == 0)
    return false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;
    GroupControlTemplate *g = std::find_if(
        &_groups[0], &_groups[0] + _group_count,
        [&](const GroupControlTemplate &x) { return x.dev_id == dev_id; });
    if (g == &_groups[0] + _group_count)
      return false;

    if (g->coverage.dev_class != cls &&
        g->coverage.dev_class != DeviceClass::UNKNOWN) {
      g->coverage = SlotCoverage{}; // 클래스 변경 → 기존 슬롯 무효화
      clearActionSlots(*g);
    }
    g->coverage.dev_class = cls;
    if (name && *name)
      setStr(g->group_name, name);
    else
      autoAssignGroupName(*g);
  }
  saveToNvs();
  return true;
}

bool ControlTemplateRegistry::findGroup(uint8_t dev_id,
                                        GroupControlTemplate &out,
                                        TickType_t timeout) const {
  if (dev_id == 0)
    return false;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked())
    return false;
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      out = _groups[i];
      return true;
    }
  }
  return false;
}

size_t ControlTemplateRegistry::getGroupsSnapshot(GroupControlTemplate *out_buf,
                                                  size_t max_count,
                                                  TickType_t timeout) const {
  if (!out_buf || !max_count)
    return 0;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked())
    return 0;
  const size_t n = std::min(_group_count, max_count);
  std::copy(&_groups[0], &_groups[0] + n, out_buf);
  return n;
}

GroupControlTemplate *
ControlTemplateRegistry::registerOrTouchUnlocked(uint8_t dev_id,
                                                 const char *name) {
  if (dev_id == 0)
    return nullptr;

  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (name && *name)
        setStr(_groups[i].group_name, name);
      return &_groups[i];
    }
  }

  GroupControlTemplate fresh{};
  fresh.dev_id = dev_id;
  if (name && *name)
    setStr(fresh.group_name, name);
  else
    autoAssignGroupName(fresh);
  return insertSorted(&_groups[0], _group_count, MAX_GROUPS, fresh);
}

size_t ControlTemplateRegistry::getGroupCount() const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  return lock.isLocked() ? _group_count : 0;
}

bool ControlTemplateRegistry::getGroupByIndex(size_t index,
                                              GroupControlTemplate &out) const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  if (!lock.isLocked() || index >= _group_count)
    return false;
  out = _groups[index];
  return true;
}

bool ControlTemplateRegistry::resetGroup(uint8_t dev_id, bool full_reset) {
  bool modified = false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked())
      return false;

    GroupControlTemplate *begin = &_groups[0], *end = begin + _group_count;
    if (full_reset) {
      if (dev_id == 0) {
        std::fill(std::begin(_groups), std::end(_groups),
                  GroupControlTemplate{});
        _group_count = 0;
        modified = true;
      } else if (GroupControlTemplate *g =
                     std::find_if(begin, end,
                                  [&](const GroupControlTemplate &x) {
                                    return x.dev_id == dev_id;
                                  });
                 g != end) {
        std::move(g + 1, end, g);
        *(end - 1) = GroupControlTemplate{};
        --_group_count;
        modified = true;
      }
    } else {
      for (GroupControlTemplate *g = begin; g != end; ++g) {
        if (dev_id != 0 && g->dev_id != dev_id)
          continue;
        clearActionSlots(*g);
        g->ack_slots = AckStateSlots{};
        g->query_slots = QueryStateSlots{};
        const DeviceClass cls = g->coverage.dev_class; // 클래스는 유지
        g->coverage = SlotCoverage{};
        g->coverage.dev_class = cls;
        modified = true;
        if (dev_id != 0)
          break;
      }
    }
  }
  if (!modified)
    return false;

  if (full_reset && dev_id == 0) {
    {
      MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
      if (nvs_lock.isLocked()) {
        char ns[16];
        getControlNamespace(ns, sizeof(ns), getCurrentProfileIndex());
        Preferences prefs;
        if (prefs.begin(ns, false)) {
          prefs.clear();
          prefs.end();
        }
      }
    }
    synthesizeFromConvergedCache();
  } else {
    saveToNvs();
  }
  return true;
}

void ControlTemplateRegistry::synthesizeFromConvergedCache() {
  const auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.offsets_locked)
    return;

  const uint8_t ctrl_opcode = ad.control_opcode ? ad.control_opcode : 0x02;
  const size_t total = g_polling_targets.totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry entry{};
    if (!g_polling_targets.getEntry(i, entry) || !entry.is_active ||
        entry.raw_query_len < 5)
      continue;

    const uint8_t d_id = entry.dev_id
                             ? entry.dev_id
                             : (ad.dev_id_offset < entry.raw_query_len
                                    ? entry.raw_query_data[ad.dev_id_offset]
                                    : 0);
    if (d_id == 0 || d_id == 0x34)
      continue;
    if (entry.source_channels == (1 << 5))
      continue; // CH5 단독 유래 제외

    modifyOrCreateGroup(d_id, [&](GroupControlTemplate &grp) {
      if (grp.frame_len != 0)
        return;
      grp.frame_len = entry.raw_query_len;
      std::copy_n(entry.raw_query_data.begin(),
                  std::min<size_t>(entry.raw_query_len, 32), grp.raw_template);
      if (ad.opcode_offset < grp.frame_len)
        grp.raw_template[ad.opcode_offset] = ctrl_opcode;
      grp.sub1_offset = ad.sub1_offset;
      grp.sub2_offset = ad.sub2_offset;
      grp.ctl_sub1_override = entry.sub1;
    });
  }

  ProfileMatcher::matchAndInject(ad, *this); // 제조사 명세 기반 슬롯 주입
  saveToNvs();
}

bool ControlTemplateRegistry::buildControlPacket(uint8_t dev_id, uint8_t sub1,
                                                 uint8_t sub2,
                                                 ControlActionType action,
                                                 int value,
                                                 StaticPacket &out) const {
  GroupControlTemplate grp{};
  if (!findGroup(dev_id, grp) || grp.frame_len < 5)
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser)
    return false;

  const size_t act_idx = static_cast<size_t>(action);
  if (act_idx >= sizeof(kActionBuilders) / sizeof(kActionBuilders[0]))
    return false;

  out.channel_id = 1;
  out.length = grp.frame_len;
  out.data.fill(0);
  std::copy(grp.raw_template, grp.raw_template + grp.frame_len,
            out.data.begin());

  // 단일 유닛 기기는 학습된 sub1 을 사용
  size_t units = 0;
  for (size_t i = 0; i < g_device_repo.count() && units < 2; ++i) {
    DeviceStateEntry snap{};
    if (g_device_repo.getSnapshot(i, snap) && snap.dev_id == dev_id)
      ++units;
  }
  const uint8_t actual_sub1 = (units <= 1 && grp.ctl_sub1_override != 0xFF)
                                  ? grp.ctl_sub1_override
                                  : sub1;
  if (grp.sub1_offset < grp.frame_len)
    out.data[grp.sub1_offset] = actual_sub1;
  if (grp.sub2_offset < grp.frame_len)
    out.data[grp.sub2_offset] = sub2;

  if (!kActionBuilders[act_idx](grp, value, out))
    return false;

  if (out.length >= 3) {
    out.data[out.length - 2] =
        parser->calculateChecksum(out.data.data(), out.length);
    out.data[out.length - 1] = parser->getEtx();
  }
  return true;
}

void ControlTemplateRegistry::saveToNvs() {
  saveToNvsForProfile(getCurrentProfileIndex());
}
void ControlTemplateRegistry::loadFromNvs() {
  loadFromNvsForProfile(getCurrentProfileIndex());
}

void ControlTemplateRegistry::saveToNvsForProfile(uint8_t prof_idx) {
  MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
  if (!nvs_lock.isLocked())
    return;

  uint8_t save_count = 0;
  {
    MutexLocker ram_lock(_mutex, kManageLockTimeout);
    if (!ram_lock.isLocked())
      return;
    for (size_t i = 0; i < _group_count; ++i)
      if (_groups[i].dev_id != 0)
        s_nvs_transfer_buf[save_count++] = _groups[i];
  }

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);
  Preferences prefs;
  if (!prefs.begin(ns, false))
    return;

  prefs.putUChar("cnt", save_count);
  for (size_t i = 0; i < save_count; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    nvsPutEnv(prefs, key, s_nvs_transfer_buf[i]);
  }
  prefs.end();
}

void ControlTemplateRegistry::loadFromNvsForProfile(uint8_t prof_idx) {
  MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
  if (!nvs_lock.isLocked())
    return;

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);

  size_t valid = 0;
  {
    Preferences prefs;
    const bool opened = prefs.begin(ns, true);
    uint8_t cnt = opened ? prefs.getUChar("cnt", 0) : 0;
    if (cnt > MAX_GROUPS)
      cnt = MAX_GROUPS;

    for (size_t i = 0; i < cnt; ++i) {
      char key[16];
      snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
      GroupControlTemplate temp{};
      bool loaded = nvsGetEnv(prefs, key, temp);
      if (!loaded &&
          prefs.getBytesLength(key) ==
              sizeof(GroupControlTemplate)) { // 구버전(봉투 없음) 호환
        loaded = prefs.getBytes(key, &temp, sizeof(temp)) == sizeof(temp);
      }
      if (!loaded || temp.dev_id == 0)
        continue;

      if (temp.sub1_offset == 2 && temp.sub2_offset > 2)
        temp.sub1_offset = temp.sub2_offset;
      if (temp.coverage.dev_class == DeviceClass::THERMOSTAT &&
          temp.temp_slot.min_val == 7)
        temp.temp_slot.min_val = 0;
      s_nvs_transfer_buf[valid++] = temp;
    }
    if (opened)
      prefs.end();
  }

  // RAM 에 원자적으로 반영 (저장된 항목이 없으면 비움)
  MutexLocker ram_lock(_mutex, kManageLockTimeout);
  if (!ram_lock.isLocked())
    return;
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
  for (size_t i = 0; i < valid; ++i)
    insertSorted(&_groups[0], _group_count, MAX_GROUPS, s_nvs_transfer_buf[i]);
}

void ControlTemplateRegistry::onProfileChanged(uint8_t old_prof_idx,
                                               uint8_t new_prof_idx) {
  if (old_prof_idx == new_prof_idx)
    return;
  saveToNvsForProfile(old_prof_idx);
  loadFromNvsForProfile(new_prof_idx);
  if (getGroupCount() == 0)
    synthesizeFromConvergedCache();
}

// ============================================================================
// GroupControlTemplate Optimized Methods
// ============================================================================

uint8_t GroupControlTemplate::getPowerOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.power_offset != 0xFF) {
      return ack_slots.power_offset;
    }
    if (query_slots.discovered && query_slots.power_offset != 0xFF) {
      return query_slots.power_offset;
    }
  }
  if (ack_slots.discovered && ack_slots.power_offset != 0xFF)
    return ack_slots.power_offset;
  if (query_slots.discovered && query_slots.power_offset != 0xFF)
    return query_slots.power_offset;
  if (power_slot.discovered && power_slot.ack_state_offset != 0xFF)
    return power_slot.ack_state_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getTargetTempOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.target_temp_offset != 0xFF) {
      return ack_slots.target_temp_offset;
    }
    if (query_slots.discovered && query_slots.target_temp_offset != 0xFF) {
      return query_slots.target_temp_offset;
    }
  }
  if (query_slots.discovered && query_slots.target_temp_offset != 0xFF)
    return query_slots.target_temp_offset;
  if (ack_slots.discovered && ack_slots.target_temp_offset != 0xFF)
    return ack_slots.target_temp_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getCurrentTempOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.current_temp_offset != 0xFF) {
      return ack_slots.current_temp_offset;
    }
    if (query_slots.discovered && query_slots.current_temp_offset != 0xFF) {
      return query_slots.current_temp_offset;
    }
  }
  if (query_slots.discovered && query_slots.current_temp_offset != 0xFF)
    return query_slots.current_temp_offset;
  if (ack_slots.discovered && ack_slots.current_temp_offset != 0xFF)
    return ack_slots.current_temp_offset;
  return 0xFF;
}

uint8_t
GroupControlTemplate::getFanSpeedOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.fan_speed_offset != 0xFF) {
      return ack_slots.fan_speed_offset;
    }
    if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF) {
      return query_slots.fan_speed_offset;
    }
  }
  if (query_slots.discovered && query_slots.fan_speed_offset != 0xFF)
    return query_slots.fan_speed_offset;
  if (ack_slots.discovered && ack_slots.fan_speed_offset != 0xFF)
    return ack_slots.fan_speed_offset;
  return 0xFF;
}

uint8_t GroupControlTemplate::decodeFanSpeed(uint8_t raw_token) const noexcept {
  if (speed_slot.level_count > 0) {
    for (uint8_t i = 0; i < speed_slot.level_count; ++i) {
      if (speed_slot.level_tokens[i] == raw_token) {
        return static_cast<uint8_t>(i + 1);
      }
    }
  }
  // Lookup Table-driven Token Resolution (Anti-Pattern Elimination)
  switch (raw_token) {
  case 0x11:
  case 0x01:
  case 0x10:
    return 1;
  case 0x13:
  case 0x03:
  case 0x02:
    return 2;
  case 0x17:
  case 0x07:
    return 3;
  default:
    if (raw_token >= 1 && raw_token <= 3)
      return raw_token;
    return 1;
  }
}

uint8_t GroupControlTemplate::decodeVentMode(uint8_t raw_byte) const noexcept {
  // Byte #8 운전 모드 토큰 (1:일반, 2:바이패스, 3:자동, 4:공기청정,
  // 0x81:Reject)
  if (raw_byte >= 1 && raw_byte <= 4)
    return raw_byte;
  const uint8_t nibble = (raw_byte >> 4) & 0x0F;
  if (nibble >= 1 && nibble <= 4)
    return nibble;
  return 1; // 기본 일반 환기 (0x01)
}

uint8_t
GroupControlTemplate::getValveStateOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0) {
    if (frame_len > 0 && pkt_len == frame_len && ack_slots.discovered &&
        ack_slots.valve_state_offset != 0xFF) {
      return ack_slots.valve_state_offset;
    }
    if (query_slots.discovered && query_slots.valve_state_offset != 0xFF) {
      return query_slots.valve_state_offset;
    }
  }
  if (query_slots.discovered && query_slots.valve_state_offset != 0xFF)
    return query_slots.valve_state_offset;
  if (ack_slots.discovered && ack_slots.valve_state_offset != 0xFF)
    return ack_slots.valve_state_offset;
  return 0xFF;
}

uint8_t GroupControlTemplate::getWattageOffset(uint8_t pkt_len) const noexcept {
  if (pkt_len > 0 && pkt_len < 16)
    return 0xFF;
  if (query_slots.discovered && query_slots.power_w_offset != 0xFF)
    return query_slots.power_w_offset;
  return 0xFF;
}

bool GroupControlTemplate::isUnidirectional() const noexcept {
  return dev_id == 0x34; // 단방향 버스트 전송 (엘리베이터 등)
}
