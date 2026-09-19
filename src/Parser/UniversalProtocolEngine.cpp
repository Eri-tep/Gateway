#include "WallpadParser.h"
#include "Common.h"
#include <cstring>
#include <cstdio>

static UniversalProtocolEngine s_universal_engine;

const char *UniversalProtocolEngine::getVendorName() const {
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  if (strcasecmp(desc.key, "auto") == 0) {
    auto ad = g_auto_probing_engine.getDescriptor();
    if (ad.is_locked) {
      static char buf[64];
      snprintf(buf, sizeof(buf), "Auto [STX 0x%02X ETX 0x%02X / %s]",
               ad.stx, ad.etx, AutoProbingEngine::getAlgoName(ad.checksum_algo));
      return buf;
    }
    return "Auto (Learning...)";
  }
  static char buf[32];
  snprintf(buf, sizeof(buf), "%s", desc.name);
  return buf;
}

const char *UniversalProtocolEngine::getProfileKey() const {
  static char buf[16];
  VendorProfileDescriptor desc;
  ProfileRepository::getActiveProfile(desc);
  snprintf(buf, sizeof(buf), "%s", desc.key);
  return buf;
}

uint8_t UniversalProtocolEngine::getVendorId() const {
  return g_config.wallpad_profile;
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

  // ★ swap 구조 보완: ACK 패킷에서 DevType은 gw_addr_offset 위치에 있음
  // (QUERY: dev_id_offset=swap_i=DevType, gw_addr_offset=swap_j=GW주소)
  // (ACK:   dev_id_offset=swap_i=GW주소, gw_addr_offset=swap_j=DevType) ← 교차!
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

    // ★ 버스 관측 기반 동적 패킷 길이 (기본: learned_query_len, 최소 11)
    uint8_t pkt_len = (ad.offsets_locked && ad.learned_query_len >= 5)
                          ? ad.learned_query_len : 11;
    out.channel_id = 1;
    out.length = pkt_len;
    out.data.fill(0);
    out.data[0] = stx;
    // ★ LEN 필드: has_len_field 활성화 시에만 len_offset 위치에 기입
    if (ad.has_len_field && ad.len_offset < pkt_len) {
      out.data[ad.len_offset] = pkt_len;
    }

    // ★ GW 주소: 버스에서 관측된 ad.gw_addr를 ad.gw_addr_offset 위치에 기입
    // (기존: data[2] = 0x01 하드코딩 → 현재: 관측값 사용)
    if (ad.gw_addr_offset < pkt_len) {
      out.data[ad.gw_addr_offset] = ad.gw_addr;
    }

    if (d_off < pkt_len) out.data[d_off] = dev_id;
    if (op_off < pkt_len) out.data[op_off] = q_op;
    if (s1_off > 0 && s1_off < pkt_len) out.data[s1_off] = sub1;
    if (s2_off > 0 && s2_off < pkt_len) out.data[s2_off] = sub2;

    // CS = Byte #(N-2), ETX = Byte #(N-1)
    if (pkt_len >= 3) {
      out.data[pkt_len - 2] = g_auto_probing_engine.calculateChecksum(algo, out.data.data(), pkt_len);
      out.data[pkt_len - 1] = etx;
    }
    return true;
  }

  // ── Non-Auto 프로파일: 저장된 프로파일의 길이와 GW 주소 규칙 사용 ──
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

  // 진입 시 단 1회만 메타데이터 스냅샷
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

// ============================================================================
// WALLPAD PARSER FACTORY (COMPATIBILITY FACADE)
// ============================================================================

void WallpadParserFactory::init() {
  ProfileRepository::init();
}

IWallpadParser *WallpadParserFactory::getActiveParser() {
  return &s_universal_engine;
}

bool WallpadParserFactory::setProfile(uint8_t index) {
  return ProfileRepository::setActiveProfileIndex(index);
}

bool WallpadParserFactory::setProfileByKey(const char *key) {
  return ProfileRepository::setActiveProfileByKey(key);
}

size_t WallpadParserFactory::getParserCount() {
  return ProfileRepository::getProfileCount();
}
