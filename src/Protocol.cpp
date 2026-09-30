#include "Protocol.h"
#include "Core.h"
#include "Console.h"
#include "Bridge.h"
#include "esp_log.h"
#include <Preferences.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

// ============================================================================
// Section from src/Parser/ProfileMatcher.cpp
// ============================================================================

static const char *TAG = "ProfileMatcher";

namespace ProfileMatcher {

static const WallpadProfile *s_active_profile = &kHyundaiProfile;

const WallpadProfile *getActiveProfile() {
  return s_active_profile;
}

const WallpadProfile *matchProfile(const AutoProbeDescriptor &ad) {
  if (!ad.offsets_locked) {
    return s_active_profile;
  }

  for (size_t i = 0; i < kWallpadProfileCount; ++i) {
    const WallpadProfile *p = kWallpadProfiles[i];
    if (!p) continue;

    // STX, ETX, Checksum 알고리즘 일치 여부 확인
    if (ad.stx == p->stx && ad.etx == p->etx && ad.checksum_algo == p->checksum_algo) {
      // 프레임 오프셋 확인
      if (ad.opcode_offset == p->opcode_offset && ad.dev_id_offset == p->dev_id_offset) {
        s_active_profile = p;
        return p;
      }
    }
  }

  return s_active_profile;
}

const DoorphoneSpec *matchDoorphone(uint8_t stx, uint8_t etx, uint8_t len) {
  if (stx == 0 || etx == 0) return nullptr;

  // 1. 현재 활성화된 프로파일의 도어폰 명세 우선 확인
  if (s_active_profile && s_active_profile->doorphone.stx == stx && s_active_profile->doorphone.etx == etx) {
    if (s_active_profile->doorphone.len == 0 || s_active_profile->doorphone.len == len) {
      return &s_active_profile->doorphone;
    }
  }

  // 2. 전체 카탈로그 순회 검색
  for (size_t i = 0; i < kWallpadProfileCount; ++i) {
    const WallpadProfile *p = kWallpadProfiles[i];
    if (p && p->doorphone.stx == stx && p->doorphone.etx == etx) {
      if (p->doorphone.len == 0 || p->doorphone.len == len) {
        return &p->doorphone;
      }
    }
  }

  return nullptr;
}

void injectProfile(const WallpadProfile *profile, ControlTemplateRegistry &registry) {
  if (!profile || !profile->devices || profile->device_count == 0) {
    return;
  }

  ESP_LOGI(TAG, "Applying profile: %s (%u devices)", profile->vendor_name, (unsigned)profile->device_count);

  for (size_t i = 0; i < profile->device_count; ++i) {
    const DeviceSpec &spec = profile->devices[i];
    registry.modifyOrCreateGroup(spec.dev_id, [&](GroupControlTemplate &grp) {
      grp.coverage.dev_class = spec.dev_class;
      snprintf(grp.group_name, sizeof(grp.group_name), "%s", spec.name);

      // 기본 제어 템플릿(CTL) 구조 설정
      grp.frame_len = spec.ctl_len;
      grp.sub1_offset = profile->sub1_offset;
      grp.sub2_offset = profile->sub2_offset;

      // 0x34 엘리베이터의 경우 월패드 쿼리가 없으므로 기본 제어 골격 주입
      if (spec.dev_id == 0x34 && spec.ctl_len == 11) {
        const uint8_t ev_proto[11] = {0xF7, 0x0B, 0x01, 0x34, 0x02, 0x41, 0x10, 0x06, 0x00, 0x9C, 0xEE};
        std::copy(ev_proto, ev_proto + 11, grp.raw_template);
        grp.power_slot.action_offset = 7;
        grp.power_slot.on_val = 0x06;
        grp.power_slot.off_val = 0x00;
        grp.ctl_sub1_override = 0x10;
      }

      // 전원 액션 슬롯 설정
      grp.power_slot.discovered = true;
      grp.power_slot.action_offset = spec.ctl_payload_offset;
      grp.power_slot.on_val = spec.pwr_on_val;
      grp.power_slot.off_val = spec.pwr_off_val;
      grp.power_slot.ack_state_offset = spec.ctl_ack_state_offset;

      // 외출 모드 토큰 주입 (난방 0x07 등)
      if (spec.pwr_away_val != 0xFF) {
        grp.away_mode_token = spec.pwr_away_val;
      }

      // 기기 클래스별 추가 슬롯 설정
      if (spec.dev_class == DeviceClass::THERMOSTAT) {
        grp.temp_slot.discovered = true;
        grp.temp_slot.category_offset = 5; // 현대통신 온도제어 카테고리 오프셋
        grp.temp_slot.category_val = 0x45;    // 현대통신 온도제어 카테고리 코드
        grp.temp_slot.action_offset = spec.ctl_payload_offset;
        grp.temp_slot.min_val = 14;
        grp.temp_slot.max_val = 36;
        grp.temp_slot.ack_state_offset = spec.ctl_ack_state_offset;
        grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
        grp.temp_slot.ack_telemetry_offset = spec.ctl_ack_ambtemp_offset;
      } else if (spec.dev_class == DeviceClass::VENT) {
        grp.speed_slot.discovered = true;
        grp.speed_slot.category_offset = 5; // 현대통신 풍량제어 카테고리 오프셋
        grp.speed_slot.category_val = 0x42;   // 현대통신 풍량제어 카테고리 코드
        grp.speed_slot.action_offset = spec.ctl_payload_offset;
        grp.speed_slot.min_val = 1;
        grp.speed_slot.max_val = 3;
        grp.speed_slot.level_count = 3;
        grp.speed_slot.level_tokens[0] = 0x01; // 1단 (약)
        grp.speed_slot.level_tokens[1] = 0x03; // 2단 (중)
        grp.speed_slot.level_tokens[2] = 0x07; // 3단 (강)
        grp.speed_slot.ack_state_offset = spec.ctl_ack_state_offset;

        // 운전 모드 슬롯 설정 (Cat 0x43, 일반 0x01, 바이패스 0x02, 자동 0x03, 공기청정 0x04)
        grp.mode_slot.discovered = true;
        grp.mode_slot.category_offset = 5;
        grp.mode_slot.category_val = 0x43;
        grp.mode_slot.action_offset = spec.ctl_payload_offset; // Byte #7
        grp.mode_slot.min_val = 1;
        grp.mode_slot.max_val = 4;
        grp.mode_slot.ack_state_offset = spec.ctl_ack_state_offset;
      } else if (spec.dev_class == DeviceClass::GAS) {
        grp.close_slot.discovered = true;
        grp.close_slot.category_offset = 5;
        grp.close_slot.category_val = 0x43;
        grp.close_slot.action_offset = spec.ctl_payload_offset; // Byte #7
        grp.close_slot.off_val = spec.pwr_off_val;              // 0x02
        grp.close_slot.ack_state_offset = spec.ctl_ack_state_offset;
      } else if (spec.dev_class == DeviceClass::AIRCON) {
        // 시스템 에어컨 희망온도 (Cat 0x45)
        grp.temp_slot.discovered = true;
        grp.temp_slot.category_offset = 5;
        grp.temp_slot.category_val = 0x45;
        grp.temp_slot.action_offset = spec.ctl_payload_offset;
        grp.temp_slot.min_val = 18;
        grp.temp_slot.max_val = 30;
        grp.temp_slot.ack_state_offset = spec.ctl_ack_state_offset;
        grp.temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;

        // 시스템 에어컨 풍량 (Cat 0x42)
        grp.speed_slot.discovered = true;
        grp.speed_slot.category_offset = 5;
        grp.speed_slot.category_val = 0x42;
        grp.speed_slot.action_offset = spec.ctl_payload_offset;
        grp.speed_slot.min_val = 1;
        grp.speed_slot.max_val = 3;
        grp.speed_slot.level_count = 3;
        grp.speed_slot.level_tokens[0] = 0x01; // 미풍
        grp.speed_slot.level_tokens[1] = 0x02; // 약풍
        grp.speed_slot.level_tokens[2] = 0x03; // 강풍
        grp.speed_slot.ack_state_offset = spec.ctl_ack_state_offset;

        // 시스템 에어컨 운전 모드 (Cat 0x41: 1:냉방, 2:제습, 3:송풍, 4:자동, 5:난방)
        grp.mode_slot.discovered = true;
        grp.mode_slot.category_offset = 5;
        grp.mode_slot.category_val = 0x41;
        grp.mode_slot.action_offset = spec.ctl_payload_offset;
        grp.mode_slot.min_val = 1;
        grp.mode_slot.max_val = 5;
        grp.mode_slot.ack_state_offset = spec.ctl_ack_state_offset;
      }

      // 제어 응답 슬롯(ack_slots) 명세 주입
      grp.ack_slots.discovered = true;
      grp.ack_slots.power_offset = spec.ctl_ack_state_offset;
      if (spec.dev_class == DeviceClass::THERMOSTAT) {
        grp.ack_slots.target_temp_offset = spec.ctl_ack_echo_offset;
        grp.ack_slots.current_temp_offset = spec.ctl_ack_ambtemp_offset;
      }

      // 쿼리 응답 슬롯(query_slots) 명세 주입
      grp.query_slots.discovered = true;
      grp.query_slots.expected_len = spec.qry_ack_len;
      grp.query_slots.power_offset = spec.qry_power_offset;
      grp.query_slots.target_temp_offset = spec.qry_settemp_offset;
      grp.query_slots.current_temp_offset = spec.qry_ambtemp_offset;
      grp.query_slots.fan_speed_offset = spec.qry_fanspeed_offset;
      grp.query_slots.valve_state_offset = spec.qry_valve_offset;
      grp.query_slots.power_w_offset = spec.qry_watt_h_offset;
    }, spec.name);

    ESP_LOGI(TAG, "Injected Dev 0x%02X (%s): CTL len=%u, QRY len=%u, StateOff=#%u",
             spec.dev_id, spec.name, spec.ctl_len, spec.qry_ack_len, spec.qry_power_offset);
  }
}

void matchAndInject(const AutoProbeDescriptor &ad, ControlTemplateRegistry &registry) {
  const WallpadProfile *profile = matchProfile(ad);
  if (profile) {
    injectProfile(profile, registry);
    // 카탈로그 스펙에 정의된 표준 제어 Opcode(0x02) 및 기본 제어 길이(11바이트)를 오토프로빙 디스크립터에 즉시 주입
    g_auto_probing_engine.injectControlSpec(0x02, 11);
  } else {
    ESP_LOGW(TAG, "No matching wallpad profile found. Fallback to default framing.");
  }
}

} // namespace ProfileMatcher


// ============================================================================
// Section from src/Parser/UniversalProtocolEngine.cpp
// ============================================================================

static UniversalProtocolEngine s_universal_engine;

size_t UniversalProtocolEngine::getVendorName(char *out, size_t max_len) const {
  if (!out || max_len == 0) return 0;
  out[0] = '\0';
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  if (strcasecmp(desc.key, "auto") == 0) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (ad.is_locked) {
      return snprintf(out, max_len, "Auto [STX 0x%02X ETX 0x%02X / %s]",
                      ad.stx, ad.etx, AutoProbingEngine::getAlgoName(ad.checksum_algo));
    }
    return snprintf(out, max_len, "%s", "Auto (Learning...)");
  }
  return snprintf(out, max_len, "%s", desc.name);
}

size_t UniversalProtocolEngine::getActiveProfileKey(char *out, size_t max_len) const {
  if (!out || max_len == 0) return 0;
  out[0] = '\0';
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  return snprintf(out, max_len, "%s", desc.key);
}

static inline bool checkFramingPure(span<const uint8_t> frame, uint8_t stx,
                                    uint8_t etx, uint8_t min_len,
                                    uint8_t max_len, ChecksumAlgo algo) {
  if (frame.size() < min_len || frame.size() > max_len || frame.size() < 3)
    return false;
  if (frame[0] != stx || frame[frame.size() - 1] != etx)
    return false;
  if (algo == ChecksumAlgo::NONE)
    return true;
  uint8_t cs = g_auto_probing_engine.calculateChecksum(algo, frame.data(),
                                                       frame.size());
  return cs == frame[frame.size() - 2];
}

bool UniversalProtocolEngine::validatePacket(span<const uint8_t> frame) const {
  if (frame.size() < 3 || frame.size() > 64)
    return false;

  VendorProfileDescriptor desc = activeProfile();

  if (isAutoProfile(desc)) {
    g_auto_probing_engine.feedFrame(frame);
    auto ad = g_auto_probing_engine.getDescriptor();
    return checkFramingPure(frame, ad.stx, ad.etx, ad.min_len, ad.max_len,
                            ad.checksum_algo);
  }

  return checkFramingPure(frame, desc.stx, desc.etx, desc.min_len, desc.max_len,
                          desc.cs_algo);
}

bool UniversalProtocolEngine::isQueryPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    return frame[ad.opcode_offset] == ad.query_opcode;
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return frame[desc.opcode_offset] == desc.query_op;
}

bool UniversalProtocolEngine::isControlPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    if (ad.control_seen && ad.control_opcode != 0) {
      return frame[ad.opcode_offset] == ad.control_opcode;
    }
    return (frame[ad.opcode_offset] != ad.query_opcode && frame[ad.opcode_offset] != ad.ack_opcode);
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return frame[desc.opcode_offset] == desc.ctrl_op;
}

bool UniversalProtocolEngine::isAckPacket(span<const uint8_t> frame) const {
  VendorProfileDescriptor desc = activeProfile();
  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (frame.size() <= ad.opcode_offset)
      return false;
    return (frame[ad.opcode_offset] == ad.ack_opcode || frame[ad.opcode_offset] == ad.query_opcode);
  }
  if (frame.size() <= desc.opcode_offset)
    return false;
  return (frame[desc.opcode_offset] == desc.ack_op || frame[desc.opcode_offset] == desc.query_op);
}

bool UniversalProtocolEngine::extractDeviceKey(span<const uint8_t> frame,
                                               uint8_t &dev_id, uint8_t &sub1,
                                               uint8_t &sub2) const {
  VendorProfileDescriptor desc = activeProfile();
  uint8_t d_off = desc.dev_id_offset;
  uint8_t s1_off = desc.sub1_offset;
  uint8_t s2_off = desc.sub2_offset;

  bool is_swapped = (desc.is_swapped_addr != 0);
  uint8_t gw_addr_off = is_swapped ? desc.gw_addr_offset : d_off;

  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (ad.offsets_locked) {
      d_off = ad.dev_id_offset;
      s1_off = ad.sub1_offset;
      s2_off = ad.sub2_offset;
      is_swapped = ad.is_swapped_addr;
      gw_addr_off = ad.gw_addr_offset;
    }
  }

  if (frame.size() <= d_off)
    return false;

  if (is_swapped && isAckPacket(frame)) {
    if (frame.size() <= gw_addr_off)
      return false;
    dev_id = frame[gw_addr_off];  // ACK에서 DevType = gw_addr_offset 위치
  } else {
    dev_id = frame[d_off];        // QUERY 또는 swap 없는 ACK: dev_id_offset 위치
  }

  sub1 = (s1_off < frame.size()) ? frame[s1_off] : 0;
  sub2 = (s2_off < frame.size()) ? frame[s2_off] : 0;
  return true;
}

bool UniversalProtocolEngine::buildQueryPacket(uint8_t dev_id, uint8_t sub1,
                                               uint8_t sub2,
                                               StaticPacket &out) const {
  VendorProfileDescriptor desc = activeProfile();

  uint8_t stx = desc.stx;
  uint8_t etx = desc.etx;
  ChecksumAlgo algo = desc.cs_algo;
  uint8_t op_off = desc.opcode_offset;
  uint8_t q_op = desc.query_op;
  uint8_t d_off = desc.dev_id_offset;
  uint8_t s1_off = desc.sub1_offset;
  uint8_t s2_off = desc.sub2_offset;

  if (isAutoProfile(desc)) {
    auto ad = g_auto_probing_engine.getDescriptor();
    stx = ad.stx;
    etx = ad.etx;
    algo = ad.checksum_algo;
    op_off = ad.opcode_offset;
    q_op = ad.query_opcode;
    if (ad.offsets_locked) {
      d_off = ad.dev_id_offset;
      s1_off = ad.sub1_offset;
      s2_off = ad.sub2_offset;
    }

    uint8_t pkt_len = (ad.offsets_locked && ad.learned_query_len >= 5)
                          ? ad.learned_query_len : 11;
    out.channel_id = 1;
    out.length = pkt_len;
    out.data.fill(0);
    out.data[0] = stx;
    if (ad.has_len_field && ad.len_offset < pkt_len) {
      out.data[ad.len_offset] = pkt_len;
    }

    if (ad.gw_addr_offset < pkt_len) {
      out.data[ad.gw_addr_offset] = ad.gw_addr;
    }

    if (d_off < pkt_len) out.data[d_off] = dev_id;
    if (op_off < pkt_len) out.data[op_off] = q_op;
    if (s1_off > 0 && s1_off < pkt_len) out.data[s1_off] = sub1;
    if (s2_off > 0 && s2_off < pkt_len) out.data[s2_off] = sub2;

    if (pkt_len >= 3) {
      out.data[pkt_len - 2] = g_auto_probing_engine.calculateChecksum(algo, out.data.data(), pkt_len);
      out.data[pkt_len - 1] = etx;
    }
    return true;
  }

  uint8_t pkt_len = (desc.learned_query_len >= 3 && desc.learned_query_len <= 64)
                        ? desc.learned_query_len : 11;
  out.channel_id = 1;
  out.length = pkt_len;
  out.data.fill(0);
  out.data[0] = stx;
  if (desc.has_len_field && desc.len_offset < pkt_len) {
    out.data[desc.len_offset] = pkt_len;
  }

  if (desc.gw_addr_offset < pkt_len) {
    out.data[desc.gw_addr_offset] = (desc.gw_addr != 0) ? desc.gw_addr : 0x01;
  }

  if (d_off < pkt_len) out.data[d_off] = dev_id;
  if (op_off < pkt_len) out.data[op_off] = q_op;
  if (s1_off > 0 && s1_off < pkt_len) out.data[s1_off] = sub1;
  if (s2_off > 0 && s2_off < pkt_len) out.data[s2_off] = sub2;

  if (pkt_len >= 3) {
    out.data[pkt_len - 2] = g_auto_probing_engine.calculateChecksum(algo, out.data.data(), pkt_len);
    out.data[pkt_len - 1] = etx;
  }
  return true;
}

uint8_t UniversalProtocolEngine::calculateChecksum(const uint8_t *data, size_t len) const {
  VendorProfileDescriptor desc = activeProfile();
  ChecksumAlgo algo = isAutoProfile(desc)
                          ? g_auto_probing_engine.getDescriptor().checksum_algo
                          : desc.cs_algo;
  return g_auto_probing_engine.calculateChecksum(algo, data, len);
}

uint8_t UniversalProtocolEngine::getStx() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().stx : desc.stx;
}

uint8_t UniversalProtocolEngine::getEtx() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().etx : desc.etx;
}

uint8_t UniversalProtocolEngine::getMinPacketLen() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().min_len : desc.min_len;
}

uint8_t UniversalProtocolEngine::getMaxPacketLen() const {
  VendorProfileDescriptor desc = activeProfile();
  return isAutoProfile(desc) ? g_auto_probing_engine.getDescriptor().max_len : desc.max_len;
}

int UniversalProtocolEngine::extractPacketLength(const uint8_t *stream, size_t stream_len, size_t stx_idx) const {
  if (stx_idx >= stream_len)
    return -1;

  VendorProfileDescriptor desc = activeProfile();
  bool is_auto = isAutoProfile(desc);
  AutoProbeDescriptor ad = is_auto ? g_auto_probing_engine.getDescriptor() : AutoProbeDescriptor{};

  uint8_t stx = is_auto ? ad.stx : desc.stx;
  uint8_t etx = is_auto ? ad.etx : desc.etx;
  uint8_t min_len = is_auto ? ad.min_len : desc.min_len;
  uint8_t max_len = is_auto ? ad.max_len : desc.max_len;
  ChecksumAlgo algo = is_auto ? ad.checksum_algo : desc.cs_algo;

  if (stream[stx_idx] != stx)
    return -1;

  uint8_t safe_min = std::max<uint8_t>(min_len, 3);
  uint8_t safe_max = (max_len >= safe_min && max_len <= 64) ? max_len : 64;

  for (size_t l = safe_min; l <= safe_max; ++l) {
    if (stx_idx + l > stream_len) {
      return 0; // 아직 패킷 바이트가 덜 들어옴 (추가 수신 대기)
    }
    if (stream[stx_idx + l - 1] == etx) {
      span<const uint8_t> cand(&stream[stx_idx], l);
      if (checkFramingPure(cand, stx, etx, safe_min, safe_max, algo)) {
        return static_cast<int>(l); // STX + ETX + Checksum 3박자 통과!
      }
    }
  }

  if (stx_idx + safe_max <= stream_len) {
    return -1; // safe_max까지 유효한 프레임 없음 -> 다음 바이트로 이동
  }
  return 0; // 추가 수신 대기
}


void WallpadParserFactory::init() {
  ProfileRepository::init();
}

UniversalProtocolEngine *WallpadParserFactory::getActiveParser() {
  return &s_universal_engine;
}

bool WallpadParserFactory::setProfile(uint8_t index) {
  return ProfileRepository::setActiveProfileIndex(index);
}

// ============================================================================
// Section from src/Parser/PollingTargetRegistry.cpp
// ============================================================================

namespace {
constexpr uint32_t EXPIRED_TARGET_EVICTION_TIMEOUT_MS = 600000;
} // namespace

PollingTargetRegistry g_polling_targets;

void PollingTargetRegistry::registerOrTouch(uint8_t ch, uint8_t dev_id,
                                            uint8_t sub1, uint8_t sub2,
                                            const uint8_t *raw_pkt,
                                            size_t raw_len) {
  // CH2 and CH3 (Wallpad/App) and CH5 (EW11 sniffing) are allowed target sources
  if (ch != 2 && ch != 3 && ch != 5) {
    return;
  }

  // 0x2A (신발장 서브 패널 / 원격검침)는 폴링 대상이 아니므로 타겟 레지스트리 등록 원천 차단
  if (dev_id == 0x2A) {
    return;
  }

  uint32_t now = millis();
  bool is_new_entry = false;
  {
    CriticalSectionLocker lock(&_mux);

    for (size_t i = 0; i < _count; ++i) {
      bool match = false;
      if (dev_id != 0 || sub1 != 0 || sub2 != 0) {
        if (_entries[i].dev_id != 0 || _entries[i].sub1 != 0 || _entries[i].sub2 != 0) {
          match = (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
                   _entries[i].sub2 == sub2);
        } else if (raw_pkt && raw_len > 0 && _entries[i].raw_query_len == raw_len) {
          match = (memcmp(_entries[i].raw_query_data.data(), raw_pkt, raw_len) == 0);
        }
      } else if (raw_pkt && raw_len > 0 && _entries[i].raw_query_len == raw_len) {
        if (_entries[i].dev_id == 0 && _entries[i].sub1 == 0 && _entries[i].sub2 == 0) {
          match = (memcmp(_entries[i].raw_query_data.data(), raw_pkt, raw_len) == 0);
        }
      }
      if (match) {
        if (_entries[i].last_requested_ms > 0 && now > _entries[i].last_requested_ms) {
          uint32_t delta = now - _entries[i].last_requested_ms;
          if (delta >= 100 && delta <= 10000) {
            if (_entries[i].last_interval_ms == 0) {
              _entries[i].last_interval_ms = delta;
            } else {
              _entries[i].last_interval_ms = (_entries[i].last_interval_ms * 3 + delta) / 4;
            }
          }
        }
        _entries[i].last_requested_ms = now;
        if (ch < 8)
          _entries[i].source_channels |= (1 << ch);
        if (_entries[i].hit_count < 65535)
          _entries[i].hit_count++;
        _entries[i].is_active = true;
        _entries[i].is_verified = true;
        if (_entries[i].dev_id == 0 && (dev_id != 0 || sub1 != 0 || sub2 != 0)) {
          _entries[i].dev_id = dev_id;
          _entries[i].sub1 = sub1;
          _entries[i].sub2 = sub2;
        }
        if (raw_pkt && raw_len > 0 && raw_len <= 64) {
          _entries[i].raw_query_len = static_cast<uint8_t>(raw_len);
          memcpy(_entries[i].raw_query_data.data(), raw_pkt, raw_len);
        }
        return;
      }
    }

    if (_count < MAX_TARGETS) {
      _entries[_count].dev_id = dev_id;
      _entries[_count].sub1 = sub1;
      _entries[_count].sub2 = sub2;
      _entries[_count].last_requested_ms = now;
      _entries[_count].last_interval_ms = 0;
      _entries[_count].source_channels = (ch < 8) ? (1 << ch) : 0;
      _entries[_count].hit_count = 1;
      _entries[_count].is_active = true;
      _entries[_count].is_verified = true;
      _entries[_count].restored_ms = 0;
      if (raw_pkt && raw_len > 0 && raw_len <= 64) {
        _entries[_count].raw_query_len = static_cast<uint8_t>(raw_len);
        memcpy(_entries[_count].raw_query_data.data(), raw_pkt, raw_len);
      } else {
        _entries[_count].raw_query_len = 0;
      }
      _count++;
      is_new_entry = true;
    }
  }

  if (is_new_entry) {
    g_warm_cache_dirty.store(true, std::memory_order_release);
    g_warm_cache_dirty_ms.store(now, std::memory_order_release);
  }
}

void PollingTargetRegistry::updateResponse(const uint8_t *query_pkt, size_t query_len,
                                           const uint8_t *ack_pkt, size_t ack_len) {
  if (!query_pkt || query_len == 0 || !ack_pkt || ack_len == 0)
    return;
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].raw_query_len == query_len &&
        memcmp(_entries[i].raw_query_data.data(), query_pkt, query_len) == 0) {
      _entries[i].raw_ack_len = std::min<uint8_t>(ack_len, 64);
      memcpy(_entries[i].raw_ack_data.data(), ack_pkt, _entries[i].raw_ack_len);
      _entries[i].is_verified = true;
      return;
    }
  }
}

void PollingTargetRegistry::reindexWithOffsets(uint8_t dev_id_offset, uint8_t sub1_offset,
                                               uint8_t sub2_offset) {
  CriticalSectionLocker lock(&_mux);
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);

  for (size_t i = 0; i < _count; ++i) {
    // 월패드(CH2, CH3) 유래 타겟이 아닌 경우(CH5 EW11/Modbus 등) 오프셋 재계산 제외
    if ((_entries[i].source_channels & CH23_MASK) == 0) {
      continue;
    }

    if (_entries[i].raw_query_len > dev_id_offset) {
      _entries[i].dev_id = _entries[i].raw_query_data[dev_id_offset];
    }
    if (sub1_offset > 0 && _entries[i].raw_query_len > sub1_offset) {
      _entries[i].sub1 = _entries[i].raw_query_data[sub1_offset];
    } else {
      _entries[i].sub1 = 0;
    }
    if (sub2_offset > 0 && _entries[i].raw_query_len > sub2_offset) {
      _entries[i].sub2 = _entries[i].raw_query_data[sub2_offset];
    } else {
      _entries[i].sub2 = 0;
    }
  }
}

void PollingTargetRegistry::sweepExpired(uint32_t stale_timeout_ms) {
  uint32_t now = millis();
  CriticalSectionLocker lock(&_mux);
  size_t write_idx = 0;
  for (size_t i = 0; i < _count; ++i) {
    uint32_t elapsed = now - _entries[i].last_requested_ms;
    if (elapsed > stale_timeout_ms) {
      _entries[i].is_active = false;
    }
    if (elapsed <= EXPIRED_TARGET_EVICTION_TIMEOUT_MS) {
      if (write_idx != i) {
        _entries[write_idx] = _entries[i];
      }
      write_idx++;
    }
  }
  _count = write_idx;
}

size_t PollingTargetRegistry::getActiveTargets(PollingTargetEntry *out_targets,
                                              size_t max_count) {
  if (!out_targets || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active) {
      out_targets[written++] = _entries[i];
    }
  }
  return written;
}

size_t PollingTargetRegistry::getActiveCandidates(PollingCandidate *out_cands,
                                                 size_t max_count) {
  if (!out_cands || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active) {
      out_cands[written].dev_id = _entries[i].dev_id;
      out_cands[written].sub1 = _entries[i].sub1;
      out_cands[written].sub2 = _entries[i].sub2;
      out_cands[written].source_channels = _entries[i].source_channels;
      out_cands[written].raw_ack_len = _entries[i].raw_ack_len;
      out_cands[written].raw_query_len = _entries[i].raw_query_len;
      out_cands[written].entry_idx = static_cast<uint8_t>(i);
      written++;
    }
  }
  return written;
}

bool PollingTargetRegistry::getQueryData(uint8_t entry_idx,
                                        const uint8_t *&out_data,
                                        uint8_t &out_len) const {
  CriticalSectionLocker lock(&_mux);
  if (entry_idx >= _count || !_entries[entry_idx].is_active) {
    out_data = nullptr;
    out_len = 0;
    return false;
  }
  out_len = _entries[entry_idx].raw_query_len;
  out_data = _entries[entry_idx].raw_query_data.data();
  return true;
}

size_t PollingTargetRegistry::activeCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t active = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active)
      active++;
  }
  return active;
}

size_t PollingTargetRegistry::totalCount() const {
  CriticalSectionLocker lock(&_mux);
  return _count;
}

size_t PollingTargetRegistry::ackedCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t acked = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].raw_ack_len > 0)
      acked++;
  }
  return acked;
}

bool PollingTargetRegistry::getEntry(size_t index, PollingTargetEntry &out) const {
  CriticalSectionLocker lock(&_mux);
  if (index >= _count)
    return false;
  out = _entries[index];
  return true;
}

void PollingTargetRegistry::resetHits() {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    _entries[i].hit_count = 0;
  }
}

void PollingTargetRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
}

void PollingTargetRegistry::loadFromWarmCache(const RtcWarmCacheEntry *entries, size_t count, uint32_t now_ms) {
  if (!entries || count == 0)
    return;
  CriticalSectionLocker lock(&_mux);
  size_t loaded = 0;
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);

  for (size_t i = 0; i < count && loaded < MAX_TARGETS; ++i) {
    // Exclude invalid entries, CH5-only entries, or EW11/remote dev_ids (0x2A)
    if (entries[i].dev_id == 0 || entries[i].dev_id == 0x2A) continue;
    if (entries[i].source_channels != 0 && (entries[i].source_channels & CH23_MASK) == 0) {
      continue; // Not from CH2 or CH3
    }

    _entries[loaded].dev_id = entries[i].dev_id;
    _entries[loaded].sub1 = entries[i].sub1;
    _entries[loaded].sub2 = entries[i].sub2;
    _entries[loaded].source_channels = entries[i].source_channels & CH23_MASK;
    _entries[loaded].raw_query_len = std::min<uint8_t>(entries[i].raw_len, 64);
    if (_entries[loaded].raw_query_len > 0) {
      memcpy(_entries[loaded].raw_query_data.data(), entries[i].raw_query, _entries[loaded].raw_query_len);
    }
    _entries[loaded].last_requested_ms = now_ms;
    _entries[loaded].last_interval_ms = 1000;
    _entries[loaded].hit_count = 0;
    _entries[loaded].is_active = true;
    _entries[loaded].is_verified = false; // Initially unverified until live bus confirmation
    _entries[loaded].restored_ms = now_ms;
    loaded++;
  }
  _count = loaded;
}

size_t PollingTargetRegistry::getWarmCacheEntries(RtcWarmCacheEntry *out_entries, size_t max_count) const {
  if (!out_entries || max_count == 0)
    return 0;
  CriticalSectionLocker lock(&_mux);
  size_t written = 0;
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);

  for (size_t i = 0; i < _count && written < max_count; ++i) {
    if (_entries[i].is_active && (_entries[i].source_channels & CH23_MASK) != 0 && _entries[i].dev_id != 0x2A) {
      out_entries[written].dev_id = _entries[i].dev_id;
      out_entries[written].sub1 = _entries[i].sub1;
      out_entries[written].sub2 = _entries[i].sub2;
      out_entries[written].source_channels = _entries[i].source_channels & CH23_MASK;
      size_t copy_sz = std::min<size_t>(_entries[i].raw_query_len, sizeof(out_entries[written].raw_query));
      out_entries[written].raw_len = static_cast<uint8_t>(copy_sz);
      if (copy_sz > 0) {
        memcpy(out_entries[written].raw_query, _entries[i].raw_query_data.data(), copy_sz);
      } else {
        memset(out_entries[written].raw_query, 0, sizeof(out_entries[written].raw_query));
      }
      written++;
    }
  }
  return written;
}

void PollingTargetRegistry::markVerified(uint8_t dev_id, uint8_t sub1, uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 && _entries[i].sub2 == sub2) {
      _entries[i].is_verified = true;
      return;
    }
  }
}

size_t PollingTargetRegistry::verifiedCount() const {
  CriticalSectionLocker lock(&_mux);
  size_t v = 0;
  for (size_t i = 0; i < _count; ++i) {
    if (_entries[i].is_active && _entries[i].is_verified)
      v++;
  }
  return v;
}


// ============================================================================
// Section from src/Parser/ProfileRepository.cpp
// ============================================================================

static constexpr VendorProfileDescriptor s_default_profiles[ProfileRepository::MAX_PROFILES] = {
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
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  for (size_t i = 0; i < total_tgts; ++i) {
    PollingTargetEntry entry;
    if (g_polling_targets.getEntry(i, entry) && entry.raw_query_len > 0) {
      if ((entry.source_channels & CH23_MASK) == 0) continue;
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


// ============================================================================
// Section from src/Parser/AutoProbingEngine.cpp
// ============================================================================

AutoProbingEngine g_auto_probing_engine;

AutoProbingEngine::AutoProbingEngine() {
  reset();
}

const char *AutoProbingEngine::getAlgoName(ChecksumAlgo algo) {
  switch (algo) {
  case ChecksumAlgo::XOR_ALL:        return "XOR (0..[N-3])";
  case ChecksumAlgo::XOR_NO_STX:     return "XOR (1..[N-3])";
  case ChecksumAlgo::SUM_ALL:        return "SUM (0..[N-3])";
  case ChecksumAlgo::SUM_NO_STX:     return "SUM (1..[N-3])";
  case ChecksumAlgo::TWOS_COMPLEMENT:return "2's Complement (1..[N-3])";
  case ChecksumAlgo::ONES_COMPLEMENT:return "1's Complement (0..[N-3])";
  case ChecksumAlgo::CRC8_MAXIM:     return "CRC-8 (Maxim/0x31)";
  case ChecksumAlgo::NONE:           return "None (Pure Framing)";
  default:                           return "Learning...";
  }
}

uint8_t AutoProbingEngine::calculateChecksum(ChecksumAlgo algo, const uint8_t *data, size_t len) const {
  if (!data || len < 3)
    return 0;
  switch (algo) {
  case ChecksumAlgo::XOR_ALL: {
    uint8_t cs = 0;
    for (size_t i = 0; i < len - 2; ++i)
      cs ^= data[i];
    return cs;
  }
  case ChecksumAlgo::XOR_NO_STX: {
    uint8_t cs = 0;
    for (size_t i = 1; i < len - 2; ++i)
      cs ^= data[i];
    return cs;
  }
  case ChecksumAlgo::SUM_ALL: {
    uint8_t cs = 0;
    for (size_t i = 0; i < len - 2; ++i)
      cs += data[i];
    return cs;
  }
  case ChecksumAlgo::SUM_NO_STX: {
    uint8_t cs = 0;
    for (size_t i = 1; i < len - 2; ++i)
      cs += data[i];
    return cs;
  }
  case ChecksumAlgo::TWOS_COMPLEMENT: {
    uint8_t sum = 0;
    for (size_t i = 1; i < len - 2; ++i)
      sum += data[i];
    return (0x100 - sum) & 0xFF;
  }
  case ChecksumAlgo::ONES_COMPLEMENT: {
    uint8_t sum = 0;
    for (size_t i = 0; i < len - 2; ++i)
      sum += data[i];
    return (~sum) & 0xFF;
  }
  case ChecksumAlgo::CRC8_MAXIM: {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len - 2; ++i) {
      crc ^= data[i];
      for (int b = 0; b < 8; ++b) {
        if (crc & 0x80)
          crc = (crc << 1) ^ 0x31;
        else
          crc <<= 1;
      }
    }
    return crc;
  }
  case ChecksumAlgo::NONE:
  default:
    return 0;
  }
}

void AutoProbingEngine::initFromNvs() {
  VendorProfileDescriptor prof;
  if (ProfileRepository::getActiveProfile(prof)) {
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
        if (_desc.gw_addr_offset == _desc.dev_id_offset || _desc.gw_addr_offset != 2) {
          _desc.gw_addr_offset = 2;
          _desc.gw_addr = (prof.gw_addr != 0) ? prof.gw_addr : 0x01;
        }
        if (_desc.sub1_offset == 2 && _desc.sub2_offset > 2) {
          _desc.sub1_offset = _desc.sub2_offset;
        }
      }
      _desc.learned_query_len = (prof.learned_query_len >= 3) ? prof.learned_query_len : 11;
      _desc.len_offset = prof.len_offset;
      _desc.has_len_field = (prof.has_len_field != 0);
      _desc.seq_offset = prof.seq_offset;
      _desc.has_seq_counter = (prof.seq_offset != 0xFF);
      _desc.ack_flag_offset = prof.ack_flag_offset;
      _desc.ctrl_len_cnt = prof.ctrl_len_cnt;
      memcpy(_desc.learned_ctrl_lens, prof.learned_ctrl_lens, sizeof(_desc.learned_ctrl_lens));
      if (_desc.control_seen && _desc.ctrl_len_cnt == 0) {
        _desc.learned_ctrl_lens[0] = _desc.learned_query_len;
        _desc.ctrl_len_cnt = 1;
      }
      int max_hdr = std::max({_desc.opcode_offset, _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset});
      if (_desc.gw_addr_offset > max_hdr) max_hdr = _desc.gw_addr_offset;
      if (_desc.len_offset != 0xFF && _desc.len_offset > max_hdr) max_hdr = _desc.len_offset;
      if (_desc.seq_offset != 0xFF && _desc.seq_offset > max_hdr) max_hdr = _desc.seq_offset;
      if (_desc.ack_flag_offset != 0xFF && _desc.ack_flag_offset > max_hdr) max_hdr = _desc.ack_flag_offset;
      _desc.payload_offset = static_cast<uint8_t>(std::max(max_hdr + 1, 8));

      _desc.offsets_locked = true;
      _desc.opcodes_locked = true;
    }

    _desc.is_locked = true;
    _consecutive_mismatches = 0;
    snprintf(_desc.description, sizeof(_desc.description),
             "Restored: %s (STX 0x%02X ETX 0x%02X / %s)",
             prof.name, prof.stx, prof.etx, getAlgoName(prof.cs_algo));
  }
}

void AutoProbingEngine::feedFrame(span<const uint8_t> raw_frame) {
  if (raw_frame.size() < 3 || raw_frame.size() > 64)
    return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    _desc.tested_packets++;

    uint8_t stx = raw_frame[0];
    uint8_t etx = raw_frame[raw_frame.size() - 1];
    uint8_t actual_cs = raw_frame[raw_frame.size() - 2];

    _stx_counts[stx]++;
    _etx_counts[etx]++;

    if (_desc.is_locked) {
      bool ok = false;
      if (stx == _desc.stx && etx == _desc.etx) {
        if (_desc.checksum_algo == ChecksumAlgo::NONE ||
            calculateChecksum(_desc.checksum_algo, raw_frame.data(), raw_frame.size()) == actual_cs) {
          _desc.matched_packets++;
          _consecutive_mismatches = 0;
          ok = true;
        }
      }
      if (!ok) {
        _consecutive_mismatches++;
        if (_consecutive_mismatches >= 5) {
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
      }
      return; // ★ lock이 여기서 소멸 → 스핀락 해제 후 return
    }

    uint8_t best_stx = 0xF7, best_etx = 0xEE;
    uint16_t max_stx_cnt = 0, max_etx_cnt = 0;
    for (int i = 0; i < 256; ++i) {
      if (_stx_counts[i] > max_stx_cnt) {
        max_stx_cnt = _stx_counts[i];
        best_stx = static_cast<uint8_t>(i);
      }
      if (_etx_counts[i] > max_etx_cnt) {
        max_etx_cnt = _etx_counts[i];
        best_etx = static_cast<uint8_t>(i);
      }
    }

    ChecksumAlgo matched_this_frame = ChecksumAlgo::UNKNOWN;
    for (uint8_t a = 1; a <= 7; ++a) {
      ChecksumAlgo algo = static_cast<ChecksumAlgo>(a);
      if (calculateChecksum(algo, raw_frame.data(), raw_frame.size()) == actual_cs) {
        _algo_matches[a]++;
        matched_this_frame = algo;
        break;
      }
    }

    if (matched_this_frame != ChecksumAlgo::UNKNOWN) {
      if (matched_this_frame == _candidate_algo) {
        _consecutive_matches++;
        if (_consecutive_matches >= 10 && max_stx_cnt >= 5 && max_etx_cnt >= 5) {
          _desc.is_locked = true;
          _desc.stx = best_stx;
          _desc.etx = best_etx;
          _desc.min_len = 3;
          _desc.max_len = 64;
          _desc.checksum_algo = matched_this_frame;
          _desc.matched_packets = _desc.tested_packets;
          _consecutive_mismatches = 0;
          snprintf(_desc.description, sizeof(_desc.description),
                   "Locked: STX 0x%02X ETX 0x%02X (%s)", best_stx, best_etx,
                   getAlgoName(matched_this_frame));
          should_sync = true;
          desc_to_sync = _desc;
        }
      } else {
        _candidate_algo = matched_this_frame;
        _consecutive_matches = 1;
      }
    }
  } // ★ CriticalSectionLocker 소멸 → 스핀락 완전 해제

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

void AutoProbingEngine::feedOpcodePair(span<const uint8_t> /*req*/, span<const uint8_t> /*ack*/) {
  // Early opcode locking removed to eliminate race conditions with frame length fields.
  // Full cross-device matrix analysis in analyzeCacheMatrix() is the sole authoritative opcode evaluator.
}


void AutoProbingEngine::feedControlFrame(span<const uint8_t> ctrl_frame) {
  if (ctrl_frame.size() < 5) return;

  bool should_sync = false;
  AutoProbeDescriptor desc_to_sync;

  {
    CriticalSectionLocker lock(&_mux);
    if (ctrl_frame.size() <= _desc.opcode_offset) return;

    uint8_t op = ctrl_frame[_desc.opcode_offset];
    // QRY나 ACK가 아닌 경우에만 제어 오프코드로 처리
    if (op == _desc.query_opcode || op == _desc.ack_opcode) return;

    if (!_desc.control_seen || _desc.control_opcode != op) {
      _desc.control_opcode = op;
      _desc.control_seen = true;
      _desc.opcodes_locked = true;

      uint8_t flen = static_cast<uint8_t>(ctrl_frame.size());
      bool len_found = false;
      for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
        if (_desc.learned_ctrl_lens[i] == flen) {
          len_found = true;
          break;
        }
      }
      if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
        _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = flen;
      }

      if (_desc.is_locked) {
        should_sync = true;
        desc_to_sync = _desc;
      }
    } else {
      // 이미 opcode는 학습되었으나 길이가 추가되는 경우 (예: 11바이트, 13바이트 등)
      uint8_t flen = static_cast<uint8_t>(ctrl_frame.size());
      bool len_found = false;
      for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
        if (_desc.learned_ctrl_lens[i] == flen) {
          len_found = true;
          break;
        }
      }
      if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
        _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = flen;
        if (_desc.is_locked) {
          should_sync = true;
          desc_to_sync = _desc;
        }
      }
    }
  }

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
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
  AutoProbeDescriptor desc_to_sync;
  {
    CriticalSectionLocker lock(&_mux);
    _desc.control_opcode = ctrl_op;
    _desc.control_seen = true;
    _desc.opcodes_locked = true;

    bool len_found = false;
    for (uint8_t i = 0; i < _desc.ctrl_len_cnt; ++i) {
      if (_desc.learned_ctrl_lens[i] == ctrl_len) {
        len_found = true;
        break;
      }
    }
    if (!len_found && _desc.ctrl_len_cnt < sizeof(_desc.learned_ctrl_lens)) {
      _desc.learned_ctrl_lens[_desc.ctrl_len_cnt++] = ctrl_len;
    }

    if (_desc.is_locked) {
      should_sync = true;
      desc_to_sync = _desc;
    }
  }

  if (should_sync) {
    ProfileRepository::syncAutoProfileToNvs(desc_to_sync);
  }
}

bool AutoProbingEngine::analyzeCacheMatrix() {
  if (g_polling_targets.ackedCount() < 2 && g_device_repo.getOnlineCount() < 2) {
    return false;
  }

  struct PktPair {
    StaticPacket q;
    StaticPacket r;
  };
  std::vector<PktPair> pairs;
  pairs.reserve(std::min<size_t>(g_polling_targets.totalCount(), 32));

  size_t target_count = g_polling_targets.totalCount();
  constexpr uint8_t CH23_MASK = (1 << 2) | (1 << 3);
  for (size_t i = 0; i < target_count; ++i) {
    PollingTargetEntry target;
    if (!g_polling_targets.getEntry(i, target))
      continue;
    if (!target.is_active || target.raw_query_len < 4)
      continue;

    // 월패드 프로토콜 분석은 오직 CH2(메인 월패드), CH3(서브 월패드)에서 유래한 쿼리만 대상으로 함
    if ((target.source_channels & CH23_MASK) == 0) {
      continue;
    }

    if (target.raw_ack_len >= 4) {
      PktPair pair;
      pair.q.length = target.raw_query_len;
      memcpy(pair.q.data.data(), target.raw_query_data.data(), target.raw_query_len);
      pair.r.length = target.raw_ack_len;
      memcpy(pair.r.data.data(), target.raw_ack_data.data(), target.raw_ack_len);
      pairs.push_back(pair);
    } else {
      const DeviceStateEntry *dev = g_device_repo.find(target.dev_id, target.sub1, target.sub2);
      if (dev && dev->is_online && dev->last_ack_len >= 4) {
        PktPair pair;
        pair.q.length = target.raw_query_len;
        memcpy(pair.q.data.data(), target.raw_query_data.data(), target.raw_query_len);
        pair.r.length = dev->last_ack_len;
        memcpy(pair.r.data.data(), dev->last_ack_data.data(), dev->last_ack_len);
        pairs.push_back(pair);
      }
    }
  }

  if (pairs.size() < 2)
    return false;

  size_t N = pairs.size();
  size_t min_common_len = 256;
  for (const auto &p : pairs) {
    if (p.q.length < min_common_len) min_common_len = p.q.length;
    if (p.r.length < min_common_len) min_common_len = p.r.length;
  }
  if (min_common_len < 4)
    return false;


  int len_idx = -1;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    bool exact_match = true;
    for (size_t m = 0; m < N; ++m) {
      if (pairs[m].q.data[k] != pairs[m].q.length || pairs[m].r.data[k] != pairs[m].r.length) {
        exact_match = false;
        break;
      }
    }
    if (exact_match) {
      len_idx = static_cast<int>(k);
      break;
    }
    for (uint8_t delta : {2, 3, 4, 5}) {
      bool delta_match = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].q.data[k] != (pairs[m].q.length - delta) ||
            pairs[m].r.data[k] != (pairs[m].r.length - delta)) {
          delta_match = false;
          break;
        }
      }
      if (delta_match) {
        len_idx = static_cast<int>(k);
        break;
      }
    }
    if (len_idx >= 0) break;
  }

  int opcode_idx = -1;
  uint8_t learned_q_op = 0, learned_ack_op = 0;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    if (static_cast<int>(k) == len_idx) continue;
    uint8_t cq = pairs[0].q.data[k];
    uint8_t cr = pairs[0].r.data[k];
    if (cq == cr) continue;
    bool all_match = true;
    for (size_t m = 1; m < N; ++m) {
      if (pairs[m].q.data[k] != cq || pairs[m].r.data[k] != cr) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      opcode_idx = static_cast<int>(k);
      learned_q_op = cq;
      learned_ack_op = cr;
      break;
    }
  }

  int swap_i = -1, swap_j = -1;
  int promoted_dev_idx = -1;
  int master_gw_idx = -1;
  for (size_t i = 1; i < min_common_len - 1; ++i) {
    if (static_cast<int>(i) == len_idx || static_cast<int>(i) == opcode_idx) continue;
    for (size_t j = i + 1; j < min_common_len - 1; ++j) {
      if (static_cast<int>(j) == len_idx || static_cast<int>(j) == opcode_idx) continue;
      bool is_cross = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].q.data[i] != pairs[m].r.data[j] ||
            pairs[m].q.data[j] != pairs[m].r.data[i] ||
            pairs[m].q.data[i] == pairs[m].q.data[j]) {
          is_cross = false;
          break;
        }
      }
      if (is_cross) {
        swap_i = static_cast<int>(i);
        swap_j = static_cast<int>(j);
        std::set<uint8_t> set_i, set_j;
        for (size_t m = 0; m < N; ++m) {
          set_i.insert(pairs[m].q.data[i]);
          set_j.insert(pairs[m].q.data[j]);
        }
        if (set_i.size() == 1 && set_j.size() > 1) {
          master_gw_idx = static_cast<int>(i);
          promoted_dev_idx = static_cast<int>(j);
        } else if (set_j.size() == 1 && set_i.size() > 1) {
          master_gw_idx = static_cast<int>(j);
          promoted_dev_idx = static_cast<int>(i);
        } else {
          master_gw_idx = static_cast<int>(i);
          promoted_dev_idx = static_cast<int>(j);
        }
        break;
      }
    }
    if (swap_i >= 0) break;
  }

  int seq_idx = -1;
  if (N >= 4) {
    for (size_t k = 1; k < min_common_len - 1; ++k) {
      int ik = static_cast<int>(k);
      if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j || ik == promoted_dev_idx)
        continue;
      std::set<uint8_t> distinct_vals;
      for (size_t m = 0; m < N; ++m) distinct_vals.insert(pairs[m].q.data[k]);
      if (distinct_vals.size() < 3)
        continue;

      bool is_inc = true;
      for (size_t m = 0; m < N - 1; ++m) {
        uint8_t diff = static_cast<uint8_t>((pairs[m + 1].q.data[k] - pairs[m].q.data[k]) & 0xFF);
        if (diff != 1) {
          is_inc = false;
          break;
        }
      }
      if (is_inc) {
        seq_idx = ik;
        break;
      }
    }
  }

  int sub_cmd_idx = -1;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j || ik == seq_idx || ik == promoted_dev_idx)
      continue;
    bool eq = true;
    for (size_t m = 0; m < N; ++m) {
      if (pairs[m].q.data[k] != pairs[m].r.data[k]) {
        eq = false;
        break;
      }
    }
    if (eq) {
      std::set<uint8_t> vals;
      for (size_t m = 0; m < N; ++m) vals.insert(pairs[m].q.data[k]);
      if (vals.size() == 1) {
        if (master_gw_idx < 0) {
          master_gw_idx = ik;
        }
      } else if (promoted_dev_idx >= 0) {
        std::map<uint8_t, uint8_t> dep_map;
        bool pure_func = true;
        for (size_t m = 0; m < N; ++m) {
          uint8_t dt = pairs[m].q.data[promoted_dev_idx];
          uint8_t sc = pairs[m].q.data[k];
          if (dep_map.count(dt) && dep_map[dt] != sc) {
            pure_func = false;
            break;
          }
          dep_map[dt] = sc;
        }
        if (pure_func && dep_map.size() > 1) {
          sub_cmd_idx = ik;
          break;
        }
      }
    }
  }

  std::vector<size_t> candidate_cols;
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j ||
        ik == seq_idx || ik == sub_cmd_idx || ik == promoted_dev_idx) {
      continue;
    }
    std::set<uint8_t> vals;
    for (size_t m = 0; m < N; ++m) vals.insert(pairs[m].q.data[k]);
    if (vals.size() >= 2) candidate_cols.push_back(k);
  }

  int dev_type_idx = promoted_dev_idx;
  int sub_id_idx = -1;
  size_t min_unique = std::max<size_t>(2, N * 8 / 10);

  if (dev_type_idx >= 0) {
    for (size_t cand : candidate_cols) {
      std::set<uint16_t> combo_keys;
      for (size_t m = 0; m < N; ++m) {
        uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[dev_type_idx]) << 8) | pairs[m].q.data[cand];
        combo_keys.insert(key);
      }
      if (combo_keys.size() >= min_unique) {
        sub_id_idx = static_cast<int>(cand);
        break;
      }
    }
  } else {
    for (size_t cand : candidate_cols) {
      std::map<uint8_t, size_t> len_map;
      bool consistent = true;
      for (size_t m = 0; m < N; ++m) {
        uint8_t val = pairs[m].q.data[cand];
        size_t r_len = pairs[m].r.length;
        if (len_map.count(val) && len_map[val] != r_len) {
          consistent = false;
          break;
        }
        len_map[val] = r_len;
      }
      if (consistent && len_map.size() > 1) {
        std::set<size_t> distinct_lens;
        for (auto &kv : len_map) distinct_lens.insert(kv.second);
        if (distinct_lens.size() > 1) {
          dev_type_idx = static_cast<int>(cand);
          break;
        }
      }
    }

    if (dev_type_idx >= 0) {
      for (size_t cand : candidate_cols) {
        if (static_cast<int>(cand) == dev_type_idx) continue;
        std::set<uint16_t> combo_keys;
        for (size_t m = 0; m < N; ++m) {
          uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[dev_type_idx]) << 8) | pairs[m].q.data[cand];
          combo_keys.insert(key);
        }
        if (combo_keys.size() >= min_unique) {
          sub_id_idx = static_cast<int>(cand);
          break;
        }
      }
    } else {
      for (size_t cand1 : candidate_cols) {
        for (size_t cand2 : candidate_cols) {
          if (cand1 == cand2) continue;
          std::set<uint16_t> combo_keys;
          for (size_t m = 0; m < N; ++m) {
            uint16_t key = (static_cast<uint16_t>(pairs[m].q.data[cand1]) << 8) | pairs[m].q.data[cand2];
            combo_keys.insert(key);
          }
          if (combo_keys.size() >= min_unique) {
            std::set<uint8_t> set1, set2;
            for (size_t m = 0; m < N; ++m) {
              set1.insert(pairs[m].q.data[cand1]);
              set2.insert(pairs[m].q.data[cand2]);
            }
            if (set1.size() <= set2.size()) {
              dev_type_idx = static_cast<int>(cand1);
              sub_id_idx = static_cast<int>(cand2);
            } else {
              dev_type_idx = static_cast<int>(cand2);
              sub_id_idx = static_cast<int>(cand1);
            }
            break;
          }
        }
        if (dev_type_idx >= 0) break;
      }
    }
  }

  int ack_flag_idx = -1;
  int max_known_hdr = std::max({opcode_idx, dev_type_idx, sub_cmd_idx, sub_id_idx, swap_i, swap_j, len_idx, seq_idx});
  for (size_t k = 1; k < min_common_len - 1; ++k) {
    int ik = static_cast<int>(k);
    if (ik == len_idx || ik == opcode_idx || ik == swap_i || ik == swap_j ||
        ik == seq_idx || ik == sub_cmd_idx || ik == dev_type_idx || ik == sub_id_idx) {
      continue;
    }
    if (ik > max_known_hdr) {
      continue;
    }
    std::set<uint8_t> r_vals;
    for (size_t m = 0; m < N; ++m) r_vals.insert(pairs[m].r.data[k]);
    if (r_vals.size() == 1 && (*r_vals.begin() == 0x00 || *r_vals.begin() == 0x01)) {
      ack_flag_idx = ik;
      break;
    }
  }

  AutoProbeDescriptor desc_to_sync;
  {
    CriticalSectionLocker lock(&_mux);
    if (opcode_idx >= 0) {
      _desc.opcode_offset = static_cast<uint8_t>(opcode_idx);
      _desc.query_opcode = learned_q_op;
      _desc.ack_opcode = learned_ack_op;
      _desc.opcodes_locked = true;
    }
    if (dev_type_idx >= 0) {
      _desc.dev_id_offset = static_cast<uint8_t>(dev_type_idx);
    }
    if (sub_cmd_idx >= 0) {
      _desc.sub1_offset = static_cast<uint8_t>(sub_cmd_idx);
    } else if (sub_id_idx >= 0) {
      _desc.sub1_offset = static_cast<uint8_t>(sub_id_idx);
    }
    if (sub_id_idx >= 0) {
      _desc.sub2_offset = static_cast<uint8_t>(sub_id_idx);
    }
    _desc.len_offset = (len_idx >= 0) ? static_cast<uint8_t>(len_idx) : 0xFF;
    _desc.has_len_field = (len_idx >= 0);
    _desc.seq_offset = (seq_idx >= 0) ? static_cast<uint8_t>(seq_idx) : 0xFF;
    _desc.has_seq_counter = (seq_idx >= 0);
    _desc.ack_flag_offset = (ack_flag_idx >= 0) ? static_cast<uint8_t>(ack_flag_idx) : 0xFF;

    _desc.is_swapped_addr = (swap_i >= 0);
    if (master_gw_idx >= 0) {
      _desc.gw_addr_offset = static_cast<uint8_t>(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (swap_i >= 0 && master_gw_idx >= 0 && promoted_dev_idx >= 0) {
      _desc.gw_addr_offset = static_cast<uint8_t>(master_gw_idx);
      _desc.gw_addr = pairs[0].q.data[master_gw_idx];
    } else if (!_desc.is_swapped_addr && dev_type_idx >= 0) {
      _desc.gw_addr_offset = _desc.dev_id_offset;
    }

    if (min_common_len >= 5 && min_common_len <= 64) {
      _desc.learned_query_len = static_cast<uint8_t>(min_common_len);
    }

    _desc.offsets_locked = (dev_type_idx >= 0 && sub_id_idx >= 0);

    int max_hdr = 0;
    if (opcode_idx > max_hdr) max_hdr = opcode_idx;
    if (dev_type_idx > max_hdr) max_hdr = dev_type_idx;
    if (sub_cmd_idx > max_hdr) max_hdr = sub_cmd_idx;
    if (sub_id_idx > max_hdr) max_hdr = sub_id_idx;
    if (swap_i > max_hdr) max_hdr = swap_i;
    if (swap_j > max_hdr) max_hdr = swap_j;
    if (len_idx > max_hdr) max_hdr = len_idx;
    if (seq_idx > max_hdr) max_hdr = seq_idx;
    if (ack_flag_idx > max_hdr) max_hdr = ack_flag_idx;

    // Zero-Variance Padding 스캐너: max_hdr 직후 모든 샘플에서 값이 0x00 고정인 더미/패딩 열 자동 스킵
    int payload_start = max_hdr + 1;
    while (payload_start < static_cast<int>(min_common_len - 2)) {
      bool is_all_zero = true;
      for (size_t m = 0; m < N; ++m) {
        if (pairs[m].r.data[payload_start] != 0x00) {
          is_all_zero = false;
          break;
        }
      }
      if (is_all_zero) {
        payload_start++;
      } else {
        break;
      }
    }
    _desc.payload_offset = static_cast<uint8_t>(payload_start);

    snprintf(_desc.description, sizeof(_desc.description),
             "Auto: OP@%u(0x%02X/0x%02X) DEV@%u SUB1@%u SUB2@%u PL@%u",
             _desc.opcode_offset, _desc.query_opcode, _desc.ack_opcode,
             _desc.dev_id_offset, _desc.sub1_offset, _desc.sub2_offset,
             _desc.payload_offset);
    desc_to_sync = _desc;
  }

  ProfileRepository::syncAutoProfileToNvs(desc_to_sync);

  if (desc_to_sync.offsets_locked) {
    g_polling_targets.reindexWithOffsets(desc_to_sync.dev_id_offset,
                                         desc_to_sync.sub1_offset,
                                         desc_to_sync.sub2_offset);
    for (size_t i = 0; i < target_count; ++i) {
      PollingTargetEntry target;
      if (g_polling_targets.getEntry(i, target) && target.is_active && target.raw_ack_len >= 4) {
        StaticPacket ack_pkt;
        ack_pkt.channel_id = 1;
        ack_pkt.length = target.raw_ack_len;
        memcpy(ack_pkt.data.data(), target.raw_ack_data.data(), target.raw_ack_len);
        g_device_repo.updateFromBus(ack_pkt);
      }
    }
  }

  g_telnet_tracer.trace("[AUTO PROBE] ★ Full-Matrix Cache Analysis Complete! Offsets locked & saved to NVS.\r\n");
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
  _desc.control_opcode = 0x00;
  _desc.ack_opcode = 0x04;
  _desc.opcodes_locked = false;
  _desc.control_seen = false;
  _desc.len_offset = 0xFF;
  _desc.has_len_field = false;
  _desc.seq_offset = 0xFF;
  _desc.has_seq_counter = false;
  _desc.ack_flag_offset = 0xFF;
  _desc.payload_offset = 7;
  _desc.is_locked = false;
  _consecutive_mismatches = 0;
  strncpy(_desc.description, "Probing bus traffic...", sizeof(_desc.description) - 1);
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
// From src/Control/Control.cpp
// ============================================================================

using namespace ControlTemplateUtils;

// ============================================================================
// DEVICE ROUTE REGISTRY
// ============================================================================

DeviceRouteRegistry g_route_registry;

void DeviceRouteRegistry::recordRoute(uint8_t channel_id, int8_t slot_idx,
                                      uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  uint32_t now = millis();

  for (size_t i = 0; i < _count; i++) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      _entries[i].endpoint.channel_id = channel_id;
      _entries[i].endpoint.slot_idx = slot_idx;
      _entries[i].endpoint.last_seen_ms = now;
      return;
    }
  }

  if (_count < MAX_ROUTES) {
    _entries[_count].dev_id = dev_id;
    _entries[_count].sub1 = sub1;
    _entries[_count].sub2 = sub2;
    _entries[_count].endpoint.channel_id = channel_id;
    _entries[_count].endpoint.slot_idx = slot_idx;
    _entries[_count].endpoint.last_seen_ms = now;
    _count++;
  }
}

bool DeviceRouteRegistry::lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                      RouteEndpoint &out_ep) const {
  CriticalSectionLocker lock(&_mux);
  for (size_t i = 0; i < _count; i++) {
    if (_entries[i].dev_id == dev_id && _entries[i].sub1 == sub1 &&
        _entries[i].sub2 == sub2) {
      out_ep = _entries[i].endpoint;
      return true;
    }
  }
  return false;
}

size_t DeviceRouteRegistry::getRoutes(DeviceRouteEntry *out_buf,
                                      size_t max_count) const {
  CriticalSectionLocker lock(&_mux);
  size_t copy_cnt = std::min(_count, max_count);
  for (size_t i = 0; i < copy_cnt; i++) {
    out_buf[i] = _entries[i];
  }
  return copy_cnt;
}

void DeviceRouteRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
  memset(_entries, 0, sizeof(_entries));
}

// ============================================================================
// CONTROL DISPATCHER
// ============================================================================

bool ControlDispatcher::dispatch(StaticPacket &req,
                                 StaticPacket &virtual_ack_out) {
  if (UNLIKELY(req.length < 5))
    return false;
  auto *parser = WallpadParserFactory::getActiveParser();
  span<const uint8_t> frame(req.data.data(), req.length);
  if (parser->isQueryPacket(frame)) {
    virtual_ack_out.channel_id = req.channel_id;
    uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
    if (!parser->extractDeviceKey(frame, dev_id, sub1, sub2)) {
      return false;
    }
    return g_device_repo.copyVirtualAck(dev_id, sub1, sub2, virtual_ack_out);
  }

  bool is_ctl = parser->isControlPacket(frame);
  uint8_t dev_id = 0, sub1 = 0, sub2 = 0;
  bool has_key = parser->extractDeviceKey(frame, dev_id, sub1, sub2);
  GroupControlTemplate grp{};
  bool has_grp = (has_key && dev_id != 0) ? g_control_registry.findGroup(dev_id, grp) : false;

  if (!is_ctl && has_grp && grp.frame_len > 4 && frame.size() >= grp.frame_len) {
    VendorProfileDescriptor desc;
    ProfileRepository::getActiveProfile(desc);
    uint8_t op_off = (desc.opcode_offset < frame.size()) ? desc.opcode_offset : 4;
    if (frame[op_off] == grp.raw_template[op_off]) {
      is_ctl = true;
    }
  }

  if (is_ctl) {
    if (has_grp) {
      if (grp.coverage.dev_class == DeviceClass::GAS) {
        if (grp.close_slot.discovered && grp.close_slot.action_offset < req.length) {
          uint8_t val = req.data[grp.close_slot.action_offset];
          if (val != grp.close_slot.off_val) {
            g_telnet_tracer.trace(req.channel_id, false, TraceType::DRP, req);
            return false;
          }
        }
      }
      if (grp.coverage.dev_class == DeviceClass::THERMOSTAT && grp.temp_slot.discovered &&
          grp.temp_slot.action_offset < req.length) {
        bool is_temp = false;
        if (grp.temp_slot.category_offset != 0xFF && grp.temp_slot.category_offset < req.length) {
          is_temp = (req.data[grp.temp_slot.category_offset] == grp.temp_slot.category_val);
        }

        if (is_temp) {
          uint8_t t_val = req.data[grp.temp_slot.action_offset];
          if (t_val < 5 || t_val > 35) {
            g_telnet_tracer.trace(req.channel_id, false, TraceType::DRP, req);
            return false;
          }
        }
      }
    }

    RouteEndpoint ep{1, -1, 0};
    bool route_known = false;
    if (has_key) {
      route_known = g_route_registry.lookupRoute(dev_id, sub1, sub2, ep);
    }

    if (route_known && ep.channel_id == 5 && ep.slot_idx >= 0 && ep.slot_idx < Config::TCP::MAX_EW11_SLOTS) {
      bool is_unidirectional = (has_grp && grp.isUnidirectional()) || (dev_id == 0x34);
      if (is_unidirectional || ep.slot_idx == 0) {
        Ew11Manager::sendBurstPacket(static_cast<uint8_t>(ep.slot_idx), req, 2, 20);
      } else {
        bool sent = Hub_SendPacket(static_cast<uint8_t>(ep.slot_idx), req);
        g_telnet_tracer.trace(5, true, sent ? TraceType::CTL : TraceType::DRP, req);
      }
      return false;
    }

    QueueHandle_t q = (req.channel_id == 6) ? g_ch1_vip_queue : g_ch1_control_queue;
    if (!Queue_EnqueueDropHead(q, req)) {
      return false;
    }
    g_telnet_tracer.trace(1, true, TraceType::CTL, req);
    return false;
  }
  return false;
}

// ============================================================================
// CONTROL TEMPLATE REGISTRY
// ============================================================================

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
