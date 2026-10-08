// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// Implementation
// ============================================================================

#include "L3_Protocol/Private/Wallpad_Engine.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Private/Routing_Engine.h"
#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Platform.h"

#include <span>
#include <Arduino.h>
#include <Preferences.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>

// ============================================================================
// PART 1: VENDOR PROFILES & UNIVERSAL PROTOCOL PARSER ENGINE
// ============================================================================
// ============================================================================
// WallpadProtocol: Level 3 Wallpad Profiles, Protocol Engine & Probing Cache
// ============================================================================



static std::atomic<bool> s_eff_profile_dirty{true};

void Wallpad_InvalidateProfileCache() noexcept {
  s_eff_profile_dirty.store(true, std::memory_order_release);
}

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

static EffProfile s_cached_eff_profile;
static portMUX_TYPE s_eff_mux = portMUX_INITIALIZER_UNLOCKED;

EffProfile computeEffectiveProfile() {
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
    const AutoProbeDescriptor ad = AutoProbe_GetEngine().getDescriptor();
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

inline const EffProfile &effectiveProfile() noexcept {
  if (__builtin_expect(s_eff_profile_dirty.load(std::memory_order_relaxed), 0)) {
    CriticalSectionLocker lock(&s_eff_mux);
    if (s_eff_profile_dirty.load(std::memory_order_relaxed)) {
      s_cached_eff_profile = computeEffectiveProfile();
      s_eff_profile_dirty.store(false, std::memory_order_release);
    }
  }
  return s_cached_eff_profile;
}

inline int opOf(std::span<const uint8_t> f, const EffProfile &e) {
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

  auto ad = AutoProbe_GetEngine().getDescriptor();
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

static inline bool checkFramingPure(std::span<const uint8_t> f, uint8_t stx,
                                    uint8_t etx, uint8_t min_len,
                                    uint8_t max_len, ChecksumAlgo algo) {
  if (f.size() < min_len || f.size() > max_len || f.size() < 3)
    return false;
  if (f[0] != stx || f[f.size() - 1] != etx)
    return false;
  if (algo == ChecksumAlgo::NONE)
    return true;
  return AutoProbe_GetEngine().calculateChecksum(algo, f.data(), f.size()) ==
         f[f.size() - 2];
}

std::expected<std::span<const uint8_t>, UniversalProtocolEngine::FrameValidationError>
UniversalProtocolEngine::validateFrame(std::span<const uint8_t> frame) const noexcept {
  if (frame.size() < 3 || frame.size() > 64)
    return std::unexpected(FrameValidationError::InvalidLength);

  const EffProfile &e = effectiveProfile();
  if (e.is_auto && !AutoProbe_GetEngine().isLocked()) {
    AutoProbe_GetEngine().feedFrame(frame); // 학습 후 갱신된 값으로 재계산
    Wallpad_InvalidateProfileCache();
  }

  if (frame.size() < e.min_len || frame.size() > e.max_len)
    return std::unexpected(FrameValidationError::InvalidLength);
  if (frame[0] != e.stx || frame[frame.size() - 1] != e.etx)
    return std::unexpected(FrameValidationError::HeaderMismatch);
  if (e.algo != ChecksumAlgo::NONE &&
      AutoProbe_GetEngine().calculateChecksum(e.algo, frame) != frame[frame.size() - 2])
    return std::unexpected(FrameValidationError::ChecksumMismatch);

  return frame;
}

bool UniversalProtocolEngine::validatePacket(std::span<const uint8_t> frame) const {
  return validateFrame(frame).has_value();
}

bool UniversalProtocolEngine::isQueryPacket(std::span<const uint8_t> frame) const {
  const EffProfile &e = effectiveProfile();
  return opOf(frame, e) == e.q_op;
}

bool UniversalProtocolEngine::isControlPacket(std::span<const uint8_t> frame) const {
  const EffProfile &e = effectiveProfile();
  const int op = opOf(frame, e);
  if (op < 0)
    return false;
  return e.ctrl_strict ? (op == e.c_op) : (op != e.q_op && op != e.a_op);
}

bool UniversalProtocolEngine::isAckPacket(std::span<const uint8_t> frame) const {
  const EffProfile &e = effectiveProfile();
  const int op = opOf(frame, e);
  return op == e.a_op || op == e.q_op;
}

bool UniversalProtocolEngine::extractDeviceKey(std::span<const uint8_t> frame,
                                               uint8_t &dev_id, uint8_t &sub1,
                                               uint8_t &sub2) const {
  const EffProfile &e = effectiveProfile();
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
  const EffProfile &e = effectiveProfile();
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
        AutoProbe_GetEngine().calculateChecksum(e.algo, out.data.data(), n);
    out.data[n - 1] = e.etx;
  }
  return true;
}

uint8_t UniversalProtocolEngine::calculateChecksum(std::span<const uint8_t> data) const noexcept {
  return AutoProbe_GetEngine().calculateChecksum(effectiveProfile().algo, data);
}

uint8_t UniversalProtocolEngine::calculateChecksum(const uint8_t *data,
                                                   size_t len) const {
  if (!data)
    return 0;
  return calculateChecksum(std::span<const uint8_t>(data, len));
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
  const EffProfile &e = effectiveProfile();
  if (stream[stx_idx] != e.stx)
    return -1;

  const uint8_t safe_min = std::max<uint8_t>(e.min_len, 3);
  const uint8_t safe_max =
      (e.max_len >= safe_min && e.max_len <= 64) ? e.max_len : 64;

  for (size_t l = safe_min; l <= safe_max; ++l) {
    if (stx_idx + l > stream_len)
      return 0; // 아직 덜 들어옴
    if (stream[stx_idx + l - 1] == e.etx &&
        checkFramingPure(std::span<const uint8_t>(&stream[stx_idx], l), e.stx, e.etx,
                         safe_min, safe_max, e.algo)) {
      return static_cast<int>(l);
    }
  }
  return (stx_idx + safe_max <= stream_len) ? -1 : 0;
}

bool UniversalProtocolEngine::isLocked() const noexcept {
  const auto &e = effectiveProfile();
  return !e.is_auto || AutoProbe_GetEngine().isLocked();
}

bool UniversalProtocolEngine::isAutoMode() const noexcept {
  return effectiveProfile().is_auto;
}

UniversalProtocolEngine &Universal_GetEngine() noexcept {
  return s_universal_engine;
}

void WallpadParserFactory::init() { ProfileRepository::init(); }
UniversalProtocolEngine *WallpadParserFactory::getActiveParser() {
  return &Universal_GetEngine();
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
  AutoProbe_GetEngine().initFromNvs();
  Wallpad_InvalidateProfileCache();
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

static_assert(ProfileRepository::MAX_PROFILES - 1 == kWallpadProfileMax,
              "Profile count mismatch between L0 and L3");

bool ProfileRepository::getActiveProfile(VendorProfileDescriptor &out) {
  const uint8_t idx = Config_GetWallpadProfile();
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
  const uint8_t old_idx = Config_GetWallpadProfile();
  const uint8_t new_idx = static_cast<uint8_t>(index);
  Config_SetWallpadProfile(new_idx);
  Config_SaveWallpadProfile();
  Wallpad_InvalidateProfileCache();

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
  Wallpad_InvalidateProfileCache();
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

  const AutoProbeDescriptor ad = AutoProbe_GetEngine().getDescriptor();

  // 관측된 월패드 쿼리 길이 범위 (UI 설명용)
  uint8_t obs_min = 255, obs_max = 0;
  const size_t total = Polling_GetRegistry().totalCount();
  for (size_t i = 0; i < total; ++i) {
    PollingTargetEntry e;
    if (Polling_GetRegistry().getEntry(i, e) && e.raw_query_len > 0 &&
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
  const uint8_t cur = Config_GetWallpadProfile();
  if (cur == index)
    setActiveProfileIndex(0);
  return true;
}

static std::atomic<bool> s_auto_nvs_sync_pending{false};

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
  Wallpad_InvalidateProfileCache();
  // Decoupled: Signal pending NVS commit to background task.
  // Task_Ch1 must NEVER execute flash NVS writes directly to prevent stack overflow
  // and UART/SoftwareSerial interrupt latency degradation.
  s_auto_nvs_sync_pending.store(true, std::memory_order_release);
}

bool ProfileRepository::commitAutoProfileNvsIfPending() noexcept {
  if (!s_auto_nvs_sync_pending.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
  VendorProfileDescriptor d;
  {
    CriticalSectionLocker lock(&s_prof_mux);
    d = s_active_profiles[0];
  }

  // Flash wear leveling guard: skip write if NVS already contains identical data.
  VendorProfileDescriptor existing{};
  if (nvsGetEnvNs("wp_profiles", "p_0", existing)) {
    if (memcmp(&existing, &d, sizeof(VendorProfileDescriptor)) == 0) {
      return false; // Identical, flash write avoided
    }
  }

  return nvsPutEnvNs("wp_profiles", "p_0", d);
}

void ProfileRepository::resetAllToDefaults() {
  init();
  {
    CriticalSectionLocker lock(&s_prof_mux);
    memcpy(s_active_profiles, s_default_profiles, sizeof(s_default_profiles));
  }
  Wallpad_InvalidateProfileCache();
  Preferences prefs;
  if (prefs.begin("wp_profiles", false)) {
    prefs.clear();
    prefs.end();
  }
}

// ============================================================================
// AutoProbingEngine
// ============================================================================





// ============================================================================
// PART 2: WALLPAD PROTOCOL ORCHESTRATION & FSM ENGINE
// ============================================================================
// ============================================================================
// Wallpad_Protocol.cpp — L3 Protocol & Routing Engine
// Wallpad Business Logic Orchestration & Domain Engine Implementation
// Canonical 4+1 Layer: L3 Routing/Protocol layer
// ============================================================================



namespace {

int Wallpad_ScoreCandidate(const PollingTargetRegistry::PollingCandidate &tgt,
                           const DeviceStateEntry *cached_dev) noexcept {
  // CH5 (EW11 TCP) 소속 타겟(FCU 모드버스, 엘리베이터 등)은 CH1 물리 버스 폴링에서 원천 배제
  if ((tgt.source_channels != 0 && !(tgt.source_channels & kWallpadChMask)) ||
      (tgt.source_channels & (1 << 5)) ||
      tgt.dev_id == Config::FCU::DEV_ID) {
    return 999;
  }
  RouteEndpoint ep;
  if (Router_LookupRoute(tgt.dev_id, tgt.sub1, tgt.sub2, ep) &&
      ep.channel_id == 5) {
    return 999;
  }
  if (!cached_dev) {
    return 1;
  }
  if (cached_dev->last_updated_ms == 0) {
    return 1;
  }
  if (cached_dev->is_online) {
    return 2;
  }
  if (TimeUtils::isElapsed(cached_dev->last_stale_poll_ms,
                           Config::Timing::CH1_STALE_POLL_INTERVAL_MS)) {
    return 3;
  }
  return 999;
}

static size_t s_current_dev_idx = 0;
static uint32_t s_stable_start_ms = 0;
static size_t s_last_active_tgts = 0;
static bool s_convergence_done = false;
static std::atomic<uint32_t> s_stale_poll_cnt{0};

} // namespace

bool Wallpad_BuildNextPollPacket(StaticPacket &out_pkt, uint8_t &poll_dev_id,
                                 uint8_t &poll_sub1, uint8_t &poll_sub2) noexcept {
  // Flush deferred NVS write here (Core 1 / Task_Ch1).
  // Network task (Core 0) deliberately skips this to avoid WDT-fatal blocking.
  WarmCache_CheckNvsDebounce();

  if (Wallpad_TakeRelearnRequest()) {
    Wallpad_CheckConvergence(true);
    System_TraceMessage("[AUTO PROBE] Convergence state reset. Re-learning "
                        "bus offsets...\r\n");
  }

  if (!s_convergence_done) {
    Wallpad_CheckConvergence(false);
  }

  Polling_GetRegistry().sweepExpired(Config::Timing::STALE_DEVICE_THRESHOLD_MS);

  PollingTargetRegistry::PollingCandidate candidates[PollingTargetRegistry::MAX_TARGETS];
  size_t active_cnt = Polling_GetRegistry().getActiveCandidates(
      candidates, PollingTargetRegistry::MAX_TARGETS);

  poll_dev_id = 0;
  poll_sub1 = 0;
  poll_sub2 = 0;
  const uint8_t *poll_raw_ptr = nullptr;
  uint8_t poll_raw_len = 0;
  bool target_selected = false;
  uint32_t now = millis();

  if (active_cnt > 0) {
    size_t chosen_idx = active_cnt;
    int chosen_score = 999;

    for (size_t i = 0; i < active_cnt; i++) {
      size_t idx = (s_current_dev_idx + i) % active_cnt;
      const auto &tgt = candidates[idx];
      DeviceStateEntry cached_dev_snap{};
      bool has_cached = Device_FindCopy(tgt.dev_id, tgt.sub1, tgt.sub2, cached_dev_snap);
      int score = Wallpad_ScoreCandidate(tgt, has_cached ? &cached_dev_snap : nullptr);

      if (score <= 3) {
        chosen_idx = idx;
        chosen_score = score;
        break;
      }
    }

    if (chosen_idx < active_cnt) {
      const auto &tgt = candidates[chosen_idx];
      poll_dev_id = tgt.dev_id;
      poll_sub1 = tgt.sub1;
      poll_sub2 = tgt.sub2;
      poll_raw_len = tgt.raw_query_len;
      if (poll_raw_len > 0) {
        Polling_GetRegistry().getQueryData(tgt.entry_idx, poll_raw_ptr, poll_raw_len);
      }
      if (chosen_score == 3) {
        Device_SetLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now);
        s_stale_poll_cnt.fetch_add(1, std::memory_order_relaxed);
      }
      s_current_dev_idx = (chosen_idx + 1) % active_cnt;
      target_selected = true;
    }
  }

  if (!target_selected) {
    size_t dev_cnt = Device_GetCount();
    if (dev_cnt > 0) {
      size_t idx = s_current_dev_idx % dev_cnt;
      DeviceStateEntry dev_snap{};
      bool has_dev = Device_GetAtCopy(idx, dev_snap);
      s_current_dev_idx = (idx + 1) % dev_cnt;
      if (has_dev && dev_snap.dev_id != Config::FCU::DEV_ID &&
          (dev_snap.is_online || dev_snap.last_updated_ms == 0 ||
           TimeUtils::isElapsed(dev_snap.last_stale_poll_ms,
                                Config::Timing::CH1_STALE_POLL_INTERVAL_MS))) {
        RouteEndpoint ep;
        if (!Router_LookupRoute(dev_snap.dev_id, dev_snap.sub1, dev_snap.sub2, ep) ||
            ep.channel_id != 5) {
          poll_dev_id = dev_snap.dev_id;
          poll_sub1 = dev_snap.sub1;
          poll_sub2 = dev_snap.sub2;
          if (!dev_snap.is_online)
            Device_SetLastStalePollMsByIndex(idx, now);
          target_selected = true;
        }
      }
    }
  }

  if (!target_selected) {
    return false;
  }

  out_pkt.channel_id = 1;
  if (poll_raw_len > 0 && poll_raw_ptr) {
    const size_t copy_len =
        std::min(static_cast<size_t>(poll_raw_len), out_pkt.data.size());
    out_pkt.length = static_cast<uint8_t>(copy_len);
    memcpy(out_pkt.data.data(), poll_raw_ptr, copy_len);
  } else {
    Universal_GetEngine().buildQueryPacket(poll_dev_id, poll_sub1, poll_sub2, out_pkt);
  }
  return true;
}

void Wallpad_HandleBusPacket(uint8_t channel_id, const StaticPacket &ack_pkt,
                             const StaticPacket *matching_query) noexcept {
  StaticPacket ack = ack_pkt;
  ack.channel_id = channel_id;

  if (matching_query && matching_query->length > 0) {
    Polling_GetRegistry().updateResponse(matching_query->data.data(), matching_query->length,
                                     ack.data.data(), ack.length);
    AutoProbe_GetEngine().feedOpcodePair(
        std::span<const uint8_t>(matching_query->data.data(), matching_query->length),
        std::span<const uint8_t>(ack.data.data(), ack.length));
  }

  Device_ProcessBusPacket(ack);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  std::span<const uint8_t> ack_span(ack.data.data(), ack.length);
  if (Universal_GetEngine().extractDeviceKey(ack_span, dev_id, sub1, sub2)) {
    if (channel_id == 1 && (!matching_query || matching_query->length == 0)) {
      Polling_GetRegistry().markVerified(dev_id, sub1, sub2);
    }
    Router_RecordRoute(channel_id, -1, dev_id, sub1, sub2);
  }
}

void Wallpad_HandlePollTimeout(uint8_t poll_dev_id, uint8_t poll_sub1,
                               uint8_t poll_sub2) noexcept {
  Device_HandlePollingTimeout(poll_dev_id, poll_sub1, poll_sub2);
}

ControlAction Wallpad_EvaluateControl(StaticPacket &req, StaticPacket &virtual_ack_out,
                                      bool &out_unidir) noexcept {
  out_unidir = false;

  if (UNLIKELY(req.length < 5))
    return ControlAction::DROP;
  auto &engine = Universal_GetEngine();
  std::span<const uint8_t> frame(req.data.data(), req.length);

  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  const bool has_key = engine.extractDeviceKey(frame, dev_id, sub1, sub2);

  if (engine.isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    if (has_key && Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out)) {
      return ControlAction::VIRTUAL_ACK_IMMEDIATE;
    }
    return ControlAction::DROP;
  }

  GroupControlTemplate grp{};
  const bool has_grp =
      (has_key && dev_id != 0) && Control_GetRegistry().findGroup(dev_id, grp);

  bool is_ctl = engine.isControlPacket(frame);
  if (!is_ctl && has_grp && grp.frame_len > 4 &&
      frame.size() >= grp.frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    const uint8_t op_off =
        (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    is_ctl = (frame[op_off] == grp.raw_template[op_off]);
  }
  if (!is_ctl)
    return ControlAction::DROP;

  if (has_grp) {
    // 가스: 원격 '열림' 차단 (닫힘 값만 허용)
    if (grp.coverage.dev_class == DeviceClass::GAS &&
        grp.close_slot.discovered &&
        grp.close_slot.action_offset < req.length &&
        req.data[grp.close_slot.action_offset] != grp.close_slot.off_val) {
      System_TracePacket(req.channel_id, false, TraceType::DRP, req);
      return ControlAction::DROP;
    }

    // 난방: 온도 설정 범위 검증
    const auto &ts = grp.temp_slot;
    if (grp.coverage.dev_class == DeviceClass::THERMOSTAT && ts.discovered &&
        ts.action_offset < req.length && ts.category_offset != 0xFF &&
        ts.category_offset < req.length &&
        req.data[ts.category_offset] == ts.category_val) {
      const uint8_t t = req.data[ts.action_offset];
      if (t < 5 || t > 35) {
        System_TracePacket(req.channel_id, false, TraceType::DRP, req);
        return ControlAction::DROP;
      }
    }
  }

  out_unidir = (has_grp && grp.isUnidirectional()) || dev_id == 0x34;
  return ControlAction::TRANSMIT_LOCAL;
}

uint32_t Wallpad_GetPollIntervalMs() noexcept {
  size_t active_tgts = Polling_GetRegistry().activeCount();
  return (s_convergence_done || active_tgts == 0)
             ? TimingConfig_Get().ch1_poll_interval_ms
             : 20;
}

static std::atomic<bool> s_relearn_requested{false};

void Wallpad_RequestRelearn() noexcept {
  s_relearn_requested.store(true, std::memory_order_release);
}

bool Wallpad_TakeRelearnRequest() noexcept {
  return s_relearn_requested.exchange(false, std::memory_order_acq_rel);
}

bool Wallpad_CheckConvergence(bool reset) noexcept {
  if (reset) {
    s_convergence_done = false;
    s_stable_start_ms = 0;
    s_last_active_tgts = 0;
    if (g_system_event_group) {
      xEventGroupClearBits(g_system_event_group, SYS_EVT_CACHE_READY);
    }
    return false;
  }

  if (s_convergence_done) {
    return true;
  }

  size_t active_tgts = Polling_GetRegistry().activeCount();
  size_t online_devs = Device_GetOnlineCount();

  if (active_tgts != s_last_active_tgts) {
    s_last_active_tgts = active_tgts;
    s_stable_start_ms = millis();
  }

  bool is_all_online = (online_devs >= active_tgts);
  auto &engine = Universal_GetEngine();
  if (engine.isAutoMode() &&
      !AutoProbe_GetEngine().isOffsetsLocked()) {
    is_all_online = (Polling_GetRegistry().verifiedCount() >= active_tgts);
  }

  if (active_tgts > 0 && is_all_online) {
    if (s_stable_start_ms == 0) {
      s_stable_start_ms = millis();
    } else if (TimeUtils::isElapsed(s_stable_start_ms,
                                    Config::Timing::CACHE_CONVERGENCE_STABLE_MS)) {
      s_convergence_done = true;
      if (g_system_event_group) {
        xEventGroupSetBits(g_system_event_group, SYS_EVT_CACHE_READY);
      }
      if (engine.isAutoMode() &&
          !AutoProbe_GetEngine().isOffsetsLocked()) {
        AutoProbe_GetEngine().analyzeCacheMatrix();
      }
      Polling_GetRegistry().resetHits();
      System_TraceMessage(
          "[SYSTEM MSG]  ★ 2nd-Tier Cache Converged (Zero Offline). "
          "Runtime metrics synchronized.\r\n");
      Control_GetRegistry().synthesizeFromConvergedCache();
      System_TraceMessage(
          "[CTL] Control template synthesis triggered.\r\n");
      return true;
    }
  } else {
    s_stable_start_ms = 0;
  }
  return false;
}

uint32_t Wallpad_GetStalePollCount() noexcept {
  return s_stale_poll_cnt.load(std::memory_order_relaxed);
}

void Wallpad_ResetStalePollCount() noexcept {
  s_stale_poll_cnt.store(0, std::memory_order_relaxed);
}

uint8_t Wallpad_GetStx() noexcept {
  return Universal_GetEngine().getStx();
}

bool Wallpad_IsAutoUnlocked() noexcept {
  auto &engine = Universal_GetEngine();
  return engine.isAutoMode() && !engine.isLocked();
}

void Wallpad_FeedAutoFrame(std::span<const uint8_t> frame) noexcept {
  AutoProbe_GetEngine().feedFrame(frame);
}

int Wallpad_ExtractLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) noexcept {
  return Universal_GetEngine().extractPacketLength(stream, stream_len, stx_idx);
}

bool Wallpad_ValidatePacket(std::span<const uint8_t> frame) noexcept {
  return Universal_GetEngine().validatePacket(frame);
}

bool Wallpad_HandleSubBusQuery(uint8_t channel_id, const StaticPacket &req,
                               StaticPacket &virtual_ack_out) noexcept {
  auto &engine = Universal_GetEngine();
  std::span<const uint8_t> frame(req.data.data(), req.length);
  if (!engine.isQueryPacket(frame)) {
    return false;
  }
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  engine.extractDeviceKey(frame, dev_id, sub1, sub2);
  Polling_GetRegistry().registerOrTouch(channel_id, dev_id, sub1, sub2,
                                    req.data.data(), req.length);
  virtual_ack_out.channel_id = channel_id;
  return Device_CopyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
}

void Wallpad_FeedControlFrame(std::span<const uint8_t> frame) noexcept {
  AutoProbe_GetEngine().feedControlFrame(frame);
}

// ── FramingTracker Implementation (L3) ───────────────────────────────────────

void FramingTracker::setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  candidate_stx.store(stx, std::memory_order_relaxed);
  candidate_etx.store(etx, std::memory_order_relaxed);
  candidate_len.store(len, std::memory_order_relaxed);
  consecutive_matches.store(10, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  is_custom_fixed.store(true, std::memory_order_relaxed);
  status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
}

void FramingTracker::reset() noexcept {
  is_custom_fixed.store(false, std::memory_order_relaxed);
  candidate_stx.store(0, std::memory_order_relaxed);
  candidate_etx.store(0, std::memory_order_relaxed);
  candidate_len.store(0, std::memory_order_relaxed);
  consecutive_matches.store(0, std::memory_order_relaxed);
  consecutive_mismatches.store(0, std::memory_order_relaxed);
  status.store(FramingStatus::WAITING, std::memory_order_relaxed);
}

void FramingTracker::clearNvs(const char *nvs_ns, const char *tag) noexcept {
  reset();
  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    prefs.clear();
    prefs.end();
    ::Serial.printf("[%s] Cleared framing NVS storage (%s).\r\n", tag, nvs_ns);
  }
}

void FramingTracker::processFrame(uint8_t stx, uint8_t etx, uint8_t len,
                                  const char *nvs_ns,
                                  const char *tag) noexcept {
  if (is_custom_fixed.load(std::memory_order_relaxed)) {
    return;
  }

  FramingStatus cur = status.load(std::memory_order_relaxed);

  if (stx == 0x7F && etx == 0xEE && (len == 0 || len == 5)) {
    setFixedLock(0x7F, 0xEE, 5);
    saveToNvs(nvs_ns, tag);
    return;
  }

  if (cur == FramingStatus::WAITING) {
    candidate_stx.store(stx, std::memory_order_relaxed);
    candidate_etx.store(etx, std::memory_order_relaxed);
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_matches.store(1, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    return;
  }

  uint8_t cand_s = candidate_stx.load(std::memory_order_relaxed);
  uint8_t cand_e = candidate_etx.load(std::memory_order_relaxed);

  if (stx == cand_s && etx == cand_e) {
    if (len > 0)
      candidate_len.store(len, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    uint8_t m = consecutive_matches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m >= 3) {
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      saveToNvs(nvs_ns, tag);
    } else {
      status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
    }
  } else {
    consecutive_matches.store(0, std::memory_order_relaxed);
    uint8_t m =
        consecutive_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cur == FramingStatus::LOCKED) {
      if (m >= 10) {
        status.store(FramingStatus::WAITING, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
      }
    } else {
      if (m >= 5) {
        candidate_stx.store(stx, std::memory_order_relaxed);
        candidate_etx.store(etx, std::memory_order_relaxed);
        if (len > 0)
          candidate_len.store(len, std::memory_order_relaxed);
        consecutive_matches.store(1, std::memory_order_relaxed);
        consecutive_mismatches.store(0, std::memory_order_relaxed);
        status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
      }
    }
  }
}

void FramingTracker::restoreFromNvs(const char *nvs_ns,
                                    const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  Preferences prefs;
  if (prefs.begin(nvs_ns, true)) {
    uint8_t s = prefs.getUChar("stx", 0);
    uint8_t e = prefs.getUChar("etx", 0);
    uint8_t l = prefs.getUChar("len", 0);
    bool locked = prefs.getBool("locked", false);
    bool fixed = prefs.getBool("fixed", false);
    prefs.end();

    if (locked && s != 0 && e != 0) {
      candidate_stx.store(s, std::memory_order_relaxed);
      candidate_etx.store(e, std::memory_order_relaxed);
      candidate_len.store(l, std::memory_order_relaxed);
      status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
      is_custom_fixed.store(fixed, std::memory_order_relaxed);
      ::Serial.printf("[%s] Restored valid framing from NVS (%s): STX=0x%02X, "
                      "ETX=0x%02X, LEN=%u\r\n",
                      tag, nvs_ns, s, e, l);
    }
  }
}

void FramingTracker::saveToNvs(const char *nvs_ns, const char *tag) noexcept {
  if (!nvs_ns)
    nvs_ns = "dp_frame_p0";

  uint8_t s = candidate_stx.load(std::memory_order_relaxed);
  uint8_t e = candidate_etx.load(std::memory_order_relaxed);
  uint8_t l = candidate_len.load(std::memory_order_relaxed);
  bool is_locked =
      (status.load(std::memory_order_relaxed) == FramingStatus::LOCKED);
  bool fixed = is_custom_fixed.load(std::memory_order_relaxed);

  static uint8_t s_last_s = 0, s_last_e = 0, s_last_l = 0;
  static bool s_last_locked = false, s_last_fixed = false;
  static char s_last_ns[16] = {0};

  if (s == s_last_s && e == s_last_e && l == s_last_l &&
      is_locked == s_last_locked && fixed == s_last_fixed &&
      strncmp(s_last_ns, nvs_ns, sizeof(s_last_ns)) == 0) {
    return; // 동일 설정 중복 쓰기 방지로 Flash I/O 지연 스킵
  }

  Preferences prefs;
  if (prefs.begin(nvs_ns, false)) {
    prefs.putUChar("stx", s);
    prefs.putUChar("etx", e);
    prefs.putUChar("len", l);
    prefs.putBool("locked", is_locked);
    prefs.putBool("fixed", fixed);
    prefs.end();

    s_last_s = s;
    s_last_e = e;
    s_last_l = l;
    s_last_locked = is_locked;
    s_last_fixed = fixed;
    strncpy(s_last_ns, nvs_ns, sizeof(s_last_ns) - 1);
    s_last_ns[sizeof(s_last_ns) - 1] = '\0';

    ::Serial.printf("[%s] Persisted framing to NVS (%s): STX=0x%02X, "
                    "ETX=0x%02X, LEN=%u%s\r\n",
                    tag, nvs_ns, s, e, l, fixed ? " [FIXED]" : "");
  }
}

bool FramingTracker::isConsistent(uint8_t stx, uint8_t etx) const noexcept {
  const FramingStatus cur = status.load(std::memory_order_relaxed);
  if (cur != FramingStatus::LOCKED)
    return true;
  return (stx == candidate_stx.load(std::memory_order_relaxed) &&
          etx == candidate_etx.load(std::memory_order_relaxed));
}

// ── Doorphone Protocol & FSM Sealed State ─────────────────────────────────────

namespace {

struct DoorphoneState {
  std::atomic<bool> front_bell{false};
  std::atomic<bool> lobby_bell{false};
  std::atomic<uint32_t> last_bell_ms{0};
};

static DoorphoneState s_doorphone_state{};
static FramingTracker s_doorphone_tracker{};
static DoorphoneTxHandler s_dp_tx_handler = nullptr;

class DoorphoneController {
public:
  enum class Step : uint8_t { IDLE = 0, CALL_SENT, OPEN_SENT };

private:
  std::atomic<Step> _step{Step::IDLE};
  std::atomic<uint8_t> _c_stx{0x7F};
  std::atomic<uint8_t> _c_etx{0xEE};
  std::atomic<uint8_t> _op_open{0};
  std::atomic<uint8_t> _op_end{0};
  esp_timer_handle_t _timer{nullptr};

public:
  void init() {
    if (!_timer) {
      esp_timer_create_args_t timer_args{};
      timer_args.callback = onTimerCallback;
      timer_args.name = "dp_fsm_timer";
      esp_timer_create(&timer_args, &_timer);
    }
  }

  bool startSequence(uint8_t stx, uint8_t etx, uint8_t op_call, uint8_t op_open,
                     uint8_t op_end) {
    Step expected = Step::IDLE;
    if (!_step.compare_exchange_strong(expected, Step::CALL_SENT)) {
      return false;
    }

    _c_stx.store(stx, std::memory_order_release);
    _c_etx.store(etx, std::memory_order_release);
    _op_open.store(op_open, std::memory_order_release);
    _op_end.store(op_end, std::memory_order_release);

    sendDpPacket(stx, op_call, etx);

    if (_timer) {
      esp_timer_start_once(_timer, 350000); // 350ms 후 문열림 패킷 전송 (골든 타임)
    }
    return true;
  }

  void cancel() {
    if (_timer) {
      esp_timer_stop(_timer);
    }
    _step.store(Step::IDLE, std::memory_order_release);
  }

  [[nodiscard]] bool isBusy() const noexcept {
    return _step.load(std::memory_order_acquire) != Step::IDLE;
  }

  [[nodiscard]] Step getStep() const noexcept {
    return _step.load(std::memory_order_acquire);
  }

  static void sendDpPacket(uint8_t stx, uint8_t op, uint8_t etx) {
    StaticPacket pkt{4, 5};
    pkt.data[0] = stx;
    pkt.data[1] = op;
    pkt.data[2] = 0x00;
    pkt.data[3] = 0x00;
    pkt.data[4] = etx;
    (void)Router_EnqueueDownlink(4, pkt);
  }

  static void onTimerCallback(void *arg);
};

static DoorphoneController s_doorphone_controller;

void DoorphoneController::onTimerCallback(void * /*arg*/) {
  const Step cur = s_doorphone_controller._step.load(std::memory_order_acquire);
  switch (cur) {
  case Step::CALL_SENT:
    sendDpPacket(
        s_doorphone_controller._c_stx.load(std::memory_order_acquire),
        s_doorphone_controller._op_open.load(std::memory_order_acquire),
        s_doorphone_controller._c_etx.load(std::memory_order_acquire));
    s_doorphone_controller._step.store(Step::OPEN_SENT,
                                       std::memory_order_release);
    if (s_doorphone_controller._timer) {
      esp_timer_start_once(s_doorphone_controller._timer,
                           750000); // 750ms 후 종료 패킷
    }
    break;

  case Step::OPEN_SENT:
    sendDpPacket(s_doorphone_controller._c_stx.load(std::memory_order_acquire),
                 s_doorphone_controller._op_end.load(std::memory_order_acquire),
                 s_doorphone_controller._c_etx.load(std::memory_order_acquire));
    s_doorphone_state.front_bell.store(false, std::memory_order_release);
    s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    s_doorphone_controller._step.store(Step::IDLE, std::memory_order_release);
    break;

  default:
    break;
  }
}

} // namespace

void Wallpad_InitDecoupledHooks() noexcept {
  Device_RegisterParserHooks(
    [](std::span<const uint8_t> frame) noexcept -> bool {
      return Universal_GetEngine().isAckPacket(frame);
    },
    [](std::span<const uint8_t> frame, uint8_t &dev_id, uint8_t &sub1, uint8_t &sub2) noexcept -> bool {
      return Universal_GetEngine().extractDeviceKey(frame, dev_id, sub1, sub2);
    }
  );

  Device_RegisterStateDecoder(ControlTemplate_DecodeByDevId);
  Device_RegisterNormSub1Hook(ControlTemplate_NormSub1);

  ControlTemplateRegistry::setDeviceUnitCountProvider([](uint8_t dev_id) -> size_t {
    size_t units = 0;
    for (size_t i = 0; i < Device_GetCount() && units < 2; ++i) {
      DeviceStateEntry snap{};
      if (Device_GetSnapshot(i, snap) && snap.dev_id == dev_id)
        ++units;
    }
    return units;
  });

  AutoProbingEngine::setDeviceHooks(
    []() -> size_t {
      return Device_GetOnlineCount();
    },
    [](uint8_t dev_id, uint8_t sub1, uint8_t sub2,
       uint8_t *out_buf, size_t max_len, size_t *out_len) -> bool {
      if (!out_buf || !out_len || max_len == 0) return false;
      DeviceStateEntry snap{};
      if (Device_FindCopy(dev_id, sub1, sub2, snap) && snap.is_online && snap.last_ack_len >= 4) {
        size_t c_len = std::min(static_cast<size_t>(snap.last_ack_len), max_len);
        memcpy(out_buf, snap.last_ack_data.data(), c_len);
        *out_len = c_len;
        return true;
      }
      return false;
    },
    [](StaticPacket &ack) {
      Device_ProcessBusPacket(ack);
    }
  );
}

bool Wallpad_DoorphoneOpen(bool is_lobby) noexcept {
  FramingStatus dp_status = FramingStatus::WAITING;
  uint8_t dp_stx = 0;
  uint8_t dp_etx = 0;
  uint8_t dp_len = 0;
  Wallpad_DoorphoneGetFraming(dp_status, dp_stx, dp_etx, dp_len);

  if (dp_stx == 0)
    dp_stx = 0x7F;
  if (dp_etx == 0)
    dp_etx = 0xEE;

  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(dp_stx, dp_etx, dp_len);

  uint8_t op_call = (!is_lobby) ? (dp_prof ? dp_prof->call_front : 0xB9)
                                : (dp_prof ? dp_prof->call_lobby : 0x5F);
  uint8_t op_open = (!is_lobby) ? (dp_prof ? dp_prof->open_front : 0xB4)
                                : (dp_prof ? dp_prof->open_lobby : 0x61);
  uint8_t op_end = (!is_lobby) ? (dp_prof ? dp_prof->end_front : 0xB8)
                               : (dp_prof ? dp_prof->end_lobby : 0x60);

  // 50ms Pre-Guard Time: 벨 수신 직후 3840 bps 반이중 버스 충돌 방지용 Line Silent 대기
  uint32_t last_bell = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  if (last_bell > 0) {
    uint32_t now_ms = millis();
    constexpr uint32_t kDpPreGuardMs = 50;
    if (now_ms - last_bell < kDpPreGuardMs) {
      uint32_t rem_ms = kDpPreGuardMs - (now_ms - last_bell);
      if (rem_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(rem_ms) > 0 ? pdMS_TO_TICKS(rem_ms) : 1);
      }
    }
  }

  return Wallpad_DoorphoneStartSequence(dp_stx, dp_etx, op_call, op_open, op_end);
}

void Wallpad_DoorphoneInit() noexcept {
  s_doorphone_controller.init();
  Device_RegisterDoorphoneOpenHandler(Wallpad_DoorphoneOpen);
  Wallpad_InitDecoupledHooks();
  ProfileRepository::addProfileChangeListener(Wallpad_DoorphoneOnProfileChanged);
  Control_GetRegistry().init();
}

bool Wallpad_DoorphoneStartSequence(uint8_t stx, uint8_t etx, uint8_t op_call,
                                    uint8_t op_open, uint8_t op_end) noexcept {
  return s_doorphone_controller.startSequence(stx, etx, op_call, op_open, op_end);
}

void Wallpad_DoorphoneCancel() noexcept {
  s_doorphone_controller.cancel();
}

bool Wallpad_DoorphoneIsBusy() noexcept {
  return s_doorphone_controller.isBusy();
}

void Wallpad_DoorphoneGetState(bool &out_front_bell, bool &out_lobby_bell,
                              uint32_t &out_last_bell_ms) noexcept {
  out_front_bell = s_doorphone_state.front_bell.load(std::memory_order_relaxed);
  out_lobby_bell = s_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
  out_last_bell_ms = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
}

void Wallpad_DoorphoneGetFraming(FramingStatus &out_status, uint8_t &out_stx,
                                uint8_t &out_etx, uint8_t &out_len) noexcept {
  out_status = s_doorphone_tracker.status.load(std::memory_order_relaxed);
  out_stx = s_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
  out_etx = s_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
  out_len = s_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
}

bool Wallpad_DoorphoneGetLockedFraming(uint8_t &stx, uint8_t &etx, uint8_t &len) noexcept {
  if (s_doorphone_tracker.status.load(std::memory_order_relaxed) == FramingStatus::LOCKED) {
    stx = s_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
    etx = s_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
    len = s_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);
    return true;
  }
  return false;
}

void Wallpad_DoorphoneFrameDetected(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  char cur_dp_ns[16];
  FramingTracker::getNvsNamespace(Config_GetWallpadProfile(), cur_dp_ns, sizeof(cur_dp_ns));

  uint8_t fixed_len = 0;
  if (Wallpad_MatchDoorphoneLock(stx, etx, len, fixed_len) && len >= 5) {
    if (s_doorphone_tracker.status.load(std::memory_order_relaxed) != FramingStatus::LOCKED) {
      s_doorphone_tracker.setFixedLock(stx, etx, fixed_len);
      s_doorphone_tracker.saveToNvs(cur_dp_ns);
    }
  } else {
    s_doorphone_tracker.processFrame(stx, etx, len, cur_dp_ns);
  }
}

void Wallpad_DoorphoneCheckBellTimeout() noexcept {
  const uint32_t last_bell = s_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  if (last_bell > 0 && TimeUtils::isElapsed(last_bell, Config::Timing::DOORPHONE_BELL_TIMEOUT_MS)) {
    Wallpad_ResetDoorphoneBellState();
  }
}

void Wallpad_DoorphoneClearNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.clearNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneRestoreNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.restoreFromNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneSaveNvs(const char *nvs_ns) noexcept {
  s_doorphone_tracker.saveToNvs(nvs_ns, "DOORPHONE");
}

void Wallpad_DoorphoneOnProfileChanged(uint8_t old_idx, uint8_t new_idx) noexcept {
  if (old_idx != new_idx) {
    char old_ns[16], new_ns[16];
    FramingTracker::getNvsNamespace(old_idx, old_ns, sizeof(old_ns));
    FramingTracker::getNvsNamespace(new_idx, new_ns, sizeof(new_ns));
    s_doorphone_tracker.saveToNvs(old_ns, "DOORPHONE");
    s_doorphone_tracker.reset();
    s_doorphone_tracker.restoreFromNvs(new_ns, "DOORPHONE");
  }
}

void Wallpad_DoorphoneRegisterTxHandler(DoorphoneTxHandler handler) noexcept {
  s_dp_tx_handler = handler;
}

const DoorphoneSpec *Wallpad_MatchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
  return ProfileMatcher::matchDoorphone(stx, etx, len);
}

void Wallpad_ResetDoorphoneBellState() noexcept {
  s_doorphone_state.front_bell.store(false, std::memory_order_release);
  s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
  s_doorphone_state.last_bell_ms.store(0, std::memory_order_release);
  Device_NotifyDoorphoneEvent(false, false);
}

void Wallpad_HandleDoorphonePacket(const StaticPacket &packet) noexcept {
  Wallpad_DoorphoneCheckBellTimeout();
  if (packet.length < 3) return;
  uint8_t opcode = packet.data[1];
  uint32_t now = millis();
  bool state_changed = false;
  uint8_t pkt_stx = packet.data[0];
  uint8_t pkt_etx = packet.data[packet.length - 1];
  const DoorphoneSpec *dp_prof =
      ProfileMatcher::matchDoorphone(pkt_stx, pkt_etx, packet.length);

  enum class DoorphoneEvent : uint8_t {
    NONE = 0,
    BELL_FRONT,
    END_FRONT,
    BELL_LOBBY,
    END_LOBBY
  };

  struct DoorphoneActionTable {
    static constexpr DoorphoneEvent resolve(uint8_t op,
                                            const DoorphoneSpec *spec) noexcept {
      if (spec) {
        if (op == spec->bell_front) return DoorphoneEvent::BELL_FRONT;
        if (op == spec->end_front)  return DoorphoneEvent::END_FRONT;
        if (op == spec->bell_lobby) return DoorphoneEvent::BELL_LOBBY;
        if (op == spec->end_lobby)  return DoorphoneEvent::END_LOBBY;
      }
      switch (op) {
      case 0xB5: return DoorphoneEvent::BELL_FRONT;
      case 0xB6: [[fallthrough]];
      case 0xB8: return DoorphoneEvent::END_FRONT;
      case 0x5A: [[fallthrough]];
      case 0x5F: return DoorphoneEvent::BELL_LOBBY;
      case 0x60: return DoorphoneEvent::END_LOBBY;
      default:   return DoorphoneEvent::NONE;
      }
    }
  };

  const DoorphoneEvent ev = DoorphoneActionTable::resolve(opcode, dp_prof);
  switch (ev) {
  case DoorphoneEvent::BELL_FRONT:
    s_doorphone_state.front_bell.store(true, std::memory_order_release);
    s_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_FRONT:
    s_doorphone_state.front_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::BELL_LOBBY:
    s_doorphone_state.lobby_bell.store(true, std::memory_order_release);
    s_doorphone_state.last_bell_ms.store(now, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::END_LOBBY:
    s_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    state_changed = true;
    break;
  case DoorphoneEvent::NONE:
  default:
    break;
  }

  if (state_changed) {
    bool f = s_doorphone_state.front_bell.load(std::memory_order_relaxed);
    bool l = s_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
    Device_NotifyDoorphoneEvent(f, l);
  }
}

bool Wallpad_MatchDoorphoneLock(uint8_t stx, uint8_t etx, uint8_t len,
                                uint8_t &out_fixed_len) noexcept {
  const DoorphoneSpec *dp_spec = ProfileMatcher::matchDoorphone(stx, etx, len);
  if (dp_spec && stx == dp_spec->stx && etx == dp_spec->etx) {
    out_fixed_len = dp_spec->len;
    return true;
  }
  return false;
}

bool Wallpad_IsQueryPacket(std::span<const uint8_t> frame) noexcept {
  return Universal_GetEngine().isQueryPacket(frame);
}
