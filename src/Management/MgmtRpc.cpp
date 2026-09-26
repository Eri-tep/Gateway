#include "MgmtRpc.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "ProfileMatcher.h"
#include "esp_core_dump.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>

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

namespace {

struct DoorphoneFsm {
  enum class Step : uint8_t { IDLE = 0, CALL_SENT, OPEN_SENT };
  std::atomic<Step> step{Step::IDLE};
  uint8_t c_stx{0x7F};
  uint8_t c_etx{0xEE};
  uint8_t op_open{0};
  uint8_t op_end{0};
  esp_timer_handle_t timer{nullptr};
};

static DoorphoneFsm s_dp_fsm;

static void sendDpPacket(uint8_t stx, uint8_t op, uint8_t etx) {
  StaticPacket pkt{4, 5};
  pkt.data[0] = stx;
  pkt.data[1] = op;
  pkt.data[2] = 0x00;
  pkt.data[3] = 0x00;
  pkt.data[4] = etx;
  if (g_ch4_passthrough_queue) {
    xQueueSend(g_ch4_passthrough_queue, &pkt, 0);
  }
}

static void onDoorphoneTimer(void *arg) {
  (void)arg;
  DoorphoneFsm::Step cur = s_dp_fsm.step.load(std::memory_order_acquire);
  if (cur == DoorphoneFsm::Step::CALL_SENT) {
    sendDpPacket(s_dp_fsm.c_stx, s_dp_fsm.op_open, s_dp_fsm.c_etx);
    s_dp_fsm.step.store(DoorphoneFsm::Step::OPEN_SENT, std::memory_order_release);
    esp_timer_start_once(s_dp_fsm.timer, 750000); // 750ms 후 종료 패킷 전송
  } else if (cur == DoorphoneFsm::Step::OPEN_SENT) {
    sendDpPacket(s_dp_fsm.c_stx, s_dp_fsm.op_end, s_dp_fsm.c_etx);
    g_doorphone_state.front_bell.store(false, std::memory_order_release);
    g_doorphone_state.lobby_bell.store(false, std::memory_order_release);
    s_dp_fsm.step.store(DoorphoneFsm::Step::IDLE, std::memory_order_release);
  }
}

} // anonymous namespace

void Mgmt_Init() {
  if (!g_mgmt_mutex) {
    g_mgmt_mutex = xSemaphoreCreateMutex();
  }
  for (size_t i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    g_mgmt_sessions[i].sock = -1;
    g_mgmt_sessions[i].len = 0;
    g_mgmt_sessions[i].connected_at_ms = 0;
  }
  if (!s_dp_fsm.timer) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = onDoorphoneTimer;
    timer_args.name = "dp_fsm_timer";
    esp_timer_create(&timer_args, &s_dp_fsm.timer);
  }
  TimingConfig_Load();
}

static inline const char *findJsonStringValue(const char *json, const char *key, char *out_val, size_t max_len) {
  if (!json || !key || !out_val || max_len == 0) return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = strstr(json, pattern);
  if (!p) return nullptr;
  p += strlen(pattern);
  while (*p == ' ' || *p == ':' || *p == '\t') p++;
  if (*p != '\"') return nullptr; // 문자열 시작 따옴표 필수 확인
  p++;
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

static void sendRpcResponse(int sock, long req_id, const char *res, const char *msg = nullptr) {
  char buf[160];
  if (req_id >= 0) {
    if (msg) {
      snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"msg\":\"%s\",\"id\":%ld}\n", res, msg, req_id);
    } else {
      snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"id\":%ld}\n", res, req_id);
    }
  } else {
    if (msg) {
      snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"msg\":\"%s\"}\n", res, msg);
    } else {
      snprintf(buf, sizeof(buf), "{\"res\":\"%s\"}\n", res);
    }
  }
  send(sock, buf, strlen(buf), MSG_DONTWAIT);
}

void Mgmt_DispatchJsonRpc(int sock, const char *json_str) {
  if (sock < 0 || !json_str) return;

  char cmd[64] = {0};
  if (!findJsonStringValue(json_str, "c", cmd, sizeof(cmd))) {
    findJsonStringValue(json_str, "cmd", cmd, sizeof(cmd));
  }
  if (cmd[0] == '\0') {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Missing 'cmd' or 'c' field\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  long req_id = findJsonIntValue(json_str, "id", -1);

  if (strcasecmp(cmd, "get_telemetry") == 0) {
    static char tel_buf[3072];
    tel_buf[0] = '\0';
    AppendBuf ab{tel_buf, sizeof(tel_buf)};
    Mgmt_SerializeTelemetry(ab, req_id);
    ab.append("\n");
    send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
    g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (strcasecmp(cmd, "set_profile") == 0) {
    long slot = findJsonIntValue(json_str, "slot", -1);
    if (slot >= 0 && slot < static_cast<long>(ProfileRepository::MAX_PROFILES)) {
      ProfileRepository::setActiveProfileIndex(static_cast<size_t>(slot));
      g_config.wallpad_profile = static_cast<uint8_t>(slot);
      sendRpcResponse(sock, req_id, "ok", "Profile updated");
    } else {
      sendRpcResponse(sock, req_id, "error", "Invalid profile slot (0~3)");
    }
    return;
  }

  if (strcasecmp(cmd, "save_auto_to_slot") == 0) {
    char name_buf[36] = {0};
    findJsonStringValue(json_str, "name", name_buf, sizeof(name_buf));
    if (strlen(name_buf) == 0) strncpy(name_buf, "Saved Custom", sizeof(name_buf) - 1);

    size_t saved_slot = 1;
    if (ProfileRepository::saveCurrentAutoAs(name_buf, saved_slot)) {
      char resp[96];
      if (req_id >= 0) {
        snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\",\"id\":%ld}\n",
                 static_cast<unsigned>(saved_slot), req_id);
      } else {
        snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\"}\n",
                 static_cast<unsigned>(saved_slot));
      }
      send(sock, resp, strlen(resp), MSG_DONTWAIT);
    } else {
      sendRpcResponse(sock, req_id, "error", "Failed to save auto profile (Auto not locked or slots full)");
    }
    return;
  }

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
      sendRpcResponse(sock, req_id, "ok", "Timing config updated & saved to NVS");
    } else {
      sendRpcResponse(sock, req_id, "error", "No valid timing parameters provided");
    }
    return;
  }

  if (strcasecmp(cmd, "cache_sync") == 0) {
    Cache_SaveToNvs();
    sendRpcResponse(sock, req_id, "ok", "Warm cache synced to NVS");
    return;
  }

  if (strcasecmp(cmd, "ping") == 0) {
    char pong_msg[64];
    if (req_id >= 0) {
      snprintf(pong_msg, sizeof(pong_msg), "{\"res\":\"ok\",\"pong\":true,\"id\":%ld}\n", req_id);
    } else {
      snprintf(pong_msg, sizeof(pong_msg), "{\"res\":\"ok\",\"pong\":true}\n");
    }
    send(sock, pong_msg, strlen(pong_msg), MSG_DONTWAIT);
    return;
  }

  if (strcasecmp(cmd, "cache_purge_rescan") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    g_probe_convergence_reset.store(true, std::memory_order_release);
    sendRpcResponse(sock, req_id, "ok", "Auto-probing reset and cache purged, bus rescan triggered");
    return;
  }

  if (strcasecmp(cmd, "wallpad_reset") == 0) {
    g_auto_probing_engine.reset();
    g_doorphone_tracker.clearNvs();
    g_polling_targets.clear();
    g_device_repo.clear();
    g_probe_convergence_reset.store(true, std::memory_order_release);
    sendRpcResponse(sock, req_id, "ok", "Wallpad auto-probing and framing reset completed");
    return;
  }

  if (strcasecmp(cmd, "clear_coredump") == 0) {
    esp_core_dump_image_erase();
    g_coredump_info.valid = false;
    memset(&g_coredump_info, 0, sizeof(g_coredump_info));
    sendRpcResponse(sock, req_id, "ok", "Flash core dump erased");
    return;
  }

  if (strcasecmp(cmd, "clear_reboot_logs") == 0) {
    LogManager::clearRebootLog();
    sendRpcResponse(sock, req_id, "ok", "Reboot logs cleared from NVS");
    return;
  }

  if (strcasecmp(cmd, "wifi_scan") == 0) {
    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) {
      WiFi.scanDelete();
      char no_ap_msg[128];
      if (req_id >= 0) {
        snprintf(no_ap_msg, sizeof(no_ap_msg),
                 "{\"id\":%ld,\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":\"No Networks Found\"}\n", req_id);
      } else {
        snprintf(no_ap_msg, sizeof(no_ap_msg),
                 "{\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":\"No Networks Found\"}\n");
      }
      send(sock, no_ap_msg, strlen(no_ap_msg), MSG_DONTWAIT);
      return;
    }

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
    int offset = 0;
    if (req_id >= 0) {
      offset = snprintf(resp, sizeof(resp), "{\"id\":%ld,\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                        req_id, n, static_cast<unsigned>(top_aps.size()));
    } else {
      offset = snprintf(resp, sizeof(resp), "{\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                        n, static_cast<unsigned>(top_aps.size()));
    }
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

  if (strcasecmp(cmd, "start_ota") == 0) {
    char url[256] = {0};
    findJsonStringValue(json_str, "url", url, sizeof(url));
    Mgmt_StartHttpOta(url[0] ? url : DEFAULT_CLOUD_OTA_URL);
    const char *ok_msg = "{\"res\":\"ok\",\"msg\":\"Cloud HTTP OTA started in background\"}\n";
    send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
    return;
  }

  if (strcasecmp(cmd, "system_reboot") == 0) {
    char reason_buf[64] = {0};
    findJsonStringValue(json_str, "reason", reason_buf, sizeof(reason_buf));
    sendRpcResponse(sock, req_id, "ok");
    vTaskDelay(pdMS_TO_TICKS(100));
    System_Restart(reason_buf[0] ? reason_buf : "RPC Requested Reboot");
    return;
  }

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
      sendRpcResponse(sock, req_id, "ok");
    } else {
      sendRpcResponse(sock, req_id, "error", "Missing mode parameter");
    }
    return;
  }

  if (strcasecmp(cmd, "set_wifi") == 0) {
    char new_ssid[64] = {0};
    char new_pass[64] = {0};
    bool has_ssid = findJsonStringValue(json_str, "ssid", new_ssid, sizeof(new_ssid));
    bool has_pass = findJsonStringValue(json_str, "password", new_pass, sizeof(new_pass));

    if (!has_ssid || strlen(new_ssid) == 0) {
      sendRpcResponse(sock, req_id, "error", "Missing or empty SSID");
      return;
    }

    if (has_pass && strlen(new_pass) > 0 && strlen(new_pass) < 8) {
      sendRpcResponse(sock, req_id, "error", "Wi-Fi password must be at least 8 characters (or empty for open network)");
      return;
    }

    strncpy(g_wifi_guard.prev_ssid, g_config.wifi_ssid, sizeof(g_wifi_guard.prev_ssid) - 1);
    strncpy(g_wifi_guard.prev_pass, g_config.wifi_password, sizeof(g_wifi_guard.prev_pass) - 1);
    g_wifi_guard.start_ms = millis();
    g_wifi_guard.testing.store(true, std::memory_order_release);

    strncpy(g_config.wifi_ssid, new_ssid, sizeof(g_config.wifi_ssid) - 1);
    strncpy(g_config.wifi_password, new_pass, sizeof(g_config.wifi_password) - 1);

    sendRpcResponse(sock, req_id, "ok");
    vTaskDelay(pdMS_TO_TICKS(50));

    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.begin(g_config.wifi_ssid, g_config.wifi_password);
    return;
  }

  if (strcasecmp(cmd, "set_uart") == 0) {
    long ch = findJsonIntValue(json_str, "ch", 0);
    long baud = findJsonIntValue(json_str, "baud", 0);
    char format[16] = {0};
    findJsonStringValue(json_str, "format", format, sizeof(format));

    if (ch >= 1 && ch <= 4 && baud >= 1200 && baud <= 921600 && format[0]) {
      if (System_ApplyUartConfig(static_cast<uint8_t>(ch), static_cast<uint32_t>(baud), format)) {
        sendRpcResponse(sock, req_id, "ok");
        return;
      }
    }
    sendRpcResponse(sock, req_id, "error", "Invalid ch (1-4), baud (1200-921600), or format (8N1/8E1/8O1/8N2)");
    return;
  }

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

      const DoorphoneSpec *dp_prof =
          ProfileMatcher::matchDoorphone(dp_stx, dp_etx, dp_len);

      uint8_t op_call = is_open_front ? (dp_prof ? dp_prof->call_front : 0xB9)
                                      : (dp_prof ? dp_prof->call_lobby : 0x5F);
      uint8_t op_open = is_open_front ? (dp_prof ? dp_prof->open_front : 0xB4)
                                      : (dp_prof ? dp_prof->open_lobby : 0x61);
      uint8_t op_end  = is_open_front ? (dp_prof ? dp_prof->end_front  : 0xB8)
                                      : (dp_prof ? dp_prof->end_lobby  : 0x60);

      DoorphoneFsm::Step expected = DoorphoneFsm::Step::IDLE;
      if (!s_dp_fsm.step.compare_exchange_strong(expected, DoorphoneFsm::Step::CALL_SENT)) {
        sendRpcResponse(sock, req_id, "busy", "Doorphone sequence already in progress");
        return;
      }

      s_dp_fsm.c_stx = dp_stx;
      s_dp_fsm.c_etx = dp_etx;
      s_dp_fsm.op_open = op_open;
      s_dp_fsm.op_end = op_end;

      sendDpPacket(dp_stx, op_call, dp_etx);
      esp_timer_start_once(s_dp_fsm.timer, 350000); // 350ms 후 문열림 패킷 전송

      sendRpcResponse(sock, req_id, "ok");
      return;
    }

    sendRpcResponse(sock, req_id, "error", "Unknown doorphone action (Use open_front or open_lobby)");
    return;
  }

  if (strcasecmp(cmd, "set_ew11") == 0) {
    long slot = findJsonIntValue(json_str, "slot", -1);
    long port = findJsonIntValue(json_str, "port", -1);
    char ip[32] = {0};
    char name[16] = {0};
    findJsonStringValue(json_str, "ip", ip, sizeof(ip));
    findJsonStringValue(json_str, "name", name, sizeof(name));

    int en_val = findJsonIntValue(json_str, "enabled", -1);
    bool enabled = (en_val == 1) || (en_val == -1 && strlen(ip) > 0);

    if (slot >= 0 && slot < Config::TCP::MAX_EW11_SLOTS) {
      uint16_t def_slot_port = Config::TCP::EW11_SLOT_PORTS[slot];
      uint16_t target_port = (port > 0 && port <= 65535) ? static_cast<uint16_t>(port) : (g_hub_slots[slot].target_port > 0 ? g_hub_slots[slot].target_port : def_slot_port);
      if (target_port == 8899) target_port = def_slot_port; // 구버전 8899 기본값 보정
      if (Hub_SetSlot(static_cast<uint8_t>(slot), enabled, ip[0] ? ip : nullptr, target_port, name[0] ? name : nullptr)) {
        const char *ok_msg = "{\"res\":\"ok\"}\n";
        send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
        return;
      }
    }
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid EW11 slot (0-4) or parameters\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  if (strcasecmp(cmd, "get_locked_devices") == 0 || strcasecmp(cmd, "gld") == 0) {
    static char dev_buf[4096];
    dev_buf[0] = '\0';
    AppendBuf ab{dev_buf, sizeof(dev_buf)};
    Mgmt_SerializeLockedDevices(ab);
    send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
    g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  if (strcasecmp(cmd, "device_control") == 0 || strcasecmp(cmd, "ctl") == 0) {
    long dev_id = findJsonIntValue(json_str, "d", -1);
    if (dev_id == -1) dev_id = findJsonIntValue(json_str, "dev_id", 0);

    long sub1 = findJsonIntValue(json_str, "s1", -1);
    if (sub1 == -1) sub1 = findJsonIntValue(json_str, "sub1", 0);

    long sub2 = findJsonIntValue(json_str, "s2", -1);
    if (sub2 == -1) sub2 = findJsonIntValue(json_str, "sub2", 0);

    char act_str[32] = {0};
    if (!findJsonStringValue(json_str, "a", act_str, sizeof(act_str))) {
      findJsonStringValue(json_str, "action", act_str, sizeof(act_str));
    }

    long val = findJsonIntValue(json_str, "v", -9999);
    if (val == -9999) val = findJsonIntValue(json_str, "value", 0);

    if (dev_id <= 0 || dev_id > 255 || sub1 < 0 || sub1 > 255 || sub2 < 0 || sub2 > 255) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid or out-of-range device parameters\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    ControlActionType act = ControlActionType::UNKNOWN;
    if (strcasecmp(act_str, "power") == 0 || strcasecmp(act_str, "pwr") == 0) act = ControlActionType::POWER;
    else if (strcasecmp(act_str, "set_temp") == 0 || strcasecmp(act_str, "temp") == 0) act = ControlActionType::SET_TEMP;
    else if (strcasecmp(act_str, "fan_speed") == 0 || strcasecmp(act_str, "spd") == 0) act = ControlActionType::FAN_SPEED;
    else if (strcasecmp(act_str, "valve_close") == 0 || strcasecmp(act_str, "cls") == 0) act = ControlActionType::VALVE_CLOSE;
    else if (strcasecmp(act_str, "momentary") == 0 || strcasecmp(act_str, "mom") == 0) act = ControlActionType::MOMENTARY_TRIGGER;
    else if (strcasecmp(act_str, "vent_mode") == 0 || strcasecmp(act_str, "vnt") == 0) act = ControlActionType::VENT_MODE;

    if (act == ControlActionType::UNKNOWN) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid action (power/set_temp/fan_speed/valve_close/momentary/vent_mode)\"}\n";
      send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
      return;
    }

    const GroupControlTemplate *grp = g_control_registry.findGroup(static_cast<uint8_t>(dev_id));
    if (!grp) {
      const char *err_msg = "{\"res\":\"error\",\"msg\":\"Device is not registered in blueprint registry\"}\n";
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

    // 현대통신 환기 장치(0x2B) 전원 ON 시 게이트웨이가 자체적으로 0x43 운전 모드 조회 패킷을 연계 주입
    if (dev_id == 0x2B && act == ControlActionType::POWER && val == 1) {
      auto *parser = WallpadParserFactory::getActiveParser();
      StaticPacket qry_req{};
      qry_req.channel_id = 6;
      qry_req.length = 11;
      qry_req.data[0] = 0xF7;
      qry_req.data[1] = 0x0B;
      qry_req.data[2] = 0x01;
      qry_req.data[3] = 0x2B;
      qry_req.data[4] = 0x01; // QRY
      qry_req.data[5] = 0x43; // Category: 운전 모드
      qry_req.data[6] = 0x11;
      qry_req.data[7] = 0x00;
      qry_req.data[8] = 0x00;
      qry_req.data[9] = parser ? parser->calculateChecksum(qry_req.data.data(), 11) : 0x84;
      qry_req.data[10] = 0xEE;
      g_control_dispatcher.dispatch(qry_req, dummy);
    }

    sendRpcResponse(sock, req_id, "ok");
    return;
  }

  sendRpcResponse(sock, req_id, "error", "Unknown command");
}

void Mgmt_Data(MgmtSession *s, const uint8_t *data, size_t len) {
  if (!s || s->sock < 0 || !data || len == 0) return;

  g_pkt_stats.ch6.rx_pkts.fetch_add(1, std::memory_order_relaxed);

  if (len > sizeof(s->buffer)) {
    s->len = 0; // 단일 패킷 크기가 전체 수신 버퍼 초과 시 드롭
    return;
  }

  if (s->len + len > sizeof(s->buffer)) {
    s->len = 0; // 누적 버퍼 오버플로우 방어: 기존 미완성 데이터 플러시
  }

  std::copy(data, data + len, s->buffer + s->len);
  s->len += len;

  size_t p = 0;
  while (p < s->len) {
    if (s->buffer[p] == '\n' || s->buffer[p] == '\r') {
      s->buffer[p] = '\0';
      if (p > 0) {
        Mgmt_DispatchJsonRpc(s->sock, reinterpret_cast<const char *>(s->buffer));
      }
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

