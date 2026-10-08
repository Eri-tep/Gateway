// ============================================================================
// RemoteService: Level 4 Network Remote Services
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include "L4_Services/Mgmt_Service.h"
#include "L3_Protocol/Public/Protocol_Device.h"
#include "L3_Protocol/Public/Protocol_Facade.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <span>

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

  HttpOtaSnapshot ota_snap{};
  System_GetHttpOtaSnapshot(ota_snap);
  out.appendFormat("\"ota\":{\"in_progress\":%s,\"status\":\"%s\",\"progress_"
                   "pct\":%u,\"last_error\":\"%s\"},",
                   ota_snap.in_progress ? "true" : "false",
                   ota_snap.status, ota_snap.progress_pct,
                   ota_snap.last_error);
}

static void serializeProfileAndTiming(
    AppendBuf &out, const ProfileInfoSnapshot &active_prof,
    const AutoProbingDescriptorSnapshot &auto_desc,
    const ProtocolDiagnosticSnapshot &diag_snap,
    const char *wc_src, size_t total_devs,
    size_t online_devs, size_t stale_devs, uint32_t ch2_rx,
    uint32_t ch2_uncached) {
  const char *cat_match_buf = diag_snap.vendor_name[0] ? diag_snap.vendor_name : "None";

  char bp_buf[64];
  snprintf(bp_buf, sizeof(bp_buf), "%u Groups",
           static_cast<unsigned>(diag_snap.group_count));

  const uint8_t active_profile_idx = Config_GetWallpadProfile();
  bool fully_locked = (active_profile_idx != 0) ||
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
      static_cast<unsigned>(active_profile_idx), active_prof.key,
      active_prof.name, auto_desc.is_locked ? "true" : "false",
      fully_locked ? "true" : "false", cat_match_buf, bp_buf, auto_desc.stx,
      auto_desc.etx, auto_desc.checksum_algo_name,
      auto_desc.query_opcode, auto_desc.control_opcode, auto_desc.ack_opcode,
      auto_desc.matched_packets);

  uint32_t ch1_poll_cnt = 0, ch1_vip_cnt = 0, ch1_norm_cnt = 0;
  System_GetCh1Metrics(ch1_poll_cnt, ch1_vip_cnt, ch1_norm_cnt);

  const auto &timing = TimingConfig_Get();
  out.appendFormat("\"timing\":{\"ch1_poll_interval_ms\":%u,\"ch2_ack_delay_"
                   "ms\":%u,\"ch3_ack_delay_ms\":%u,"
                   "\"vip_preemptions\":%u,\"last_cmd_latency_ms\":%u},",
                   static_cast<unsigned>(timing.ch1_poll_interval_ms),
                   static_cast<unsigned>(timing.ch2_cache_delay_ms),
                   static_cast<unsigned>(timing.ch3_cache_delay_ms),
                   ch1_vip_cnt,
                   22);

  out.appendFormat(
      "\"cache\":{\"source\":\"%s\",\"total_devices\":%u,\"online_devices\":%u,"
      "\"stale_devices\":%u,\"cache_hit_rate\":%.1f},",
      wc_src, static_cast<unsigned>(total_devs),
      static_cast<unsigned>(online_devs), static_cast<unsigned>(stale_devs),
      (ch2_rx > 0
           ? (100.0f - (static_cast<float>(ch2_uncached) * 100.0f / ch2_rx))
           : 100.0f));

  const auto &cfg = Config_Get();
  const char *f1 = formatFramingStr(
      cfg.uart_data_bits, cfg.uart_parity, cfg.uart_stop_bits);
  const char *f2 = formatFramingStr(cfg.ch2_data_bits, cfg.ch2_parity,
                                    cfg.ch2_stop_bits);
  const char *f3 = formatFramingStr(cfg.ch3_data_bits, cfg.ch3_parity,
                                    cfg.ch3_stop_bits);
  const char *f4 =
      formatFramingStr(cfg.doorphone_data_bits, cfg.doorphone_parity,
                       cfg.doorphone_stop_bits);

  out.appendFormat("\"uart\":{"
                   "\"ch1\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch2\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch3\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch4\":{\"baud\":%u,\"format\":\"%s\"}},",
                   static_cast<unsigned>(cfg.uart_baud_rate), f1,
                   static_cast<unsigned>(cfg.ch2_baud_rate), f2,
                   static_cast<unsigned>(cfg.ch3_baud_rate), f3,
                   static_cast<unsigned>(cfg.doorphone_baud_rate), f4);
}

static void serializeDiagnostics(AppendBuf &out, const char *rst_reason) {
  out.append("\"diagnostics\":{");
  out.appendFormat("\"last_reboot_reason\":\"%s\",\"rollback_detected\":%s,"
                   "\"rescue_mode\":%s,",
                   rst_reason, System_IsRollbackDetected() ? "true" : "false",
                   System_IsRescueMode() ? "true"
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
  size_t log_cnt = System_GetRebootLogCount();
  size_t max_logs_to_emit = (log_cnt > 5) ? 5 : log_cnt;
  for (size_t i = 0; i < max_logs_to_emit; i++) {
    LogEntry e{};
    if (System_GetRebootLogEntry(i, e)) {
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

  bool f_bell = false, l_bell = false;
  uint32_t b_ms = 0;
  Device_DoorphoneGetState(f_bell, l_bell, b_ms);
  out.appendFormat(
      "\"doorphone\":{\"front_bell\":%s,\"lobby_bell\":%s,\"last_bell_ms\":%u}",
      f_bell ? "true" : "false", l_bell ? "true" : "false",
      static_cast<unsigned>(b_ms));

  uint32_t telem_drops = 0, telem_hw = 0;
  Telemetry_GetStats(telem_drops, telem_hw);
  out.appendFormat(
      ",\"telemetry\":{\"drop_count\":%u,\"high_watermark\":%u}",
      telem_drops, telem_hw);

  uint32_t nvs_errs = 0, nvs_sync_ms = 0;
  System_GetNvsStats(nvs_errs, nvs_sync_ms);
  out.appendFormat(
      ",\"nvs\":{\"write_errors\":%u,\"last_sync_s\":%u}",
      nvs_errs, (nvs_sync_ms > 0) ? static_cast<unsigned>((millis() - nvs_sync_ms) / 1000) : 0);

  out.append("}}");
}

void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id) {
  uint8_t c0 = 0, c1 = 0;
  int8_t temp_c = 0;
  System_GetCpuAndTemp(c0, c1, temp_c);
  int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t uptime_s = millis() / 1000;
  uint32_t free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap_kb =
      heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  bool ntp_synced = (time(nullptr) > 1672531200);

  ProfileInfoSnapshot active_prof{};
  ProtocolDiag_GetProfileInfo(Config_GetWallpadProfile(), active_prof);
  AutoProbingDescriptorSnapshot auto_desc{};
  ProtocolDiag_GetAutoProbingDescriptor(auto_desc);
  ProtocolDiagnosticSnapshot diag_snap{};
  ProtocolDiag_GetSnapshot(diag_snap);
  const char *wc_src =
      (diag_snap.wc_source == 1)
          ? "RTC_SRAM"
          : (diag_snap.wc_source == 2 ? "NVS_FLASH" : "COLD_BOOT");
  size_t total_devs = Device_GetCount();
  size_t online_devs = Device_GetOnlineCount();
  size_t stale_devs =
      (total_devs >= online_devs) ? (total_devs - online_devs) : 0;

  PktSnapshot pkt_snap{};
  System_GetPktSnapshot(pkt_snap);

  uint32_t ch1_rx = pkt_snap.ch1.rx_pkts;
  uint32_t ch1_tx = pkt_snap.ch1.tx_pkts;
  uint32_t ch1_crc = pkt_snap.ch1.crc_errors;
  uint32_t ch1_tout = pkt_snap.ch1.timeouts;

  uint32_t ch2_rx = pkt_snap.ch2.rx_pkts;
  uint32_t ch2_tx = pkt_snap.ch2.tx_pkts;
  uint32_t ch2_uncached = pkt_snap.ch2.uncached_pkts;

  uint32_t ch3_rx = pkt_snap.ch3.rx_pkts;
  uint32_t ch3_tx = pkt_snap.ch3.tx_pkts;
  uint32_t ch3_uncached = pkt_snap.ch3.uncached_pkts;

  uint32_t ch4_rx = pkt_snap.ch4.rx_pkts;
  uint32_t ch4_tx = pkt_snap.ch4.tx_pkts;
  uint32_t ch4_inv = pkt_snap.ch4.invalid_frames;

  uint32_t ch5_rx = pkt_snap.ch5.rx_pkts;
  uint32_t ch5_tx = pkt_snap.ch5.tx_pkts;
  uint32_t ch5_drp = pkt_snap.ch5.dropped_pkts;

  uint32_t ch6_rx = pkt_snap.ch6.rx_pkts;
  uint32_t ch6_tx = pkt_snap.ch6.tx_pkts;

  float crc_rate =
      (ch1_rx > 0)
          ? (static_cast<float>(ch1_crc) * 100.0f / static_cast<float>(ch1_rx))
          : 0.0f;

  esp_reset_reason_t rr = esp_reset_reason();
  const char *rst_reason = System_ResetReasonToString(rr);

  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",", req_id);
  } else {
    out.append("{\"res\":\"ok\",");
  }

  serializeSysMetrics(out, c0, c1, temp_c, rssi, uptime_s, free_heap_kb,
                      min_free_heap_kb, ntp_synced);
  serializeProfileAndTiming(out, active_prof, auto_desc, diag_snap, wc_src, total_devs,
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

// ── Table-Driven Property Formatters for Device Classes (Rule 8 Compliance) ──
using DevicePropFormatter = void (*)(AppendBuf &out, const DecodedDeviceState &st);

struct DevicePropFormatEntry {
  DeviceClass cls;
  DevicePropFormatter format;
};

static void FormatThermostatProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"target_temp\":%d,\"current_temp\":%d", st.target_temp,
                   st.current_temp);
}

static void FormatVentProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"fan_speed\":%d,\"vent_mode\":%d", st.fan_speed,
                   st.vent_mode);
}

static void FormatGasProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"valve\":\"%s\"", st.valve_state);
}

static void FormatOutletProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"power_w\":%.1f", st.power_w);
}

static void FormatMomentaryProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"floor\":%d,\"direction\":%d", st.floor, st.direction);
}

static void FormatAirconProps(AppendBuf &out, const DecodedDeviceState &st) {
  out.appendFormat(",\"target_temp\":%d,\"current_temp\":%d,\"fan_speed\":%d",
                   st.target_temp, st.current_temp, st.fan_speed);
}

static constexpr DevicePropFormatEntry kPropFormatters[] = {
    {DeviceClass::THERMOSTAT, FormatThermostatProps},
    {DeviceClass::VENT,       FormatVentProps},
    {DeviceClass::GAS,        FormatGasProps},
    {DeviceClass::OUTLET,     FormatOutletProps},
    {DeviceClass::MOMENTARY,  FormatMomentaryProps},
    {DeviceClass::AIRCON,     FormatAirconProps},
};

void Mgmt_SerializeDevices(AppendBuf &out, long req_id) {
  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",\"devices\":[", req_id);
  } else {
    out.append("{\"res\":\"ok\",\"devices\":[");
  }
  size_t total_count = Device_GetCount();
  size_t locked_count = 0;

  constexpr size_t kChunkSize = 8;
  DeviceStateEntry chunk[kChunkSize];

  for (size_t start = 0; start < total_count; start += kChunkSize) {
    size_t chunk_len = Device_GetSnapshotChunk(start, chunk, kChunkSize);
    for (size_t i = 0; i < chunk_len; ++i) {
      const auto &snap = chunk[i];
      if (snap.dev_id == 0)
        continue;

      StaticPacket ack{};
      ack.length = snap.last_ack_len;
      memcpy(ack.data.data(), snap.last_ack_data.data(),
             std::min<size_t>(snap.last_ack_len, 32));

      DecodedDeviceState st{};
      if (!Device_DecodeState(snap.dev_id, ack, &snap, st))
        continue;

      DeviceClass dc = st.dev_class;
      const char *cls_str = DeviceClassToTelemetryString(dc);
      const char *grp_name = ProtocolDiag_GetGroupName(snap.dev_id);
      bool is_outlet = (dc == DeviceClass::OUTLET);

      char name_buf[32];
      if (dc == DeviceClass::GAS || dc == DeviceClass::VENT ||
          dc == DeviceClass::MOMENTARY) {
        snprintf(name_buf, sizeof(name_buf), "%s", grp_name);
      } else {
        snprintf(name_buf, sizeof(name_buf), "%s %u-%u", grp_name, snap.sub1,
                 snap.sub2);
      }

      const uint8_t ch = Protocol_LookupDeviceChannel(snap.dev_id, snap.sub1, snap.sub2);

      if (locked_count > 0)
        out.append(",");
      out.appendFormat("{\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"%s\","
                       "\"name\":\"%s\",\"channel\":%u,\"power\":%d",
                       snap.dev_id, snap.sub1, snap.sub2, cls_str, name_buf, ch,
                       st.power);

      for (const auto &fmt : kPropFormatters) {
        if (fmt.cls == dc || (fmt.cls == DeviceClass::OUTLET && is_outlet)) {
          fmt.format(out, st);
          break;
        }
      }
      out.append("}");
      locked_count++;
    }
  }

  // ── CH5 FCU 슬롯(1~4) 활성 기기 직렬화 (SmartThings get_devices 자식 기기
  // 목록 추가) ──
  {
    for (uint8_t s = 1; s < Config::TCP::MAX_EW11_SLOTS; ++s) {
      HubClientSlotSnapshot slot;
      System_GetBridgeSlotSnapshot(s, slot);
      DeviceStateEntry fcu_dev{};
      bool has_fcu_dev = Device_FindCopy(Config::FCU::DEV_ID, s, 0, fcu_dev);

      // 소켓 설정이 활성화되어 있거나 수신 이력이 있는 경우 노출
      FcuDeviceSnapshot fcu_snap{};
      bool has_fcu_snap = Device_GetFcuSnapshot(s, fcu_snap);
      if (slot.enabled || (has_fcu_snap && fcu_snap.is_online) ||
          (has_fcu_dev && fcu_dev.last_ack_len > 0)) {
        if (locked_count > 0)
          out.append(",");
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "%s",
                 slot.name[0] ? slot.name : "Air Conditioner");

        int pwr = fcu_snap.power ? 1 : 0;
        int mode = static_cast<int>(fcu_snap.mode);
        int fan = static_cast<int>(fcu_snap.fan_speed);
        int swg = static_cast<int>(fcu_snap.swing);
        int tgt = (fcu_snap.target_temp > 0)
                      ? fcu_snap.target_temp
                      : ((has_fcu_dev && fcu_dev.last_target_temp > 0)
                             ? fcu_dev.last_target_temp
                             : 24);
        int cur = (fcu_snap.room_temp > 0)
                      ? fcu_snap.room_temp
                      : ((has_fcu_dev && fcu_dev.last_current_temp > 0)
                             ? fcu_dev.last_current_temp
                             : tgt);

        int err_code = 0;

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

namespace {

int formatDeviceStateJson(char *buf, size_t buf_size, uint8_t dev_id,
                          uint8_t sub1, uint8_t sub2, DeviceClass dev_class,
                          int power, int target_temp, int current_temp,
                          int speed, const char *valve_state, float power_w,
                          int floor, int direction, int ho, int vent_mode) noexcept {
  switch (dev_class) {
  case DeviceClass::THERMOSTAT:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                    "\"sub2\":%u,\"class\":\"thermostat\",\"power\":%d,\"target_"
                    "temp\":%d,\"current_temp\":%d}\n",
                    dev_id, sub1, sub2, power, target_temp, current_temp);

  case DeviceClass::VENT:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
                    "\"class\":\"vent\",\"power\":%d,\"fan_speed\":%d,\"vent_mode\":%d}\n",
                    dev_id, sub1, sub2, power, speed, vent_mode);

  case DeviceClass::GAS:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                    "\"sub2\":%u,\"class\":\"gas\",\"valve\":\"%s\"}\n",
                    dev_id, sub1, sub2, valve_state ? valve_state : "closed");

  case DeviceClass::OUTLET:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
                    "\"class\":\"outlet\",\"power\":%d,\"power_w\":%.1f}\n",
                    dev_id, sub1, sub2, power, power_w);

  case DeviceClass::MOMENTARY:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                    "\"sub2\":%u,\"class\":\"momentary\",\"power\":%d,\"floor\":"
                    "%d,\"direction\":%d,\"ho\":%d}\n",
                    dev_id, sub1, sub2, power, floor, direction, ho);

  case DeviceClass::AIRCON:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,"
                    "\"class\":\"aircon\",\"power\":%d,\"target_temp\":%d,\"current_temp\":%d,"
                    "\"fan_speed\":%d}\n",
                    dev_id, sub1, sub2, power, target_temp, current_temp, speed);

  case DeviceClass::SWITCH:
  case DeviceClass::UNKNOWN:
  default:
    return snprintf(buf, buf_size,
                    "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,"
                    "\"sub2\":%u,\"class\":\"switch\",\"power\":%d}\n",
                    dev_id, sub1, sub2, power);
  }
}

} // namespace

void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp, int current_temp, int speed,
                               const char *valve_state, float power_w,
                               int floor, int direction, int ho,
                               int vent_mode) {
  char buf[256];
  int len = formatDeviceStateJson(buf, sizeof(buf), dev_id, sub1, sub2,
                                  dev_class, power, target_temp, current_temp,
                                  speed, valve_state, power_w, floor, direction,
                                  ho, vent_mode);

  if (len <= 0 || !Remote_GetSessionMutex())
    return;

  MutexLocker lock(Remote_GetSessionMutex());
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (Remote_GetSessions()[i].sock >= 0) {
      send(Remote_GetSessions()[i].sock, buf, len, MSG_DONTWAIT);
      System_RecordCh6Tx();
    }
  }
}

void Mgmt_DrainTelemetryQueue() noexcept {
  constexpr size_t MAX_DRAIN_PER_TICK = 16;
  TelemetryItem item{};
  size_t drained = 0;
  while (drained < MAX_DRAIN_PER_TICK && Telemetry_Dequeue(item)) {
    ++drained;
    switch (item.type) {
    case TelemetryEventType::DEVICE_RESULT: {
      const auto &res = item.device_res;
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
      break;
    }
    case TelemetryEventType::DOORPHONE: {
      char buf[128];
      int len = snprintf(
          buf, sizeof(buf),
          "{\"event\":\"doorphone\",\"front_bell\":%s,\"lobby_bell\":%s}\n",
          item.front_bell ? "true" : "false", item.lobby_bell ? "true" : "false");
      if (len > 0 && Remote_GetSessionMutex()) {
        MutexLocker lock(Remote_GetSessionMutex());
        for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
          if (Remote_GetSessions()[i].sock >= 0) {
            send(Remote_GetSessions()[i].sock, buf, len, MSG_DONTWAIT);
            System_RecordCh6Tx();
          }
        }
      }
      break;
    }
    case TelemetryEventType::ELEVATOR: {
      if (item.is_arrival) {
        Mgmt_BroadcastDeviceState(0x34, item.sub1, item.sub2, DeviceClass::MOMENTARY, 0,
                                  0, 0, 0, nullptr, 0.0f, item.floor, 0, item.ho);
      } else {
        Mgmt_BroadcastDeviceState(0x34, item.sub1, item.sub2, DeviceClass::MOMENTARY, item.power,
                                  0, 0, 0, nullptr, 0.0f, 15, 0, 0);
      }
      break;
    }
    }
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
      System_RecordCh6Tx();
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
      System_RecordCh6Tx();
    }
  }
}

