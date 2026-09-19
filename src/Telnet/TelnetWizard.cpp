#include "TelnetCli.h"
#include "WallpadParser.h"
#include <cstdio>
#include <cstring>

// ============================================================================
// INTERACTIVE CONTROL LEARNING WIZARD (NON-BLOCKING EVENT-DRIVEN FSM)
// ============================================================================

struct WizardTargetDef {
  DeviceClass cls;
  const char *name;
  const char *step_name;
};

static const WizardTargetDef s_wizard_targets[] = {
    {DeviceClass::SWITCH, "Light", "Step 1: Light (조명)"},
    {DeviceClass::SWITCH, "Outlet", "Step 2: Outlet (콘센트/대기전력)"},
    {DeviceClass::VENT, "Vent", "Step 3: Ventilation (전열교환기/환기)"},
    {DeviceClass::THERMOSTAT, "Thermo", "Step 4: Thermostat (난방/온도조절기)"},
    {DeviceClass::GAS, "Gas", "Step 5: Gas Valve (가스밸브)"},
    {DeviceClass::AIRCON, "Aircon", "Step 6: Air Conditioner (시스템 에어컨)"},
    {DeviceClass::MOMENTARY, "Elevator", "Step 7: Elevator (엘리베이터 호출)"},
};
static constexpr uint8_t WIZARD_TOTAL_STEPS = sizeof(s_wizard_targets) / sizeof(s_wizard_targets[0]);

// 정적 BSS 스크래치 버퍼 (스택 오버플로우 방지 및 재진입 안전: _cli_mutex 보호 하에 사용)
static char s_wizard_scratch_buf[4096];

void TelnetManager::handleWizardStepAdvance(TelnetSession *s, bool skipped, bool match) {
  if (!s || s->sock < 0 || s->wizard_step == 0) return;

  uint8_t cur_idx = s->wizard_step - 1; // 0-based
  if (cur_idx >= WIZARD_TOTAL_STEPS) {
    s->wizard_step = 0;
    return;
  }

  if (skipped && !match) {
    sendTelnetMsgf(s->sock, ">> [SKIP] No traffic detected for '%s'. Skipping to next...\r\n",
                   s_wizard_targets[cur_idx].name);
  }

  if (match && s->wizard_dev_id != 0) {
    if (s->wizard_learned_count < 8) {
      s->wizard_learned_devs[s->wizard_learned_count++] = s->wizard_dev_id;
    }
  }
  // 스텝 전이 시 이전 기기의 잔여 버스트 패킷이 다음 스텝을 오염시키지 않도록 1.5초 쿨다운 설정
  s->wizard_step_cooldown_until_ms = millis() + 1500;

  uint8_t next_idx = cur_idx + 1;
  if (next_idx < WIZARD_TOTAL_STEPS) {
    s->wizard_step = next_idx + 1;
    s->wizard_dev_id = 0;
    s->wizard_sub_phase = 0;
    s->wizard_last_prompt = 0;
    s->thermo_phase = TelnetSession::ThermoPhase::WAIT_ON;
    s->vent_phase = TelnetSession::VentPhase::WAIT_ON;
    s->switch_phase = TelnetSession::SwitchPhase::WAIT_ON;
    s->wizard_step_start_ms = millis();
    s->last_activity_ms = millis();

    // 다음 단계 기기들의 마지막 학습 시각 스냅샷 갱신
    size_t count = g_control_registry.getGroupCount();
    size_t valid_cnt = 0;
    for (size_t i = 0; i < count && valid_cnt < ControlTemplateRegistry::MAX_GROUPS; ++i) {
      GroupControlTemplate grp;
      if (g_control_registry.getGroupByIndex(i, grp) && grp.dev_id != 0) {
        s->prev_learned_ms[valid_cnt++] = grp.last_learned_ms;
      }
    }

    sendTelnetMsgf(s->sock, "\r\n[%s]\r\n", s_wizard_targets[next_idx].step_name);
    if (s_wizard_targets[next_idx].cls == DeviceClass::THERMOSTAT) {
      sendTelnetMsgf(s->sock, ">> Please TURN ON '%s' on your wallpad now...\r\n",
                     s_wizard_targets[next_idx].name);
    } else if (s_wizard_targets[next_idx].cls == DeviceClass::VENT) {
      sendTelnetMsgf(s->sock, ">> Please TURN ON '%s' on your wallpad now...\r\n",
                     s_wizard_targets[next_idx].name);
    } else if (s_wizard_targets[next_idx].cls == DeviceClass::GAS) {
      sendTelnetMsgf(s->sock, ">> Please CLOSE (차단) '%s' valve on your wallpad now...\r\n",
                     s_wizard_targets[next_idx].name);
    } else if (s_wizard_targets[next_idx].cls == DeviceClass::MOMENTARY) {
      sendTelnetMsgf(s->sock, ">> Please operate '%s' on your wallpad or wall switch now...\r\n",
                     s_wizard_targets[next_idx].name);
    } else {
      sendTelnetMsgf(s->sock, ">> Please TURN ON and TURN OFF '%s' on your wallpad/switch now...\r\n",
                     s_wizard_targets[next_idx].name);
    }
    sendTelnetMsg(s->sock, ">> (Waiting for packet transaction... 45s timeout | Enter: Skip | 'q': Abort)\r\n");
  } else {
    // 모든 위저드 목표 완료!
    s->wizard_step = 0;
    s->wizard_dev_id = 0;
    s->wizard_sub_phase = 0;
    s->wizard_last_prompt = 0;
    sendTelnetMsg(s->sock, "\r\n================================================================================\r\n");
    sendTelnetMsg(s->sock, "             LEARNING WIZARD COMPLETE - UPDATED BLUEPRINT TABLE                \r\n");
    sendTelnetMsg(s->sock, "================================================================================\r\n\r\n");
    s_wizard_scratch_buf[0] = '\0';
    AppendBuf out{s_wizard_scratch_buf, sizeof(s_wizard_scratch_buf)};
    WallpadCli::wallpadPrintControlTable(out);
    sendTelnetMsgLen(s->sock, out.buf, out.offset);
  }
}

void TelnetManager::handleWizardInput(TelnetSession *s, char c) {
  if (!s || s->sock < 0 || s->wizard_step == 0) return;

  if (c == 'q' || c == 'Q') {
    sendTelnetMsg(s->sock, ">> [ABORT] Learning wizard aborted by user.\r\n");
    // abort 시 현재 진행 중인 기기(wizard_dev_id)의 coverage/슬롯 리셋
    // → 반쯤 학습된 상태로 남아 버스 패킷에 의해 VERIFIED로 오전환되는 것을 방지
    if (s->wizard_dev_id != 0) {
      GroupControlTemplate *live = g_control_registry.findGroup(s->wizard_dev_id);
      if (live && live->status != GroupControlTemplate::Status::LOCKED) {
        DeviceClass cur_cls = live->coverage.dev_class;
        live->coverage = SlotCoverage{};
        live->coverage.dev_class = cur_cls;
        live->power_slot = ActionSlot{};
        live->speed_slot = ActionSlot{};
        live->temp_slot = ActionSlot{};
        live->close_slot = ActionSlot{};
        live->mode_slot = ActionSlot{};
        live->ack_slots = AckStateSlots{};
        live->status = GroupControlTemplate::Status::CAPTURING;
      }
    }
    s->wizard_step = 0;
  } else if (c == '\r' || c == '\n') {
    uint8_t cur_idx = s->wizard_step - 1;
    if (cur_idx < WIZARD_TOTAL_STEPS && s_wizard_targets[cur_idx].cls == DeviceClass::THERMOSTAT &&
        s->thermo_phase == TelnetSession::ThermoPhase::WAIT_AWAY) {
      // 외출 모드 스킵 후 바로 복원 검증 단계(Re-ON)로 안내
      s->thermo_phase = TelnetSession::ThermoPhase::WAIT_RECALL;
      s->wizard_sub_phase = 1;
      sendTelnetMsg(s->sock, ">> [SKIP] Away mode skipped. Please TURN ON 'Thermo' again to verify Target Temp Recall...\r\n");
      s->wizard_step_start_ms = millis();
      return;
    }
    if (cur_idx < WIZARD_TOTAL_STEPS && s_wizard_targets[cur_idx].cls == DeviceClass::VENT) {
      if (s->vent_phase == TelnetSession::VentPhase::WAIT_SPEED_MID) {
        // 중풍 스킵 시 단일 전원 스위치로 격하
        GroupControlTemplate *v_grp = g_control_registry.findGroup(s->wizard_dev_id);
        if (v_grp) {
          v_grp->speed_slot = ActionSlot{};
          v_grp->speed_slot.discovered = false;
          v_grp->coverage.speed_l1_seen = false;
          v_grp->coverage.speed_l2_seen = false;
          v_grp->coverage.speed_l3_seen = false;
        }
        s->vent_phase = TelnetSession::VentPhase::VERIFIED;
        sendTelnetMsg(s->sock, ">> [SKIP] Fan speed adjustment skipped. Configured as Simple Power Switch.\r\n");
        handleWizardStepAdvance(s, false, true);
        return;
      } else if (s->vent_phase == TelnetSession::VentPhase::WAIT_SPEED_HIGH) {
        // 강풍 스킵 시 현재까지 학습된 풍량(약풍/중풍 2단)으로 마무리 검증
        s->vent_phase = TelnetSession::VentPhase::VERIFIED;
        sendTelnetMsg(s->sock, ">> [SKIP] High speed learning skipped. Configured with 2 Fan Speed levels.\r\n");
        handleWizardStepAdvance(s, false, true);
        return;
      }
    }
    handleWizardStepAdvance(s, true, false);
  }
}

void TelnetManager::notifyControlTransaction(uint8_t dev_id) {
  if (dev_id == 0 || dev_id == 0xFF) return;

  // 동적 프로토콜 파서의 STX/ETX 프레임 경계 바이트 필터링 (하드코딩 배제)
  auto *parser = WallpadParserFactory::getActiveParser();
  if (parser) {
    if (dev_id == parser->getStx() || dev_id == parser->getEtx()) return;
  }

  MutexLocker cliLock(_cli_mutex);
  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    TelnetSession &s = _sessions[i];
    if (s.sock >= 0 && s.wizard_step >= 1 && s.wizard_step <= WIZARD_TOTAL_STEPS) {
      uint8_t cur_idx = s.wizard_step - 1;
      const auto &tgt = s_wizard_targets[cur_idx];

      // 스텝 전환 직후 이전 기기의 잔여 버스트 패킷 무시 (쿨다운)
      if (s.wizard_step_cooldown_until_ms > 0 && millis() < s.wizard_step_cooldown_until_ms) {
        continue;
      }

      // 조작 액션(ON -> OFF 등) 전환 직후 동일 조작의 잔여 버스트 패킷 무시 (쿨다운)
      if (s.wizard_action_cooldown_until_ms > 0 && millis() < s.wizard_action_cooldown_until_ms) {
        continue;
      }

      // 현재 단계에서 한 기기(wizard_dev_id)가 학습을 시작했다면 다른 기기 패킷은 혼선 방지를 위해 무시
      if (s.wizard_dev_id != 0 && s.wizard_dev_id != dev_id) {
        continue;
      }

      // ★ [원칙: 오로지 LOCKED 기기만 보호, VERIFIED는 언제든 덮어쓰기 허용]
      // 아직 이번 단계의 대상 기기가 지정되지 않은 상태(wizard_dev_id == 0)에서만 검사
      if (s.wizard_dev_id == 0) {
        // 1) 이번 위저드 세션에서 이미 앞선 스텝(예: Step 1 Light)에서 완료된 기기는 다른 스텝에서 중복 캡처 방지!
        bool already_learned_in_session = false;
        for (uint8_t l = 0; l < s.wizard_learned_count; ++l) {
          if (s.wizard_learned_devs[l] == dev_id) {
            already_learned_in_session = true;
            break;
          }
        }
        if (already_learned_in_session) {
          continue;
        }

        // 2) LOCKED 기기 보호
        const GroupControlTemplate *existing = g_control_registry.findGroup(dev_id);
        if (existing && existing->status == GroupControlTemplate::Status::LOCKED) {
          // LOCKED 기기는 MOMENTARY 단계가 아닌 한 다른 스텝에서 가로채지 못하도록 보호
          if (tgt.cls != DeviceClass::MOMENTARY) {
            continue;
          }
        }
      }

      // 현재 단계에 처음으로 매칭되는 기기라면 ID 바인딩 및 클래스 활성화
      // 단, MOMENTARY 단계에서는 평상시 정적 패킷이 스쳐갈 때 성급히 바인딩하지 않고,
      // 실제 물리 조작 차분(call_seen)이 있는 기기만 선별 바인딩!
      const GroupControlTemplate *grp = g_control_registry.findGroup(dev_id);
      if (s.wizard_dev_id == 0) {
        if (tgt.cls == DeviceClass::MOMENTARY) {
          if (!grp || !grp->coverage.call_seen) {
            continue; // 실제 물리 호출 차분이 없으면 선점 금지!
          }
        }
        s.wizard_dev_id = dev_id;
        g_control_registry.setGroupClass(dev_id, tgt.cls, tgt.name);
      }

      grp = g_control_registry.findGroup(dev_id);
      bool need_dual_action = (tgt.cls != DeviceClass::MOMENTARY && tgt.cls != DeviceClass::GAS);

      if (need_dual_action && grp) {
        bool on_done  = grp->coverage.power_on_seen;
        bool off_done = grp->coverage.power_off_seen;
        bool extra_done = true;

        if (tgt.cls == DeviceClass::THERMOSTAT) {
          switch (s.thermo_phase) {
            case TelnetSession::ThermoPhase::WAIT_ON:
              if (on_done) {
                s.thermo_phase = TelnetSession::ThermoPhase::WAIT_OFF;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #1] DevID 0x%02X (%s) ON recorded! Please now TURN OFF '%s'...\r\n",
                               dev_id, tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::WAIT_OFF:
              if (off_done) {
                s.thermo_phase = TelnetSession::ThermoPhase::WAIT_RE_ON;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #2] DevID 0x%02X (%s) OFF recorded! Please TURN ON '%s' to adjust temperature...\r\n",
                               dev_id, tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::WAIT_RE_ON:
              // 전원 OFF 이후 다시 ON 패킷이 수신되었거나 temp_ready_on 플래그가 활성화된 경우
              if (grp->coverage.temp_ready_on || (grp->last_ctl_len > grp->power_slot.action_offset && grp->last_ctl_raw[grp->power_slot.action_offset] == grp->power_slot.on_val)) {
                s.thermo_phase = TelnetSession::ThermoPhase::WAIT_TEMP;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #3] '%s' is ON. Please change Target Temperature (희망온도 조절) for '%s'...\r\n",
                               tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::WAIT_TEMP:
              if (grp->coverage.temp_set_seen) {
                s.thermo_phase = TelnetSession::ThermoPhase::WAIT_AWAY;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #4] Target Temp recorded! Please press 'Away (외출)' mode on wallpad (or press Enter to skip)...\r\n");
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::WAIT_AWAY:
              if (grp->coverage.away_mode_seen) {
                s.thermo_phase = TelnetSession::ThermoPhase::WAIT_RECALL;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #5] Please TURN ON '%s' again to verify Target Temp Recall...\r\n", tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::WAIT_RECALL:
              if (grp->temp_recall_verified) {
                s.thermo_phase = TelnetSession::ThermoPhase::VERIFIED;
              } else {
                continue;
              }
              break;

            case TelnetSession::ThermoPhase::VERIFIED:
              break;
          }
        } else if (tgt.cls == DeviceClass::VENT) {
          switch (s.vent_phase) {
            case TelnetSession::VentPhase::WAIT_ON:
              if (on_done) {
                s.vent_phase = TelnetSession::VentPhase::WAIT_OFF;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #1] DevID 0x%02X (%s) ON recorded! Please now TURN OFF '%s'...\r\n",
                               dev_id, tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::VentPhase::WAIT_OFF:
              if (off_done) {
                s.vent_phase = TelnetSession::VentPhase::WAIT_RE_ON;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #2] DevID 0x%02X (%s) OFF recorded! Please TURN ON '%s' to adjust Fan Speed...\r\n",
                               dev_id, tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::VentPhase::WAIT_RE_ON:
              if (grp->coverage.speed_ready_on || (grp->last_ctl_len > grp->power_slot.action_offset && grp->last_ctl_raw[grp->power_slot.action_offset] == grp->power_slot.on_val)) {
                s.vent_phase = TelnetSession::VentPhase::WAIT_SPEED_MID;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #3] '%s' is ON. Please set Fan Speed to MEDIUM (중풍) on your wallpad (or press Enter to skip)...\r\n",
                               tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::VentPhase::WAIT_SPEED_MID:
              if (grp->speed_slot.level_count >= 2 || grp->coverage.speed_l2_seen) {
                s.vent_phase = TelnetSession::VentPhase::WAIT_SPEED_HIGH;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #4] Fan Speed MEDIUM (중풍=0x%02X) recorded! Please now set Fan Speed to HIGH (강풍) (or press Enter to complete)...\r\n",
                               (grp->speed_slot.level_count >= 2) ? grp->speed_slot.level_tokens[1] : 0x03);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::VentPhase::WAIT_SPEED_HIGH:
              if (grp->speed_slot.level_count >= 3 || grp->coverage.speed_l3_seen) {
                s.vent_phase = TelnetSession::VentPhase::VERIFIED;
              } else {
                continue;
              }
              break;

            case TelnetSession::VentPhase::VERIFIED:
              break;
          }
        } else {
          switch (s.switch_phase) {
            case TelnetSession::SwitchPhase::WAIT_ON:
              if (on_done) {
                s.switch_phase = TelnetSession::SwitchPhase::WAIT_OFF;
                s.wizard_action_cooldown_until_ms = millis() + 300;
                sendTelnetMsgf(s.sock, "\r\n>> [CAPTURED #1] DevID 0x%02X (%s) ON recorded! Please now TURN OFF '%s'...\r\n",
                               dev_id, tgt.name, tgt.name);
                s.wizard_step_start_ms = millis();
                continue;
              }
              break;

            case TelnetSession::SwitchPhase::WAIT_OFF:
              if (off_done) {
                s.switch_phase = TelnetSession::SwitchPhase::VERIFIED;
              } else {
                continue;
              }
              break;

            case TelnetSession::SwitchPhase::VERIFIED:
              break;
          }
        }
      }

      // ★ [FSM 완전 격리 방어벽]
      bool is_verified = false;
      if (tgt.cls == DeviceClass::THERMOSTAT) {
        is_verified = (s.thermo_phase == TelnetSession::ThermoPhase::VERIFIED);
      } else if (tgt.cls == DeviceClass::VENT) {
        is_verified = (s.vent_phase == TelnetSession::VentPhase::VERIFIED);
      } else if (tgt.cls == DeviceClass::MOMENTARY) {
        // 단발성 기기(엘리베이터 등)는 정적 패킷이 아닌 실제 호출/트리거 차분(call_seen)이 관측되어야만 검증 완료!
        is_verified = (grp && grp->coverage.call_seen);
      } else if (need_dual_action) {
        is_verified = (s.switch_phase == TelnetSession::SwitchPhase::VERIFIED);
      } else {
        is_verified = true;
      }

      if (!is_verified) {
        continue;
      }

      // ★ [최종 확정 시점에만 기기 분류 및 그룹명 영구 등록]
      g_control_registry.setGroupClass(dev_id, tgt.cls, tgt.name);

      // ★ MATCH 완료 시 커버리지 보정 후 즉시 VERIFIED로 전환 (버스 패킷 대기 없이)
      // MOMENTARY/GAS는 onControlTransaction 시점에 클래스가 UNKNOWN이어서 coverage 플래그가 누락될 수 있음
      {
        GroupControlTemplate *matched = g_control_registry.findGroup(dev_id);
        if (matched) {
          if (tgt.cls == DeviceClass::MOMENTARY) {
            if (matched->power_slot.action_offset == 0xFF) {
              matched->power_slot.action_offset = (matched->frame_len >= 3) ? (matched->frame_len - 3) : 7;
            }
            // 실제 수신된 제어 패킷의 바이트(raw_template)에서만 추출하며, 임의 추정 금지
            if (matched->power_slot.action_offset < matched->frame_len &&
                matched->raw_template[matched->power_slot.action_offset] != 0x00) {
              matched->power_slot.on_val = matched->raw_template[matched->power_slot.action_offset];
              matched->power_slot.discovered = true;
              matched->coverage.call_seen = true;
            }
            matched->power_slot.off_val = 0x00;
          }
          if (tgt.cls == DeviceClass::GAS) {
            matched->coverage.valve_close_seen = true;
            matched->coverage.power_off_seen = true;
            matched->close_slot.discovered = true;
            if (matched->close_slot.action_offset == 0xFF) {
              matched->close_slot.action_offset = (matched->frame_len >= 3) ? (matched->frame_len - 3) : 7;
            }
            if (matched->close_slot.off_val == 0x02 || matched->close_slot.off_val == 0) {
              // 실제 패킷 상의 제어 토큰(0x03 등)으로 보정
              if (matched->close_slot.action_offset < matched->frame_len) {
                matched->close_slot.off_val = matched->raw_template[matched->close_slot.action_offset];
              } else {
                matched->close_slot.off_val = 0x03;
              }
            }
          }
          if (matched->coverage.isFullyCovered()) {
            matched->status = GroupControlTemplate::Status::VERIFIED;
          }
        }
      }

      sendTelnetMsgf(s.sock, "\r\n>> [MATCH DETECTED!] DevID 0x%02X matched to '%s'!\r\n",
                     dev_id, tgt.name);

      // 즉시 상세 청사진 출력 (정적 버퍼 사용하여 스택 소모 0)
      s_wizard_scratch_buf[0] = '\0';
      AppendBuf out{s_wizard_scratch_buf, sizeof(s_wizard_scratch_buf)};
      WallpadCli::wallpadPrintControlDetail(out, dev_id);
      sendTelnetMsgLen(s.sock, out.buf, out.offset);

      // 다음 단계로 비동기 즉시 전이
      handleWizardStepAdvance(&s, false, true);
    }
  }
}

// ============================================================================
// WIZARD HINT PEEK
// Engine.cpp가 onControlTransaction() 호출 직전에 읽어 hint를 전달.
// 락 없는 읽기 전용 함수 — 위저드가 어떤 단계를 기다리는지 의미론적으로 추론.
// ============================================================================
AckSlotHint TelnetManager::peekWizardHint(uint8_t dev_id) const noexcept {
  static constexpr AckSlotHint THERMO_HINTS[] = {
      AckSlotHint::POWER, // WAIT_ON
      AckSlotHint::POWER, // WAIT_OFF
      AckSlotHint::POWER, // WAIT_RE_ON
      AckSlotHint::TEMP,  // WAIT_TEMP (오직 4단계에서만 TEMP 슬롯 탐색)
      AckSlotHint::POWER, // WAIT_AWAY
      AckSlotHint::POWER, // WAIT_RECALL
      AckSlotHint::NONE   // VERIFIED
  };

  static constexpr AckSlotHint VENT_HINTS[] = {
      AckSlotHint::POWER, // WAIT_ON
      AckSlotHint::POWER, // WAIT_OFF
      AckSlotHint::POWER, // WAIT_RE_ON
      AckSlotHint::SPEED, // WAIT_SPEED_MID (중풍 조절 대기)
      AckSlotHint::SPEED, // WAIT_SPEED_HIGH (강풍 조절 대기)
      AckSlotHint::NONE   // VERIFIED
  };

  for (int i = 0; i < Config::TCP::MAX_TELNET_CLIENTS; ++i) {
    const TelnetSession &s = _sessions[i];
    if (s.sock < 0 || s.wizard_step < 1 || s.wizard_step > WIZARD_TOTAL_STEPS) continue;
    if (s.wizard_dev_id != 0 && s.wizard_dev_id != dev_id) continue;

    uint8_t cur_idx = s.wizard_step - 1;
    if (cur_idx >= WIZARD_TOTAL_STEPS) continue;
    const auto &tgt = s_wizard_targets[cur_idx];

    // 이미 이 세션에서 학습 완료된 기기라면 NONE 반환 → VERIFIED 상태 보호
    for (uint8_t l = 0; l < s.wizard_learned_count; ++l) {
      if (s.wizard_learned_devs[l] == dev_id) return AckSlotHint::NONE;
    }

    // 이번 스텝의 타겟 기기가 이미 지정되어 있는데(wizard_dev_id != 0), dev_id가 다르면 NONE
    if (s.wizard_dev_id != 0 && s.wizard_dev_id != dev_id) {
      return AckSlotHint::NONE;
    }

    switch (tgt.cls) {
      case DeviceClass::THERMOSTAT:
        return (static_cast<uint8_t>(s.thermo_phase) < sizeof(THERMO_HINTS) / sizeof(THERMO_HINTS[0]))
                   ? THERMO_HINTS[static_cast<uint8_t>(s.thermo_phase)]
                   : AckSlotHint::NONE;

      case DeviceClass::VENT:
        return (static_cast<uint8_t>(s.vent_phase) < sizeof(VENT_HINTS) / sizeof(VENT_HINTS[0]))
                   ? VENT_HINTS[static_cast<uint8_t>(s.vent_phase)]
                   : AckSlotHint::NONE;

      default:
        // SWITCH, GAS, MOMENTARY 등은 기본적으로 전원 토큰 탐색
        return AckSlotHint::POWER;
    }
  }
  return AckSlotHint::NONE;
}