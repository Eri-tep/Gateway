// ============================================================================
// RemoteService: Level 4 Network Remote Services
// ============================================================================

#include "L4_Services/Mgmt_Service.h"
static IPAddress s_trusted_hub_ip(0, 0, 0, 0);
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Public/Protocol_Facade.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <esp_core_dump.h>
#include <lwip/sockets.h>


void Remote_ResetTrustedHubIp() noexcept {
  s_trusted_hub_ip = IPAddress(0, 0, 0, 0);
}

IPAddress Remote_GetTrustedHubIp() noexcept {
  return s_trusted_hub_ip;
}

static bool Socket_SendAll(int sock, const void *buf, size_t len,
                           uint32_t timeout_ms = 1000) {
  if (sock < 0 || !buf || len == 0)
    return false;
  const uint8_t *p = static_cast<const uint8_t *>(buf);
  size_t total_sent = 0;
  uint32_t start_ms = millis();

  while (total_sent < len) {
    ssize_t sent = send(sock, p + total_sent, len - total_sent, MSG_DONTWAIT);
    if (sent > 0) {
      total_sent += static_cast<size_t>(sent);
      continue;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (millis() - start_ms >= timeout_ms) {
        return false;
      }
      fd_set wfds;
      FD_ZERO(&wfds);
      FD_SET(sock, &wfds);
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 20000; // 20ms
      int r = select(sock + 1, nullptr, &wfds, nullptr, &tv);
      if (r <= 0 && (millis() - start_ms >= timeout_ms)) {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

static inline const char *findJsonStringValue(const char *json, const char *key,
                                              char *out_val, size_t max_len) {
  if (!json || !key || !out_val || max_len == 0)
    return nullptr;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = json;
  while ((p = strstr(p, pattern)) != nullptr) {
    const char *after = p + strlen(pattern);
    while (*after == ' ' || *after == '\t')
      after++;
    if (*after == ':') {
      after++;
      while (*after == ' ' || *after == '\t')
        after++;
      if (*after != '\"')
        return nullptr;
      after++;
      size_t idx = 0;
      bool escaped = false;
      while (*after && *after != '\r' && *after != '\n' && idx + 1 < max_len) {
        if (!escaped && *after == '\\') {
          escaped = true;
          after++;
          continue;
        }
        if (!escaped && *after == '\"') {
          break;
        }
        escaped = false;
        out_val[idx++] = *after++;
      }
      out_val[idx] = '\0';
      return out_val;
    }
    p += strlen(pattern);
  }
  return nullptr;
}

static inline long findJsonIntValue(const char *json, const char *key,
                                    long default_val = -1) {
  if (!json || !key)
    return default_val;
  char pattern[64];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char *p = json;
  while ((p = strstr(p, pattern)) != nullptr) {
    const char *after = p + strlen(pattern);
    while (*after == ' ' || *after == '\t')
      after++;
    if (*after == ':') {
      after++;
      while (*after == ' ' || *after == '\t')
        after++;
      if (*after == '\"')
        after++;
      char *endp = nullptr;
      long val = strtol(after, &endp, 10);
      if (endp == after)
        return default_val;
      return val;
    }
    p += strlen(pattern);
  }
  return default_val;
}

static void sendRpcResponse(int sock, long req_id, const char *res,
                            const char *msg = nullptr) {
  if (sock < 0)
    return;
  char buf[384];
  int len = 0;
  if (req_id >= 0) {
    if (msg) {
      len = snprintf(buf, sizeof(buf),
                     "{\"res\":\"%s\",\"msg\":\"%s\",\"id\":%ld}\n", res, msg,
                     req_id);
    } else {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"id\":%ld}\n", res,
                     req_id);
    }
  } else {
    if (msg) {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\",\"msg\":\"%s\"}\n", res,
                     msg);
    } else {
      len = snprintf(buf, sizeof(buf), "{\"res\":\"%s\"}\n", res);
    }
  }
  if (len > 0 && static_cast<size_t>(len) < sizeof(buf)) {
    Socket_SendAll(sock, buf, len);
  }
}

// Task_Network 단일 태스크 동기 전송 환경: 직렬 응답 버퍼 단일화 (-3,072B)
static char s_mgmt_resp_buf[4096];

// ── Individual RPC Command Handlers ──

static void HandleRpc_GetTelemetry(int sock, long req_id,
                                   const char * /*json_str*/,
                                   const IPAddress & /*client_ip*/) {
  s_mgmt_resp_buf[0] = '\0';
  AppendBuf ab{s_mgmt_resp_buf, sizeof(s_mgmt_resp_buf)};
  Mgmt_SerializeTelemetry(ab, req_id);
  ab.append("\n");
  Socket_SendAll(sock, ab.buf, ab.offset);
  System_RecordCh6Tx();
}

static void HandleRpc_SetProfile(int sock, long req_id, const char *json_str,
                                 const IPAddress & /*client_ip*/) {
  long slot = findJsonIntValue(json_str, "slot", -1);
  if (slot >= 0 && slot < static_cast<long>(ProtocolDiag_GetMaxProfiles())) {
    ProtocolDiag_SetActiveProfile(static_cast<size_t>(slot));
    Config_SetWallpadProfile(static_cast<uint8_t>(slot));
    Config_SaveWallpadProfile();
    sendRpcResponse(sock, req_id, "ok", "Profile updated");
  } else {
    sendRpcResponse(sock, req_id, "error", "Invalid profile slot (0~3)");
  }
}

static void HandleRpc_SaveAutoToSlot(int sock, long req_id,
                                     const char *json_str,
                                     const IPAddress & /*client_ip*/) {
  char name_buf[36] = {0};
  findJsonStringValue(json_str, "name", name_buf, sizeof(name_buf));
  if (strlen(name_buf) == 0)
    strncpy(name_buf, "Saved Custom", sizeof(name_buf) - 1);

  size_t saved_slot = 1;
  if (ProtocolDiag_SaveCurrentProfileAs(name_buf, saved_slot)) {
    char resp[96];
    if (req_id >= 0) {
      snprintf(resp, sizeof(resp),
               "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile "
               "saved\",\"id\":%ld}\n",
               static_cast<unsigned>(saved_slot), req_id);
    } else {
      snprintf(
          resp, sizeof(resp),
          "{\"res\":\"ok\",\"saved_slot\":%u,\"msg\":\"Auto profile saved\"}\n",
          static_cast<unsigned>(saved_slot));
    }
    send(sock, resp, strlen(resp), MSG_DONTWAIT);
  } else {
    sendRpcResponse(
        sock, req_id, "error",
        "Failed to save auto profile (Auto not locked or slots full)");
  }
}

static void HandleRpc_SetTiming(int sock, long req_id, const char *json_str,
                                const IPAddress & /*client_ip*/) {
  long ch1_poll = findJsonIntValue(json_str, "ch1_poll_intvl", -1);
  long ch2_del = findJsonIntValue(json_str, "ch2_delay", -1);
  long ch3_del = findJsonIntValue(json_str, "ch3_delay", -1);

  RuntimeTimingConfig staged_timing = TimingConfig_Get();
  bool updated = false;
  if (ch1_poll >= 200 && ch1_poll <= 5000) {
    staged_timing.ch1_poll_interval_ms = static_cast<uint16_t>(ch1_poll);
    updated = true;
  }
  if (ch2_del >= 5 && ch2_del <= 300) {
    staged_timing.ch2_cache_delay_ms = static_cast<uint16_t>(ch2_del);
    updated = true;
  }
  if (ch3_del >= 20 && ch3_del <= 1000) {
    staged_timing.ch3_cache_delay_ms = static_cast<uint16_t>(ch3_del);
    updated = true;
  }

  if (updated && Config_SaveStaged(Config_Get(), staged_timing)) {
    sendRpcResponse(sock, req_id, "ok",
                    "Timing config saved to NVS. Reboot required to apply.");
  } else {
    sendRpcResponse(sock, req_id, "error",
                    "Invalid timing parameters (ch1: 200-5000, ch2: 5-300, ch3: 20-1000)");
  }
}

static void HandleRpc_CacheSync(int sock, long req_id,
                                const char * /*json_str*/,
                                const IPAddress & /*client_ip*/) {
  ProtocolDiag_WarmCacheSaveToNvs();
  sendRpcResponse(sock, req_id, "ok", "Warm cache synced to NVS");
}

static void HandleRpc_Ping(int sock, long req_id, const char * /*json_str*/,
                           const IPAddress & /*client_ip*/) {
  char pong_msg[64];
  if (req_id >= 0) {
    snprintf(pong_msg, sizeof(pong_msg),
             "{\"res\":\"ok\",\"pong\":true,\"id\":%ld}\n", req_id);
  } else {
    snprintf(pong_msg, sizeof(pong_msg), "{\"res\":\"ok\",\"pong\":true}\n");
  }
  send(sock, pong_msg, strlen(pong_msg), MSG_DONTWAIT);
}

static void HandleRpc_CachePurgeRescan(int sock, long req_id,
                                       const char * /*json_str*/,
                                       const IPAddress & /*client_ip*/) {
  ProtocolDiag_WallpadReset();
  ProtocolDiag_PollingClear();
  Device_Clear();
  ProtocolDiag_RequestRelearn();
  sendRpcResponse(sock, req_id, "ok",
                  "Auto-probing reset and cache purged, bus rescan triggered");
}

static void HandleRpc_WallpadReset(int sock, long req_id,
                                   const char * /*json_str*/,
                                   const IPAddress & /*client_ip*/) {
  ProtocolDiag_WallpadReset();
  ProtocolDiag_PollingClear();
  Device_Clear();
  ProtocolDiag_RequestRelearn();
  sendRpcResponse(sock, req_id, "ok",
                  "Wallpad auto-probing and framing reset completed");
}

static void HandleRpc_ClearCoredump(int sock, long req_id,
                                    const char * /*json_str*/,
                                    const IPAddress & /*client_ip*/) {
  esp_core_dump_image_erase();
  g_coredump_info.valid = false;
  memset(&g_coredump_info, 0, sizeof(g_coredump_info));
  sendRpcResponse(sock, req_id, "ok", "Flash core dump erased");
}

static void HandleRpc_ClearRebootLogs(int sock, long req_id,
                                      const char * /*json_str*/,
                                      const IPAddress & /*client_ip*/) {
  System_ClearRebootLog();
  sendRpcResponse(sock, req_id, "ok", "Reboot logs cleared from NVS");
}

static void HandleRpc_WifiScan(int sock, long req_id, const char * /*json_str*/,
                               const IPAddress & /*client_ip*/) {
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    WiFi.scanDelete();
    char no_ap_msg[128];
    if (req_id >= 0) {
      snprintf(no_ap_msg, sizeof(no_ap_msg),
               "{\"id\":%ld,\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":["
               "],\"msg\":\"No Networks Found\"}\n",
               req_id);
    } else {
      snprintf(no_ap_msg, sizeof(no_ap_msg),
               "{\"res\":\"ok\",\"count\":0,\"ap_count\":0,\"aps\":[],\"msg\":"
               "\"No Networks Found\"}\n");
    }
    send(sock, no_ap_msg, strlen(no_ap_msg), MSG_DONTWAIT);
    return;
  }

  constexpr int MAX_SCAN = 32;
  const int scan_limit = std::min(n, MAX_SCAN);
  std::array<int, MAX_SCAN> indices{};
  for (int i = 0; i < scan_limit; ++i)
    indices[i] = i;
  std::sort(indices.begin(), indices.begin() + scan_limit,
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });

  struct ApInfo {
    char ssid[34];
    int pct;
  };
  constexpr size_t MAX_TOP_APS = 4;
  std::array<ApInfo, MAX_TOP_APS> top_aps{};
  size_t top_aps_count = 0;

  for (int i = 0; i < scan_limit; ++i) {
    const int idx = indices[i];
    const String raw_str = WiFi.SSID(idx);
    const char *src = raw_str.c_str();
    while (*src && isspace(static_cast<unsigned char>(*src)))
      src++;
    if (!*src)
      continue;

    // 중복 SSID 검사
    bool duplicate = false;
    for (size_t a = 0; a < top_aps_count; ++a) {
      if (strcmp(top_aps[a].ssid, src) == 0) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    int rssi = WiFi.RSSI(idx);
    int pct = std::min(100, std::max(0, 2 * (rssi + 100)));

    ApInfo &info = top_aps[top_aps_count++];
    info.pct = pct;
    // JSON escape 단순 복사 (32바이트 바운드)
    size_t d_idx = 0;
    while (*src && d_idx + 2 < sizeof(info.ssid)) {
      if (*src == '"' || *src == '\\') {
        info.ssid[d_idx++] = '\\';
      }
      info.ssid[d_idx++] = *src++;
    }
    info.ssid[d_idx] = '\0';

    if (top_aps_count >= MAX_TOP_APS)
      break;
  }
  WiFi.scanDelete();

  char resp[512];
  int offset = 0;
  if (req_id >= 0) {
    offset = snprintf(
        resp, sizeof(resp),
        "{\"id\":%ld,\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
        req_id, n, static_cast<unsigned>(top_aps_count));
  } else {
    offset = snprintf(resp, sizeof(resp),
                      "{\"res\":\"ok\",\"count\":%d,\"ap_count\":%u,\"aps\":[",
                      n, static_cast<unsigned>(top_aps_count));
  }
  for (size_t i = 0; i < top_aps_count; ++i) {
    offset += snprintf(resp + offset, sizeof(resp) - offset,
                       "%s{\"ssid\":\"%s\",\"pct\":%d}", (i > 0 ? "," : ""),
                       top_aps[i].ssid, top_aps[i].pct);
    if (offset >= (int)sizeof(resp) - 8)
      break;
  }
  snprintf(resp + offset, sizeof(resp) - offset, "]}\n");
  send(sock, resp, strlen(resp), MSG_DONTWAIT);
}

static void HandleRpc_StartOta(int sock, long req_id, const char *json_str,
                               const IPAddress & /*client_ip*/) {
  char url[256] = {0};
  findJsonStringValue(json_str, "url", url, sizeof(url));
  const char *target_url = url[0] ? url : "https://raw.githubusercontent.com/Eri-tep/Gateway/main/bin/firmware.bin";
  if (strncmp(target_url, "https://", 8) != 0 || strlen(target_url) < 10) {
    sendRpcResponse(sock, req_id, "error",
                    "Invalid OTA URL: Only HTTPS allowed");
    return;
  }
  System_StartHttpOta(target_url);
  sendRpcResponse(sock, req_id, "ok", "Cloud HTTPS OTA started in background");
}

static void HandleRpc_SystemReboot(int sock, long req_id, const char *json_str,
                                   const IPAddress & /*client_ip*/) {
  char reason_buf[64] = {0};
  findJsonStringValue(json_str, "reason", reason_buf, sizeof(reason_buf));
  sendRpcResponse(sock, req_id, "ok");
  vTaskDelay(pdMS_TO_TICKS(100));
  System_Restart(reason_buf[0] ? reason_buf : "RPC Requested Reboot");
}

static void HandleRpc_SetWifiMode(int sock, long req_id, const char *json_str,
                                  const IPAddress & /*client_ip*/) {
  char mode_buf[16] = {0};
  if (findJsonStringValue(json_str, "mode", mode_buf, sizeof(mode_buf))) {
    wifi_mode_t target_mode = WIFI_STA;
    switch (Hash::fnv1a32_ci_rt(mode_buf)) {
    case Hash::fnv1a32_ci("AP"):
      target_mode = WIFI_AP;
      break;
    case Hash::fnv1a32_ci("AP_STA"):
    case Hash::fnv1a32_ci("AP+STA"):
      target_mode = WIFI_AP_STA;
      break;
    case Hash::fnv1a32_ci("STA"):
    default:
      target_mode = WIFI_STA;
      break;
    }

    WiFi.mode(target_mode);
    sendRpcResponse(sock, req_id, "ok");
  } else {
    sendRpcResponse(sock, req_id, "error", "Missing mode parameter");
  }
}

static void HandleRpc_SetWifi(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
  char new_ssid[64] = {0};
  char new_pass[64] = {0};
  bool has_ssid =
      findJsonStringValue(json_str, "ssid", new_ssid, sizeof(new_ssid));
  bool has_pass =
      findJsonStringValue(json_str, "password", new_pass, sizeof(new_pass));

  if (!has_ssid || strlen(new_ssid) == 0 || strlen(new_ssid) > 32) {
    sendRpcResponse(sock, req_id, "error", "Invalid SSID length (1-32 chars)");
    return;
  }

  auto has_bad_chars = [](const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
      unsigned char c = static_cast<unsigned char>(str[i]);
      if (c < 32 || c == 127 || c == '\r' || c == '\n')
        return true;
    }
    return false;
  };
  if (has_bad_chars(new_ssid) || (has_pass && has_bad_chars(new_pass))) {
    sendRpcResponse(sock, req_id, "error",
                    "SSID or password contains invalid control characters");
    return;
  }

  if (has_pass && strlen(new_pass) > 0 &&
      (strlen(new_pass) < 8 || strlen(new_pass) > 63)) {
    sendRpcResponse(sock, req_id, "error",
                    "Wi-Fi password must be between 8 and 63 characters (or "
                    "empty for open network)");
    return;
  }

  const auto &cfg = Config_Get();
  Remote_StartWifiFallbackTest(cfg.wifi_ssid, cfg.wifi_password);

  RuntimeConfig staged_cfg = cfg;
  strncpy(staged_cfg.wifi_ssid, new_ssid, sizeof(staged_cfg.wifi_ssid) - 1);
  strncpy(staged_cfg.wifi_password, new_pass, sizeof(staged_cfg.wifi_password) - 1);
  Config_SaveStaged(staged_cfg, TimingConfig_Get());

  sendRpcResponse(sock, req_id, "ok");
  vTaskDelay(pdMS_TO_TICKS(50));

  WiFi.disconnect(false);
  vTaskDelay(pdMS_TO_TICKS(50));
  WiFi.begin(new_ssid, new_pass);
}

static void HandleRpc_SetUart(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
  long ch = findJsonIntValue(json_str, "ch", 0);
  long baud = findJsonIntValue(json_str, "baud", 0);
  char format[16] = {0};
  findJsonStringValue(json_str, "format", format, sizeof(format));

  uint8_t db = 8, pr = 0, sb = 1;
  if (ch >= 1 && ch <= 4 && baud >= 1200 && baud <= 921600 &&
      parseFramingStr(format, db, pr, sb)) {
    if (Config_SetUartFraming(static_cast<uint8_t>(ch),
                              static_cast<uint32_t>(baud), db, pr, sb)) {
      Config_Save();
      sendRpcResponse(sock, req_id, "ok",
                      "UART configuration updated (restart recommended)");
      return;
    }
  }
  sendRpcResponse(
      sock, req_id, "error",
      "Invalid ch (1-4), baud (1200-921600), or format (8N1/8E1/8O1/8N2)");
}

static void HandleRpc_DoorphoneAction(int sock, long req_id,
                                      const char *json_str,
                                      const IPAddress & /*client_ip*/) {
  char action_buf[32] = {0};
  findJsonStringValue(json_str, "action", action_buf, sizeof(action_buf));

  bool is_lobby = false;
  bool is_valid = false;
  switch (Hash::fnv1a32_ci_rt(action_buf)) {
  case Hash::fnv1a32_ci("open_front"):
  case Hash::fnv1a32_ci("open"):
    is_valid = true;
    is_lobby = false;
    break;
  case Hash::fnv1a32_ci("open_lobby"):
    is_valid = true;
    is_lobby = true;
    break;
  default:
    break;
  }

  if (is_valid) {
    if (!Device_DoorphoneOpen(is_lobby)) {
      sendRpcResponse(sock, req_id, "busy",
                      "Doorphone sequence already in progress");
      return;
    }

    sendRpcResponse(sock, req_id, "ok");
    return;
  }

  sendRpcResponse(sock, req_id, "error",
                  "Unknown doorphone action (Use open_front or open_lobby)");
}

static void HandleRpc_SetEw11(int sock, long req_id, const char *json_str,
                              const IPAddress & /*client_ip*/) {
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
    HubClientSlotSnapshot slot_snap;
    System_GetBridgeSlotSnapshot(static_cast<uint8_t>(slot), slot_snap);
    uint16_t target_port =
        (port > 0 && port <= 65535)
            ? static_cast<uint16_t>(port)
            : (slot_snap.target_port > 0 ? slot_snap.target_port
                                         : def_slot_port);
    if (target_port == 8899)
      target_port = def_slot_port; // 구버전 8899 기본값 보정
    if (ProtocolDiag_SetBridgeSlotConfig(static_cast<uint8_t>(slot), enabled, ip[0] ? ip : nullptr,
                                   target_port, name[0] ? name : nullptr)) {
      const char *ok_msg = "{\"res\":\"ok\"}\n";
      send(sock, ok_msg, strlen(ok_msg), MSG_DONTWAIT);
      return;
    }
  }
  const char *err_msg =
      "{\"res\":\"error\",\"msg\":\"Invalid EW11 slot (0-4) or parameters\"}\n";
  send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
}

static void HandleRpc_GetDevices(int sock, long req_id,
                                 const char * /*json_str*/,
                                 const IPAddress & /*client_ip*/) {
  s_mgmt_resp_buf[0] = '\0';
  AppendBuf ab{s_mgmt_resp_buf, sizeof(s_mgmt_resp_buf)};
  Mgmt_SerializeDevices(ab, req_id);
  send(sock, ab.buf, ab.offset, MSG_DONTWAIT);
  System_RecordCh6Tx();
}

static void HandleRpc_DeviceControl(int sock, long req_id, const char *json_str,
                                    const IPAddress & /*client_ip*/) {
  long dev_id = findJsonIntValue(json_str, "d", -1);
  if (dev_id == -1)
    dev_id = findJsonIntValue(json_str, "dev_id", 0);

  long sub1 = findJsonIntValue(json_str, "s1", -1);
  if (sub1 == -1)
    sub1 = findJsonIntValue(json_str, "sub1", 0);

  long sub2 = findJsonIntValue(json_str, "s2", -1);
  if (sub2 == -1)
    sub2 = findJsonIntValue(json_str, "sub2", 0);

  char act_str[32] = {0};
  if (!findJsonStringValue(json_str, "a", act_str, sizeof(act_str))) {
    findJsonStringValue(json_str, "action", act_str, sizeof(act_str));
  }

  long val = findJsonIntValue(json_str, "v", -9999);
  if (val == -9999)
    val = findJsonIntValue(json_str, "value", 0);

  if (dev_id <= 0 || dev_id > 255 || sub1 < 0 || sub1 > 255 || sub2 < 0 ||
      sub2 > 255) {
    const char *err_msg = "{\"res\":\"error\",\"msg\":\"Invalid or "
                          "out-of-range device parameters\"}\n";
    send(sock, err_msg, strlen(err_msg), MSG_DONTWAIT);
    return;
  }

  // ── FCU (0x2C) 전용 Modbus 제어 디스패치 (AGENTS.md & MODERN_CPP_GUIDELINES
  // §1) ──
  if (dev_id == Config::FCU::DEV_ID) {
    uint8_t slot_idx = static_cast<uint8_t>(sub1);
    if (slot_idx < 1 || slot_idx >= Config::TCP::MAX_EW11_SLOTS) {
      sendRpcResponse(sock, req_id, "error", "Invalid FCU slot (1-4)");
      return;
    }

    uint16_t m = static_cast<uint16_t>(findJsonIntValue(json_str, "mode", 1));
    uint16_t f = static_cast<uint16_t>(findJsonIntValue(json_str, "fan", 4));
    uint16_t s = static_cast<uint16_t>(findJsonIntValue(json_str, "swing", 0));
    uint8_t t = static_cast<uint8_t>(findJsonIntValue(json_str, "temp", 24));

    if (Device_ControlFcu(slot_idx, act_str, static_cast<int>(val), m, f, s, t)) {
      sendRpcResponse(sock, req_id, "ok");
    } else {
      sendRpcResponse(sock, req_id, "error", "Failed to execute FCU action");
    }
    return;
  }

  ControlActionType act = ControlActionType::UNKNOWN;
  if (act_str[0] != '\0') {
    switch (Hash::fnv1a32_ci_rt(act_str)) {
    case Hash::fnv1a32_ci("power"):
    case Hash::fnv1a32_ci("pwr"):
      act = ControlActionType::POWER;
      break;
    case Hash::fnv1a32_ci("set_temp"):
    case Hash::fnv1a32_ci("temp"):
      act = ControlActionType::SET_TEMP;
      break;
    case Hash::fnv1a32_ci("fan_speed"):
    case Hash::fnv1a32_ci("spd"):
      act = ControlActionType::FAN_SPEED;
      break;
    case Hash::fnv1a32_ci("valve_close"):
    case Hash::fnv1a32_ci("cls"):
      act = ControlActionType::VALVE_CLOSE;
      break;
    case Hash::fnv1a32_ci("momentary"):
    case Hash::fnv1a32_ci("mom"):
      act = ControlActionType::MOMENTARY_TRIGGER;
      break;
    case Hash::fnv1a32_ci("vent_mode"):
    case Hash::fnv1a32_ci("vnt"):
    case Hash::fnv1a32_ci("mode"):
    case Hash::fnv1a32_ci("ac_mode"):
      act = ControlActionType::VENT_MODE;
      break;
    default:
      act = ControlActionType::UNKNOWN;
      break;
    }
  }

  if (act == ControlActionType::UNKNOWN) {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Invalid action "
        "(power/set_temp/fan_speed/valve_close/momentary/vent_mode/mode)\"}\n";
    Socket_SendAll(sock, err_msg, strlen(err_msg));
    return;
  }

  StaticPacket req{};
  if (!ProtocolDiag_BuildControlPacket(
          static_cast<uint8_t>(dev_id), static_cast<uint8_t>(sub1),
          static_cast<uint8_t>(sub2), act, static_cast<int>(val), req)) {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Failed to build control packet (ctl_spec "
        "missing or forbidden action)\"}\n";
    Socket_SendAll(sock, err_msg, strlen(err_msg));
    return;
  }

  req.channel_id = 6;
  StaticPacket dummy{};
  (void)Protocol_DispatchControl(req, dummy);

  if (act == ControlActionType::SET_TEMP) {
    Device_SetTargetTemp(
        static_cast<uint8_t>(dev_id), static_cast<uint8_t>(sub1),
        static_cast<uint8_t>(sub2), static_cast<uint8_t>(val));
  }

  // 현대통신 환기 장치(0x2B) 전원 ON 시 게이트웨이가 자체적으로 0x43 운전 모드
  // 조회 패킷을 연계 주입
  if (dev_id == 0x2B && act == ControlActionType::POWER && val == 1) {
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
    const uint16_t cs = ProtocolDiag_CalculateChecksum(qry_req.data.data(), 11);
    if (cs == kChecksumInvalid) [[unlikely]] {
      sendRpcResponse(sock, req_id, "error", "Invalid checksum algorithm state");
      return;
    }
    qry_req.data[9] = static_cast<uint8_t>(cs);
    qry_req.data[10] = 0xEE;
    (void)Protocol_DispatchControl(qry_req, dummy);
  }

  sendRpcResponse(sock, req_id, "ok");
}

// ── Hash-Driven O(1) RPC Dispatcher (FNV-1a 32-bit Jump Table) ──

using RpcHandlerFunc = void (*)(int sock, long req_id, const char *json_str,
                                const IPAddress &client_ip);

// 32-bit FNV-1a Hash aliases from L0 Foundation (System_Buffer.h)
using Hash::fnv1a32_ci;
using Hash::fnv1a32_ci_rt;
static constexpr auto fnv1a_32_ci = Hash::fnv1a32_ci;
static inline auto fnv1a_32_ci_rt = Hash::fnv1a32_ci_rt;

void Mgmt_DispatchJsonRpc(int sock, const char *json_str) {
  if (sock < 0 || !json_str)
    return;

  char cmd[64] = {0};
  if (!findJsonStringValue(json_str, "c", cmd, sizeof(cmd))) {
    findJsonStringValue(json_str, "cmd", cmd, sizeof(cmd));
  }
  if (cmd[0] == '\0') {
    const char *err_msg =
        "{\"res\":\"error\",\"msg\":\"Missing 'cmd' or 'c' field\"}\n";
    Socket_SendAll(sock, err_msg, strlen(err_msg));
    return;
  }

  const long req_id = findJsonIntValue(json_str, "id", -1);
  const IPAddress client_ip = Remote_GetClientIp(sock);
  const uint32_t cmd_hash = fnv1a_32_ci_rt(cmd);

  RpcHandlerFunc handler = nullptr;
  bool is_dangerous = false;

  switch (cmd_hash) {
  case fnv1a_32_ci("get_telemetry"):
    // SmartThings Edge Driver가 주기적으로 telemetry 요청 시 허브 IP 자동 학습 및 Lock-in
    if (client_ip != IPAddress(0, 0, 0, 0) &&
        s_trusted_hub_ip == IPAddress(0, 0, 0, 0)) {
      s_trusted_hub_ip = client_ip;
      ESP_LOGI("NET", "[RPC] Trusted Hub IP locked to %s",
               s_trusted_hub_ip.toString().c_str());
    }
    handler = HandleRpc_GetTelemetry;
    break;

  case fnv1a_32_ci("ping"):
    handler = HandleRpc_Ping;
    break;

  case fnv1a_32_ci("wifi_scan"):
    handler = HandleRpc_WifiScan;
    break;

  case fnv1a_32_ci("set_wifi_mode"):
    handler = HandleRpc_SetWifiMode;
    break;

  case fnv1a_32_ci("set_ew11"):
    handler = HandleRpc_SetEw11;
    break;

  case fnv1a_32_ci("get_devices"):
  case fnv1a_32_ci("gd"):
  case fnv1a_32_ci("get_locked_devices"):
  case fnv1a_32_ci("gld"):
    handler = HandleRpc_GetDevices;
    break;

  // ── Dangerous Commands (Require Trusted Hub Verification) ──
  case fnv1a_32_ci("set_profile"):
    handler = HandleRpc_SetProfile;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("save_auto_to_slot"):
    handler = HandleRpc_SaveAutoToSlot;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("set_timing"):
    handler = HandleRpc_SetTiming;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("cache_sync"):
    handler = HandleRpc_CacheSync;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("cache_purge_rescan"):
    handler = HandleRpc_CachePurgeRescan;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("wallpad_reset"):
    handler = HandleRpc_WallpadReset;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("clear_coredump"):
    handler = HandleRpc_ClearCoredump;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("clear_reboot_logs"):
    handler = HandleRpc_ClearRebootLogs;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("start_ota"):
    handler = HandleRpc_StartOta;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("system_reboot"):
    handler = HandleRpc_SystemReboot;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("set_wifi"):
    handler = HandleRpc_SetWifi;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("set_uart"):
    handler = HandleRpc_SetUart;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("doorphone_action"):
    handler = HandleRpc_DoorphoneAction;
    is_dangerous = true;
    break;

  case fnv1a_32_ci("device_control"):
  case fnv1a_32_ci("ctl"):
  case fnv1a_32_ci("control"):
    handler = HandleRpc_DeviceControl;
    is_dangerous = true;
    break;

  default:
    sendRpcResponse(sock, req_id, "error", "Unknown command");
    return;
  }

  if (is_dangerous) {
    // 보안 검증:
    // 1) 아직 허브가 등록되지 않은 상태(0.0.0.0)에서는 위험 명령 원천 거부 (부팅 직후 바이패스 차단)
    // 2) 등록된 허브 IP와 불일치하는 제3자의 위험 명령 차단 (탈취 및 임의 조작 방어)
    if (s_trusted_hub_ip == IPAddress(0, 0, 0, 0) ||
        client_ip != s_trusted_hub_ip) {
      sendRpcResponse(sock, req_id, "error",
                      "403 Access Denied: Unauthorized client IP");
      return;
    }
  }

  handler(sock, req_id, json_str, client_ip);
}

void Mgmt_Data(MgmtSession *s, std::span<const uint8_t> data) {
  if (!s || s->sock < 0 || data.empty())
    return;

  System_RecordCh6Rx();

  const size_t len = data.size();
  if (len > sizeof(s->buffer)) {
    s->len = 0; // 단일 패킷 크기가 전체 수신 버퍼 초과 시 드롭
    return;
  }

  if (s->len + len > sizeof(s->buffer)) {
    s->len = 0; // 누적 버퍼 오버플로우 방어: 기존 미완성 데이터 플러시
  }

  std::copy(data.begin(), data.end(), s->buffer + s->len);
  s->len += len;

  size_t p = 0;
  while (p < s->len) {
    if (s->buffer[p] == '\n' || s->buffer[p] == '\r') {
      s->buffer[p] = '\0';
      if (p > 0) {
        Mgmt_DispatchJsonRpc(s->sock,
                             reinterpret_cast<const char *>(s->buffer));
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

