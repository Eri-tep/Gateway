#include "ProfileMatcher.h"
#include "ControlTemplate.h"
#include "esp_log.h"
#include <algorithm>

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
    GroupControlTemplate *grp = registry.registerOrTouch(spec.dev_id, spec.name);
    if (!grp) continue;

    grp->coverage.dev_class = spec.dev_class;
    snprintf(grp->group_name, sizeof(grp->group_name), "%s", spec.name);

    // 기본 제어 템플릿(CTL) 구조 설정
    grp->frame_len = spec.ctl_len;
    grp->sub1_offset = profile->sub1_offset;
    grp->sub2_offset = profile->sub2_offset;

    // 0x34 엘리베이터의 경우 월패드 쿼리가 없으므로 기본 제어 골격 주입
    if (spec.dev_id == 0x34 && spec.ctl_len == 11) {
      const uint8_t ev_proto[11] = {0xF7, 0x0B, 0x01, 0x34, 0x02, 0x41, 0x10, 0x06, 0x00, 0x9C, 0xEE};
      std::copy(ev_proto, ev_proto + 11, grp->raw_template);
      grp->power_slot.action_offset = 7;
      grp->power_slot.on_val = 0x06;
      grp->power_slot.off_val = 0x00;
      grp->ctl_sub1_override = 0x10;
    }

    // 전원 액션 슬롯 설정
    grp->power_slot.discovered = true;
    grp->power_slot.action_offset = spec.ctl_payload_offset;
    grp->power_slot.on_val = spec.pwr_on_val;
    grp->power_slot.off_val = spec.pwr_off_val;
    grp->power_slot.ack_state_offset = spec.ctl_ack_state_offset;

    // 외출 모드 토큰 주입 (난방 0x07 등)
    if (spec.pwr_away_val != 0xFF) {
      grp->away_mode_token = spec.pwr_away_val;
    }

    // 기기 클래스별 추가 슬롯 설정
    if (spec.dev_class == DeviceClass::THERMOSTAT) {
      grp->temp_slot.discovered = true;
      grp->temp_slot.category_offset = 5; // 현대통신 온도제어 카테고리 오프셋
      grp->temp_slot.category_val = 0x45;    // 현대통신 온도제어 카테고리 코드
      grp->temp_slot.action_offset = spec.ctl_payload_offset;
      grp->temp_slot.min_val = 14;
      grp->temp_slot.max_val = 36;
      grp->temp_slot.ack_state_offset = spec.ctl_ack_state_offset;
      grp->temp_slot.ack_target_offset = spec.ctl_ack_echo_offset;
      grp->temp_slot.ack_telemetry_offset = spec.ctl_ack_ambtemp_offset;
    } else if (spec.dev_class == DeviceClass::VENT) {
      grp->speed_slot.discovered = true;
      grp->speed_slot.category_offset = 5; // 현대통신 풍량제어 카테고리 오프셋
      grp->speed_slot.category_val = 0x42;   // 현대통신 풍량제어 카테고리 코드
      grp->speed_slot.action_offset = spec.ctl_payload_offset;
      grp->speed_slot.min_val = 1;
      grp->speed_slot.max_val = 3;
      grp->speed_slot.level_count = 3;
      grp->speed_slot.level_tokens[0] = 0x01; // 1단 (약)
      grp->speed_slot.level_tokens[1] = 0x03; // 2단 (중)
      grp->speed_slot.level_tokens[2] = 0x07; // 3단 (강)
      grp->speed_slot.ack_state_offset = spec.ctl_ack_state_offset;

      // 운전 모드 슬롯 설정 (Cat 0x43, 일반 0x01, 바이패스 0x02, 자동 0x03, 공기청정 0x04)
      grp->mode_slot.discovered = true;
      grp->mode_slot.category_offset = 5;
      grp->mode_slot.category_val = 0x43;
      grp->mode_slot.action_offset = spec.ctl_payload_offset; // Byte #7
      grp->mode_slot.min_val = 1;
      grp->mode_slot.max_val = 4;
      grp->mode_slot.ack_state_offset = spec.ctl_ack_state_offset;
    } else if (spec.dev_class == DeviceClass::GAS) {
      grp->close_slot.discovered = true;
      grp->close_slot.action_offset = spec.ctl_payload_offset;
      grp->close_slot.off_val = spec.pwr_off_val;
      grp->close_slot.ack_state_offset = spec.ctl_ack_state_offset;
    }

    // 제어 응답 슬롯(ack_slots) 명세 주입
    grp->ack_slots.discovered = true;
    grp->ack_slots.power_offset = spec.ctl_ack_state_offset;
    if (spec.dev_class == DeviceClass::THERMOSTAT) {
      grp->ack_slots.target_temp_offset = spec.ctl_ack_echo_offset;
      grp->ack_slots.current_temp_offset = spec.ctl_ack_ambtemp_offset;
    }

    // 쿼리 응답 슬롯(query_slots) 명세 주입
    grp->query_slots.discovered = true;
    grp->query_slots.expected_len = spec.qry_ack_len;
    grp->query_slots.power_offset = spec.qry_power_offset;
    grp->query_slots.target_temp_offset = spec.qry_settemp_offset;
    grp->query_slots.current_temp_offset = spec.qry_ambtemp_offset;
    grp->query_slots.fan_speed_offset = spec.qry_fanspeed_offset;
    grp->query_slots.valve_state_offset = spec.qry_valve_offset;
    grp->query_slots.power_w_offset = spec.qry_watt_h_offset;

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

