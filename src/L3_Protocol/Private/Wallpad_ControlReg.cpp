// ============================================================================
// WallpadRegistry: Level 3 Wallpad Device Learning, Polling & Control SSOT Domain
// Implementation
// ============================================================================
// SRP_COHESIVE: ControlTemplateRegistry and GroupControlTemplate share private normalization/NVS template helpers within a single domain.

#include "L3_Protocol/Private/Wallpad_Learning.h"
#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/Seqlock.h"

#include <Arduino.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <utility>
#include "L3_Protocol/Public/Protocol_Facade.h"


// ============================================================================
// PART 3: GROUP CONTROL TEMPLATE REGISTRY IMPLEMENTATION
// ============================================================================
// ============================================================================
// ControlTemplate: Level 3 Device Capability Blueprint Implementation
// ============================================================================



using namespace ControlTemplateUtils;

// ============================================================================
// Control: ControlTemplateRegistry
// ============================================================================

static ControlTemplateRegistry s_control_registry;

ControlTemplateRegistry &Control_GetRegistry() noexcept {
  return s_control_registry;
}

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
static_assert(std::size(kActionBuilders) == 6,
              "kActionBuilders size must match ControlActionType count");

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
  _commit_mutex = xSemaphoreCreateMutexStatic(&_commit_mutex_storage);
  clear();
}

void ControlTemplateRegistry::init() {
  ProfileRepository::setProfileChangeListener([](uint8_t old_idx, uint8_t new_idx) {
    s_control_registry.onProfileChanged(old_idx, new_idx);
  });
  loadFromNvs();
}

namespace {
struct NormSub1Rule {
  uint8_t dev_id{0};
  uint8_t temp_sub1{0};
  uint8_t speed_sub1{0};
  uint8_t power_sub1{0};
};

static NormSub1Rule s_norm_rules[ControlTemplateRegistry::MAX_GROUPS]{};
static portMUX_TYPE s_norm_rules_mux = portMUX_INITIALIZER_UNLOCKED;
static Gateway::Foundation::SequenceLock s_norm_rules_seqlock;
} // namespace

static std::atomic<uint8_t> s_norm_rule_count{0};

void ControlTemplateRegistry::rebuildNormSub1LutLocked() noexcept {
  NormSub1Rule next_rules[MAX_GROUPS]{};
  size_t next_count = 0;
  for (size_t i = 0; i < _group_count && next_count < MAX_GROUPS; ++i) {
    const auto &grp = _groups[i];
    if (grp.power_slot.category_val != 0 && grp.power_slot.category_val != 0xFF) {
      if ((grp.temp_slot.category_val != 0 && grp.temp_slot.category_val != 0xFF) ||
          (grp.speed_slot.category_val != 0 && grp.speed_slot.category_val != 0xFF)) {
        next_rules[next_count++] = NormSub1Rule{
            .dev_id = grp.dev_id,
            .temp_sub1 = grp.temp_slot.category_val,
            .speed_sub1 = grp.speed_slot.category_val,
            .power_sub1 = grp.power_slot.category_val,
        };
      }
    }
  }
  {
    Gateway::Foundation::CriticalSeqWriterGuard lock(&s_norm_rules_mux, s_norm_rules_seqlock);
    for (size_t i = 0; i < next_count; ++i) {
      s_norm_rules[i] = next_rules[i];
    }
    s_norm_rule_count.store(static_cast<uint8_t>(next_count), std::memory_order_release);
  }
}

void ControlTemplateRegistry::clear() {
  MutexLocker lock(_mutex, kManageLockTimeout);
  if (!lock.isLocked())
    return;
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
  rebuildNormSub1LutLocked();
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

namespace {
struct ClassNamePattern {
  const char *pattern;
  DeviceClass dev_class;
  bool is_substring;
};

static constexpr ClassNamePattern kClassNameRules[] = {
    {"Elevator", DeviceClass::MOMENTARY, false},
    {"EV",       DeviceClass::MOMENTARY, false},
    {"Outlet",   DeviceClass::OUTLET,    true},
};

static DeviceClass resolveDeviceClassFromName(const char *name) noexcept {
  if (!name || !*name)
    return DeviceClass::UNKNOWN;
  for (const auto &rule : kClassNameRules) {
    if (rule.is_substring) {
      if (strcasestr(name, rule.pattern) != nullptr)
        return rule.dev_class;
    } else {
      if (strcasecmp(name, rule.pattern) == 0)
        return rule.dev_class;
    }
  }
  return DeviceClass::UNKNOWN;
}
} // namespace

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
    portENTER_CRITICAL(&_snap_mux);
    setStr(g->group_name, name);
    const DeviceClass inferred_cls = resolveDeviceClassFromName(name);
    if (inferred_cls != DeviceClass::UNKNOWN) {
      g->coverage.dev_class = inferred_cls;
    }
    portEXIT_CRITICAL(&_snap_mux);
  }
  requestNvsSave();
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

    portENTER_CRITICAL(&_snap_mux);
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
    portEXIT_CRITICAL(&_snap_mux);
  }
  requestNvsSave();
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
      } else {
        GroupControlTemplate *g =
            std::find_if(begin, end, [&](const GroupControlTemplate &x) {
              return x.dev_id == dev_id;
            });
        portENTER_CRITICAL(&_snap_mux);
        if (g != end) {
          std::move(g + 1, end, g);
          *(end - 1) = GroupControlTemplate{};
          --_group_count;
          modified = true;
        }
        portEXIT_CRITICAL(&_snap_mux);
      }
    } else {
      portENTER_CRITICAL(&_snap_mux);
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
      portEXIT_CRITICAL(&_snap_mux);
    }
    if (modified) {
      rebuildNormSub1LutLocked();
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
        nvsClearNs(ns);
      }
    }
    synthesizeFromConvergedCache();
  } else {
    requestNvsSave();
  }
  return true;
}

// 슬롯 한 개를 한 줄로 설정 (category_offset 은 현대통신 공통 5)
static void setSlot(ActionSlot &s, uint8_t cat, uint8_t act, uint8_t ack,
                    uint8_t mn = 0, uint8_t mx = 0) {
  s.discovered = true;
  s.category_offset = 5;
  s.category_val = cat;
  s.action_offset = act;
  s.ack_state_offset = ack;
  if (mx) {
    s.min_val = mn;
    s.max_val = mx;
  }
}

static void setLevels(ActionSlot &s, std::initializer_list<uint8_t> tokens) {
  s.level_count = static_cast<decltype(s.level_count)>(tokens.size());
  size_t i = 0;
  for (uint8_t t : tokens)
    s.level_tokens[i++] = t;
}

void ControlTemplateRegistry::applyProfile(const WallpadProfile *profile) {
  if (!profile || !profile->devices || profile->device_count == 0)
    return;

  ESP_LOGI("ControlTemplate", "Applying profile: %s (%u devices)", profile->vendor_name,
           (unsigned)profile->device_count);

  for (size_t i = 0; i < profile->device_count; ++i) {
    const DeviceSpec &spec = profile->devices[i];
    modifyOrCreateGroup(
        spec.dev_id,
        [&](GroupControlTemplate &grp) {
          grp.coverage.dev_class = spec.dev_class;
          setStr(grp.group_name, spec.name);
          grp.frame_len = spec.ctl_len;
          grp.sub1_offset = profile->sub1_offset;
          grp.sub2_offset = profile->sub2_offset;

          // 0x34 엘리베이터: 월패드 쿼리가 없으므로 기본 제어 골격 주입
          if (spec.dev_id == 0x34 && spec.ctl_len == 11) {
            static const uint8_t ev_proto[11] = {0xF7, 0x0B, 0x01, 0x34,
                                                 0x02, 0x41, 0x10, 0x06,
                                                 0x00, 0x9C, 0xEE};
            std::copy(ev_proto, ev_proto + 11, grp.raw_template);
            grp.ctl_sub1_override = 0x10;
          }

          // 전원 슬롯
          grp.power_slot.discovered = true;
          grp.power_slot.action_offset = spec.ctl_payload_offset;
          grp.power_slot.on_val = spec.pwr_on_val;
          grp.power_slot.off_val = spec.pwr_off_val;
          grp.power_slot.ack_state_offset = spec.ctl_ack_state_offset;
          if (spec.pwr_away_val != 0xFF)
            grp.away_mode_token = spec.pwr_away_val;

          const uint8_t act = spec.ctl_payload_offset,
                        ack = spec.ctl_ack_state_offset;
          switch (spec.dev_class) {
          case DeviceClass::THERMOSTAT:
            setSlot(grp.temp_slot, 0x45, act, ack, 14, 36);
            grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
            grp.temp_slot.ack_telemetry_offset = spec.ctl_ack_ambtemp_offset;
            break;
          case DeviceClass::VENT:
            setSlot(grp.speed_slot, 0x42, act, ack, 1, 3);
            setLevels(grp.speed_slot, {0x01, 0x03, 0x07}); // 약/중/강
            setSlot(grp.mode_slot, 0x43, act, ack, 1,
                    4); // 일반/바이패스/자동/공기청정
            break;
          case DeviceClass::GAS:
            setSlot(grp.close_slot, 0x43, act, ack);
            grp.close_slot.off_val = spec.pwr_off_val;
            break;
          case DeviceClass::AIRCON:
            setSlot(grp.temp_slot, 0x45, act, ack, 18, 30);
            grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
            setSlot(grp.speed_slot, 0x42, act, ack, 1, 3);
            setLevels(grp.speed_slot, {0x01, 0x02, 0x03}); // 미풍/약풍/강풍
            setSlot(grp.mode_slot, 0x41, act, ack, 1,
                    5); // 냉방/제습/송풍/자동/난방
            break;
          default:
            break;
          }

          // 제어 응답(ack) 슬롯
          grp.ack_slots.discovered = true;
          grp.ack_slots.power_offset = spec.ctl_ack_state_offset;
          if (spec.dev_class == DeviceClass::THERMOSTAT) {
            grp.ack_slots.target_temp_offset = spec.ctl_ack_echo_offset;
            grp.ack_slots.current_temp_offset = spec.ctl_ack_ambtemp_offset;
          }

          // 쿼리 응답 슬롯
          auto &q = grp.query_slots;
          q.discovered = true;
          q.expected_len = spec.qry_ack_len;
          q.power_offset = spec.qry_power_offset;
          q.target_temp_offset = spec.qry_settemp_offset;
          q.current_temp_offset = spec.qry_ambtemp_offset;
          q.fan_speed_offset = spec.qry_fanspeed_offset;
          q.valve_state_offset = spec.qry_valve_offset;
          q.power_w_offset = spec.qry_watt_h_offset;
        },
        spec.name);

    ESP_LOGI("ControlTemplate",
             "Injected Dev 0x%02X (%s): CTL len=%u, QRY len=%u, StateOff=#%u",
             spec.dev_id, spec.name, spec.ctl_len, spec.qry_ack_len,
             spec.qry_power_offset);
  }
}

void ControlTemplateRegistry::matchAndInject(const AutoProbeDescriptor &ad) {
  if (const WallpadProfile *profile = ProfileMatcher::matchProfile(ad)) {
    applyProfile(profile);
    AutoProbe_GetEngine().injectControlSpec(0x02, 11);
  } else {
    ESP_LOGW("ControlTemplate",
             "No matching wallpad profile found. Fallback to default framing.");
  }
}

void ControlTemplateRegistry::synthesizeFromConvergedCache() {
  const auto ad = AutoProbe_GetEngine().getDescriptor();
  if (!ad.offsets_locked)
    return;

  const uint8_t ctrl_opcode = ad.control_opcode ? ad.control_opcode : 0x02;
  const size_t total = Polling_GetRegistry().totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry entry{};
    if (!Polling_GetRegistry().getEntry(i, entry) || !entry.is_active ||
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

  matchAndInject(ad); // 제조사 명세 기반 슬롯 주입
  requestNvsSave();
}

bool ControlTemplateRegistry::buildControlPacket(uint8_t dev_id, uint8_t sub1,
                                                 uint8_t sub2,
                                                 ControlActionType action,
                                                 int value,
                                                 StaticPacket &out) const {
  GroupControlTemplate grp{};
  if (!findGroup(dev_id, grp) || grp.frame_len < 5)
    return false;
  auto &parser = Universal_GetEngine();

  const size_t act_idx = std::to_underlying(action);
  if (act_idx >= std::size(kActionBuilders))
    return false;

  out.channel_id = 1;
  out.length = grp.frame_len;
  out.data.fill(0);
  std::copy(grp.raw_template, grp.raw_template + grp.frame_len,
            out.data.begin());

  // 단일 유닛 기기는 학습된 sub1 을 사용
  size_t units = 0;
  for (size_t i = 0; i < Device_GetCount() && units < 2; ++i) {
    DeviceMetadata meta{};
    if (Device_GetAtMetadata(i, meta) && meta.dev_id == dev_id)
      ++units;
  }
  if (units == 0)
    units = 1;
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
    const uint16_t cs = parser.calculateChecksum(out.data.data(), out.length);
    if (cs == kChecksumInvalid) [[unlikely]] {
      return false;
    }
    out.data[out.length - 2] = static_cast<uint8_t>(cs);
    out.data[out.length - 1] = parser.getEtx();
  }
  return true;
}

bool ControlTemplateRegistry::saveToNvs() {
  return forceFlush(100);
}
void ControlTemplateRegistry::loadFromNvs() {
  loadFromNvsForProfile(getCurrentProfileIndex());
}

bool ControlTemplateRegistry::saveToNvsForProfile(uint8_t prof_idx) {
  return forceFlush(100);
}

void ControlTemplateRegistry::requestNvsSave() noexcept {
  const uint32_t now = millis();
  bool expected = false;
  if (_nvs_dirty.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    _first_request_ms.store(now, std::memory_order_release);
    _has_first_request.store(true, std::memory_order_release);
  }
  _last_request_ms.store(now, std::memory_order_release);
}

bool ControlTemplateRegistry::commitNvsIfPending() noexcept {
  // 1. Confirm dirty flag only (do not clear until policy and lock are satisfied)
  if (!_nvs_dirty.load(std::memory_order_acquire)) {
    return false;
  }

  const uint32_t now = millis();
  if (_next_retry_ms > 0 && static_cast<int32_t>(now - _next_retry_ms) < 0) {
    return false; // Backoff window active -> return without clearing dirty
  }

  const bool has_first = _has_first_request.load(std::memory_order_relaxed);
  const uint32_t first_req = _first_request_ms.load(std::memory_order_relaxed);
  const uint32_t last_req = _last_request_ms.load(std::memory_order_relaxed);
  const bool starvation_triggered =
      (has_first &&
       static_cast<int32_t>(now - (first_req + Config::Timing::NVS_COMMIT_STARVATION_MS)) >= 0);

  if (!starvation_triggered) {
    // 2-A. Request debounce (quiet window since last mutation)
    if (last_req > 0 &&
        static_cast<int32_t>(now - (last_req + Config::Timing::NVS_COMMIT_GUARD_MS)) < 0) {
      return false;
    }
    // 2-B. Realistic bus quiet window (100ms idle on CH1 inter-packet gap)
    const uint32_t last_ch1_rx = ProtocolDiag_GetLastCh1RxMs();
    if (last_ch1_rx > 0 &&
        static_cast<int32_t>(now - (last_ch1_rx + Config::Timing::NVS_COMMIT_BUS_QUIET_MS)) < 0) {
      return false; // Bus in active transaction
    }
  }

  // 3. Try-lock commit mutex (10ms). If busy, return without clearing dirty
  if (xSemaphoreTake(_commit_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
    return false;
  }

  // 4. Clear dirty flag ONLY after lock acquisition and policy satisfaction
  _nvs_dirty.exchange(false, std::memory_order_acq_rel);

  // 5. Zero padding to guarantee deterministic CRC Wear Guard
  memset(&s_nvs_transfer_buf, 0, sizeof(s_nvs_transfer_buf));

  size_t save_count = 0;
  {
    MutexLocker ram_lock(_mutex, kQueryLockTimeout);
    portENTER_CRITICAL(&_snap_mux);
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id != 0) {
        s_nvs_transfer_buf[save_count++] = _groups[i];
      }
    }
    portEXIT_CRITICAL(&_snap_mux);
  }

  const uint32_t current_crc = FastCrc32(
      reinterpret_cast<const uint8_t *>(s_nvs_transfer_buf),
      save_count * sizeof(GroupControlTemplate));

  if (_has_committed_crc && current_crc == _last_committed_crc32) {
    _failure_count = 0;
    _next_retry_ms = 0;
    _has_first_request.store(false, std::memory_order_release);
    xSemaphoreGive(_commit_mutex);
    return true; // Wear guard: identical data, flash write skipped
  }

  System_TaskWdtResetSelf();

  System_SetNvsBusy(true);
  const uint8_t prof_idx = getCurrentProfileIndex();
  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);
  SystemNvs prefs;
  bool success = false;
  if (prefs.begin(ns, false)) {
    prefs.putUChar("cnt", save_count);
    for (size_t i = 0; i < save_count; ++i) {
      char key[16];
      snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
      nvsPutEnv(prefs, key, s_nvs_transfer_buf[i]);
    }
    prefs.end();
    success = true;
  }
  System_SetNvsBusy(false);

  if (success) {
    _last_committed_crc32 = current_crc;
    _has_committed_crc = true;
    _failure_count = 0;
    _next_retry_ms = 0;
    _has_first_request.store(false, std::memory_order_release);
    ESP_LOGI("CTRL_REG", "[NVS] Decoupled commit succeeded: %u groups (CRC 0x%08X)",
             save_count, current_crc);
  } else {
    // 6. Failure recovery: restore dirty flag, preserve _has_first_request so starvation cap holds!
    _nvs_dirty.store(true, std::memory_order_release);
    _failure_count = std::min<uint8_t>(_failure_count + 1, 5);
    const uint32_t backoff_ms =
        std::min<uint32_t>(3000u << std::min<uint8_t>(_failure_count - 1, 4), 60000u);
    _next_retry_ms = millis() + backoff_ms;
    ESP_LOGW("CTRL_REG", "[NVS] Commit failed (attempt %u), backoff %u ms",
             _failure_count, backoff_ms);
  }

  xSemaphoreGive(_commit_mutex);
  return success;
}

bool ControlTemplateRegistry::forceFlush(uint32_t timeout_ms) noexcept {
  if (!_nvs_dirty.load(std::memory_order_acquire)) {
    return true;
  }

  if (xSemaphoreTake(_commit_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW("CTRL_REG", "[WARN] forceFlush lock timeout (%u ms), skipping to prevent hang", timeout_ms);
    return false;
  }

  _nvs_dirty.exchange(false, std::memory_order_acq_rel);

  memset(&s_nvs_transfer_buf, 0, sizeof(s_nvs_transfer_buf));

  size_t save_count = 0;
  {
    MutexLocker ram_lock(_mutex, kQueryLockTimeout);
    portENTER_CRITICAL(&_snap_mux);
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id != 0) {
        s_nvs_transfer_buf[save_count++] = _groups[i];
      }
    }
    portEXIT_CRITICAL(&_snap_mux);
  }

  const uint32_t current_crc = FastCrc32(
      reinterpret_cast<const uint8_t *>(s_nvs_transfer_buf),
      save_count * sizeof(GroupControlTemplate));

  if (_has_committed_crc && current_crc == _last_committed_crc32) {
    _has_first_request.store(false, std::memory_order_release);
    xSemaphoreGive(_commit_mutex);
    return true;
  }

  System_TaskWdtResetSelf();

  System_SetNvsBusy(true);
  const uint8_t prof_idx = getCurrentProfileIndex();
  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);
  SystemNvs prefs;
  bool success = false;
  if (prefs.begin(ns, false)) {
    prefs.putUChar("cnt", save_count);
    for (size_t i = 0; i < save_count; ++i) {
      char key[16];
      snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
      nvsPutEnv(prefs, key, s_nvs_transfer_buf[i]);
    }
    prefs.end();
    success = true;
  }
  System_SetNvsBusy(false);

  if (success) {
    _last_committed_crc32 = current_crc;
    _has_committed_crc = true;
    _failure_count = 0;
    _next_retry_ms = 0;
    _has_first_request.store(false, std::memory_order_release);
  } else {
    _nvs_dirty.store(true, std::memory_order_release);
  }

  xSemaphoreGive(_commit_mutex);
  return success;
}

void ControlTemplateRegistry::loadFromNvsForProfile(uint8_t prof_idx) {
  MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
  if (!nvs_lock.isLocked())
    return;

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);

  size_t valid = 0;
  {
    SystemNvs prefs;
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

  // Flash wear leveling guard 초기화: 부팅 시 로드된 데이터의 CRC32 등록
  _last_committed_crc32 = FastCrc32(
      reinterpret_cast<const uint8_t *>(s_nvs_transfer_buf),
      valid * sizeof(GroupControlTemplate));
  _has_committed_crc = true;

  // RAM 에 원자적으로 반영 (저장된 항목이 없으면 비움)
  MutexLocker ram_lock(_mutex, kManageLockTimeout);
  if (!ram_lock.isLocked())
    return;
  portENTER_CRITICAL(&_snap_mux);
  std::fill(std::begin(_groups), std::end(_groups), GroupControlTemplate{});
  _group_count = 0;
  for (size_t i = 0; i < valid; ++i)
    insertSorted(&_groups[0], _group_count, MAX_GROUPS, s_nvs_transfer_buf[i]);
  portEXIT_CRITICAL(&_snap_mux);
  rebuildNormSub1LutLocked();
}

void ControlTemplateRegistry::onProfileChanged(uint8_t old_prof_idx,
                                               uint8_t new_prof_idx) {
  if (old_prof_idx == new_prof_idx)
    return;
  forceFlush(100);
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

namespace {

static void decodeOutlet(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *,
                         DecodedDeviceState &out) {
  out.power_w = 0.0f;
  const uint8_t w_off = grp.getWattageOffset(ack.length);
  const size_t valid_len = std::min<size_t>(ack.length, ack.data.size());
  if (auto raw_w = Endian::loadBe16At(std::span(ack.data.data(), valid_len), w_off)) {
    out.power_w = (*raw_w < 50000) ? static_cast<float>(*raw_w) : 0.0f;
  }
}

static void decodeSwitch(const GroupControlTemplate &grp,
                         const StaticPacket &ack, const DeviceStateEntry *dev,
                         DecodedDeviceState &out) {
  if (grp.frame_len >= 17) {
    out.dev_class = DeviceClass::OUTLET;
    decodeOutlet(grp, ack, dev, out);
  }
}

static void decodeGas(const GroupControlTemplate &grp, const StaticPacket &ack,
                         const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t v_off = grp.getValveStateOffset(ack.length);
  bool is_closed =
      (v_off >= ack.length || ack.data[v_off] == grp.close_slot.off_val);
  snprintf(out.valve_state, sizeof(out.valve_state), "%s",
           is_closed ? "closed" : "open");
}

static void decodeMomentary(const GroupControlTemplate &grp,
                            const StaticPacket &ack,
                            const DeviceStateEntry *dev,
                            DecodedDeviceState &out) {
  out.floor = 15;
  out.direction = 0;
  out.ho = 0;
  if (grp.dev_id == 0x34) {
    out.power =
        (ack.length == 11 && ack.data[4] == 0x04)
            ? ((ack.data[8] == 0x06) ? 1 : 0)
            : ((dev && dev->last_ack_len > 0) ? (dev->last_ack_data[0] & 0x01)
                                              : 0);
    return;
  }
  if (ack.length >= 6) {
    out.floor = constrain(static_cast<int>(ack.data[5]), 1, 60);
    out.direction = (ack.length >= 7) ? ack.data[6] : 0;
  }
}

static void decodeThermostat(const GroupControlTemplate &grp,
                             const StaticPacket &ack,
                             const DeviceStateEntry *dev,
                             DecodedDeviceState &out) {
  out.target_temp = dev ? dev->last_target_temp : 0;
  out.current_temp = (dev && dev->last_current_temp > 0)
                         ? dev->last_current_temp
                         : out.target_temp;

  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off < ack.length && grp.away_mode_token != 0 &&
      ack.data[p_off] == grp.away_mode_token)
    out.power = 2;

  uint8_t t_off = grp.getTargetTempOffset(ack.length);
  if (t_off < ack.length && ack.data[t_off] >= 5 && ack.data[t_off] <= 35) {
    out.target_temp = ack.data[t_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = ack.data[t_off];
  }

  uint8_t c_off = grp.getCurrentTempOffset(ack.length);
  if (c_off < ack.length && ack.data[c_off] >= 5 && ack.data[c_off] <= 50) {
    out.current_temp = ack.data[c_off];
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = ack.data[c_off];
  }
}

static void decodeVent(const GroupControlTemplate &grp, const StaticPacket &ack,
                       const DeviceStateEntry *, DecodedDeviceState &out) {
  uint8_t p_off = grp.getPowerOffset(ack.length);
  out.power = (p_off < ack.length && ack.data[p_off] == 0x01) ? 1 : 0;
  out.fan_speed = 1;
  out.vent_mode = 1;

  uint8_t spd_off = grp.getFanSpeedOffset(ack.length);
  if (spd_off < ack.length)
    out.fan_speed = grp.decodeFanSpeed(ack.data[spd_off]);

  if (out.power == 1 && (ack.length >= 6 && ack.data[5] == 0x43) &&
      p_off < ack.length) {
    uint8_t m = ack.data[p_off];
    if (m >= 1 && m <= 4)
      out.vent_mode = m;
  }
}

static void decodeAircon(const GroupControlTemplate &, const StaticPacket &ack,
                         const DeviceStateEntry *dev, DecodedDeviceState &out) {
  size_t base = (ack.length == 14) ? 7 : 8;
  if (base + 4 >= ack.length)
    return;

  out.power = ((ack.data[base] & 0x7F) == 0x01) ? 1 : 0;
  out.vent_mode = constrain(static_cast<int>(ack.data[base + 1]), 1, 5);
  out.fan_speed = (ack.data[base + 2] >= 1 && ack.data[base + 2] <= 4)
                      ? ack.data[base + 2]
                      : 4;

  uint8_t amb = ack.data[base + 3];
  if (amb >= 5 && amb <= 50) {
    out.current_temp = amb;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_current_temp = amb;
  }
  uint8_t tgt = ack.data[base + 4] & 0x7F;
  if (tgt >= 5 && tgt <= 35) {
    out.target_temp = tgt;
    if (dev)
      const_cast<DeviceStateEntry *>(dev)->last_target_temp = tgt;
  }
}

static void decodeUnknown(const GroupControlTemplate &, const StaticPacket &,
                          const DeviceStateEntry *, DecodedDeviceState &) {}

using ClassDecoderFn = void (*)(const GroupControlTemplate &grp,
                                const StaticPacket &ack,
                                const DeviceStateEntry *dev,
                                DecodedDeviceState &out);

static constexpr ClassDecoderFn kClassDecoders[] = {
    decodeUnknown,    // UNKNOWN = 0
    decodeSwitch,     // SWITCH = 1
    decodeOutlet,     // OUTLET = 2
    decodeGas,        // GAS = 3
    decodeMomentary,  // MOMENTARY = 4
    decodeThermostat, // THERMOSTAT = 5
    decodeVent,       // VENT = 6
    decodeAircon      // AIRCON = 7
};
static_assert(std::size(kClassDecoders) == 8,
              "kClassDecoders size must match DeviceClass count");

} // namespace

void ControlTemplate_DecodeDeviceState(const GroupControlTemplate &grp,
                                       const StaticPacket &ack,
                                       const DeviceStateEntry *dev,
                                       DecodedDeviceState &out) noexcept {
  out.dev_class = grp.coverage.dev_class;
  out.should_broadcast = false;
  out.power = 0;
  out.target_temp = 0;
  out.current_temp = 0;
  out.fan_speed = 0;
  out.vent_mode = 1;
  out.power_w = 0.0f;
  out.floor = 1;
  out.direction = 0;
  out.ho = 0;
  snprintf(out.valve_state, sizeof(out.valve_state), "closed");

  // 1. 공통 기본 전원 슬롯 디코딩
  uint8_t p_off = grp.getPowerOffset(ack.length);
  if (p_off != 0xFF && p_off < ack.length) {
    out.power = (ack.data[p_off] == grp.power_slot.on_val) ? 1 : 0;
  }

  // 2. 클래스별 디스패치 (특수 전원 및 파라미터 개별 디코딩)
  const size_t idx = std::to_underlying(grp.coverage.dev_class);
  if (idx < std::size(kClassDecoders)) {
    kClassDecoders[idx](grp, ack, dev, out);
  } else {
    decodeUnknown(grp, ack, dev, out);
  }
}

bool ControlTemplate_DecodeByDevId(uint8_t dev_id,
                                   const StaticPacket &ack,
                                   const DeviceStateEntry *dev,
                                   DecodedDeviceState &out) noexcept {
  GroupControlTemplate grp{};
  bool has_grp = s_control_registry.findGroup(dev_id, grp);
  if (!has_grp && dev_id == 0x34) {
    grp.dev_id = 0x34;
    grp.coverage.dev_class = DeviceClass::MOMENTARY;
    has_grp = true;
  }
  if (!has_grp) {
    return false;
  }
  ControlTemplate_DecodeDeviceState(grp, ack, dev, out);
  return true;
}

static uint8_t ControlTemplate_NormSub1_Slow(uint8_t dev_id, uint8_t sub1) noexcept {
  // Read-mostly derived cache with Seqlock optimistic read (Thread-safe, 0-Copy, O(1))
  // Max 8 elements (typically 1~2 active rules for HVAC). Rebuilt exclusively in cold path.
  constexpr size_t MAX_RETRIES = 4;
  for (size_t retry = 0; retry < MAX_RETRIES; ++retry) {
    const uint32_t seq = s_norm_rules_seqlock.read_begin();
    const size_t count = s_norm_rule_count.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
      if (s_norm_rules[i].dev_id == dev_id) {
        if (sub1 == s_norm_rules[i].temp_sub1 || sub1 == s_norm_rules[i].speed_sub1) {
          uint8_t pwr = s_norm_rules[i].power_sub1;
          if (!s_norm_rules_seqlock.read_retry(seq)) [[likely]] {
            return pwr;
          }
          goto retry_loop;
        }
        if (!s_norm_rules_seqlock.read_retry(seq)) [[likely]] {
          return sub1;
        }
        goto retry_loop;
      }
    }
    if (!s_norm_rules_seqlock.read_retry(seq)) [[likely]] {
      return sub1;
    }
  retry_loop:;
  }
  return sub1;
}

uint8_t ControlTemplate_NormSub1(uint8_t dev_id, uint8_t sub1) noexcept {
  if (s_norm_rule_count.load(std::memory_order_relaxed) == 0) [[likely]] {
    return sub1;
  }
  return ControlTemplate_NormSub1_Slow(dev_id, sub1);
}


