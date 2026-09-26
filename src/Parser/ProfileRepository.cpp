#include "ControlTemplate.h"
#include "WallpadParser.h"
#include "Common.h"
#include <Preferences.h>
#include <cstring>
#include <cstdio>

static const VendorProfileDescriptor s_default_profiles[ProfileRepository::MAX_PROFILES] = {
    {"Auto", "Universal Auto-Probing", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    {"Custom1", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    {"Custom2", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0},
    {"Custom3", "[Empty Custom Slot]", 0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0, 2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0}
};

static VendorProfileDescriptor s_active_profiles[ProfileRepository::MAX_PROFILES];
static bool s_profiles_initialized = false;
static portMUX_TYPE s_prof_mux = portMUX_INITIALIZER_UNLOCKED;

void ProfileRepository::init() {
  if (s_profiles_initialized)
    return;

  VendorProfileDescriptor loaded_profiles[MAX_PROFILES];
  memcpy(loaded_profiles, s_default_profiles, sizeof(s_default_profiles));

  Preferences prefs;
  if (prefs.begin("wp_profiles", true)) {
    for (size_t i = 0; i < MAX_PROFILES; ++i) {
      char pkey[16];
      snprintf(pkey, sizeof(pkey), "p_%u", static_cast<unsigned>(i));
      if (!prefs.isKey(pkey))
        continue;
      NvsEnvelope<VendorProfileDescriptor> env;
      size_t len = prefs.getBytesLength(pkey);
      if (len == sizeof(env)) {
        if (prefs.getBytes(pkey, &env, sizeof(env)) == sizeof(env) && env.verify()) {
          loaded_profiles[i] = env.payload;
        }
      }
    }
    prefs.end();
  }

  {
    CriticalSectionLocker lock(&s_prof_mux);
    memcpy(s_active_profiles, loaded_profiles, sizeof(loaded_profiles));
    s_profiles_initialized = true;
  }

  g_auto_probing_engine.initFromNvs();
}

size_t ProfileRepository::getProfileCount() {
  return MAX_PROFILES;
}

bool ProfileRepository::getProfile(size_t index, VendorProfileDescriptor &out) {
  if (index >= MAX_PROFILES)
    return false;
  init();
  CriticalSectionLocker lock(&s_prof_mux);
  out = s_active_profiles[index];
  return true;
}

bool ProfileRepository::getProfileByKey(const char *key, VendorProfileDescriptor &out) {
  if (!key)
    return false;
  init();
  CriticalSectionLocker lock(&s_prof_mux);
  for (size_t i = 0; i < MAX_PROFILES; ++i) {
    if (strcasecmp(key, s_active_profiles[i].key) == 0) {
      out = s_active_profiles[i];
      return true;
    }
  }
  return false;
}

bool ProfileRepository::getActiveProfile(VendorProfileDescriptor &out) {
  uint8_t prof_idx = 0;
  {
    CriticalSectionLocker lock(&g_config_mux);
    prof_idx = g_config.wallpad_profile;
  }
  return getProfile(prof_idx, out);
}

bool ProfileRepository::setActiveProfileIndex(size_t index) {
  if (index >= MAX_PROFILES)
    return false;
  uint8_t old_idx = 0;
  {
    CriticalSectionLocker lock(&g_config_mux);
    old_idx = g_config.wallpad_profile;
    g_config.wallpad_profile = static_cast<uint8_t>(index);
    g_config_dirty.store(true, std::memory_order_release);
  }
  Config_Save();

  if (old_idx != static_cast<uint8_t>(index)) {
    char old_ns[16], new_ns[16];
    Config::Doorphone::FramingTracker::getNvsNamespace(old_idx, old_ns, sizeof(old_ns));
    Config::Doorphone::FramingTracker::getNvsNamespace(static_cast<uint8_t>(index), new_ns, sizeof(new_ns));

    g_doorphone_tracker.saveToNvs(old_ns, "DOORPHONE");
    g_doorphone_tracker.reset();
    g_doorphone_tracker.restoreFromNvs(new_ns, "DOORPHONE");
  }

  g_control_registry.onProfileChanged(old_idx, static_cast<uint8_t>(index));
  return true;
}

bool ProfileRepository::setActiveProfileByKey(const char *key) {
  if (!key)
    return false;
  init();
  for (size_t i = 0; i < MAX_PROFILES; ++i) {
    if (strcasecmp(key, s_active_profiles[i].key) == 0) {
      return setActiveProfileIndex(i);
    }
  }
  return false;
}

bool ProfileRepository::saveCustomProfile(size_t index, const VendorProfileDescriptor &profile) {
  if (index >= MAX_PROFILES)
    return false;
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    s_active_profiles[index] = profile;
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    char pkey[16];
    snprintf(pkey, sizeof(pkey), "p_%u", static_cast<unsigned>(index));
    NvsEnvelope<VendorProfileDescriptor> env;
    env.payload = profile;
    env.seal();
    prefs.putBytes(pkey, &env, sizeof(env));
    prefs.end();
  }
  return true;
}

namespace {
const char *getChecksumShortName(ChecksumAlgo algo) noexcept {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:
  case ChecksumAlgo::XOR_NO_STX:      return "XOR";
  case ChecksumAlgo::SUM_ALL:
  case ChecksumAlgo::SUM_NO_STX:      return "SUM";
  case ChecksumAlgo::TWOS_COMPLEMENT: return "2'sComp";
  case ChecksumAlgo::ONES_COMPLEMENT: return "1'sComp";
  case ChecksumAlgo::CRC8_MAXIM:      return "CRC8";
  case ChecksumAlgo::NONE:            return "None";
  default:                            return "CS";
  }
}
} // namespace

void ProfileRepository::inferVendorDescription(const AutoProbeDescriptor &ad, char *out_desc, size_t max_len) {
  if (!out_desc || max_len == 0)
    return;

  uint8_t stx = (ad.stx > 0) ? ad.stx : 0xF7;
  uint8_t etx = (ad.etx > 0) ? ad.etx : 0xEE;

  char len_buf[16];
  if (ad.min_len == ad.max_len && ad.min_len >= 3) {
    snprintf(len_buf, sizeof(len_buf), "%uB", ad.min_len);
  } else if (ad.min_len >= 3 && ad.max_len <= 64 && ad.max_len > ad.min_len) {
    snprintf(len_buf, sizeof(len_buf), "%u-%uB", ad.min_len, ad.max_len);
  } else {
    snprintf(len_buf, sizeof(len_buf), "Var");
  }

  snprintf(out_desc, max_len, "Profile (%02X..%02X, %s, %s)",
           stx, etx, len_buf, getChecksumShortName(ad.checksum_algo));
}

bool ProfileRepository::saveCurrentAutoAs(const char *name, size_t &saved_idx) {
  if (!name || strlen(name) == 0)
    return false;
  init();

  AutoProbeDescriptor ad = g_auto_probing_engine.getDescriptor();

  size_t total_tgts = g_polling_targets.totalCount();
  uint8_t obs_min_len = 255, obs_max_len = 0;
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry) && entry.raw_query_len > 0) {
      if (entry.raw_query_len < obs_min_len) obs_min_len = entry.raw_query_len;
      if (entry.raw_query_len > obs_max_len) obs_max_len = entry.raw_query_len;
    }
  }

  AutoProbeDescriptor ui_desc = ad;
  if (obs_min_len <= obs_max_len && obs_min_len >= 3) {
    ui_desc.min_len = obs_min_len;
    ui_desc.max_len = obs_max_len;
  }

  VendorProfileDescriptor new_prof;
  memset(&new_prof, 0, sizeof(new_prof));
  strncpy(new_prof.key, name, sizeof(new_prof.key) - 1);
  
  inferVendorDescription(ui_desc, new_prof.name, sizeof(new_prof.name));

  new_prof.stx = (ad.stx > 0) ? ad.stx : 0xF7;
  new_prof.etx = (ad.etx > 0) ? ad.etx : 0xEE;
  new_prof.min_len = 3;  // 최소 3바이트 이상 모든 프레임 수용
  new_prof.max_len = 64; // 최대 64바이트 이하 모든 프레임 수용 (타임아웃 원천 차단)
  new_prof.cs_algo = ad.checksum_algo;
  new_prof.opcode_offset = (ad.opcode_offset > 0 && ad.opcode_offset < 10) ? ad.opcode_offset : 4;
  new_prof.query_op = (ad.query_opcode > 0) ? ad.query_opcode : 0x01;
  new_prof.ctrl_op = ad.control_seen ? ad.control_opcode : 0x02;
  new_prof.ack_op = (ad.ack_opcode > 0) ? ad.ack_opcode : 0x04;
  new_prof.dev_id_offset = ad.offsets_locked ? ad.dev_id_offset : 3;
  new_prof.sub1_offset = ad.offsets_locked ? ad.sub1_offset : 5;
  new_prof.sub2_offset = ad.offsets_locked ? ad.sub2_offset : 6;
  new_prof.is_swapped_addr = ad.offsets_locked ? (ad.is_swapped_addr ? 1 : 0) : 0;
  new_prof.gw_addr_offset = ad.offsets_locked ? ad.gw_addr_offset : 2;
  new_prof.gw_addr = ad.offsets_locked ? ad.gw_addr : 0x01;
  new_prof.learned_query_len = (ad.offsets_locked && ad.learned_query_len >= 3) ? ad.learned_query_len : 11;
  new_prof.len_offset = ad.offsets_locked ? ad.len_offset : 0xFF;
  new_prof.has_len_field = (ad.offsets_locked && ad.has_len_field) ? 1 : 0;
  new_prof.seq_offset = ad.offsets_locked ? ad.seq_offset : 0xFF;
  new_prof.ack_flag_offset = ad.offsets_locked ? ad.ack_flag_offset : 0xFF;

  size_t target_slot = 1;
  bool found_match = false;
  for (size_t i = 1; i < MAX_PROFILES; ++i) {
    if (strcasecmp(s_active_profiles[i].key, name) == 0) {
      target_slot = i;
      found_match = true;
      break;
    }
  }
  if (!found_match) {
    for (size_t i = 1; i < MAX_PROFILES; ++i) {
      if (strncasecmp(s_active_profiles[i].name, "[Empty", 6) == 0 ||
          strncasecmp(s_active_profiles[i].key, "Custom", 6) == 0) {
        target_slot = i;
        break;
      }
    }
  }

  saveCustomProfile(target_slot, new_prof);
  setActiveProfileIndex(target_slot);
  saved_idx = target_slot;
  return true;
}

bool ProfileRepository::deleteProfile(size_t index) {
  if (index == 0 || index >= MAX_PROFILES)
    return false;
  init();
  VendorProfileDescriptor empty_prof = s_default_profiles[index];
  saveCustomProfile(index, empty_prof);
  if (g_config.wallpad_profile == index) {
    setActiveProfileIndex(0);
  }
  return true;
}

void ProfileRepository::syncAutoProfileToNvs(const AutoProbeDescriptor &auto_desc) {
  init();
  VendorProfileDescriptor desc;
  {
    CriticalSectionLocker lock(&s_prof_mux);
    desc = s_active_profiles[0]; // 0 is 'auto'
    desc.stx = auto_desc.stx;
    desc.etx = auto_desc.etx;
    desc.min_len = auto_desc.min_len;
    desc.max_len = auto_desc.max_len;
    desc.cs_algo = auto_desc.checksum_algo;
    desc.opcode_offset = auto_desc.opcode_offset;
    desc.query_op = auto_desc.query_opcode;
    if (auto_desc.control_seen) {
      desc.ctrl_op = auto_desc.control_opcode;
    }
    desc.ack_op = auto_desc.ack_opcode;
    if (auto_desc.offsets_locked) {
      desc.dev_id_offset = auto_desc.dev_id_offset;
      desc.sub1_offset = auto_desc.sub1_offset;
      desc.sub2_offset = auto_desc.sub2_offset;
      desc.is_swapped_addr = auto_desc.is_swapped_addr ? 1 : 0;
      desc.gw_addr_offset = auto_desc.gw_addr_offset;
      desc.gw_addr = auto_desc.gw_addr;
      desc.learned_query_len = auto_desc.learned_query_len;
      desc.len_offset = auto_desc.len_offset;
      desc.has_len_field = auto_desc.has_len_field ? 1 : 0;
      desc.seq_offset = auto_desc.seq_offset;
      desc.ack_flag_offset = auto_desc.ack_flag_offset;
      desc.ctrl_len_cnt = auto_desc.ctrl_len_cnt;
      memcpy(desc.learned_ctrl_lens, auto_desc.learned_ctrl_lens, sizeof(desc.learned_ctrl_lens));
    }
    s_active_profiles[0] = desc;
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    NvsEnvelope<VendorProfileDescriptor> env;
    env.payload = desc;
    env.seal();
    prefs.putBytes("p_0", &env, sizeof(env));
    prefs.end();
  }
}

void ProfileRepository::resetAllToDefaults() {
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    memcpy(s_active_profiles, s_default_profiles, sizeof(s_default_profiles));
  }
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    prefs.clear();
    prefs.end();
  }
}

