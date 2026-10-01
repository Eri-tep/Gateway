#include "Protocol.h"
#include "Bridge.h"
#include "Console.h"
#include "Core.h"
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

// ============================================================================
// 공통 헬퍼 (파일 전체에서 공유)
// ============================================================================
namespace {

constexpr uint8_t kWallpadChMask =
    (1 << 2) | (1 << 3); // CH2, CH3 (월패드 유래)
constexpr uint32_t EXPIRED_TARGET_EVICTION_TIMEOUT_MS = 600000;

// 고정 크기 char 배열 안전 복사 (strncpy + 널종료 대체)
template <size_t N> inline void setStr(char (&dst)[N], const char *src) {
  snprintf(dst, N, "%s", src ? src : "");
}

// NVS: NvsEnvelope(CRC 포함) 읽기/쓰기
template <class T> bool nvsGetEnv(Preferences &p, const char *key, T &out) {
  NvsEnvelope<T> e{};
  if (p.getBytesLength(key) != sizeof(e))
    return false;
  if (p.getBytes(key, &e, sizeof(e)) != sizeof(e) || !e.verify())
    return false;
  out = e.payload;
  return true;
}
template <class T> bool nvsPutEnv(Preferences &p, const char *key, const T &v) {
  NvsEnvelope<T> e{};
  e.payload = v;
  e.seal();
  return p.putBytes(key, &e, sizeof(e)) == sizeof(e);
}
template <class T>
bool nvsPutEnvNs(const char *ns, const char *key, const T &v) {
  Preferences p;
  if (!p.begin(ns, false))
    return false;
  bool ok = nvsPutEnv(p, key, v);
  p.end();
  return ok;
}

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
// HYUNDAI WALLPAD PROFILE (현대통신 실측 데이터 기반 정규화 - rodata 플래시 배치)
// ============================================================================

static constexpr DeviceSpec s_hyundai_devices[] = {
  // 0x19 일반 조명 (Switch)
  {
    0x19, DeviceClass::SWITCH, "Light",
    11, 7, 0x01, 0x02, 0xFF,
    11, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  },
  // 0x18 난방 / 보일러 (Thermostat)
  {
    0x18, DeviceClass::THERMOSTAT, "Thermo",
    11, 7, 0x01, 0x04, 0x07,
    18, 8, 10, 9, 0xFF, false, 0xFF, 0xFF, 0xFF,
    13, 7, 8, 9
  },
  // 0x1F 콘센트 (Outlet)
  {
    0x1F, DeviceClass::OUTLET, "Outlet",
    11, 7, 0x01, 0x02, 0xFF,
    18, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 9, 10,
    11, 7, 8, 0xFF
  },
  // 0x2B 환기 / 전열교환기 (Vent)
  {
    0x2B, DeviceClass::VENT, "Vent",
    11, 7, 0x01, 0x02, 0xFF,
    13, 8, 0xFF, 0xFF, 9, true, 0xFF, 0xFF, 0xFF,
    13, 7, 8, 0xFF
  },
  // 0x1B 가스 차단기 (Gas)
  {
    0x1B, DeviceClass::GAS, "Gas",
    11, 7, 0x00, 0x02, 0xFF,
    13, 0xFF, 0xFF, 0xFF, 0xFF, false, 8, 0xFF, 0xFF,
    13, 7, 8, 0xFF
  },
  // 0x34 엘리베이터 (Momentary)
  {
    0x34, DeviceClass::MOMENTARY, "Elevator",
    11, 7, 0x06, 0x00, 0xFF,
    13, 8, 0xFF, 0xFF, 0xFF, false, 0xFF, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  },
  // 0x1C 시스템 에어컨 / FCU (Aircon)
  {
    0x1C, DeviceClass::AIRCON, "Aircon",
    11, 7, 0x01, 0x02, 0xFF,
    15, 8, 12, 11, 10, true, 9, 0xFF, 0xFF,
    11, 7, 8, 0xFF
  }
};

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
  {
    3860, 0x7F, 0xEE, 5, "Hyundai HT Standard",
    0xB5, 0x5A, 0xB9, 0x5F, 0xB4, 0x61, 0xB8, 0x60
  }
};

const WallpadProfile *const kWallpadProfiles[] = {
  &kHyundaiProfile
};

const size_t kWallpadProfileCount = sizeof(kWallpadProfiles) / sizeof(kWallpadProfiles[0]);

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

void injectProfile(const WallpadProfile *profile,
                   ControlTemplateRegistry &registry) {
  if (!profile || !profile->devices || profile->device_count == 0)
    return;

  ESP_LOGI(TAG, "Applying profile: %s (%u devices)", profile->vendor_name,
           (unsigned)profile->device_count);

  for (size_t i = 0; i < profile->device_count; ++i) {
    const DeviceSpec &spec = profile->devices[i];
    registry.modifyOrCreateGroup(
        spec.dev_id,
        [&](GroupControlTemplate &grp) {
          grp.coverage.dev_class = spec.dev_class;
          setStr(grp.group_name, spec.name);
          grp.frame_len = spec.ctl_len;
          grp.sub1_offset = profile->sub1_offset;
          grp.sub2_offset = profile->sub2_offset;

          // 0x34 엘리베이터: 월패드 쿼리가 없으므로 기본 제어 골격 주입
          // (power_slot 은 아래 공통 블록에서 spec 값으로 덮어써지므로 여기서는
          // 골격/sub1 만 설정)
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

    ESP_LOGI(TAG,
             "Injected Dev 0x%02X (%s): CTL len=%u, QRY len=%u, StateOff=#%u",
             spec.dev_id, spec.name, spec.ctl_len, spec.qry_ack_len,
             spec.qry_power_offset);
  }
}

void matchAndInject(const AutoProbeDescriptor &ad,
                    ControlTemplateRegistry &registry) {
  if (const WallpadProfile *profile = matchProfile(ad)) {
    injectProfile(profile, registry);
    g_auto_probing_engine.injectControlSpec(0x02,
                                            11); // 표준 제어 Opcode / 길이
  } else {
    ESP_LOGW(TAG,
             "No matching wallpad profile found. Fallback to default framing.");
  }
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

bool UniversalProtocolEngine::validatePacket(span<const uint8_t> frame) const {
  if (frame.size() < 3 || frame.size() > 64)
    return false;
  EffProfile e = effectiveProfile();
  if (e.is_auto) {
    g_auto_probing_engine.feedFrame(frame); // 학습 후 갱신된 값으로 재계산
    e = effectiveProfile();
  }
  return checkFramingPure(frame, e.stx, e.etx, e.min_len, e.max_len, e.algo);
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

uint8_t UniversalProtocolEngine::calculateChecksum(const uint8_t *data,
                                                   size_t len) const {
  return g_auto_probing_engine.calculateChecksum(effectiveProfile().algo, data,
                                                 len);
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
// PollingTargetRegistry
// ============================================================================

PollingTargetRegistry g_polling_targets;

namespace {
bool entryMatches(const PollingTargetEntry &e, uint8_t d, uint8_t s1,
                  uint8_t s2, const uint8_t *raw, size_t n) {
  const bool in_key = d || s1 || s2;
  const bool e_key = e.dev_id || e.sub1 || e.sub2;
  if (in_key && e_key)
    return e.dev_id == d && e.sub1 == s1 && e.sub2 == s2;
  if (!in_key && e_key)
    return false;
  return raw && n > 0 && e.raw_query_len == n &&
         memcmp(e.raw_query_data.data(), raw, n) == 0;
}
} // namespace

void PollingTargetRegistry::registerOrTouch(uint8_t ch, uint8_t dev_id,
                                            uint8_t sub1, uint8_t sub2,
                                            const uint8_t *raw_pkt,
                                            size_t raw_len) {
  // CH2/CH3(월패드/앱), CH5(EW11 스니핑)만 대상. 0x2A(신발장/원격검침)는 폴링
  // 제외
  if ((ch != 2 && ch != 3 && ch != 5) || dev_id == 0x2A)
    return;

  const uint32_t now = millis();
  const uint8_t ch_bit = (ch < 8) ? static_cast<uint8_t>(1 << ch) : 0;
  const bool has_raw = raw_pkt && raw_len > 0 && raw_len <= 64;
  bool is_new_entry = false;
  {
    CriticalSectionLocker lock(&_mux);

    PollingTargetEntry *hit = nullptr;
    for (size_t i = 0; i < _count; ++i) {
      if (entryMatches(_entries[i], dev_id, sub1, sub2, raw_pkt, raw_len)) {
        hit = &_entries[i];
        break;
      }
    }

    PollingTargetEntry *e = hit;
    if (hit) {
      if (hit->last_requested_ms > 0 && now > hit->last_requested_ms) {
        const uint32_t delta = now - hit->last_requested_ms;
        if (delta >= 100 && delta <= 10000) {
          hit->last_interval_ms = hit->last_interval_ms
                                      ? (hit->last_interval_ms * 3 + delta) / 4
                                      : delta;
        }
      }
      if (hit->hit_count < 65535)
        hit->hit_count++;
      if (hit->dev_id == 0 && (dev_id || sub1 || sub2)) {
        hit->dev_id = dev_id;
        hit->sub1 = sub1;
        hit->sub2 = sub2;
      }
    } else if (_count < MAX_TARGETS) {
      e = &_entries[_count++];
      *e = PollingTargetEntry{}; // 이전 슬롯의 ACK/잔여값 제거
      e->dev_id = dev_id;
      e->sub1 = sub1;
      e->sub2 = sub2;
      e->hit_count = 1;
      is_new_entry = true;
    }

    if (e) {
      e->last_requested_ms = now;
      e->source_channels |= ch_bit;
      e->is_active = true;
      e->is_verified = true;
      if (has_raw) {
        e->raw_query_len = static_cast<uint8_t>(raw_len);
        memcpy(e->raw_query_data.data(), raw_pkt, raw_len);
      }
    }
  }

  if (is_new_entry) {
    g_warm_cache_dirty.store(true, std::memory_order_release);
    g_warm_cache_dirty_ms.store(now, std::memory_order_release);
  }
}

void PollingTargetRegistry::updateResponse(const uint8_t *q, size_t ql,
                                           const uint8_t *a, size_t al) {
  if (!q || !ql || !a || !al)
    return;
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    PollingTargetEntry &e = _entries[i];
    if (e.is_active && e.raw_query_len == ql &&
        memcmp(e.raw_query_data.data(), q, ql) == 0) {
      e.raw_ack_len = std::min<uint8_t>(al, 64);
      memcpy(e.raw_ack_data.data(), a, e.raw_ack_len);
      e.is_verified = true;
      return;
    }
  }
}

void PollingTargetRegistry::reindexWithOffsets(uint8_t dev_off, uint8_t s1_off,
                                               uint8_t s2_off) {
  CriticalSectionLocker lock(&_mux);
  auto at = [](const PollingTargetEntry &e, uint8_t off) -> uint8_t {
    return (off > 0 && e.raw_query_len > off) ? e.raw_query_data[off] : 0;
  };
  for (size_t i = 0; i < _count; ++i) {
    PollingTargetEntry &e = _entries[i];
    if (!(e.source_channels & kWallpadChMask))
      continue; // CH5 등은 재계산 제외
    if (e.raw_query_len > dev_off)
      e.dev_id = e.raw_query_data[dev_off];
    e.sub1 = at(e, s1_off);
    e.sub2 = at(e, s2_off);
  }
}

void PollingTargetRegistry::sweepExpired(uint32_t stale_timeout_ms) {
  const uint32_t now = millis();
  CriticalSectionLocker lock(&_mux);
  PollingTargetEntry *first = &_entries[0];
  for (size_t i = 0; i < _count; ++i) {
    if (now - first[i].last_requested_ms > stale_timeout_ms)
      first[i].is_active = false;
  }
  PollingTargetEntry *new_end =
      std::remove_if(first, first + _count, [&](const PollingTargetEntry &e) {
        return now - e.last_requested_ms > EXPIRED_TARGET_EVICTION_TIMEOUT_MS;
      });
  _count = static_cast<size_t>(new_end - first);
}

size_t PollingTargetRegistry::getActiveTargets(PollingTargetEntry *out,
                                               size_t max_count) {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i)
    if (_entries[i].is_active)
      out[w++] = _entries[i];
  return w;
}

size_t PollingTargetRegistry::getActiveCandidates(PollingCandidate *out,
                                                  size_t max_count) {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i) {
    const PollingTargetEntry &e = _entries[i];
    if (!e.is_active)
      continue;
    out[w].dev_id = e.dev_id;
    out[w].sub1 = e.sub1;
    out[w].sub2 = e.sub2;
    out[w].source_channels = e.source_channels;
    out[w].raw_ack_len = e.raw_ack_len;
    out[w].raw_query_len = e.raw_query_len;
    out[w].entry_idx = static_cast<uint8_t>(i);
    ++w;
  }
  return w;
}

bool PollingTargetRegistry::getQueryData(uint8_t idx, const uint8_t *&out_data,
                                         uint8_t &out_len) const {
  CriticalSectionLocker lock(&_mux);
  if (idx >= _count || !_entries[idx].is_active) {
    out_data = nullptr;
    out_len = 0;
    return false;
  }
  out_len = _entries[idx].raw_query_len;
  out_data = _entries[idx].raw_query_data.data();
  return true;
}

size_t PollingTargetRegistry::activeCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count,
                 [](const PollingTargetEntry &e) { return e.is_active; });
}
size_t PollingTargetRegistry::totalCount() const {
  CriticalSectionLocker lock(&_mux);
  return _count;
}
size_t PollingTargetRegistry::ackedCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count, [](const PollingTargetEntry &e) {
    return e.is_active && e.raw_ack_len > 0;
  });
}
size_t PollingTargetRegistry::verifiedCount() const {
  CriticalSectionLocker lock(&_mux);
  return countIf(&_entries[0], _count, [](const PollingTargetEntry &e) {
    return e.is_active && e.is_verified;
  });
}

bool PollingTargetRegistry::getEntry(size_t index,
                                     PollingTargetEntry &out) const {
  CriticalSectionLocker lock(&_mux);
  if (index >= _count)
    return false;
  out = _entries[index];
  return true;
}

void PollingTargetRegistry::resetHits() {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i)
    _entries[i].hit_count = 0;
}

void PollingTargetRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
}

void PollingTargetRegistry::loadFromWarmCache(const RtcWarmCacheEntry *entries,
                                              size_t count, uint32_t now_ms) {
  if (!entries || !count)
    return;
  CriticalSectionLocker lock(&_mux);
  size_t loaded = 0;
  for (size_t i = 0; i < count && loaded < MAX_TARGETS; ++i) {
    const RtcWarmCacheEntry &w = entries[i];
    if (w.dev_id == 0 || w.dev_id == 0x2A)
      continue;
    if (w.source_channels != 0 && !(w.source_channels & kWallpadChMask))
      continue; // CH2/CH3 아님

    PollingTargetEntry &e = _entries[loaded++];
    e = PollingTargetEntry{};
    e.dev_id = w.dev_id;
    e.sub1 = w.sub1;
    e.sub2 = w.sub2;
    e.source_channels = w.source_channels & kWallpadChMask;
    e.raw_query_len = std::min<uint8_t>(w.raw_len, 64);
    if (e.raw_query_len)
      memcpy(e.raw_query_data.data(), w.raw_query, e.raw_query_len);
    e.last_requested_ms = now_ms;
    e.last_interval_ms = 1000;
    e.is_active = true;
    e.is_verified = false; // 실제 버스 확인 전까지 미검증
    e.restored_ms = now_ms;
  }
  _count = loaded;
}

size_t PollingTargetRegistry::getWarmCacheEntries(RtcWarmCacheEntry *out,
                                                  size_t max_count) const {
  if (!out || !max_count)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t w = 0;
  for (size_t i = 0; i < _count && w < max_count; ++i) {
    const PollingTargetEntry &e = _entries[i];
    if (!e.is_active || !(e.source_channels & kWallpadChMask) ||
        e.dev_id == 0x2A)
      continue;
    RtcWarmCacheEntry &o = out[w++];
    o.dev_id = e.dev_id;
    o.sub1 = e.sub1;
    o.sub2 = e.sub2;
    o.source_channels = e.source_channels & kWallpadChMask;
    const size_t n = std::min<size_t>(e.raw_query_len, sizeof(o.raw_query));
    o.raw_len = static_cast<uint8_t>(n);
    memset(o.raw_query, 0, sizeof(o.raw_query));
    if (n)
      memcpy(o.raw_query, e.raw_query_data.data(), n);
  }
  return w;
}

void PollingTargetRegistry::markVerified(uint8_t dev_id, uint8_t sub1,
                                         uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      _entries[i].is_verified = true;
      return;
    }
  }
}

// ============================================================================
// ProfileRepository
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
    char old_ns[16], new_ns[16];
    Config::Doorphone::FramingTracker::getNvsNamespace(old_idx, old_ns,
                                                       sizeof(old_ns));
    Config::Doorphone::FramingTracker::getNvsNamespace(new_idx, new_ns,
                                                       sizeof(new_ns));
    g_doorphone_tracker.saveToNvs(old_ns, "DOORPHONE");
    g_doorphone_tracker.reset();
    g_doorphone_tracker.restoreFromNvs(new_ns, "DOORPHONE");
  }
  g_control_registry.onProfileChanged(old_idx, new_idx);
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

AutoProbingEngine g_auto_probing_engine;

AutoProbingEngine::AutoProbingEngine() { reset(); }

const char *AutoProbingEngine::getAlgoName(ChecksumAlgo algo) {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:
    return "XOR (0..[N-3])";
  case ChecksumAlgo::XOR_NO_STX:
    return "XOR (1..[N-3])";
  case ChecksumAlgo::SUM_ALL:
    return "SUM (0..[N-3])";
  case ChecksumAlgo::SUM_NO_STX:
    return "SUM (1..[N-3])";
  case ChecksumAlgo::TWOS_COMPLEMENT:
    return "2's Complement (1..[N-3])";
  case ChecksumAlgo::ONES_COMPLEMENT:
    return "1's Complement (0..[N-3])";
  case ChecksumAlgo::CRC8_MAXIM:
    return "CRC-8 (Maxim/0x31)";
  case ChecksumAlgo::NONE:
    return "None (Pure Framing)";
  default:
    return "Learning...";
  }
}

namespace {
uint8_t crc8Poly31(const uint8_t *d, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= d[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
  }
  return crc;
}

// 제어 프레임 길이 학습 (신규면 true)
bool addCtrlLen(AutoProbeDescriptor &d, uint8_t len) {
  for (uint8_t i = 0; i < d.ctrl_len_cnt; ++i)
    if (d.learned_ctrl_lens[i] == len)
      return false;
  if (d.ctrl_len_cnt >= sizeof(d.learned_ctrl_lens))
    return false;
  d.learned_ctrl_lens[d.ctrl_len_cnt++] = len;
  return true;
}

template <class T> uint8_t argmax256(const T *a, uint16_t &mx) {
  uint8_t best = 0;
  mx = 0;
  for (int i = 0; i < 256; ++i)
    if (a[i] > mx) {
      mx = a[i];
      best = static_cast<uint8_t>(i);
    }
  return best;
}
} // namespace

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo,
                                             const uint8_t *data,
                                             size_t len) const {
  if (!data || len < 3)
    return 0;
  if (algo == ChecksumAlgo::CRC8_MAXIM)
    return crc8Poly31(data, len - 2);

  const bool is_xor =
      (algo == ChecksumAlgo::XOR_ALL || algo == ChecksumAlgo::XOR_NO_STX);
  const bool is_sum =
      (algo == ChecksumAlgo::SUM_ALL || algo == ChecksumAlgo::SUM_NO_STX ||
       algo == ChecksumAlgo::TWOS_COMPLEMENT ||
       algo == ChecksumAlgo::ONES_COMPLEMENT);
  if (!is_xor && !is_sum)
    return 0;

  // STX 를 제외하는 알고리즘은 1부터 시작
  const size_t start =
      (algo == ChecksumAlgo::XOR_NO_STX || algo == ChecksumAlgo::SUM_NO_STX ||
       algo == ChecksumAlgo::TWOS_COMPLEMENT)
          ? 1
          : 0;
  uint8_t r = 0;
  for (size_t i = start; i < len - 2; ++i)
    r = is_xor ? (r ^ data[i]) : (r + data[i]);

  if (algo == ChecksumAlgo::TWOS_COMPLEMENT)
    return static_cast<uint8_t>(-r);
  if (algo == ChecksumAlgo::ONES_COMPLEMENT)
    return static_cast<uint8_t>(~r);
  return r;
}

void AutoProbingEngine::initFromNvs() {
  VendorProfileDescriptor prof;
  if (!ProfileRepository::getActiveProfile(prof))
    return;

  CriticalSectionLocker lock(&_mux);
  _desc.stx = prof.stx;
  _desc.etx = prof.etx;
  _desc.min_len = prof.min_len;
  _desc.max_len = prof.max_len;
  _desc.checksum_algo = prof.cs_algo;
  _desc.opcode_offset = prof.opcode_offset;
  _desc.query_opcode = prof.query_op;
  _desc.control_opcode = prof.ctrl_op;
  _desc.ack_opcode = prof.ack_op;
  _desc.control_seen = (prof.ctrl_op != 0);

  if (prof.dev_id_offset > 0 || prof.sub1_offset > 0 || prof.sub2_offset > 0) {
    _desc.dev_id_offset = prof.dev_id_offset;
    _desc.sub1_offset = prof.sub1_offset;
    _desc.sub2_offset = prof.sub2_offset;
    if (!_desc.is_swapped_addr) {
      if (_desc.gw_addr_offset == _desc.dev_id_offset ||
          _desc.gw_addr_offset != 2) {
        _desc.gw_addr_offset = 2;
        _desc.gw_addr = prof.gw_addr ? prof.gw_addr : 0x01;
      }
      if (_desc.sub1_offset == 2 && _desc.sub2_offset > 2)
        _desc.sub1_offset = _desc.sub2_offset;
    }
    _desc.learned_query_len =
        (prof.learned_query_len >= 3) ? prof.learned_query_len : 11;
    _desc.len_offset = prof.len_offset;
    _desc.has_len_field = (prof.has_len_field != 0);
    _desc.seq_offset = prof.seq_offset;
    _desc.has_seq_counter = (prof.seq_offset != 0xFF);
    _desc.ack_flag_offset = prof.ack_flag_offset;
    _desc.ctrl_len_cnt = prof.ctrl_len_cnt;
    memcpy(_desc.learned_ctrl_lens, prof.learned_ctrl_lens,
           sizeof(_desc.learned_ctrl_lens));
    if (_desc.control_seen && _desc.ctrl_len_cnt == 0) {
      _desc.learned_ctrl_lens[0] = _desc.learned_query_len;
      _desc.ctrl_len_cnt = 1;
    }

    int max_hdr =
        std::max({_desc.opcode_offset, _desc.dev_id_offset, _desc.sub1_offset,
                  _desc.sub2_offset, _desc.gw_addr_offset});
    for (uint8_t o :
         {_desc.len_offset, _desc.seq_offset, _desc.ack_flag_offset})
      if (o != 0xFF && o > max_hdr)
        max_hdr = o;
    _desc.payload_offset = static_cast<uint8_t>(std::max(max_hdr + 1, 8));
    _desc.offsets_locked = true;
    _desc.opcodes_locked = true;
  }

  _desc.is_locked = true;
  _consecutive_mismatches = 0;
  snprintf(_desc.description, sizeof(_desc.description),
           "Restored: %s (STX 0x%02X ETX 0x%02X / %s)", prof.name, prof.stx,
           prof.etx, getAlgoName(prof.cs_algo));
}

void AutoProbingEngine::feedFrame(span<const uint8_t> f) {
  if (f.size() < 3 || f.size() > 64)
    return;

  bool should_sync = false;
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.tested_packets++;

    const uint8_t stx = f[0], etx = f[f.size() - 1],
                  actual_cs = f[f.size() - 2];
    _stx_counts[stx]++;
    _etx_counts[etx]++;

    if (_desc.is_locked) {
      const bool ok = stx == _desc.stx && etx == _desc.etx &&
                      (_desc.checksum_algo == ChecksumAlgo::NONE ||
                       calculateChecksum(_desc.checksum_algo, f.data(),
                                         f.size()) == actual_cs);
      if (ok) {
        _desc.matched_packets++;
        _consecutive_mismatches = 0;
      } else if (++_consecutive_mismatches >= 5) { // 5연속 불일치 → 재학습
        _desc.is_locked = false;
        _desc.opcodes_locked = false;
        _desc.matched_packets = 0;
        _consecutive_matches = 0;
        _consecutive_mismatches = 0;
        _candidate_algo = ChecksumAlgo::UNKNOWN;
        memset(_stx_counts, 0, sizeof(_stx_counts));
        memset(_etx_counts, 0, sizeof(_etx_counts));
        memset(_algo_matches, 0, sizeof(_algo_matches));
        memset(_diff_idx_counts, 0, sizeof(_diff_idx_counts));
        snprintf(_desc.description, sizeof(_desc.description),
                 "Auto-unlocked (Mismatch detected, re-probing...)");
      }
      return;
    }

    uint16_t max_stx = 0, max_etx = 0;
    const uint8_t best_stx = argmax256(_stx_counts, max_stx);
    const uint8_t best_etx = argmax256(_etx_counts, max_etx);

    ChecksumAlgo matched = ChecksumAlgo::UNKNOWN;
    for (uint8_t a = 1; a <= 7; ++a) {
      ChecksumAlgo algo = static_cast<ChecksumAlgo>(a);
      if (calculateChecksum(algo, f.data(), f.size()) == actual_cs) {
        _algo_matches[a]++;
        matched = algo;
        break;
      }
    }
    if (matched == ChecksumAlgo::UNKNOWN)
      return;

    if (matched != _candidate_algo) {
      _candidate_algo = matched;
      _consecutive_matches = 1;
      return;
    }
    if (++_consecutive_matches >= 10 && max_stx >= 5 && max_etx >= 5) {
      _desc.is_locked = true;
      _desc.stx = best_stx;
      _desc.etx = best_etx;
      _desc.min_len = 3;
      _desc.max_len = 64;
      _desc.checksum_algo = matched;
      _desc.matched_packets = _desc.tested_packets;
      _consecutive_mismatches = 0;
      snprintf(_desc.description, sizeof(_desc.description),
               "Locked: STX 0x%02X ETX 0x%02X (%s)", best_stx, best_etx,
               getAlgoName(matched));
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
}

void AutoProbingEngine::feedOpcodePair(span<const uint8_t>,
                                       span<const uint8_t>) {
  // 조기 opcode 잠금 제거 — analyzeCacheMatrix() 가 유일한 opcode 판정자
}

void AutoProbingEngine::feedControlFrame(span<const uint8_t> ctrl) {
  if (ctrl.size() < 5)
    return;

  bool should_sync = false;
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    if (ctrl.size() <= _desc.opcode_offset)
      return;
    const uint8_t op = ctrl[_desc.opcode_offset];
    if (op == _desc.query_opcode || op == _desc.ack_opcode)
      return;

    bool changed = !_desc.control_seen || _desc.control_opcode != op;
    if (changed) {
      _desc.control_opcode = op;
      _desc.control_seen = true;
      _desc.opcodes_locked = true;
    }
    changed |= addCtrlLen(_desc, static_cast<uint8_t>(ctrl.size()));
    if (changed && _desc.is_locked) {
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
}

bool AutoProbingEngine::isLocked() const {
  CriticalSectionLocker lock(&_mux);
  return _desc.is_locked;
}

bool AutoProbingEngine::isOffsetsLocked() const {
  CriticalSectionLocker lock(&_mux);
  return _desc.offsets_locked;
}

void AutoProbingEngine::injectControlSpec(uint8_t ctrl_op, uint8_t ctrl_len) {
  bool should_sync = false;
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.control_opcode = ctrl_op;
    _desc.control_seen = true;
    _desc.opcodes_locked = true;
    addCtrlLen(_desc, ctrl_len);
    if (_desc.is_locked) {
      should_sync = true;
      snap = _desc;
    }
  }
  if (should_sync)
    ProfileRepository::syncAutoProfileToNvs(snap);
}

// ----------------------------------------------------------------------------
// 전체 매트릭스 분석: 쿼리/응답 쌍의 컬럼 통계로 헤더 필드 위치를 추정
// (std::set/map 제거 → bitset/배열 사용, 반복 패턴은 람다로 공통화)
// ----------------------------------------------------------------------------
bool AutoProbingEngine::analyzeCacheMatrix() {
  if (g_polling_targets.ackedCount() < 2 && g_device_repo.getOnlineCount() < 2)
    return false;

  struct PktPair {
    StaticPacket q, r;
  };
  std::vector<PktPair> pairs;
  const size_t target_count = g_polling_targets.totalCount();
  pairs.reserve(std::min<size_t>(target_count, 32));

  for (size_t i = 0; i < target_count; ++i) {
    PollingTargetEntry t;
    if (!g_polling_targets.getEntry(i, t) || !t.is_active ||
        t.raw_query_len < 4 ||
        !(t.source_channels & kWallpadChMask)) // 월패드(CH2/CH3) 유래만 분석
      continue;

    const uint8_t *ack = nullptr;
    size_t ack_len = 0;
    if (t.raw_ack_len >= 4) {
      ack = t.raw_ack_data.data();
      ack_len = t.raw_ack_len;
    } else if (const DeviceStateEntry *dev =
                   g_device_repo.find(t.dev_id, t.sub1, t.sub2);
               dev && dev->is_online && dev->last_ack_len >= 4) {
      ack = dev->last_ack_data.data();
      ack_len = dev->last_ack_len;
    }
    if (!ack)
      continue;

    PktPair p;
    p.q.length = t.raw_query_len;
    memcpy(p.q.data.data(), t.raw_query_data.data(), t.raw_query_len);
    p.r.length = ack_len;
    memcpy(p.r.data.data(), ack, ack_len);
    pairs.push_back(p);
  }

  const size_t N = pairs.size();
  if (N < 2)
    return false;

  size_t min_len = 256;
  for (const auto &p : pairs)
    min_len = std::min<size_t>({min_len, p.q.length, p.r.length});
  if (min_len < 4)
    return false;
  const size_t K = min_len - 1; // 스캔 대상: 컬럼 1 .. K-1

  using Bits = std::bitset<256>;
  auto all = [&](auto &&pred) {
    for (const auto &p : pairs)
      if (!pred(p))
        return false;
    return true;
  };
  auto qBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs)
      b.set(p.q.data[k]);
    return b;
  };
  auto rBits = [&](size_t k) {
    Bits b;
    for (const auto &p : pairs)
      b.set(p.r.data[k]);
    return b;
  };
  auto in = [](int k, std::initializer_list<int> ex) {
    for (int x : ex)
      if (x == k)
        return true;
    return false;
  };
  auto combo = [&](size_t a, size_t b) { // (a,b) 컬럼 조합의 고유 개수
    std::vector<uint16_t> v;
    v.reserve(N);
    for (const auto &p : pairs)
      v.push_back(static_cast<uint16_t>((p.q.data[a] << 8) | p.q.data[b]));
    std::sort(v.begin(), v.end());
    return static_cast<size_t>(std::unique(v.begin(), v.end()) - v.begin());
  };

  // 1) 길이 필드: 값 == 길이 - delta (delta 0 = 정확히 일치)
  int len_idx = -1;
  for (size_t k = 1; k < K && len_idx < 0; ++k) {
    for (int delta : {0, 2, 3, 4, 5}) {
      if (all([&](const PktPair &p) {
            return p.q.data[k] == int(p.q.length) - delta &&
                   p.r.data[k] == int(p.r.length) - delta;
          })) {
        len_idx = static_cast<int>(k);
        break;
      }
    }
  }

  // 2) Opcode: 쿼리/응답 값이 다르면서 모든 쌍에서 동일한 컬럼
  int opcode_idx = -1;
  uint8_t learned_q_op = 0, learned_ack_op = 0;
  for (size_t k = 1; k < K; ++k) {
    if (int(k) == len_idx)
      continue;
    const uint8_t cq = pairs[0].q.data[k], cr = pairs[0].r.data[k];
    if (cq == cr)
      continue;
    if (all([&](const PktPair &p) {
          return p.q.data[k] == cq && p.r.data[k] == cr;
        })) {
      opcode_idx = int(k);
      learned_q_op = cq;
      learned_ack_op = cr;
      break;
    }
  }

  // 3) 주소 swap (쿼리의 i,j 가 응답에서 교차)
  int swap_i = -1, swap_j = -1, promoted_dev_idx = -1, master_gw_idx = -1;
  for (size_t i = 1; i < K && swap_i < 0; ++i) {
    if (in(int(i), {len_idx, opcode_idx}))
      continue;
    for (size_t j = i + 1; j < K; ++j) {
      if (in(int(j), {len_idx, opcode_idx}))
        continue;
      if (!all([&](const PktPair &p) {
            return p.q.data[i] == p.r.data[j] && p.q.data[j] == p.r.data[i] &&
                   p.q.data[i] != p.q.data[j];
          }))
        continue;
      swap_i = int(i);
      swap_j = int(j);
      const size_t ci = qBits(i).count(), cj = qBits(j).count();
      if (ci == 1 && cj > 1) {
        master_gw_idx = int(i);
        promoted_dev_idx = int(j);
      } else if (cj == 1 && ci > 1) {
        master_gw_idx = int(j);
        promoted_dev_idx = int(i);
      } else {
        master_gw_idx = int(i);
        promoted_dev_idx = int(j);
      }
      break;
    }
  }

  // 4) 시퀀스 카운터 (+1 씩 증가하는 컬럼)
  int seq_idx = -1;
  if (N >= 4) {
    for (size_t k = 1; k < K && seq_idx < 0; ++k) {
      if (in(int(k), {len_idx, opcode_idx, swap_i, swap_j, promoted_dev_idx}))
        continue;
      if (qBits(k).count() < 3)
        continue;
      bool inc = true;
      for (size_t m = 0; m + 1 < N && inc; ++m)
        inc = static_cast<uint8_t>(pairs[m + 1].q.data[k] -
                                   pairs[m].q.data[k]) == 1;
      if (inc)
        seq_idx = int(k);
    }
  }

  // 5) 서브 커맨드 / 마스터(게이트웨이) 주소
  int sub_cmd_idx = -1;
  for (size_t k = 1; k < K; ++k) {
    if (in(int(k),
           {len_idx, opcode_idx, swap_i, swap_j, seq_idx, promoted_dev_idx}))
      continue;
    if (!all([&](const PktPair &p) { return p.q.data[k] == p.r.data[k]; }))
      continue;

    if (qBits(k).count() == 1) {
      if (master_gw_idx < 0)
        master_gw_idx = int(k);
    } else if (promoted_dev_idx >= 0) {
      int16_t dep[256];
      std::fill(std::begin(dep), std::end(dep), int16_t(-1));
      bool pure = true;
      size_t keys = 0;
      for (const auto &p : pairs) {
        const uint8_t dt = p.q.data[promoted_dev_idx], sc = p.q.data[k];
        if (dep[dt] >= 0 && dep[dt] != sc) {
          pure = false;
          break;
        }
        if (dep[dt] < 0)
          ++keys;
        dep[dt] = sc;
      }
      if (pure && keys > 1) {
        sub_cmd_idx = int(k);
        break;
      }
    }
  }

  // 6) 기기 타입 / 서브 ID 후보
  std::vector<size_t> cols;
  for (size_t k = 1; k < K; ++k) {
    if (in(int(k), {len_idx, opcode_idx, swap_i, swap_j, seq_idx, sub_cmd_idx,
                    promoted_dev_idx}))
      continue;
    if (qBits(k).count() >= 2)
      cols.push_back(k);
  }

  int dev_type_idx = promoted_dev_idx, sub_id_idx = -1;
  const size_t min_unique = std::max<size_t>(2, N * 8 / 10);

  if (dev_type_idx < 0) { // 응답 길이를 결정하는 컬럼 = 기기 타입
    for (size_t cand : cols) {
      int16_t lenOf[256];
      std::fill(std::begin(lenOf), std::end(lenOf), int16_t(-1));
      bool ok = true;
      size_t keys = 0;
      for (const auto &p : pairs) {
        const uint8_t v = p.q.data[cand];
        const int16_t rl = static_cast<int16_t>(p.r.length);
        if (lenOf[v] >= 0 && lenOf[v] != rl) {
          ok = false;
          break;
        }
        if (lenOf[v] < 0)
          ++keys;
        lenOf[v] = rl;
      }
      if (!ok || keys <= 1)
        continue;
      Bits distinct;
      for (int16_t l : lenOf)
        if (l >= 0)
          distinct.set(static_cast<size_t>(l));
      if (distinct.count() > 1) {
        dev_type_idx = int(cand);
        break;
      }
    }
  }

  if (dev_type_idx >= 0) {
    for (size_t cand : cols) {
      if (int(cand) == dev_type_idx)
        continue;
      if (combo(dev_type_idx, cand) >= min_unique) {
        sub_id_idx = int(cand);
        break;
      }
    }
  } else {
    for (size_t c1 : cols) {
      for (size_t c2 : cols) {
        if (c1 == c2 || combo(c1, c2) < min_unique)
          continue;
        const bool c1_is_dev = qBits(c1).count() <= qBits(c2).count();
        dev_type_idx = int(c1_is_dev ? c1 : c2);
        sub_id_idx = int(c1_is_dev ? c2 : c1);
        break;
      }
      if (dev_type_idx >= 0)
        break;
    }
  }

  // 7) ACK 플래그 (응답에서 항상 0x00/0x01 고정인 헤더 내부 컬럼)
  int ack_flag_idx = -1;
  const int max_known_hdr =
      std::max({opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i,
                swap_j, len_idx, seq_idx});
  for (size_t k = 1; k < K; ++k) {
    const int ik = int(k);
    if (in(ik, {len_idx, opcode_idx, swap_i, swap_j, seq_idx, sub_cmd_idx,
                dev_type_idx, sub_id_idx}) ||
        ik > max_known_hdr)
      continue;
    const Bits rb = rBits(k);
    if (rb.count() == 1 && (rb.test(0x00) || rb.test(0x01))) {
      ack_flag_idx = ik;
      break;
    }
  }

  // 8) 디스크립터 반영
  AutoProbeDescriptor snap;
  {
    CriticalSectionLocker lock(&_mux);
    if (opcode_idx >= 0) {
      _desc.opcode_offset = uint8_t(opcode_idx);
      _desc.query_opcode = learned_q_op;
      _desc.ack_opcode = learned_ack_op;
      _desc.opcodes_locked = true;
    }
    if (dev_type_idx >= 0)
      _desc.dev_id_offset = uint8_t(dev_type_idx);
    if (sub_cmd_idx >= 0)
      _desc.sub1_offset = uint8_t(sub_cmd_idx);
    else if (sub_id_idx >= 0)
      _desc.sub1_offset = uint8_t(sub_id_idx);
    if (sub_id_idx >= 0)
      _desc.sub2_offset = uint8_t(sub_id_idx);

    _desc.len_offset = (len_idx >= 0) ? uint8_t(len_idx) : 0xFF;
    _desc.has_len_field = (len_idx >= 0);
    _desc.seq_offset = (seq_idx >= 0) ? uint8_t(seq_idx) : 0xFF;
    _desc.has_seq_counter = (seq_idx >= 0);
    _desc.ack_flag_offset = (ack_flag_idx >= 0) ? uint8_t(ack_flag_idx) : 0xFF;
    _desc.is_swapped_addr = (swap_i >= 0);

    if (master_gw_idx >= 0) {
      _desc.gw_addr_offset = uint8_t(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (!_desc.is_swapped_addr && dev_type_idx >= 0) {
      _desc.gw_addr_offset = _desc.dev_id_offset;
    }

    if (min_len >= 5 && min_len <= 64)
      _desc.learned_query_len = uint8_t(min_len);
    _desc.offsets_locked = (dev_type_idx >= 0 && sub_id_idx >= 0);

    const int max_hdr =
        std::max({0, opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i,
                  swap_j, len_idx, seq_idx, ack_flag_idx});

    // 제로 분산 패딩 스킵: max_hdr 직후 모든 응답에서 0x00 고정인 열
    int payload_start = max_hdr + 1;
    while (payload_start < int(min_len) - 2 && all([&](const PktPair &p) {
             return p.r.data[payload_start] == 0x00;
           }))
      ++payload_start;
    _desc.payload_offset = uint8_t(payload_start);

    snprintf(_desc.description, sizeof(_desc.description),
             "Auto: OP@%u(0x%02X/0x%02X) DEV@%u SUB1@%u SUB2@%u PL@%u",
             _desc.opcode_offset, _desc.query_opcode, _desc.ack_opcode,
             _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset,
             _desc.payload_offset);
    snap = _desc;
  }

  ProfileRepository::syncAutoProfileToNvs(snap);

  if (snap.offsets_locked) {
    g_polling_targets.reindexWithOffsets(snap.dev_id_offset, snap.sub1_offset,
                                         snap.sub2_offset);
    for (size_t i = 0; i < target_count; ++i) {
      PollingTargetEntry t;
      if (g_polling_targets.getEntry(i, t) && t.is_active &&
          t.raw_ack_len >= 4) {
        StaticPacket ack_pkt;
        ack_pkt.channel_id = 1;
        ack_pkt.length = t.raw_ack_len;
        memcpy(ack_pkt.data.data(), t.raw_ack_data.data(), t.raw_ack_len);
        g_device_repo.updateFromBus(ack_pkt);
      }
    }
  }

  g_telnet_tracer.trace("[AUTO PROBE] ★ Full-Matrix Cache Analysis Complete! "
                        "Offsets locked & saved to NVS.\r\n");
  return true;
}

AutoProbeDescriptor AutoProbingEngine::getDescriptor() const {
  CriticalSectionLocker lock(&_mux);
  return _desc;
}

void AutoProbingEngine::reset() {
  CriticalSectionLocker lock(&_mux);
  memset(&_desc, 0, sizeof(_desc));
  _desc.stx = 0xF7;
  _desc.etx = 0xEE;
  _desc.min_len = 3;
  _desc.max_len = 64;
  _desc.checksum_algo = ChecksumAlgo::XOR_ALL;
  _desc.opcode_offset = 4;
  _desc.query_opcode = 0x01;
  _desc.ack_opcode = 0x04;
  _desc.len_offset = 0xFF;
  _desc.seq_offset = 0xFF;
  _desc.ack_flag_offset = 0xFF;
  _desc.payload_offset = 7;
  _consecutive_mismatches = 0;
  strncpy(_desc.description, "Probing bus traffic...",
          sizeof(_desc.description) - 1);
  memset(_stx_counts, 0, sizeof(_stx_counts));
  memset(_etx_counts, 0, sizeof(_etx_counts));
  memset(_algo_matches, 0, sizeof(_algo_matches));
  memset(_diff_idx_counts, 0, sizeof(_diff_idx_counts));
  _consecutive_matches = 0;
  _candidate_algo = ChecksumAlgo::UNKNOWN;
  _control_matches = 0;
  _candidate_ctrl_op = 0;
}

// ============================================================================
// Control: DeviceRouteRegistry
// ============================================================================

using namespace ControlTemplateUtils;

DeviceRouteRegistry g_route_registry;

namespace {
template <class E>
int findRouteIdx(const E *e, size_t n, uint8_t d, uint8_t s1, uint8_t s2) {
  for (size_t i = 0; i < n; ++i)
    if (e[i].dev_id == d && e[i].sub1 == s1 && e[i].sub2 == s2)
      return static_cast<int>(i);
  return -1;
}
} // namespace

void DeviceRouteRegistry::recordRoute(uint8_t channel_id, int8_t slot_idx,
                                      uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  int i = findRouteIdx(&_entries[0], _count, dev_id, sub1, sub2);
  if (i < 0) {
    if (_count >= MAX_ROUTES)
      return;
    i = static_cast<int>(_count++);
    _entries[i].dev_id = dev_id;
    _entries[i].sub1 = sub1;
    _entries[i].sub2 = sub2;
  }
  _entries[i].endpoint.channel_id = channel_id;
  _entries[i].endpoint.slot_idx = slot_idx;
  _entries[i].endpoint.last_seen_ms = millis();
}

bool DeviceRouteRegistry::lookupRoute(uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2,
                                      RouteEndpoint &out_ep) const {
  CriticalSectionLocker lock(&_mux);
  const int i = findRouteIdx(&_entries[0], _count, dev_id, sub1, sub2);
  if (i < 0)
    return false;
  out_ep = _entries[i].endpoint;
  return true;
}

size_t DeviceRouteRegistry::getRoutes(DeviceRouteEntry *out_buf,
                                      size_t max_count) const {
  CriticalSectionLocker lock(&_mux);
  const size_t n = std::min(_count, max_count);
  for (size_t i = 0; i < n; i++)
    out_buf[i] = _entries[i];
  return n;
}

void DeviceRouteRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
  memset(_entries, 0, sizeof(_entries));
}

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

uint8_t GroupControlTemplate::getTargetTempOffset(uint8_t pkt_len) const noexcept {
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

uint8_t GroupControlTemplate::getCurrentTempOffset(uint8_t pkt_len) const noexcept {
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

uint8_t GroupControlTemplate::getFanSpeedOffset(uint8_t pkt_len) const noexcept {
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
  // Byte #8 운전 모드 토큰 (1:일반, 2:바이패스, 3:자동, 4:공기청정, 0x81:Reject)
  if (raw_byte >= 1 && raw_byte <= 4)
    return raw_byte;
  const uint8_t nibble = (raw_byte >> 4) & 0x0F;
  if (nibble >= 1 && nibble <= 4)
    return nibble;
  return 1; // 기본 일반 환기 (0x01)
}

uint8_t GroupControlTemplate::getValveStateOffset(uint8_t pkt_len) const noexcept {
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
