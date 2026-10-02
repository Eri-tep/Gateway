// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================

#include "Protocol/WallpadProtocol.h"
#include "System/SystemDiagnostics.h"

#include "esp_log.h"
#include <Preferences.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

static const char *TAG = "ProfileMatcher";

namespace {

 // CH2, CH3 (월패드 유래)


template <class F> size_t countIf(const PollingTargetEntry *e, size_t n, F f) {
  size_t c = 0;
  for (size_t i = 0; i < n; ++i)
    c += f(e[i]) ? 1 : 0;
  return c;
}

// ★ 활성 프로필(Auto/수동)을 하나의 형태로 정규화 → 이후 모든 분기 제거
struct EffProfile {
  bool is_auto = false;
  uint8_t stx = 0, etx = 0, min_len = 0, max_len = 0;
  ChecksumAlgo algo = ChecksumAlgo::NONE;
  uint8_t op_off = 0, q_op = 0, c_op = 0, a_op = 0;
  bool ctrl_strict =
      true; // true: ctrl==c_op 로만 판정, false: query/ack 가 아니면 제어
  uint8_t dev_off = 0, sub1_off = 0, sub2_off = 0;
  bool swapped = false;
  uint8_t gw_off = 0, gw_addr = 1;
  bool has_len = false;
  uint8_t len_off = 0xFF;
  uint8_t qlen = 11;
};

EffProfile effectiveProfile() {
  VendorProfileDescriptor d;
  ProfileRepository::getActiveProfile(d);

  EffProfile e;
  e.stx = d.stx;
  e.etx = d.etx;
  e.min_len = d.min_len;
  e.max_len = d.max_len;
  e.algo = d.cs_algo;
  e.op_off = d.opcode_offset;
  e.q_op = d.query_op;
  e.c_op = d.ctrl_op;
  e.a_op = d.ack_op;
  e.dev_off = d.dev_id_offset;
  e.sub1_off = d.sub1_offset;
  e.sub2_off = d.sub2_offset;
  e.swapped = (d.is_swapped_addr != 0);
  e.gw_off = d.gw_addr_offset;
  e.gw_addr = d.gw_addr ? d.gw_addr : 0x01;
  e.has_len = (d.has_len_field != 0);
  e.len_off = d.len_offset;
  e.qlen = (d.learned_query_len >= 3 && d.learned_query_len <= 64)
               ? d.learned_query_len
               : 11;

  if (strcasecmp(d.key, "auto") == 0) {
    const AutoProbeDescriptor ad = g_auto_probing_engine.getDescriptor();
    e.is_auto = true;
    e.stx = ad.stx;
    e.etx = ad.etx;
    e.min_len = ad.min_len;
    e.max_len = ad.max_len;
    e.algo = ad.checksum_algo;
    e.op_off = ad.opcode_offset;
    e.q_op = ad.query_opcode;
    e.c_op = ad.control_opcode;
    e.a_op = ad.ack_opcode;
    e.ctrl_strict = ad.control_seen && ad.control_opcode != 0;
    e.has_len = ad.has_len_field;
    e.len_off = ad.len_offset;
    e.gw_off = ad.gw_addr_offset;
    e.gw_addr = ad.gw_addr;
    e.qlen = (ad.offsets_locked && ad.learned_query_len >= 5)
                 ? ad.learned_query_len
                 : 11;
    if (ad.offsets_locked) {
      e.dev_off = ad.dev_id_offset;
      e.sub1_off = ad.sub1_offset;
      e.sub2_off = ad.sub2_offset;
      e.swapped = ad.is_swapped_addr;
    }
  }
  return e;
}

inline int opOf(span<const uint8_t> f, const EffProfile &e) {
  return f.size() > e.op_off ? f[e.op_off] : -1;
}

} // namespace

// ============================================================================
// HYUNDAI WALLPAD PROFILE (현대통신 실측 데이터 기반 정규화 - rodata 플래시
// 배치)
// ============================================================================

static constexpr DeviceSpec s_hyundai_devices[] = {
    // 0x19 일반 조명 (Switch)
    {0x19,    DeviceClass::SWITCH,
     "Light", 11,
     7,       0x01,
     0x02,    0xFF,
     11,      8,
     0xFF,    0xFF,
     0xFF,    false,
     0xFF,    0xFF,
     0xFF,    11,
     7,       8,
     0xFF},
    // 0x18 난방 / 보일러 (Thermostat)
    {0x18,     DeviceClass::THERMOSTAT,
     "Thermo", 11,
     7,        0x01,
     0x04,     0x07,
     18,       8,
     10,       9,
     0xFF,     false,
     0xFF,     0xFF,
     0xFF,     13,
     7,        8,
     9},
    // 0x1F 콘센트 (Outlet)
    {0x1F,     DeviceClass::OUTLET,
     "Outlet", 11,
     7,        0x01,
     0x02,     0xFF,
     18,       8,
     0xFF,     0xFF,
     0xFF,     false,
     0xFF,     9,
     10,       11,
     7,        8,
     0xFF},
    // 0x2B 환기 / 전열교환기 (Vent)
    {0x2B,   DeviceClass::VENT,
     "Vent", 11,
     7,      0x01,
     0x02,   0xFF,
     13,     8,
     0xFF,   0xFF,
     9,      true,
     0xFF,   0xFF,
     0xFF,   13,
     7,      8,
     0xFF},
    // 0x1B 가스 차단기 (Gas)
    {0x1B,  DeviceClass::GAS,
     "Gas", 11,
     7,     0x00,
     0x02,  0xFF,
     13,    0xFF,
     0xFF,  0xFF,
     0xFF,  false,
     8,     0xFF,
     0xFF,  13,
     7,     8,
     0xFF},
    // 0x34 엘리베이터 (Momentary)
    {0x34,       DeviceClass::MOMENTARY,
     "Elevator", 11,
     7,          0x06,
     0x00,       0xFF,
     13,         8,
     0xFF,       0xFF,
     0xFF,       false,
     0xFF,       0xFF,
     0xFF,       11,
     7,          8,
     0xFF},
    // 0x1C 시스템 에어컨 / FCU (Aircon)
    {0x1C,     DeviceClass::AIRCON,
     "Aircon", 11,
     7,        0x01,
     0x02,     0xFF,
     15,       8,
     12,       11,
     10,       true,
     9,        0xFF,
     0xFF,     11,
     7,        8,
     0xFF}};

const WallpadProfile kHyundaiProfile = {
    WallpadVendorId::HYUNDAI,
    "Hyundai HT",
    0xF7,
    0xEE,
    ChecksumAlgo::XOR_NO_STX,
    4, // opcode_offset
    2, // dev_id_offset
    6, // sub1_offset
    6, // sub2_offset
    s_hyundai_devices,
    sizeof(s_hyundai_devices) / sizeof(s_hyundai_devices[0]),
    {3860, 0x7F, 0xEE, 5, "Hyundai HT Standard", 0xB5, 0x5A, 0xB9, 0x5F, 0xB4,
     0x61, 0xB8, 0x60}};

const WallpadProfile *const kWallpadProfiles[] = {&kHyundaiProfile};

const size_t kWallpadProfileCount =
    sizeof(kWallpadProfiles) / sizeof(kWallpadProfiles[0]);

// ============================================================================
// ProfileMatcher
// ============================================================================

namespace ProfileMatcher {

static const WallpadProfile *s_active_profile = &kHyundaiProfile;

const WallpadProfile *getActiveProfile() { return s_active_profile; }

const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad) {
  if (!ad.offsets_locked)
    return s_active_profile;
  for (size_t i = 0; i < kWallpadProfileCount; ++i) {
    const WallpadProfile *p = kWallpadProfiles[i];
    if (p && ad.stx == p->stx && ad.etx == p->etx &&
        ad.checksum_algo == p->checksum_algo &&
        ad.opcode_offset == p->opcode_offset &&
        ad.dev_id_offset == p->dev_id_offset) {
      return s_active_profile = p;
    }
  }
  return s_active_profile;
}

const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) {
  if (stx == 0 || etx == 0)
    return nullptr;
  auto ok = [&](const WallpadProfile *p) {
    return p && p->doorphone.stx == stx && p->doorphone.etx == etx &&
           (p->doorphone.len == 0 || p->doorphone.len == len);
  };
  if (ok(s_active_profile))
    return &s_active_profile->doorphone; // 활성 프로파일 우선
  for (size_t i = 0; i < kWallpadProfileCount; ++i) {
    if (ok(kWallpadProfiles[i]))
      return &kWallpadProfiles[i]->doorphone;
  }
  return nullptr;
}

} // namespace ProfileMatcher

// ============================================================================
// UniversalProtocolEngine
// ============================================================================

static UniversalProtocolEngine s_universal_engine;

size_t UniversalProtocolEngine::getVendorName(char *out, size_t max_len) const {
  if (!out || max_len == 0)
    return 0;
  out[0] = '\0';
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  if (strcasecmp(desc.key, "auto") != 0)
    return snprintf(out, max_len, "%s", desc.name);

  auto ad = g_auto_probing_engine.getDescriptor();
  if (!ad.is_locked)
    return snprintf(out, max_len, "%s", "Auto (Learning...)");
  return snprintf(out, max_len, "Auto [STX 0x%02X ETX 0x%02X / %s]", ad.stx,
                  ad.etx, AutoProbingEngine::getAlgoName(ad.checksum_algo));
}

size_t UniversalProtocolEngine::getActiveProfileKey(char *out,
                                                    size_t max_len) const {
  if (!out || max_len == 0)
    return 0;
  out[0] = '\0';
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  return snprintf(out, max_len, "%s", desc.key);
}

static inline bool checkFramingPure(span<const uint8_t> f, uint8_t stx,
                                    uint8_t etx, uint8_t min_len,
                                    uint8_t max_len, ChecksumAlgo algo) {
  if (f.size() < min_len || f.size() > max_len || f.size() < 3)
    return false;
  if (f[0] != stx || f[f.size() - 1] != etx)
    return false;
  if (algo == ChecksumAlgo::NONE)
    return true;
  return g_auto_probing_engine.calculateChecksum(algo, f.data(), f.size()) ==
         f[f.size() - 2];
}

std::expected<span<const uint8_t>, UniversalProtocolEngine::FrameValidationError>
UniversalProtocolEngine::validateFrame(span<const uint8_t> frame) const noexcept {
  if (frame.size() < 3 || frame.size() > 64)
    return std::unexpected(FrameValidationError::InvalidLength);

  EffProfile e = effectiveProfile();
  if (e.is_auto) {
    g_auto_probing_engine.feedFrame(frame); // 학습 후 갱신된 값으로 재계산
    e = effectiveProfile();
  }

  if (frame.size() < e.min_len || frame.size() > e.max_len)
    return std::unexpected(FrameValidationError::InvalidLength);
  if (frame[0] != e.stx || frame[frame.size() - 1] != e.etx)
    return std::unexpected(FrameValidationError::HeaderMismatch);
  if (e.algo != ChecksumAlgo::NONE &&
      g_auto_probing_engine.calculateChecksum(e.algo, frame) != frame[frame.size() - 2])
    return std::unexpected(FrameValidationError::ChecksumMismatch);

  return frame;
}

bool UniversalProtocolEngine::validatePacket(span<const uint8_t> frame) const {
  return validateFrame(frame).has_value();
}

bool UniversalProtocolEngine::isQueryPacket(span<const uint8_t> frame) const {
  const EffProfile e = effectiveProfile();
  return opOf(frame, e) == e.q_op;
}

bool UniversalProtocolEngine::isControlPacket(span<const uint8_t> frame) const {
  const EffProfile e = effectiveProfile();
  const int op = opOf(frame, e);
  if (op < 0)
    return false;
  return e.ctrl_strict ? (op == e.c_op) : (op != e.q_op && op != e.a_op);
}

bool UniversalProtocolEngine::isAckPacket(span<const uint8_t> frame) const {
  const EffProfile e = effectiveProfile();
  const int op = opOf(frame, e);
  return op == e.a_op || op == e.q_op;
}

bool UniversalProtocolEngine::extractDeviceKey(span<const uint8_t> frame,
                                               uint8_t &dev_id, uint8_t &sub1,
                                               uint8_t &sub2) const {
  const EffProfile e = effectiveProfile();
  if (frame.size() <= e.dev_off)
    return false;

  size_t id_off = e.dev_off;
  const int op = opOf(frame, e);
  if (e.swapped &&
      (op == e.a_op || op == e.q_op)) { // swap 주소체계: ACK 의 DevType 위치
    if (frame.size() <= e.gw_off)
      return false;
    id_off = e.gw_off;
  }
  dev_id = frame[id_off];
  sub1 = (e.sub1_off < frame.size()) ? frame[e.sub1_off] : 0;
  sub2 = (e.sub2_off < frame.size()) ? frame[e.sub2_off] : 0;
  return true;
}

bool UniversalProtocolEngine::buildQueryPacket(uint8_t dev_id, uint8_t sub1,
                                               uint8_t sub2,
                                               StaticPacket &out) const {
  const EffProfile e = effectiveProfile();
  const size_t n = std::min<size_t>(e.qlen, out.data.size());

  out.channel_id = 1;
  out.length = static_cast<decltype(out.length)>(n);
  out.data.fill(0);
  out.data[0] = e.stx;
  if (e.has_len && e.len_off < n)
    out.data[e.len_off] = static_cast<uint8_t>(n);
  if (e.gw_off > 0 && e.gw_off < n)
    out.data[e.gw_off] = e.gw_addr; // STX(0) 덮어쓰기 방지
  if (e.dev_off < n)
    out.data[e.dev_off] = dev_id;
  if (e.op_off < n)
    out.data[e.op_off] = e.q_op;
  if (e.sub1_off > 0 && e.sub1_off < n)
    out.data[e.sub1_off] = sub1;
  if (e.sub2_off > 0 && e.sub2_off < n)
    out.data[e.sub2_off] = sub2;
  if (n >= 3) {
    out.data[n - 2] =
        g_auto_probing_engine.calculateChecksum(e.algo, out.data.data(), n);
    out.data[n - 1] = e.etx;
  }
  return true;
}

uint8_t UniversalProtocolEngine::calculateChecksum(span<const uint8_t> data) const noexcept {
  return g_auto_probing_engine.calculateChecksum(effectiveProfile().algo, data);
}

uint8_t UniversalProtocolEngine::calculateChecksum(const uint8_t *data,
                                                   size_t len) const {
  if (!data)
    return 0;
  return calculateChecksum(span<const uint8_t>(data, len));
}
uint8_t UniversalProtocolEngine::getStx() const {
  return effectiveProfile().stx;
}
uint8_t UniversalProtocolEngine::getEtx() const {
  return effectiveProfile().etx;
}
uint8_t UniversalProtocolEngine::getMinPacketLen() const {
  return effectiveProfile().min_len;
}
uint8_t UniversalProtocolEngine::getMaxPacketLen() const {
  return effectiveProfile().max_len;
}

int UniversalProtocolEngine::extractPacketLength(const uint8_t *stream,
                                                 size_t stream_len,
                                                 size_t stx_idx) const {
  if (stx_idx >= stream_len)
    return -1;
  const EffProfile e = effectiveProfile();
  if (stream[stx_idx] != e.stx)
    return -1;

  const uint8_t safe_min = std::max<uint8_t>(e.min_len, 3);
  const uint8_t safe_max =
      (e.max_len >= safe_min && e.max_len <= 64) ? e.max_len : 64;

  for (size_t l = safe_min; l <= safe_max; ++l) {
    if (stx_idx + l > stream_len)
      return 0; // 아직 덜 들어옴
    if (stream[stx_idx + l - 1] == e.etx &&
        checkFramingPure(span<const uint8_t>(&stream[stx_idx], l), e.stx, e.etx,
                         safe_min, safe_max, e.algo)) {
      return static_cast<int>(l);
    }
  }
  return (stx_idx + safe_max <= stream_len) ? -1 : 0;
}

void WallpadParserFactory::init() { ProfileRepository::init(); }
UniversalProtocolEngine *WallpadParserFactory::getActiveParser() {
  return &s_universal_engine;
}
bool WallpadParserFactory::setProfile(uint8_t index) {
  return ProfileRepository::setActiveProfileIndex(index);
}


// ============================================================================

// 기본 프로파일 공통 꼬리값 (STX/ETX/길이/체크섬/오프셋 기본값)
#define PROFILE_DEFAULT_TAIL                                                   \
  0xF7, 0xEE, 3, 64, ChecksumAlgo::XOR_ALL, 4, 0x01, 0x00, 0x04, 3, 5, 6, 0,   \
      2, 0x01, 11, 0xFF, 0, 0xFF, 0xFF, {0}, 0

static constexpr VendorProfileDescriptor
    s_default_profiles[ProfileRepository::MAX_PROFILES] = {
        {"Auto", "Universal Auto-Probing", PROFILE_DEFAULT_TAIL},
        {"Custom1", "[Empty Custom Slot]", PROFILE_DEFAULT_TAIL},
        {"Custom2", "[Empty Custom Slot]", PROFILE_DEFAULT_TAIL},
        {"Custom3", "[Empty Custom Slot]", PROFILE_DEFAULT_TAIL}};
#undef PROFILE_DEFAULT_TAIL

static VendorProfileDescriptor
    s_active_profiles[ProfileRepository::MAX_PROFILES];
static bool s_profiles_initialized = false;
static portMUX_TYPE s_prof_mux = portMUX_INITIALIZER_UNLOCKED;

static void profileKey(size_t i, char (&k)[16]) {
  snprintf(k, sizeof(k), "p_%u", static_cast<unsigned>(i));
}

void ProfileRepository::init() {
  if (s_profiles_initialized)
    return;

  VendorProfileDescriptor loaded[MAX_PROFILES];
  memcpy(loaded, s_default_profiles, sizeof(s_default_profiles));

  Preferences prefs;
  if (prefs.begin("wp_profiles", true)) {
    for (size_t i = 0; i < MAX_PROFILES; ++i) {
      char k[16];
      profileKey(i, k);
      nvsGetEnv(prefs, k, loaded[i]); // 실패 시 기본값 유지
    }
    prefs.end();
  }
  {
    CriticalSectionLocker lock(&s_prof_mux);
    memcpy(s_active_profiles, loaded, sizeof(loaded));
    s_profiles_initialized = true;
  }
  g_auto_probing_engine.initFromNvs();
}

size_t ProfileRepository::getProfileCount() { return MAX_PROFILES; }

bool ProfileRepository::getProfile(size_t index, VendorProfileDescriptor &out) {
  if (index >= MAX_PROFILES)
    return false;
  init();
  CriticalSectionLocker lock(&s_prof_mux);
  out = s_active_profiles[index];
  return true;
}

// 키로 슬롯 검색 (락 내부에서만 접근)
static int findProfileSlotByKey(const char *key) {
  CriticalSectionLocker lock(&s_prof_mux);
  for (size_t i = 0; i < ProfileRepository::MAX_PROFILES; ++i)
    if (strcasecmp(key, s_active_profiles[i].key) == 0)
      return static_cast<int>(i);
  return -1;
}

bool ProfileRepository::getProfileByKey(const char *key,
                                        VendorProfileDescriptor &out) {
  if (!key)
    return false;
  init();
  int i = findProfileSlotByKey(key);
  return i >= 0 && getProfile(static_cast<size_t>(i), out);
}

bool ProfileRepository::getActiveProfile(VendorProfileDescriptor &out) {
  uint8_t idx;
  {
    CriticalSectionLocker lock(&g_config_mux);
    idx = g_config.wallpad_profile;
  }
  return getProfile(idx, out);
}

static ProfileRepository::ProfileChangeCallbackFn s_profile_change_cbs[4]{};
static size_t s_profile_change_cb_count = 0;

void ProfileRepository::addProfileChangeListener(ProfileChangeCallbackFn cb) {
  if (!cb)
    return;
  for (size_t i = 0; i < s_profile_change_cb_count; ++i) {
    if (s_profile_change_cbs[i] == cb)
      return;
  }
  if (s_profile_change_cb_count < 4) {
    s_profile_change_cbs[s_profile_change_cb_count++] = cb;
  }
}

void ProfileRepository::setProfileChangeListener(ProfileChangeCallbackFn cb) {
  addProfileChangeListener(cb);
}

bool ProfileRepository::setActiveProfileIndex(size_t index) {
  if (index >= MAX_PROFILES)
    return false;
  uint8_t old_idx;
  {
    CriticalSectionLocker lock(&g_config_mux);
    old_idx = g_config.wallpad_profile;
    g_config.wallpad_profile = static_cast<uint8_t>(index);
    g_config_dirty.store(true, std::memory_order_release);
  }
  Config_Save();

  const uint8_t new_idx = static_cast<uint8_t>(index);
  if (old_idx != new_idx) {
    for (size_t i = 0; i < s_profile_change_cb_count; ++i) {
      if (s_profile_change_cbs[i]) {
        s_profile_change_cbs[i](old_idx, new_idx);
      }
    }
  }
  return true;
}

bool ProfileRepository::setActiveProfileByKey(const char *key) {
  if (!key)
    return false;
  init();
  int i = findProfileSlotByKey(key);
  return i >= 0 && setActiveProfileIndex(static_cast<size_t>(i));
}

bool ProfileRepository::saveCustomProfile(
    size_t index, const VendorProfileDescriptor &profile) {
  if (index >= MAX_PROFILES)
    return false;
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    s_active_profiles[index] = profile;
  }
  char k[16];
  profileKey(index, k);
  nvsPutEnvNs("wp_profiles", k, profile);
  return true;
}

namespace {
const char *getChecksumShortName(ChecksumAlgo algo) noexcept {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:
  case ChecksumAlgo::XOR_NO_STX:
    return "XOR";
  case ChecksumAlgo::SUM_ALL:
  case ChecksumAlgo::SUM_NO_STX:
    return "SUM";
  case ChecksumAlgo::TWOS_COMPLEMENT:
    return "2'sComp";
  case ChecksumAlgo::ONES_COMPLEMENT:
    return "1'sComp";
  case ChecksumAlgo::CRC8_MAXIM:
    return "CRC8";
  case ChecksumAlgo::NONE:
    return "None";
  default:
    return "CS";
  }
}

// Auto 디스크립터의 '오프셋 학습 결과'를 Vendor 프로필로 복사 (sync/save 공통)
void copyLockedFields(VendorProfileDescriptor &d,
                      const AutoProbeDescriptor &a) {
  d.dev_id_offset = a.dev_id_offset;
  d.sub1_offset = a.sub1_offset;
  d.sub2_offset = a.sub2_offset;
  d.is_swapped_addr = a.is_swapped_addr ? 1 : 0;
  d.gw_addr_offset = a.gw_addr_offset;
  d.gw_addr = a.gw_addr;
  d.learned_query_len = a.learned_query_len;
  d.len_offset = a.len_offset;
  d.has_len_field = a.has_len_field ? 1 : 0;
  d.seq_offset = a.seq_offset;
  d.ack_flag_offset = a.ack_flag_offset;
}
} // namespace

void ProfileRepository::inferVendorDescription(const AutoProbeDescriptor &ad,
                                               char *out_desc, size_t max_len) {
  if (!out_desc || max_len == 0)
    return;
  const uint8_t stx = ad.stx ? ad.stx : 0xF7;
  const uint8_t etx = ad.etx ? ad.etx : 0xEE;

  char len_buf[16];
  if (ad.min_len == ad.max_len && ad.min_len >= 3)
    snprintf(len_buf, sizeof(len_buf), "%uB", ad.min_len);
  else if (ad.min_len >= 3 && ad.max_len <= 64 && ad.max_len > ad.min_len)
    snprintf(len_buf, sizeof(len_buf), "%u-%uB", ad.min_len, ad.max_len);
  else
    snprintf(len_buf, sizeof(len_buf), "Var");

  snprintf(out_desc, max_len, "Profile (%02X..%02X, %s, %s)", stx, etx, len_buf,
           getChecksumShortName(ad.checksum_algo));
}

bool ProfileRepository::saveCurrentAutoAs(const char *name, size_t &saved_idx) {
  if (!name || !*name)
    return false;
  init();

  const AutoProbeDescriptor ad = g_auto_probing_engine.getDescriptor();

  // 관측된 월패드 쿼리 길이 범위 (UI 설명용)
  uint8_t obs_min = 255, obs_max = 0;
  const size_t total = g_polling_targets.totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry e;
    if (g_polling_targets.getEntry(i, e) && e.raw_query_len > 0 &&
        (e.source_channels & kWallpadChMask)) {
      obs_min = std::min(obs_min, e.raw_query_len);
      obs_max = std::max(obs_max, e.raw_query_len);
    }
  }
  AutoProbeDescriptor ui = ad;
  if (obs_min <= obs_max && obs_min >= 3) {
    ui.min_len = obs_min;
    ui.max_len = obs_max;
  }

  VendorProfileDescriptor p;
  memset(&p, 0, sizeof(p));
  setStr(p.key, name);
  inferVendorDescription(ui, p.name, sizeof(p.name));

  p.stx = ad.stx ? ad.stx : 0xF7;
  p.etx = ad.etx ? ad.etx : 0xEE;
  p.min_len = 3; // 모든 프레임 수용 (타임아웃 방지)
  p.max_len = 64;
  p.cs_algo = ad.checksum_algo;
  p.opcode_offset =
      (ad.opcode_offset > 0 && ad.opcode_offset < 10) ? ad.opcode_offset : 4;
  p.query_op = ad.query_opcode ? ad.query_opcode : 0x01;
  p.ctrl_op = ad.control_seen ? ad.control_opcode : 0x02;
  p.ack_op = ad.ack_opcode ? ad.ack_opcode : 0x04;

  if (ad.offsets_locked) {
    copyLockedFields(p, ad);
    if (p.learned_query_len < 3)
      p.learned_query_len = 11;
  } else { // 미학습: 현대통신 기본 오프셋
    p.dev_id_offset = 3;
    p.sub1_offset = 5;
    p.sub2_offset = 6;
    p.is_swapped_addr = 0;
    p.gw_addr_offset = 2;
    p.gw_addr = 0x01;
    p.learned_query_len = 11;
    p.len_offset = 0xFF;
    p.has_len_field = 0;
    p.seq_offset = 0xFF;
    p.ack_flag_offset = 0xFF;
  }

  // 같은 이름 슬롯 → 없으면 비어있는 Custom 슬롯 → 없으면 1번
  size_t slot = 1;
  {
    CriticalSectionLocker lock(&s_prof_mux);
    bool found = false;
    for (size_t i = 1; i < MAX_PROFILES && !found; ++i) {
      if (strcasecmp(s_active_profiles[i].key, name) == 0) {
        slot = i;
        found = true;
      }
    }
    for (size_t i = 1; i < MAX_PROFILES && !found; ++i) {
      if (strncasecmp(s_active_profiles[i].name, "[Empty", 6) == 0 ||
          strncasecmp(s_active_profiles[i].key, "Custom", 6) == 0) {
        slot = i;
        break;
      }
    }
  }

  saveCustomProfile(slot, p);
  setActiveProfileIndex(slot);
  saved_idx = slot;
  return true;
}

bool ProfileRepository::deleteProfile(size_t index) {
  if (index == 0 || index >= MAX_PROFILES)
    return false;
  init();
  saveCustomProfile(index, s_default_profiles[index]);
  uint8_t cur;
  {
    CriticalSectionLocker lock(&g_config_mux);
    cur = g_config.wallpad_profile;
  }
  if (cur == index)
    setActiveProfileIndex(0);
  return true;
}

void ProfileRepository::syncAutoProfileToNvs(const AutoProbeDescriptor &a) {
  init();
  VendorProfileDescriptor d;
  {
    CriticalSectionLocker lock(&s_prof_mux);
    d = s_active_profiles[0]; // 0 = 'auto'
    d.stx = a.stx;
    d.etx = a.etx;
    d.min_len = a.min_len;
    d.max_len = a.max_len;
    d.cs_algo = a.checksum_algo;
    d.opcode_offset = a.opcode_offset;
    d.query_op = a.query_opcode;
    d.ack_op = a.ack_opcode;
    if (a.control_seen)
      d.ctrl_op = a.control_opcode;
    if (a.offsets_locked) {
      copyLockedFields(d, a);
      d.ctrl_len_cnt = a.ctrl_len_cnt;
      memcpy(d.learned_ctrl_lens, a.learned_ctrl_lens,
             sizeof(d.learned_ctrl_lens));
    }
    s_active_profiles[0] = d;
  }
  nvsPutEnvNs("wp_profiles", "p_0", d);
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

// ============================================================================
// AutoProbingEngine
// ============================================================================




