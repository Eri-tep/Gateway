#include "ControlTemplate.h"
#include "WallpadParser.h"
#include "MgmtRpc.h"
#include "ProfileMatcher.h"
#include <Preferences.h>
#include <algorithm>

using namespace ControlTemplateUtils;

ControlTemplateRegistry g_control_registry;
static GroupControlTemplate s_nvs_transfer_buf[ControlTemplateRegistry::MAX_GROUPS];

ControlTemplateRegistry::ControlTemplateRegistry() {
  _mutex = xSemaphoreCreateMutexStatic(&_mutex_storage);
  _nvs_mutex = xSemaphoreCreateMutexStatic(&_nvs_mutex_storage);
  clear();
}

void ControlTemplateRegistry::init() {
  loadFromNvs();
}

void ControlTemplateRegistry::clear() {
  MutexLocker lock(_mutex, kManageLockTimeout);
  if (lock.isLocked()) {
    for (size_t i = 0; i < MAX_GROUPS; ++i) {
      _groups[i] = GroupControlTemplate{};
    }
    _group_count = 0;
  }
}

namespace {

// ── 제어 액션 빌더 테이블 디스패치 ──
bool buildActionPower(const GroupControlTemplate &grp, int value, StaticPacket &out) {
  if (value > 0 && grp.coverage.dev_class == DeviceClass::GAS) {
    return false; // 가스 밸브 원격 열림 방지 안전 가드
  }
  if (!grp.power_slot.discovered) return false;
  if (grp.power_slot.category_offset < grp.frame_len) {
    out.data[grp.power_slot.category_offset] = grp.power_slot.category_val;
  }
  if (grp.power_slot.action_offset < grp.frame_len) {
    if (value == 2 && grp.away_mode_token != 0) {
      out.data[grp.power_slot.action_offset] = grp.away_mode_token;
    } else {
      out.data[grp.power_slot.action_offset] = (value > 0) ? grp.power_slot.on_val : grp.power_slot.off_val;
    }
  }
  return true;
}

bool buildActionSetTemp(const GroupControlTemplate &grp, int value, StaticPacket &out) {
  if (!grp.temp_slot.discovered) return false;
  if (grp.temp_slot.category_offset < grp.frame_len) {
    out.data[grp.temp_slot.category_offset] = grp.temp_slot.category_val;
  }
  if (grp.temp_slot.action_offset < grp.frame_len) {
    uint8_t t_val = static_cast<uint8_t>(constrain(value, 5, 35));
    out.data[grp.temp_slot.action_offset] = t_val;
  }
  return true;
}

bool buildActionFanSpeed(const GroupControlTemplate &grp, int value, StaticPacket &out) {
  if (!grp.speed_slot.discovered) return false;
  if (grp.speed_slot.category_offset < grp.frame_len) {
    out.data[grp.speed_slot.category_offset] = grp.speed_slot.category_val;
  }
  if (grp.speed_slot.action_offset < grp.frame_len) {
    uint8_t speed_token = 0;
    if (grp.speed_slot.level_count > 0) {
      int idx = constrain(value - 1, 0, grp.speed_slot.level_count - 1);
      speed_token = grp.speed_slot.level_tokens[idx];
    } else {
      uint8_t min_s = (grp.speed_slot.min_val > 0) ? grp.speed_slot.min_val : 1;
      uint8_t max_s = (grp.speed_slot.max_val > 0) ? grp.speed_slot.max_val : 3;
      speed_token = static_cast<uint8_t>(constrain(value, min_s, max_s));
    }
    out.data[grp.speed_slot.action_offset] = speed_token;
  }
  return true;
}

bool buildActionValveClose(const GroupControlTemplate &grp, int /*value*/, StaticPacket &out) {
  if (!grp.close_slot.discovered) return false;
  if (grp.close_slot.category_offset < grp.frame_len) {
    out.data[grp.close_slot.category_offset] = grp.close_slot.category_val;
  }
  if (grp.close_slot.action_offset < grp.frame_len) {
    out.data[grp.close_slot.action_offset] = grp.close_slot.off_val;
  }
  return true;
}

bool buildActionVentMode(const GroupControlTemplate &grp, int value, StaticPacket &out) {
  if (!grp.mode_slot.discovered) return false;
  if (grp.mode_slot.category_offset < grp.frame_len) {
    out.data[grp.mode_slot.category_offset] = grp.mode_slot.category_val;
  }
  if (grp.mode_slot.action_offset < grp.frame_len) {
    uint8_t max_m = (grp.mode_slot.max_val > 0) ? grp.mode_slot.max_val : 5;
    uint8_t m_val = static_cast<uint8_t>(constrain(value, 1, max_m));
    out.data[grp.mode_slot.action_offset] = m_val;
  }
  return true;
}

using ActionBuilderFn = bool (*)(const GroupControlTemplate &grp, int value, StaticPacket &out);

static constexpr ActionBuilderFn kActionBuilders[] = {
  buildActionPower,       // POWER = 0
  buildActionSetTemp,     // SET_TEMP = 1
  buildActionFanSpeed,    // FAN_SPEED = 2
  buildActionValveClose,  // VALVE_CLOSE = 3
  buildActionPower,       // MOMENTARY_TRIGGER = 4
  buildActionVentMode     // VENT_MODE = 5
};

} // anonymous namespace

void ControlTemplateRegistry::autoAssignGroupName(GroupControlTemplate &group) {
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

  const char *default_name = DeviceClassToName(group.coverage.dev_class);
  snprintf(group.group_name, sizeof(group.group_name), "%s",
           (group.coverage.dev_class == DeviceClass::UNKNOWN) ? "-" : default_name);
}

bool ControlTemplateRegistry::setGroupName(uint8_t dev_id, const char *name) {
  if (dev_id == 0 || !name || strlen(name) == 0) return false;

  bool modified = false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked()) return false;
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id == dev_id) {
        strncpy(_groups[i].group_name, name, sizeof(_groups[i].group_name) - 1);
        _groups[i].group_name[sizeof(_groups[i].group_name) - 1] = '\0';
        if (strcasecmp(name, "Elevator") == 0 || strcasecmp(name, "EV") == 0) {
          _groups[i].coverage.dev_class = DeviceClass::MOMENTARY;
        } else if (strcasestr(name, "Outlet") != nullptr) {
          _groups[i].coverage.dev_class = DeviceClass::OUTLET;
        }
        modified = true;
        break;
      }
    }
  }
  if (modified) {
    saveToNvs();
    return true;
  }
  return false;
}

bool ControlTemplateRegistry::setGroupClass(uint8_t dev_id, DeviceClass cls, const char *name) {
  if (dev_id == 0) return false;

  bool modified = false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked()) return false;
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id == dev_id) {
        if (_groups[i].coverage.dev_class != cls) {
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
        modified = true;
        break;
      }
    }
  }
  if (modified) {
    saveToNvs();
    return true;
  }
  return false;
}

bool ControlTemplateRegistry::findGroup(uint8_t dev_id, GroupControlTemplate &out, TickType_t timeout) const {
  if (dev_id == 0) return false;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked()) return false;
  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      out = _groups[i];
      return true;
    }
  }
  return false;
}

size_t ControlTemplateRegistry::getGroupsSnapshot(GroupControlTemplate *out_buf, size_t max_count, TickType_t timeout) const {
  if (!out_buf || max_count == 0) return 0;
  MutexLocker lock(_mutex, timeout);
  if (!lock.isLocked()) return 0;
  size_t count = std::min(_group_count, max_count);
  for (size_t i = 0; i < count; ++i) {
    out_buf[i] = _groups[i];
  }
  return count;
}

GroupControlTemplate *ControlTemplateRegistry::registerOrTouchUnlocked(uint8_t dev_id, const char *name) {
  if (dev_id == 0) return nullptr;

  for (size_t i = 0; i < _group_count; ++i) {
    if (_groups[i].dev_id == dev_id) {
      if (name && strlen(name) > 0) {
        strncpy(_groups[i].group_name, name, sizeof(_groups[i].group_name) - 1);
        _groups[i].group_name[sizeof(_groups[i].group_name) - 1] = '\0';
      }
      return &_groups[i];
    }
  }

  if (_group_count < MAX_GROUPS) {
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
    return &new_grp;
  }
  return nullptr;
}

size_t ControlTemplateRegistry::getGroupCount() const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  if (!lock.isLocked()) return 0;
  return _group_count;
}

bool ControlTemplateRegistry::getGroupByIndex(size_t index, GroupControlTemplate &out) const {
  MutexLocker lock(_mutex, kQueryLockTimeout);
  if (!lock.isLocked()) return false;
  if (index < _group_count) {
    out = _groups[index];
    return true;
  }
  return false;
}

bool ControlTemplateRegistry::resetGroup(uint8_t dev_id, bool full_reset) {
  bool modified = false;
  {
    MutexLocker lock(_mutex, kManageLockTimeout);
    if (!lock.isLocked()) return false;

    if (full_reset) {
      if (dev_id == 0) {
        for (size_t i = 0; i < MAX_GROUPS; ++i) {
          _groups[i] = GroupControlTemplate{};
        }
        _group_count = 0;
        modified = true;
      } else {
        for (size_t i = 0; i < _group_count; ++i) {
          if (_groups[i].dev_id == dev_id) {
            for (size_t j = i; j + 1 < _group_count; ++j) {
              _groups[j] = _groups[j + 1];
            }
            _groups[_group_count - 1] = GroupControlTemplate{};
            _group_count--;
            modified = true;
            break;
          }
        }
      }
    } else {
      for (size_t i = 0; i < _group_count; ++i) {
        if (dev_id == 0 || _groups[i].dev_id == dev_id) {
          _groups[i].power_slot  = ActionSlot{};
          _groups[i].temp_slot   = ActionSlot{};
          _groups[i].speed_slot  = ActionSlot{};
          _groups[i].close_slot  = ActionSlot{};
          _groups[i].mode_slot   = ActionSlot{};
          _groups[i].ack_slots   = AckStateSlots{};
          _groups[i].query_slots = QueryStateSlots{};

          DeviceClass preserved_cls = _groups[i].coverage.dev_class;
          _groups[i].coverage = SlotCoverage{};
          _groups[i].coverage.dev_class = preserved_cls;

          modified = true;
          if (dev_id != 0) break;
        }
      }
    }
  }

  if (modified) {
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
  return false;
}

void ControlTemplateRegistry::synthesizeFromConvergedCache() {
  auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.offsets_locked) return;

  uint8_t opcode_offset = ad.opcode_offset;
  uint8_t ctrl_opcode = (ad.control_opcode != 0) ? ad.control_opcode : 0x02;
  uint8_t sub1_offset = ad.sub1_offset;
  uint8_t sub2_offset = ad.sub2_offset;

  size_t total = g_polling_targets.totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry entry{};
    if (!g_polling_targets.getEntry(i, entry) || !entry.is_active || entry.raw_query_len < 5) {
      continue;
    }

    uint8_t d_id = (entry.dev_id != 0) ? entry.dev_id :
                   (ad.dev_id_offset < entry.raw_query_len ? entry.raw_query_data[ad.dev_id_offset] : 0);
    if (d_id == 0 || d_id == 0x34) continue;
    if (entry.source_channels == (1 << 5)) continue;

    modifyOrCreateGroup(d_id, [&](GroupControlTemplate &grp) {
      if (grp.frame_len == 0) {
        grp.frame_len = entry.raw_query_len;
        std::copy(entry.raw_query_data.begin(),
                  entry.raw_query_data.begin() + std::min<size_t>(entry.raw_query_len, 32),
                  grp.raw_template);

        if (opcode_offset < grp.frame_len && ctrl_opcode != 0) {
          grp.raw_template[opcode_offset] = ctrl_opcode;
        }
        grp.sub1_offset = sub1_offset;
        grp.sub2_offset = sub2_offset;
        grp.ctl_sub1_override = entry.sub1;
      }
    });
  }

  // 제조사 하드코딩 명세 기반 슬롯 주입 (ProfileMatcher)
  ProfileMatcher::matchAndInject(ad, *this);

  saveToNvs();
}

bool ControlTemplateRegistry::buildControlPacket(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                                 ControlActionType action, int value,
                                                 StaticPacket &out) const {
  GroupControlTemplate grp{};
  if (!findGroup(dev_id, grp) || grp.frame_len < 5) return false;

  auto *parser = WallpadParserFactory::getActiveParser();
  if (!parser) return false;

  out.channel_id = 1;
  out.length = grp.frame_len;
  out.data.fill(0);
  std::copy(grp.raw_template, grp.raw_template + grp.frame_len, out.data.begin());

  size_t unit_count = 0;
  for (size_t i = 0; i < g_device_repo.count(); ++i) {
    DeviceStateEntry snap{};
    if (g_device_repo.getSnapshot(i, snap) && snap.dev_id == dev_id) {
      unit_count++;
      if (unit_count > 1) break;
    }
  }

  uint8_t actual_sub1 = (unit_count <= 1 && grp.ctl_sub1_override != 0xFF) ? grp.ctl_sub1_override : sub1;
  if (grp.sub1_offset < grp.frame_len) out.data[grp.sub1_offset] = actual_sub1;
  if (grp.sub2_offset < grp.frame_len) out.data[grp.sub2_offset] = sub2;

  const size_t act_idx = static_cast<size_t>(action);
  if (act_idx >= sizeof(kActionBuilders) / sizeof(kActionBuilders[0])) {
    return false;
  }
  if (!kActionBuilders[act_idx](grp, value, out)) {
    return false;
  }

  if (out.length >= 3) {
    out.data[out.length - 2] = parser->calculateChecksum(out.data.data(), out.length);
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
  if (!nvs_lock.isLocked()) return;

  uint8_t save_count = 0;
  {
    MutexLocker ram_lock(_mutex, kManageLockTimeout);
    if (!ram_lock.isLocked()) return;
    for (size_t i = 0; i < _group_count; ++i) {
      if (_groups[i].dev_id != 0) {
        s_nvs_transfer_buf[save_count++] = _groups[i];
      }
    }
  }

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);

  Preferences prefs;
  if (!prefs.begin(ns, false)) return;

  prefs.putUChar("cnt", save_count);
  for (size_t i = 0; i < save_count; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    NvsEnvelope<GroupControlTemplate> env{};
    env.payload = s_nvs_transfer_buf[i];
    env.seal();
    prefs.putBytes(key, &env, sizeof(env));
  }
  prefs.end();
}

void ControlTemplateRegistry::loadFromNvsForProfile(uint8_t prof_idx) {
  MutexLocker nvs_lock(_nvs_mutex, kManageLockTimeout);
  if (!nvs_lock.isLocked()) return;

  char ns[16];
  getControlNamespace(ns, sizeof(ns), prof_idx);

  Preferences prefs;
  if (!prefs.begin(ns, true) || prefs.getUChar("cnt", 0) == 0) {
    prefs.end();
    MutexLocker ram_lock(_mutex, kManageLockTimeout);
    if (ram_lock.isLocked()) {
      _group_count = 0;
      for (size_t i = 0; i < MAX_GROUPS; ++i) {
        _groups[i] = GroupControlTemplate{};
      }
    }
    return;
  }

  uint8_t cnt = prefs.getUChar("cnt", 0);
  if (cnt > MAX_GROUPS) cnt = MAX_GROUPS;

  size_t valid_count = 0;
  for (size_t i = 0; i < cnt; ++i) {
    char key[16];
    snprintf(key, sizeof(key), "grp_%u", static_cast<unsigned>(i));
    GroupControlTemplate temp{};
    bool loaded = false;

    NvsEnvelope<GroupControlTemplate> env{};
    size_t rlen = prefs.getBytes(key, &env, sizeof(env));
    if (rlen == sizeof(env) && env.verify()) {
      temp = env.payload;
      loaded = true;
    } else if (rlen == sizeof(GroupControlTemplate)) {
      memcpy(&temp, &env, sizeof(GroupControlTemplate));
      loaded = true;
    }

    if (loaded && temp.dev_id != 0) {
      if (temp.sub1_offset == 2 && temp.sub2_offset > 2) {
        temp.sub1_offset = temp.sub2_offset;
      }
      if (temp.coverage.dev_class == DeviceClass::THERMOSTAT && temp.temp_slot.min_val == 7) {
        temp.temp_slot.min_val = 0;
      }
      s_nvs_transfer_buf[valid_count++] = temp;
    }
  }
  prefs.end();

  // Atomically commit transfer buffer into RAM under _mutex
  {
    MutexLocker ram_lock(_mutex, kManageLockTimeout);
    if (!ram_lock.isLocked()) return;
    _group_count = 0;
    for (size_t i = 0; i < MAX_GROUPS; ++i) {
      _groups[i] = GroupControlTemplate{};
    }

    for (size_t i = 0; i < valid_count; ++i) {
      const GroupControlTemplate &temp = s_nvs_transfer_buf[i];
      if (_group_count < MAX_GROUPS) {
        size_t insert_idx = _group_count;
        for (size_t j = 0; j < _group_count; ++j) {
          if (_groups[j].dev_id > temp.dev_id) {
            insert_idx = j;
            break;
          }
        }
        for (size_t k = _group_count; k > insert_idx; --k) {
          _groups[k] = _groups[k - 1];
        }
        _groups[insert_idx] = temp;
        _group_count++;
      }
    }
  }
}

void ControlTemplateRegistry::onProfileChanged(uint8_t old_prof_idx, uint8_t new_prof_idx) {
  if (old_prof_idx == new_prof_idx) return;

  saveToNvsForProfile(old_prof_idx);
  loadFromNvsForProfile(new_prof_idx);

  if (getGroupCount() == 0) {
    synthesizeFromConvergedCache();
  }
}

