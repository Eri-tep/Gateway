#include "MgmtRpc.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "esp_core_dump.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>

// ============================================================================
// 전역 인스턴스 및 세션 풀
// ============================================================================
RuntimeTimingConfig g_timing_config{};
MgmtSession g_mgmt_sessions[Config::TCP::MAX_MGMT_CLIENTS];
SemaphoreHandle_t g_mgmt_mutex = nullptr;

void TimingConfig_Load() {
  Preferences p;
  if (p.begin("timing_cfg", true)) {
    g_timing_config.ch1_poll_interval_ms = p.getUShort("ch1_poll", 1000);
    g_timing_config.ch2_cache_delay_ms   = p.getUShort("ch2_del", 30);
    g_timing_config.ch3_cache_delay_ms   = p.getUShort("ch3_del", 240);
    p.end();
  } else {
    g_timing_config.ch1_poll_interval_ms = 1000;
    g_timing_config.ch2_cache_delay_ms   = 30;
    g_timing_config.ch3_cache_delay_ms   = 240;
  }

  // 안전 범위 클램핑
  if (g_timing_config.ch1_poll_interval_ms < 200 || g_timing_config.ch1_poll_interval_ms > 5000)
    g_timing_config.ch1_poll_interval_ms = 1000;
  if (g_timing_config.ch2_cache_delay_ms < 5 || g_timing_config.ch2_cache_delay_ms > 300)
    g_timing_config.ch2_cache_delay_ms = 30;
  if (g_timing_config.ch3_cache_delay_ms < 20 || g_timing_config.ch3_cache_delay_ms > 1000)
    g_timing_config.ch3_cache_delay_ms = 240;

  ::Serial.printf("[TIMING] Loaded: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
                  g_timing_config.ch1_poll_interval_ms,
                  g_timing_config.ch2_cache_delay_ms,
                  g_timing_config.ch3_cache_delay_ms);
}

void TimingConfig_Save() {
  Preferences p;
  if (p.begin("timing_cfg", false)) {
    p.putUShort("ch1_poll", g_timing_config.ch1_poll_interval_ms);
    p.putUShort("ch2_del", g_timing_config.ch2_cache_delay_ms);
    p.putUShort("ch3_del", g_timing_config.ch3_cache_delay_ms);
    p.end();
    ::Serial.printf("[TIMING] Saved to NVS: CH1 Poll %u ms, CH2 Delay %u ms, CH3 Delay %u ms\r\n",
                    g_timing_config.ch1_poll_interval_ms,
                    g_timing_config.ch2_cache_delay_ms,
                    g_timing_config.ch3_cache_delay_ms);
  }
}

void Mgmt_Init() {
  if (!g_mgmt_mutex) {
    g_mgmt_mutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    g_mgmt_sessions[i].sock = -1;
    g_mgmt_sessions[i].len = 0;
    g_mgmt_sessions[i].connected_at_ms = 0;
  }
  TimingConfig_Load();
}

// JSON-RPC 디스패처
// ============================================================================
static inline const char *findJsonStringValue(const char *json, const char *key, char *out_val, size_t max_len) {
  if (!json || !key || !out_val || max_len == 0) return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\"') {
    if (*p == '\"') { p++; break; }
    p++;
  }
  size_t idx = 0;
  while (*p && *p != '\"' && *p != '\r' && *p != '\n' && idx + 1 < max_len) {
    out_val[idx++] = *p++;
  }
  out_val[idx] = '\0';
  return out_val;
}

static inline long findJsonIntValue(const char *json, const char *key, long default_val = -1) {
  if (!json || !key) return default_val;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return default_val;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  char *endp = nullptr;
  long val = strtol(p, &endp, 10);
  if (endp == p) return default_val;
  return val;
}

void Mgmt_DispatchJsonRpc(int sock, const char *json_str) {
  if (sock < 0 || !json_str) return;

  char cmd[64] = {0};
  if (!findJsonStringValue(json_str, "cmd", cmd, sizeof(cmd))) {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing 'cmd' field\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // 1. get_telemetry
  if (strcasecmp(cmd, "get_telemetry") == 0) {
    static char tel_buf[3072];
    tel_buf[0] = '\0';
    AppendBuf ab{tel_buf, sizeof(tel_buf)};
    Mgmt_SerializeTelemetry(ab);
    ab.append("\n");
    send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
    g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // 2. set_profile
  if (strcasecmp(cmd, "set_profile") == 0) {
    long slot = findJsonIntValue(json_str, "slot", -1);
    if (slot >= 0 && slot < static_cast<long>(ProfileRepository::MAX_PROFILES)) {
      ProfileRepository::setActiveProfileIndex(static_cast<size_t>(slot));
      g_config.wallpad_profile = static_cast<uint8_t>(slot);
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Profile updated\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid profile slot (0~3)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 3. save_auto_to_slot
  if (strcasecmp(cmd, "save_auto_to_slot") == 0) {
    char name_buf[36] = {0};
    findJsonStringValue(json_str, "name", name_buf, sizeof(name_buf));
    if (strlen(name_buf) == 0) strncpy(name_buf, "Saved Custom", sizeof(name_buf) - 1);

    size_t saved_slot = 1;
    if (ProfileRepository::saveCurrentAutoAs(name_buf, saved_slot)) {
      char resp[96];
      snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\"}\n",
               static_cast<unsigned>(saved_slot));
      send(sock, resp, strlen(resp), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Failed to save auto profile (Auto not locked or slots full)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 4. set_timing
  if (strcasecmp(cmd, "set_timing") == 0) {
    long ch1_poll = findJsonIntValue(json_str, "ch1_poll_intvl", -1);
    long ch2_del  = findJsonIntValue(json_str, "ch2_delay", -1);
    long ch3_del  = findJsonIntValue(json_str, "ch3_delay", -1);

    bool updated = false;
    if (ch1_poll >= 50 && ch1_poll <= 5000) {
      g_timing_config.ch1_poll_interval_ms = static_cast<uint16_t>(ch1_poll);
      updated = true;
    }
    if (ch2_del >= 0 && ch2_del <= 500) {
      g_timing_config.ch2_cache_delay_ms = static_cast<uint16_t>(ch2_del);
      updated = true;
    }
    if (ch3_del >= 0 && ch3_del <= 1000) {
      g_timing_config.ch3_cache_delay_ms = static_cast<uint16_t>(ch3_del);
      updated = true;
    }

    if (updated) {
      TimingConfig_Save();
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Timing config updated & saved to NVS\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"No valid timing parameters provided\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 5. cache_sync
  if (strcasecmp(cmd, "cache_sync") == 0) {
    Cache_SaveToNvs();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Warm cache synced to NVS\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 6. cache_purge_rescan
  if (strcasecmp(cmd, "cache_purge_rescan") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Auto-probing reset and cache purged, bus rescan triggered\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 6.1. wallpad_reset (Explicit Wallpad Auto-probing Reset)
  if (strcasecmp(cmd, "wallpad_reset") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Wallpad auto-probing and framing reset completed\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 7. clear_coredump
  if (strcasecmp(cmd, "clear_coredump") == 0) {
    esp_core_dump_image_erase();
    g_coredump_info.valid = false;
    memset(&g_coredump_info, 0, sizeof(g_coredump_info));
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Flash core dump erased\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 8. clear_reboot_logs
  if (strcasecmp(cmd, "clear_reboot_logs") == 0) {
    LogManager::clearRebootLog();
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Reboot logs cleared from NVS\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 9. wifi_scan (Top 4 Strongest SSIDs with signal percentage, skipping empty/hidden)
  if (strcasecmp(cmd, "wifi_scan") == 0) {
    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) {
      WiFi.scanDelete();
      const char *no_ap_msg = "{\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":\"No Networks Found\"}\n";
      send(sock, no_ap_msg, strlen(no_ap_msg), MSG_DONTWAIT);
      return;
    }

    // 인덱스 배열 정렬 (RSSI 내림차순)
    std::vector<int> indices(n);
    for (int i = 0; i < n; ++i) indices[i] = i;
    std::sort(indices.begin(), indices.end(), [](int a, int b) {
      return WiFi.RSSI(a) > WiFi.RSSI(b);
    });

    struct ApInfo {
      String ssid;
      int pct;
    };
    std::vector<ApInfo> top_aps;
    top_aps.reserve(4);

    for (int idx : indices) {
      String s = WiFi.SSID(idx);
      s.trim();
      if (s.length() == 0) continue;

      // 중복 SSID 방지
      bool duplicate = false;
      for (const auto &item : top_aps) {
        if (item.ssid == s) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) continue;

      int rssi = WiFi.RSSI(idx);
      int pct = std::min(100, std::max(0, 2 * (rssi + 100)));
      s.replace("\"", "\\\""); // JSON escape
      top_aps.push_back({s, pct});
      if (top_aps.size() >= 4) break;
    }
    WiFi.scanDelete();

    char resp[512];
    int offset = snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                          n, static_cast<unsigned>(top_aps.size()));
    for (size_t i = 0; i < top_aps.size(); ++i) {
      offset += snprintf(resp + offset, sizeof(resp) - offset,
                         "%s{\"ssid\":\"%s\",\"pct\":%d}",
                         (i > 0 ? "," : ""),
                         top_aps[i].ssid.c_str(),
                         top_aps[i].pct);
      if (offset >= (int)sizeof(resp) - 8) break;
    }
    snprintf(resp + offset, sizeof(resp) - offset, "]}\n");
    send(sock, resp, strlen(resp), MSG_DONTWAIT);
    return;
  }

  // 10. start_http_ota (Cloud OTA from GitHub Release)
  if (strcasecmp(cmd, "start_ota") == 0) {
    char url[256] = {0};
    findJsonStringValue(json_str, "url", url, sizeof(url));
    Mgmt_StartHttpOta(url[0] ? url : DEFAULT_CLOUD_OTA_URL);
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Cloud HTTP OTA started in background\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // 10. system_reboot
  if (strcasecmp(cmd, "system_reboot") == 0) {
    char reason[48] = "ST Remote Reboot";
    findJsonStringValue(json_str, "reason", reason, sizeof(reason));
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"System rebooting...\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    vTaskDelay(pdMS_TO_TICKS(100));
    System_Restart(reason);
    return;
  }

  // 11. set_wifi_mode
  if (strcasecmp(cmd, "set_wifi_mode") == 0) {
    char mode_buf[16] = {0};
    if (findJsonStringValue(json_str, "mode", mode_buf, sizeof(mode_buf))) {
      if (strcasecmp(mode_buf, "AP") == 0) {
        WiFi.mode(WIFI_AP);
      } else if (strcasecmp(mode_buf, "AP_STA") == 0 || strcasecmp(mode_buf, "AP+STA") == 0) {
        WiFi.mode(WIFI_AP_STA);
      } else {
        WiFi.mode(WIFI_STA);
      }
      const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Wi-Fi mode updated\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    } else {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing mode parameter\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    }
    return;
  }

  // 12. set_wifi
  if (strcasecmp(cmd, "set_wifi") == 0) {
    char new_ssid[64] = {0};
    char new_pass[64] = {0};
    bool has_ssid = findJsonStringValue(json_str, "ssid", new_ssid, sizeof(new_ssid));
    bool has_pass = findJsonStringValue(json_str, "password", new_pass, sizeof(new_pass));

    if (!has_ssid || strlen(new_ssid) == 0) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing or empty SSID\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    if (has_pass && strlen(new_pass) > 0 && strlen(new_pass) < 8) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Wi-Fi password must be at least 8 characters (or empty for open network)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    // 현재 정상 동작 중인 Wi-Fi 정보를 백업하여 15초 내 접속 실패 시 자동 롤백 준비
    strncpy(g_wifi_guard.prev_ssid, g_config.wifi_ssid, sizeof(g_wifi_guard.prev_ssid) - 1);
    strncpy(g_wifi_guard.prev_pass, g_config.wifi_password, sizeof(g_wifi_guard.prev_pass) - 1);
    g_wifi_guard.start_ms = millis();
    g_wifi_guard.testing.store(true, std::memory_order_release);

    strncpy(g_config.wifi_ssid, new_ssid, sizeof(g_config.wifi_ssid) - 1);
    strncpy(g_config.wifi_password, new_pass, sizeof(g_config.wifi_password) - 1);

    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Testing new Wi-Fi credentials (15s automatic fallback guard active)...\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    vTaskDelay(pdMS_TO_TICKS(50));

    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
    return;
  }

  // 13. set_uart (Edge Driver RS-485 Dynamic Configuration)
  if (strcasecmp(cmd, "set_uart") == 0) {
    long ch = findJsonIntValue(json_str, "ch", 0);
    long baud = findJsonIntValue(json_str, "baud", 0);
    char format[16] = {0};
    findJsonStringValue(json_str, "format", format, sizeof(format));

    if (ch >= 1 && ch <= 4 && baud >= 1200 && baud <= 921600 && format[0]) {
      if (System_ApplyUartConfig(static_cast<uint8_t>(ch), static_cast<uint32_t>(baud), format)) {
        char ok_msg[128];
        snprintf(ok_msg, sizeof(ok_msg),
                 "{\"res\":\"ok\",\"msg\":\"CH%ld UART updated to %ld %s and saved to NVS\"}\n",
                 ch, baud, format);
        send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
        return;
      }
    }
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid ch (1-4), baud (1200-921600), or format (8N1/8E1/8O1/8N2)\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // 14. doorphone_action (SmartThings / RPC Doorphone 3-Step Sequence Controller)
  if (strcasecmp(cmd, "doorphone_action") == 0) {
    char action_buf[32] = {0};
    findJsonStringValue(json_str, "action", action_buf, sizeof(action_buf));

    bool is_open_front = (strcasecmp(action_buf, "open_front") == 0 || strcasecmp(action_buf, "open") == 0);
    bool is_open_lobby = (strcasecmp(action_buf, "open_lobby") == 0);

    if (is_open_front || is_open_lobby) {
      uint8_t dp_stx = g_doorphone_tracker.candidate_stx.load(std::memory_order_relaxed);
      uint8_t dp_etx = g_doorphone_tracker.candidate_etx.load(std::memory_order_relaxed);
      uint8_t dp_len = g_doorphone_tracker.candidate_len.load(std::memory_order_relaxed);

      if (dp_stx == 0) dp_stx = 0x7F;
      if (dp_etx == 0) dp_etx = 0xEE;

      const Config::Doorphone::DoorphoneProfile *dp_prof =
          Config::Doorphone::matchDoorphoneCatalog(dp_stx, dp_etx, dp_len);

      uint8_t op_call = is_open_front ? (dp_prof ? dp_prof->call_front : 0xB9)
                                      : (dp_prof ? dp_prof->call_lobby : 0x5F);
      uint8_t op_open = is_open_front ? (dp_prof ? dp_prof->open_front : 0xB4)
                                      : (dp_prof ? dp_prof->open_lobby : 0x61);
      uint8_t op_end  = is_open_front ? (dp_prof ? dp_prof->end_front  : 0xB8)
                                      : (dp_prof ? dp_prof->end_lobby  : 0x60);

      // Structure for task parameters
      struct DpTaskArgs {
        uint8_t c_stx;
        uint8_t c_etx;
        uint8_t c_call;
        uint8_t c_open;
        uint8_t c_end;
      };

      DpTaskArgs *args = new DpTaskArgs{dp_stx, dp_etx, op_call, op_open, op_end};

      xTaskCreate([](void *param) {
        DpTaskArgs *a = reinterpret_cast<DpTaskArgs *>(param);
        uint8_t c_call = a->c_call;
        uint8_t c_open = a->c_open;
        uint8_t c_end  = a->c_end;
        uint8_t c_stx  = a->c_stx;
        uint8_t c_etx  = a->c_etx;
        delete a;

        auto send_dp = [c_stx, c_etx](uint8_t op) {
          StaticPacket pkt{4, 5};
          pkt.data[0] = c_stx;
          pkt.data[1] = op;
          pkt.data[2] = 0x00;
          pkt.data[3] = 0x00;
          pkt.data[4] = c_etx;
          if (g_ch4_passthrough_queue) {
            xQueueSend(g_ch4_passthrough_queue, &pkt, 0);
          }
        };

        // 1단계: 통화 시작
        send_dp(c_call);
        vTaskDelay(pdMS_TO_TICKS(350));

        // 2단계: 문열림
        send_dp(c_open);
        vTaskDelay(pdMS_TO_TICKS(750));

        // 3단계: 통화 종료
        send_dp(c_end);

        // 초인종 벨 플래그 리셋
        g_doorphone_state.front_bell.store(false, std::memory_order_release);
        g_doorphone_state.lobby_bell.store(false, std::memory_order_release);

        vTaskDelete(nullptr);
      }, "DP_Seq", 2048, args, 2, nullptr);

      char ok_msg[128];
      snprintf(ok_msg, sizeof(ok_msg),
               "{\"res\":\"ok\",\"action\":\"%s\",\"msg\":\"Doorphone sequence triggered (Call -> Open -> End)\"}\n",
               action_buf);
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
      return;
    }

    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Unknown doorphone action (Use open_front or open_lobby)\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // 15. set_ew11 (CH5 EW11 Multi-Client Slot Configuration)
  if (strcasecmp(cmd, "set_ew11") == 0) {
    long slot = findJsonIntValue(json_str, "slot", -1);
    long port = findJsonIntValue(json_str, "port", 0);
    char ip[32] = {0};
    char name[16] = {0};
    findJsonStringValue(json_str, "ip", ip, sizeof(ip));
    findJsonStringValue(json_str, "name", name, sizeof(name));

    // enabled 여부 판별 (명시적 enabled 필드가 있거나, ip가 제공되면 true)
    int en_val = findJsonIntValue(json_str, "enabled", -1);
    bool enabled = (en_val == 1) || (en_val == -1 && strlen(ip) > 0);

    if (slot >= 0 && slot < Config::TCP::MAX_EW11_SLOTS) {
      uint16_t def_slot_port = Config::TCP::EW11_SLOT_PORTS[slot];
      uint16_t target_port = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : (g_ew11_slots[slot].target_port > 0 ? g_ew11_slots[slot].target_port : def_slot_port);
      if (target_port == 8899) target_port = def_slot_port; // 구버전 8899 기본값 보정
      if (Ew11_SetSlot(static_cast<uint8_t>(slot), enabled, ip[0] ? ip : nullptr, target_port, name[0] ? name : nullptr)) {
        char ok_msg[192];
        snprintf(ok_msg, sizeof(ok_msg),
                 "{\"res\":\"ok\",\"slot\":%ld,\"enabled\":%s,\"ip\":\"%s\",\"port\":%u,\"msg\":\"EW11 slot %ld updated & saved to NVS\"}\n",
                 slot, enabled ? "true" : "false", ip, target_port, slot);
        send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
        return;
      }
    }
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid EW11 slot (0-4) or parameters\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // 16. get_locked_devices (Query all verified & LOCKED devices)
  if (strcasecmp(cmd, "get_locked_devices") == 0) {
    static char dev_buf[4096];
    dev_buf[0] = '\0';
    AppendBuf ab{dev_buf, sizeof(dev_buf)};
    Mgmt_SerializeLockedDevices(ab);
    send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
    g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // 17. device_control (Bidirectional Control Assembly & Safe Dispatch)
  if (strcasecmp(cmd, "device_control") == 0) {
    long dev_id = findJsonIntValue(json_str, "dev_id", 0);
    long sub1 = findJsonIntValue(json_str, "sub1", 0);
    long sub2 = findJsonIntValue(json_str, "sub2", 0);
    char act_str[32] = {0};
    findJsonStringValue(json_str, "action", act_str, sizeof(act_str));
    long val = findJsonIntValue(json_str, "value", 0);

    if (dev_id <= 0 || dev_id > 255) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid or missing dev_id\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    const GroupControlTemplate *grp = g_control_registry.findGroup(static_cast<uint8_t>(dev_id));
    if (!grp || grp->status != GroupControlTemplate::Status::LOCKED) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Device is not registered or not LOCKED\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    ControlActionType act = ControlActionType::UNKNOWN;
    if (strcasecmp(act_str, "power") == 0) act = ControlActionType::POWER;
    else if (strcasecmp(act_str, "set_temp") == 0) act = ControlActionType::SET_TEMP;
    else if (strcasecmp(act_str, "fan_speed") == 0) act = ControlActionType::FAN_SPEED;
    else if (strcasecmp(act_str, "valve_close") == 0) act = ControlActionType::VALVE_CLOSE;
    else if (strcasecmp(act_str, "momentary") == 0) act = ControlActionType::MOMENTARY_TRIGGER;

    if (act == ControlActionType::UNKNOWN) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid action (power/set_temp/fan_speed/valve_close/momentary)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    StaticPacket req{};
    if (!g_control_registry.buildControlPacket(static_cast<uint8_t>(dev_id),
                                              static_cast<uint8_t>(sub1),
                                              static_cast<uint8_t>(sub2),
                                              act, static_cast<int>(val), req)) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Failed to build control packet (blueprint missing or forbidden action)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    req.channel_id = 6;
    StaticPacket dummy{};
    g_control_dispatcher.dispatch(req, dummy);

    if (act == ControlActionType::SET_TEMP) {
      DeviceStateEntry *dev = g_device_repo.findMutable(static_cast<uint8_t>(dev_id), static_cast<uint8_t>(sub1), static_cast<uint8_t>(sub2), false);
      if (dev) {
        dev->last_target_temp = static_cast<uint8_t>(val);
      }
    }

    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Control packet dispatched\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  // Unknown Command
  const char *unk_msg = "{\"res\":\"error\",\"msg\":\"Unknown command\"}\n";
  send(sock, unk_msg, strlen(unk_msg), MSG_DONTWAIT);
}

// ============================================================================
// Mgmt TCP 세션 데이터 수신 핸들러 (스트림 프레이밍)
// ============================================================================
void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0) return;

  g_pkt_stats.ch6.rx_pkts.fetch_add(1, std::memory_order_relaxed);

  if (s->len + len > sizeof(s->buffer)) {
    s->len = 0; // 버퍼 오버플로우 방어
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;

  size_t p = 0;
  while (p < s->len) {
    if (s->buffer[p] == '\n' || s->buffer[p] == '\r') {
      s->buffer[p] = '\0';
      if (p > 0) {
        // 완전한 한 줄 JSON 수신!
        Mgmt_DispatchJsonRpc(s->sock, reinterpret_cast<const char *>(s->buffer));
      }
      // 앞선 라인 소진 후 시프트
      size_t rem = s->len - (p + 1);
      if (rem > 0) {
        memmove(s->buffer, s->buffer + p + 1, rem);
      }
      s->len = rem;
      p = 0;
      continue;
    }
    p++;
  }
}

// ============================================================================
// CH7 실시간 도어폰 이벤트 브로드캐스트 (Server Push)
// ============================================================================
