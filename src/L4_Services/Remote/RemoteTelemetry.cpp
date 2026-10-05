// ============================================================================
// RemoteService: Level 4 Network Remote Services
// ============================================================================

#include "L4_Services/Remote/RemoteInternal.h"
#include "L4_Services/EW11_Service.h"
#include "L4_Services/ST_Service.h"
#include "L3_Routing/Wallpad_Protocol.h"
#include "L1_Drivers/Diagnostics_Driver.h"
#include "L1_Drivers/NVS_Driver.h"
#include "L2_Channels/TCP_CH.h"
#include "L3_Routing/Packet_Router.h"
#include "L3_Routing/Device_Registry.h"

#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <esp_core_dump.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <fcntl.h>
#include <lwip/ip.h>
#include <lwip/sockets.h>
#include <lwip/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// ── JSON-RPC & TCP Management Server (formerly Service.cpp) ──
// ============================================================================
// From src/Management/Management.cpp
// ============================================================================

// ============================================================================
// ============================================================================
// From src/Management/Telemetry.cpp
// ============================================================================

static void serializeSysMetrics(AppendBuf &out, uint8_t c0, uint8_t c1,
                                int8_t temp_c, int8_t rssi, uint32_t uptime_s,
                                uint32_t free_heap_kb,
                                uint32_t min_free_heap_kb, bool ntp_synced) {
  out.appendFormat(
      "\"system\":{\"temp_c\":%d,\"wifi_rssi\":%d,\"uptime_s\":%u,"
      "\"cpu0_load\":%u,\"cpu1_load\":%u,\"free_heap_kb\":%u,\"min_free_heap_"
      "kb\":%u,"
      "\"ntp_synced\":%s,\"firmware\":\"%s\",\"latest_firmware\":\"Ready\"},",
      static_cast<int>(temp_c), static_cast<int>(rssi), uptime_s,
      static_cast<unsigned>(c0), static_cast<unsigned>(c1), free_heap_kb,
      min_free_heap_kb, ntp_synced ? "true" : "false",
      Config::FIRMWARE_VERSION);

  wifi_mode_t cur_wmode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&cur_wmode);
  const char *mode_str =
      (cur_wmode == WIFI_MODE_AP)
          ? "AP"
          : (cur_wmode == WIFI_MODE_APSTA ? "AP_STA" : "STA");

  out.appendFormat(
      "\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"mode\":\"%s\"},",
      WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "Disconnected",
      WiFi.status() == WL_CONNECTED ? static_cast<int>(WiFi.RSSI()) : -100,
      WiFi.localIP().toString().c_str(), mode_str);

  out.appendFormat("\"ota\":{\"in_progress\":%s,\"status\":\"%s\",\"progress_"
                   "pct\":%u,\"last_error\":\"%s\"},",
                   g_http_ota_state.in_progress.load() ? "true" : "false",
                   g_http_ota_state.status, g_http_ota_state.progress_pct,
                   g_http_ota_state.last_error);
}

static void serializeProfileAndTiming(
    AppendBuf &out, const VendorProfileDescriptor &active_prof,
    const AutoProbeDescriptor &auto_desc, const char *wc_src, size_t total_devs,
    size_t online_devs, size_t stale_devs, uint32_t ch2_rx,
    uint32_t ch2_uncached) {
  char cat_match_buf[64] = "None";
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    snprintf(cat_match_buf, sizeof(cat_match_buf), "%s",
             matched_p->vendor_name);
  }

  char bp_buf[64];
  snprintf(bp_buf, sizeof(bp_buf), "%u Groups",
           static_cast<unsigned>(g_control_registry.getGroupCount()));

  bool fully_locked = (g_config.wallpad_profile != 0) ||
                      (auto_desc.is_locked && auto_desc.opcodes_locked &&
                       auto_desc.offsets_locked);

  out.appendFormat(
      "\"profile\":{\"active_slot\":%u,\"active_key\":\"%s\",\"active_name\":"
      "\"%s\","
      "\"is_locked\":%s,\"fully_locked\":%s,\"catalog_match\":\"%s\","
      "\"blueprints\":\"%s\","
      "\"stx\":\"0x%02X\",\"etx\":\"0x%02X\","
      "\"cs_algo\":\"%s\",\"opcodes\":{\"query\":\"0x%02X\",\"control\":\"0x%"
      "02X\",\"ack\":\"0x%02X\"},"
      "\"match_count\":%u},",
      static_cast<unsigned>(g_config.wallpad_profile), active_prof.key,
      active_prof.name, auto_desc.is_locked ? "true" : "false",
      fully_locked ? "true" : "false", cat_match_buf, bp_buf, auto_desc.stx,
      auto_desc.etx, AutoProbingEngine::getAlgoName(auto_desc.checksum_algo),
      auto_desc.query_opcode, auto_desc.control_opcode, auto_desc.ack_opcode,
      auto_desc.matched_packets);

  out.appendFormat("\"timing\":{\"ch1_poll_interval_ms\":%u,\"ch2_ack_delay_"
                   "ms\":%u,\"ch3_ack_delay_ms\":%u,"
                   "\"vip_preemptions\":%u,\"last_cmd_latency_ms\":%u},",
                   static_cast<unsigned>(g_timing_config.ch1_poll_interval_ms),
                   static_cast<unsigned>(g_timing_config.ch2_cache_delay_ms),
                   static_cast<unsigned>(g_timing_config.ch3_cache_delay_ms),
                   g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed),
                   22);

  out.appendFormat(
      "\"cache\":{\"source\":\"%s\",\"total_devices\":%u,\"online_devices\":%u,"
      "\"stale_devices\":%u,\"cache_hit_rate\":%.1f},",
      wc_src, static_cast<unsigned>(total_devs),
      static_cast<unsigned>(online_devs), static_cast<unsigned>(stale_devs),
      (ch2_rx > 0
           ? (100.0f - (static_cast<float>(ch2_uncached) * 100.0f / ch2_rx))
           : 100.0f));

  const char *f1 = formatFramingStr(
      g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits);
  const char *f2 = formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity,
                                    g_config.ch2_stop_bits);
  const char *f3 = formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity,
                                    g_config.ch3_stop_bits);
  const char *f4 =
      formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity,
                       g_config.doorphone_stop_bits);

  out.appendFormat("\"uart\":{"
                   "\"ch1\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch2\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch3\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch4\":{\"baud\":%u,\"format\":\"%s\"}},",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1,
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2,
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3,
                   static_cast<unsigned>(g_config.doorphone_baud_rate), f4);
}

static void serializeDiagnostics(AppendBuf &out, const char *rst_reason) {
  out.append("\"diagnostics\":{");
  out.appendFormat("\"last_reboot_reason\":\"%s\",\"rollback_detected\":%s,"
                   "\"rescue_mode\":%s,",
                   rst_reason, g_rollback_detected ? "true" : "false",
                   g_rescue_mode.load(std::memory_order_relaxed) ? "true"
                                                                 : "false");

  out.append("\"coredump\":{");
  if (g_coredump_info.valid) {
    out.appendFormat("\"valid\":true,\"task\":\"%s\",\"pc\":\"0x%08X\","
                     "\"cause\":%u,\"bt_depth\":%u,"
                     "\"summary\":\"⚠️ Crash in %s at 0x%08X (Cause %u)\"},",
                     g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause, g_coredump_info.bt_depth,
                     g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause);
  } else {
    out.append("\"valid\":false,\"task\":\"\",\"pc\":\"0x00000000\",\"cause\":"
               "0,\"bt_depth\":0,"
               "\"summary\":\"No Crash Dump (Flash Clean)\"},");
  }

  out.append("\"reboot_logs\":[");
  size_t log_cnt = LogManager::getLogCount();
  size_t max_logs_to_emit = (log_cnt > 5) ? 5 : log_cnt;
  for (size_t i = 0; i < max_logs_to_emit; i++) {
    LogEntry e{};
    if (LogManager::getLogEntry(i, e)) {
      char time_buf[32] = "N/A";
      if (e.timestamp > 0) {
        struct tm timeinfo;
        time_t sec = static_cast<time_t>(e.timestamp);
        localtime_r(&sec, &timeinfo);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      }
      uint32_t up_s = e.stats_snapshot.uptime_ms / 1000;
      char up_str[24];
      snprintf(up_str, sizeof(up_str), "%02uh %02um", up_s / 3600,
               (up_s % 3600) / 60);

      if (i > 0)
        out.append(",");
      out.appendFormat(
          "{\"id\":%u,\"time\":\"%s\",\"reason\":\"%s\",\"uptime\":\"%s\"}",
          static_cast<unsigned>(i + 1), time_buf, e.reason, up_str);
    }
  }
  out.append("],");

  bool f_bell = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
  bool l_bell = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
  uint32_t b_ms =
      g_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  out.appendFormat(
      "\"doorphone\":{\"front_bell\":%s,\"lobby_bell\":%s,\"last_bell_ms\":%u}",
      f_bell ? "true" : "false", l_bell ? "true" : "false",
      static_cast<unsigned>(b_ms));

  out.append("}}");
}

void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id) {
  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  int8_t temp_c = System_ReadTempC();
  int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t uptime_s = millis() / 1000;
  uint32_t free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap_kb =
      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  bool ntp_synced = (time(nullptr) > 1672531200);

  VendorProfileDescriptor active_prof{};
  ProfileRepository::getActiveProfile(active_prof);
  auto auto_desc = g_auto_probing_engine.getDescriptor();

  const char *wc_src =
      (g_warm_cache_source == 1)
          ? "RTC_SRAM"
          : (g_warm_cache_source == 2 ? "NVS_FLASH" : "COLD_BOOT");
  size_t total_devs = Device_GetCount();
  size_t online_devs = Device_GetOnlineCount();
  size_t stale_devs =
      (total_devs >= online_devs) ? (total_devs - online_devs) : 0;

  uint32_t ch1_rx = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_tx = g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_crc = g_pkt_stats.ch1.crc_errors.load(std::memory_order_relaxed);
  uint32_t ch1_tout = g_pkt_stats.ch1.timeouts.load(std::memory_order_relaxed);

  uint32_t ch2_rx = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_tx = g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_uncached =
      g_pkt_stats.ch2.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch3_rx = g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_tx = g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_uncached =
      g_pkt_stats.ch3.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch4_rx = g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_tx = g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_inv =
      g_pkt_stats.ch4.invalid_frames.load(std::memory_order_relaxed);

  uint32_t ch5_rx = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_tx = g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_drp =
      g_pkt_stats.ch5.dropped_pkts.load(std::memory_order_relaxed);

  uint32_t ch6_rx = g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_tx = g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);

  float crc_rate =
      (ch1_rx > 0)
          ? (static_cast<float>(ch1_crc) * 100.0f / static_cast<float>(ch1_rx))
          : 0.0f;

  const char *rst_reason = "Normal Boot";
  esp_reset_reason_t rr = esp_reset_reason();
  switch (rr) {
  case ESP_RST_POWERON:
    rst_reason = "Power-On Reset";
    break;
  case ESP_RST_EXT:
    rst_reason = "Hardware Reset Pin (EXT)";
    break;
  case ESP_RST_PANIC:
    rst_reason = "CPU Panic / Crash Exception";
    break;
  case ESP_RST_TASK_WDT:
    rst_reason = "Task Watchdog Reset";
    break;
  case ESP_RST_BROWNOUT:
    rst_reason = "HW: Brownout (Low Voltage)";
    break;
  case ESP_RST_SW:
    rst_reason = "Software Restart";
    break;
  default:
    rst_reason = "Other Reset";
    break;
  }

  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",", req_id);
  } else {
    out.append("{\"res\":\"ok\",");
  }

  serializeSysMetrics(out, c0, c1, temp_c, rssi, uptime_s, free_heap_kb,
                      min_free_heap_kb, ntp_synced);
  serializeProfileAndTiming(out, active_prof, auto_desc, wc_src, total_devs,
                            online_devs, stale_devs, ch2_rx, ch2_uncached);

  out.appendFormat("\"channels\":{\"ch1\":{\"rx\":%u,\"tx\":%u,\"crc_err\":%u,"
                   "\"timeout\":%u,\"crc_rate\":%.2f},"
                   "\"ch2\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch3\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch4\":{\"rx\":%u,\"tx\":%u,\"inv\":%u},"
                   "\"ch5\":{\"rx\":%u,\"tx\":%u,\"dropped\":%u},"
                   "\"ch6\":{\"rx\":%u,\"tx\":%u}},",
                   ch1_rx, ch1_tx, ch1_crc, ch1_tout, crc_rate, ch2_rx, ch2_tx,
                   ch2_uncached, ch3_rx, ch3_tx, ch3_uncached, ch4_rx, ch4_tx,
                   ch4_inv, ch5_rx, ch5_tx, ch5_drp, ch6_rx, ch6_tx);

  serializeDiagnostics(out, rst_reason);
}

void Mgmt_SerializeDevices(AppendBuf &out, long req_id) {
  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",\"devices\":[", req_id);
  } else {
    out.append("{\"res\":\"ok\",\"devices\":[");
  }
  size_t count = Device_GetCount();
  size_t locked_count = 0;

  for (size_t i = 0; i < count; ++i) {
    DeviceStateEntry snap{};
    if (!Device_GetSnapshot(i, snap) || snap.dev_id == 0)
      continue;

    GroupControlTemplate grp{};
    if (!g_control_registry.findGroup(snap.dev_id, grp))
      continue;

    StaticPacket ack{};
    ack.length = snap.last_ack_len;
    memcpy(ack.data.data(), snap.last_ack_data.data(),
           std::min<size_t>(snap.last_ack_len, 32));

    DecodedDeviceState st{};
    DeviceRepository::decodeDeviceState(grp, ack, &snap, st);

    DeviceClass dc = st.dev_class;
    const char *cls_str = DeviceClassToTelemetryString(dc);
    const char *grp_name = grp.group_name;
    bool is_outlet = (dc == DeviceClass::OUTLET);

    char name_buf[32];
    if (dc == DeviceClass::GAS || dc == DeviceClass::VENT ||
        dc == DeviceClass::MOMENTARY) {
      snprintf(name_buf, sizeof(name_buf), "%s", grp_name);
    } else {
      snprintf(name_buf, sizeof(name_buf), "%s %u-%u", grp_name, snap.sub1,
               snap.sub2);
    }

    RouteEndpoint ep{1, -1, 0};
    uint8_t ch = 1;
    if (Router_LookupRoute(snap.dev_id, snap.sub1, snap.sub2, ep)) {
      ch = ep.channel_id;
    }

    if (locked_count > 0)
      out.append(",");
    out.appendFormat("{\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"%s\","
                     "\"name\":\"%s\",\"channel\":%u,\"power\":%d",
                     snap.dev_id, snap.sub1, snap.sub2, cls_str, name_buf, ch,
                     st.power);

    if (dc == DeviceClass::THERMOSTAT) {
      out.appendFormat(",\"target_temp\":%d,\"current_temp\":%d",
                       st.target_temp, st.current_temp);
    } else if (dc == DeviceClass::VENT) {
      out.appendFormat(",\"fan_speed\":%d,\"vent_mode\":%d", st.fan_speed,
                       st.vent_mode);
    } else if (dc == DeviceClass::GAS) {
      out.appendFormat(",\"valve\":\"%s\"", st.valve_state);
    } else if (is_outlet) {
      out.appendFormat(",\"power_w\":%.1f", st.power_w);
    } else if (dc == DeviceClass::MOMENTARY) {
      out.appendFormat(",\"floor\":%d,\"direction\":%d", st.floor,
                       st.direction);
    }
    out.append("}");
    locked_count++;
  }

  // ── CH5 FCU 슬롯(1~4) 활성 기기 직렬화 (SmartThings get_devices 자식 기기
  // 목록 추가) ──
  {
    for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
      HubClientSlotSnapshot slot;
      Bridge_GetSlotSnapshot(s, slot);
      const DeviceStateEntry *fcu_dev =
          Device_Find(Config::FCU::DEV_ID, s, 0);

      // 소켓 설정이 활성화되어 있거나 수신 이력이 있는 경우 노출
      Fcu::SlotRuntime fcu_rt{};
      bool has_fcu_rt = Fcu::GetSlotRuntime(static_cast<uint8_t>(s), fcu_rt);
      if (slot.enabled || (has_fcu_rt && fcu_rt.is_online) ||
          (fcu_dev && fcu_dev->last_ack_len > 0)) {
        if (locked_count > 0)
          out.append(",");
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "%s",
                 slot.name[0] ? slot.name : "Air Conditioner");

        int pwr = fcu_rt.snap.power ? 1 : 0;
        int mode = static_cast<int>(fcu_rt.snap.mode);
        int fan = static_cast<int>(fcu_rt.snap.fan_speed);
        int swg = static_cast<int>(fcu_rt.snap.swing);
        int tgt = (fcu_rt.snap.target_temp > 0)
                      ? fcu_rt.snap.target_temp
                      : ((fcu_dev && fcu_dev->last_target_temp > 0)
                             ? fcu_dev->last_target_temp
                             : 24);
        int cur = (fcu_rt.snap.room_temp > 0)
                      ? fcu_rt.snap.room_temp
                      : ((fcu_dev && fcu_dev->last_current_temp > 0)
                             ? fcu_dev->last_current_temp
                             : tgt);

        int err_code = static_cast<int>(fcu_rt.snap.error_code);

        out.appendFormat(
            "{\"dev_id\":%u,\"sub1\":%u,\"sub2\":0,\"class\":\"fcu\",\"name\":"
            "\"%s\",\"channel\":5,\"power\":%d,\"mode\":%d,\"fan_speed\":%d,"
            "\"swing\":%d,\"target_temp\":%d,\"current_temp\":%d,\"error\":%d}",
            Config::FCU::DEV_ID, s, name_buf, pwr, mode, fan, swg, tgt, cur,
            err_code);
        locked_count++;
      }
    }
  }

  out.appendFormat("],\"count\":%u}\n", static_cast<unsigned>(locked_count));
}
void Mgmt_BroadcastDoorphoneEvent(bool front_bell, bool lobby_bell) noexcept {
  char buf[128];
  int len = snprintf(
      buf, sizeof(buf),
      "{\"event\":\"doorphone\",\"front_bell\":%s,\"lobby_bell\":%s}\n",
      front_bell ? "true" : "false", lobby_bell ? "true" : "false");
  if (len <= 0 || !Remote_GetSessionMutex())
    return;

  MutexLocker lock(Remote_GetSessionMutex());
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (Remote_GetSessions()[i].sock >= 0) {
      send(Remote_GetSessions()[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp, int current_temp, int speed,
                               const char *valve_state, float power_w,
                               int floor, int direction, int ho,
                               int vent_mode) {
  char buf[256];
  int len = 0;
  switch (dev_class) {
  case DeviceClass::THERMOSTAT:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"thermostat\",\"power\":%d,\"target_"
                   "temp\":%d,\"current_temp\":%d}\n",
                   dev_id, sub1, sub2, power, target_temp, current_temp);
    break;
  case DeviceClass::VENT:
    len = snprintf(
        buf, sizeof(buf),
        "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
        "\"class\":\"vent\",\"power\":%d,\"fan_speed\":%d,\"vent_mode\":%d}\n",
        dev_id, sub1, sub2, power, speed, vent_mode);
    break;
  case DeviceClass::GAS:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"gas\",\"valve\":\"%s\"}\n",
                   dev_id, sub1, sub2, valve_state ? valve_state : "closed");
    break;
  case DeviceClass::OUTLET:
    len = snprintf(
        buf, sizeof(buf),
        "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
        "\"class\":\"outlet\",\"power\":%d,\"power_w\":%.1f}\n",
        dev_id, sub1, sub2, power, power_w);
    break;
  case DeviceClass::MOMENTARY:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"momentary\",\"power\":%d,\"floor\":"
                   "%d,\"direction\":%d,\"ho\":%d}\n",
                   dev_id, sub1, sub2, power, floor, direction, ho);
    break;
  default:
    len = snprintf(buf, sizeof(buf),
                   "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                   "\"sub2\":%u,\"class\":\"switch\",\"power\":%d}\n",
                   dev_id, sub1, sub2, power);
    break;
  }

  if (len <= 0 || !Remote_GetSessionMutex())
    return;

  MutexLocker lock(Remote_GetSessionMutex());
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (Remote_GetSessions()[i].sock >= 0) {
      send(Remote_GetSessions()[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDeviceResult(const DeviceUpdateResult &res) noexcept {
  if (!res.should_broadcast)
    return;

  Mgmt_BroadcastDeviceState(
      res.dev_id, res.sub1, res.sub2, res.state.dev_class, res.state.power,
      res.state.target_temp, res.state.current_temp, res.state.fan_speed,
      res.state.valve_state, res.state.power_w, res.state.floor,
      res.state.direction, res.state.ho, res.state.vent_mode);

  for (uint8_t i = 0; i < res.extra_count; ++i) {
    const auto &ex = res.extra[i];
    Mgmt_BroadcastDeviceState(
        res.dev_id, ex.sub1, 0, ex.state.dev_class, ex.state.power,
        ex.state.target_temp, ex.state.current_temp, ex.state.fan_speed,
        ex.state.valve_state, ex.state.power_w, ex.state.floor,
        ex.state.direction, ex.state.ho, ex.state.vent_mode);
  }
}

void Mgmt_BroadcastElevatorEvent(uint8_t sub1, uint8_t sub2, uint8_t floor,
                                 uint8_t ho, uint8_t power,
                                 bool is_arrival) noexcept {
  if (is_arrival) {
    Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY, 0,
                              0, 0, 0, nullptr, 0.0f, floor, 0, ho);
  } else {
    Mgmt_BroadcastDeviceState(0x34, sub1, sub2, DeviceClass::MOMENTARY, power,
                              0, 0, 0, nullptr, 0.0f, 15, 0, 0);
  }
}

void Mgmt_BroadcastDevicesUpdated() {
  const char *msg = "{\"event\":\"devices_updated\"}\n";
  size_t len = strlen(msg);
  if (!Remote_GetSessionMutex())
    return;

  MutexLocker lock(Remote_GetSessionMutex());
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (Remote_GetSessions()[i].sock >= 0) {
      send(Remote_GetSessions()[i].sock, msg, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastRawJson(const char *json_payload) {
  if (!json_payload || !Remote_GetSessionMutex())
    return;

  char buf[256];
  int len = snprintf(buf, sizeof(buf), "%s\n", json_payload);
  if (len <= 0)
    return;

  MutexLocker lock(Remote_GetSessionMutex());
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (Remote_GetSessions()[i].sock >= 0) {
      send(Remote_GetSessions()[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

