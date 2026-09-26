#include "MgmtRpc.h"
#include "WallpadParser.h"
#include "ControlTemplate.h"
#include "ProfileMatcher.h"
#include <WiFi.h>
#include <cstring>
#include <cstdio>

void Mgmt_SerializeTelemetry(AppendBuf &out, long req_id) {
  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  int8_t temp_c = System_ReadTempC();
  int8_t rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t uptime_s = millis() / 1000;
  uint32_t free_heap_kb = heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024;
  uint32_t min_free_heap_kb = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT) / 1024;
  bool ntp_synced = (time(nullptr) > 1672531200);

  VendorProfileDescriptor active_prof{};
  ProfileRepository::getActiveProfile(active_prof);
  auto auto_desc = g_auto_probing_engine.getDescriptor();

  const char *wc_src = (g_warm_cache_source == 1) ? "RTC_SRAM" : (g_warm_cache_source == 2 ? "NVS_FLASH" : "COLD_BOOT");
  size_t total_devs = g_device_repo.count();
  size_t online_devs = g_device_repo.getOnlineCount();
  size_t stale_devs = (total_devs >= online_devs) ? (total_devs - online_devs) : 0;

  uint32_t ch1_rx = g_pkt_stats.ch1.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_tx = g_pkt_stats.ch1.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch1_crc = g_pkt_stats.ch1.crc_errors.load(std::memory_order_relaxed);
  uint32_t ch1_tout = g_pkt_stats.ch1.timeouts.load(std::memory_order_relaxed);

  uint32_t ch2_rx = g_pkt_stats.ch2.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_tx = g_pkt_stats.ch2.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch2_uncached = g_pkt_stats.ch2.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch3_rx = g_pkt_stats.ch3.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_tx = g_pkt_stats.ch3.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch3_uncached = g_pkt_stats.ch3.uncached_pkts.load(std::memory_order_relaxed);

  uint32_t ch4_rx = g_pkt_stats.ch4.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_tx = g_pkt_stats.ch4.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch4_inv = g_pkt_stats.ch4.invalid_frames.load(std::memory_order_relaxed);

  uint32_t ch5_rx = g_pkt_stats.ch5.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_tx = g_pkt_stats.ch5.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch5_drp = g_pkt_stats.ch5.dropped_pkts.load(std::memory_order_relaxed);

  uint32_t ch6_rx = g_pkt_stats.ch6.rx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_tx = g_pkt_stats.ch6.tx_pkts.load(std::memory_order_relaxed);
  uint32_t ch6_drp = g_pkt_stats.ch6.dropped_pkts.load(std::memory_order_relaxed);

  float crc_rate = (ch1_rx > 0) ? (static_cast<float>(ch1_crc) * 100.0f / static_cast<float>(ch1_rx)) : 0.0f;

  const char *rst_reason = "Normal Boot";
  esp_reset_reason_t rr = esp_reset_reason();
  switch (rr) {
    case ESP_RST_POWERON: rst_reason = "Power-On Reset"; break;
    case ESP_RST_EXT: rst_reason = "Hardware Reset Pin (EXT)"; break;
    case ESP_RST_PANIC: rst_reason = "CPU Panic / Crash Exception"; break;
    case ESP_RST_TASK_WDT: rst_reason = "Task Watchdog Reset"; break;
    case ESP_RST_BROWNOUT: rst_reason = "HW: Brownout (Low Voltage)"; break;
    case ESP_RST_SW: rst_reason = "Software Restart"; break;
    default: rst_reason = "Other Reset"; break;
  }

  if (req_id != -1) {
    out.appendFormat("{\"id\":%ld,\"res\":\"ok\",", req_id);
  } else {
    out.append("{\"res\":\"ok\",");
  }

  out.appendFormat("\"system\":{\"temp_c\":%d,\"wifi_rssi\":%d,\"uptime_s\":%u,"
                   "\"cpu0_load\":%u,\"cpu1_load\":%u,\"free_heap_kb\":%u,\"min_free_heap_kb\":%u,"
                   "\"ntp_synced\":%s,\"firmware\":\"%s\",\"latest_firmware\":\"Ready\"},",
                   static_cast<int>(temp_c), static_cast<int>(rssi), uptime_s,
                   static_cast<unsigned>(c0), static_cast<unsigned>(c1),
                   free_heap_kb, min_free_heap_kb,
                   ntp_synced ? "true" : "false", Config::FIRMWARE_VERSION);

  wifi_mode_t cur_wmode = WIFI_MODE_NULL;
  esp_wifi_get_mode(&cur_wmode);
  const char *mode_str = "STA";
  if (cur_wmode == WIFI_MODE_AP) mode_str = "AP";
  else if (cur_wmode == WIFI_MODE_APSTA) mode_str = "AP_STA";

  out.appendFormat("\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\",\"mode\":\"%s\"},",
                   WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "Disconnected",
                   WiFi.status() == WL_CONNECTED ? static_cast<int>(WiFi.RSSI()) : -100,
                   WiFi.localIP().toString().c_str(),
                   mode_str);

  out.appendFormat("\"ota\":{\"in_progress\":%s,\"status\":\"%s\",\"progress_pct\":%u,\"last_error\":\"%s\"},",
                   g_http_ota_state.in_progress.load() ? "true" : "false",
                   g_http_ota_state.status,
                   g_http_ota_state.progress_pct,
                   g_http_ota_state.last_error);

  ProfileRepository::getActiveProfile(active_prof);

  char cat_match_buf[64] = "None";
  const auto *matched_p = ProfileMatcher::getActiveProfile();
  if (matched_p) {
    snprintf(cat_match_buf, sizeof(cat_match_buf), "%s (%u Devices)",
             matched_p->vendor_name, static_cast<unsigned>(matched_p->device_count));
  }

  size_t grp_cnt = g_control_registry.getGroupCount();
  char bp_buf[64];
  if (auto_desc.offsets_locked && grp_cnt >= 6) {
    snprintf(bp_buf, sizeof(bp_buf), "%u Groups (Locked)", static_cast<unsigned>(grp_cnt));
  } else if (grp_cnt > 0) {
    snprintf(bp_buf, sizeof(bp_buf), "%u Groups (Learning)", static_cast<unsigned>(grp_cnt));
  } else {
    snprintf(bp_buf, sizeof(bp_buf), "0 Groups (Waiting)");
  }

  bool fully_locked = (g_config.wallpad_profile != 0) ||
                      (auto_desc.is_locked && auto_desc.opcodes_locked && auto_desc.offsets_locked);

  out.appendFormat("\"profile\":{\"active_slot\":%u,\"active_key\":\"%s\",\"active_name\":\"%s\","
                   "\"is_locked\":%s,\"fully_locked\":%s,\"catalog_match\":\"%s\",\"blueprints\":\"%s\","
                   "\"stx\":\"0x%02X\",\"etx\":\"0x%02X\","
                   "\"cs_algo\":\"%s\",\"opcodes\":{\"query\":\"0x%02X\",\"control\":\"0x%02X\",\"ack\":\"0x%02X\"},"
                   "\"match_count\":%u},",
                   static_cast<unsigned>(g_config.wallpad_profile), active_prof.key, active_prof.name,
                   auto_desc.is_locked ? "true" : "false",
                   fully_locked ? "true" : "false",
                   cat_match_buf, bp_buf,
                   auto_desc.stx, auto_desc.etx,
                   AutoProbingEngine::getAlgoName(auto_desc.checksum_algo),
                   auto_desc.query_opcode, auto_desc.control_opcode, auto_desc.ack_opcode,
                   auto_desc.matched_packets);

  out.appendFormat("\"timing\":{\"ch1_poll_interval_ms\":%u,\"ch2_ack_delay_ms\":%u,\"ch3_ack_delay_ms\":%u,"
                   "\"vip_preemptions\":%u,\"last_cmd_latency_ms\":%u},",
                   static_cast<unsigned>(g_timing_config.ch1_poll_interval_ms),
                   static_cast<unsigned>(g_timing_config.ch2_cache_delay_ms),
                   static_cast<unsigned>(g_timing_config.ch3_cache_delay_ms),
                   g_ch1_state_metrics.vip_cnt.load(std::memory_order_relaxed),
                   22);

  out.appendFormat("\"cache\":{\"source\":\"%s\",\"total_devices\":%u,\"online_devices\":%u,"
                   "\"stale_devices\":%u,\"cache_hit_rate\":%.1f},",
                   wc_src, static_cast<unsigned>(total_devs), static_cast<unsigned>(online_devs),
                   static_cast<unsigned>(stale_devs),
                   (ch2_rx > 0 ? (100.0f - (static_cast<float>(ch2_uncached) * 100.0f / ch2_rx)) : 100.0f));

  const char *f1 = formatFramingStr(g_config.uart_data_bits, g_config.uart_parity, g_config.uart_stop_bits);
  const char *f2 = formatFramingStr(g_config.ch2_data_bits, g_config.ch2_parity, g_config.ch2_stop_bits);
  const char *f3 = formatFramingStr(g_config.ch3_data_bits, g_config.ch3_parity, g_config.ch3_stop_bits);
  const char *f4 = formatFramingStr(g_config.doorphone_data_bits, g_config.doorphone_parity, g_config.doorphone_stop_bits);

  out.appendFormat("\"uart\":{"
                   "\"ch1\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch2\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch3\":{\"baud\":%u,\"format\":\"%s\"},"
                   "\"ch4\":{\"baud\":%u,\"format\":\"%s\"}},",
                   static_cast<unsigned>(g_config.uart_baud_rate), f1,
                   static_cast<unsigned>(g_config.ch2_baud_rate), f2,
                   static_cast<unsigned>(g_config.ch3_baud_rate), f3,
                   static_cast<unsigned>(g_doorphone_serial.baudRate() > 0 ? g_doorphone_serial.baudRate() : g_config.doorphone_baud_rate), f4);

  out.appendFormat("\"channels\":{\"ch1\":{\"rx\":%u,\"tx\":%u,\"crc_err\":%u,\"timeout\":%u,\"crc_rate\":%.2f},"
                   "\"ch2\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch3\":{\"rx\":%u,\"tx\":%u,\"uncached\":%u},"
                   "\"ch4\":{\"rx\":%u,\"tx\":%u,\"inv\":%u},"
                   "\"ch5\":{\"rx\":%u,\"tx\":%u,\"dropped\":%u},"
                   "\"ch6\":{\"rx\":%u,\"tx\":%u}},",
                   ch1_rx, ch1_tx, ch1_crc, ch1_tout, crc_rate,
                   ch2_rx, ch2_tx, ch2_uncached,
                   ch3_rx, ch3_tx, ch3_uncached,
                   ch4_rx, ch4_tx, ch4_inv,
                   ch5_rx, ch5_tx, ch5_drp,
                   ch6_rx, ch6_tx);

  out.append("\"diagnostics\":{");
  out.appendFormat("\"last_reboot_reason\":\"%s\",\"rollback_detected\":%s,\"rescue_mode\":%s,",
                   rst_reason, g_rollback_detected ? "true" : "false",
                   g_rescue_mode.load(std::memory_order_relaxed) ? "true" : "false");

  out.append("\"coredump\":{");
  if (g_coredump_info.valid) {
    out.appendFormat("\"valid\":true,\"task\":\"%s\",\"pc\":\"0x%08X\",\"cause\":%u,\"bt_depth\":%u,"
                     "\"summary\":\"⚠️ Crash in %s at 0x%08X (Cause %u)\"},",
                     g_coredump_info.task_name, g_coredump_info.exc_pc, g_coredump_info.exc_cause,
                     g_coredump_info.bt_depth, g_coredump_info.task_name, g_coredump_info.exc_pc,
                     g_coredump_info.exc_cause);
  } else {
    out.append("\"valid\":false,\"task\":\"\",\"pc\":\"0x00000000\",\"cause\":0,\"bt_depth\":0,"
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
      snprintf(up_str, sizeof(up_str), "%02uh %02um", up_s / 3600, (up_s % 3600) / 60);

      if (i > 0) out.append(",");
      out.appendFormat("{\"id\":%u,\"time\":\"%s\",\"reason\":\"%s\",\"uptime\":\"%s\"}",
                       static_cast<unsigned>(i + 1), time_buf, e.reason, up_str);
    }
  }
  out.append("],");

  bool f_bell = g_doorphone_state.front_bell.load(std::memory_order_relaxed);
  bool l_bell = g_doorphone_state.lobby_bell.load(std::memory_order_relaxed);
  uint32_t b_ms = g_doorphone_state.last_bell_ms.load(std::memory_order_relaxed);
  out.appendFormat("\"doorphone\":{\"front_bell\":%s,\"lobby_bell\":%s,\"last_bell_ms\":%u}",
                   f_bell ? "true" : "false", l_bell ? "true" : "false", static_cast<unsigned>(b_ms));

  out.append("}}");
}

void Mgmt_SerializeLockedDevices(AppendBuf &out) {
  out.append("{\"res\":\"ok\",\"devices\":[");
  size_t count = g_device_repo.count();
  size_t locked_count = 0;

  for (size_t i = 0; i < count; ++i) {
    DeviceStateEntry snap{};
    if (!g_device_repo.getSnapshot(i, snap) || snap.dev_id == 0) continue;

    DeviceClass dc = DeviceClass::SWITCH;
    const char *cls_str = "switch";
    const char *grp_name = "Switch";
    bool is_outlet = false;

    const GroupControlTemplate *grp = g_control_registry.findGroup(snap.dev_id);
    if (!grp) continue;

    dc = grp->coverage.dev_class;
    grp_name = grp->group_name;
    switch (dc) {
      case DeviceClass::THERMOSTAT: cls_str = "thermostat"; break;
      case DeviceClass::VENT:       cls_str = "vent"; break;
      case DeviceClass::GAS:        cls_str = "gas"; break;
      case DeviceClass::MOMENTARY:  cls_str = "momentary"; break;
      case DeviceClass::AIRCON:     cls_str = "aircon"; break;
      case DeviceClass::OUTLET:     cls_str = "outlet"; break;
      case DeviceClass::SWITCH:
      default:                      cls_str = "switch"; break;
    }

    is_outlet = (dc == DeviceClass::OUTLET) ||
                (dc == DeviceClass::SWITCH && grp->frame_len >= 17);
    if (is_outlet) {
      cls_str = "outlet";
    }

    char name_buf[32];
    if (dc == DeviceClass::GAS || dc == DeviceClass::VENT || dc == DeviceClass::MOMENTARY) {
      snprintf(name_buf, sizeof(name_buf), "%s", grp_name);
    } else {
      snprintf(name_buf, sizeof(name_buf), "%s %u-%u", grp_name, snap.sub1, snap.sub2);
    }

    int power = 0;
    int target_temp = 0;
    int current_temp = 0;
    int fan_speed = 0;
    int vent_mode = 1;
    float power_w = 0.0f;
    int floor = 1;
    int direction = 0;
    const char *valve_state = "closed";

    if (grp) {
      uint8_t p_off = grp->getPowerOffset(snap.last_ack_len);
      if (p_off != 0xFF && p_off < snap.last_ack_len) {
        uint8_t b = snap.last_ack_data[p_off];
        if (grp->coverage.dev_class == DeviceClass::VENT) {
          power = (b == 0x01) ? 1 : 0;
        } else if (b == grp->power_slot.on_val) {
          power = 1;
        } else if (b == grp->power_slot.off_val || b == 0) {
          power = 0;
        } else if (grp->coverage.dev_class == DeviceClass::THERMOSTAT && grp->away_mode_token != 0 && b == grp->away_mode_token) {
          power = 2;
        } else {
          power = 0;
        }
      }
    }

    if (grp && grp->coverage.dev_class == DeviceClass::THERMOSTAT) {
      target_temp = snap.last_target_temp > 0 ? snap.last_target_temp : 22;
      current_temp = snap.last_current_temp > 0 ? snap.last_current_temp : target_temp;

      uint8_t t_off = grp->getTargetTempOffset(snap.last_ack_len);
      uint8_t c_off = grp->getCurrentTempOffset(snap.last_ack_len);

      if (t_off == 0xFF && c_off == 0xFF) {
        bool is_temp_ack = true;
        if (grp->temp_slot.category_offset != 0xFF && grp->temp_slot.category_offset < snap.last_ack_len) {
          is_temp_ack = (snap.last_ack_data[grp->temp_slot.category_offset] == grp->temp_slot.category_val);
        }
        if (is_temp_ack) {
          t_off = grp->ack_slots.target_temp_offset;
        }
        c_off = grp->ack_slots.current_temp_offset;
      }

      if (power != 2 && t_off != 0xFF && t_off < snap.last_ack_len) {
        uint8_t b = snap.last_ack_data[t_off];
        if (b >= 5 && b <= 35) target_temp = b;
      }
      if (c_off != 0xFF && c_off < snap.last_ack_len) {
        uint8_t b = snap.last_ack_data[c_off];
        if (b >= 5 && b <= 50) current_temp = b;
      }
    }

    if (grp && grp->coverage.dev_class == DeviceClass::VENT) {
      fan_speed = 1;
      uint8_t spd_off = grp->getFanSpeedOffset(snap.last_ack_len);
      if (spd_off != 0xFF && spd_off < snap.last_ack_len) {
        uint8_t raw_b = snap.last_ack_data[spd_off];
        fan_speed = grp->decodeFanSpeed(raw_b);
      }
      bool is_mode_pkt = (snap.last_ack_len >= 6 && snap.last_ack_data[5] == 0x43);
      if (power == 1 && is_mode_pkt) {
        uint8_t p_off = grp->getPowerOffset(snap.last_ack_len);
        if (p_off != 0xFF && p_off < snap.last_ack_len) {
          uint8_t raw_mode = snap.last_ack_data[p_off];
          if (raw_mode >= 1 && raw_mode <= 4) {
            vent_mode = raw_mode;
          }
        }
      }
    }

    if (grp && grp->coverage.dev_class == DeviceClass::GAS) {
      uint8_t v_off = grp->getValveStateOffset(snap.last_ack_len);
      if (v_off != 0xFF && v_off < snap.last_ack_len) {
        valve_state = (snap.last_ack_data[v_off] == grp->close_slot.off_val) ? "closed" : "open";
      } else {
        valve_state = "closed";
      }
    }

    if (is_outlet && grp) {
      uint8_t w_off = grp->getWattageOffset(snap.last_ack_len);
      if (w_off != 0xFF && w_off + 1 < snap.last_ack_len) {
        uint16_t raw_w = (static_cast<uint16_t>(snap.last_ack_data[w_off]) << 8) | snap.last_ack_data[w_off + 1];
        if (raw_w < 50000) {
          power_w = static_cast<float>(raw_w);
        }
      }
    }

    if (dc == DeviceClass::MOMENTARY && snap.last_ack_len >= 6) {
      floor = snap.last_ack_data[5];
      if (floor < 1 || floor > 60) floor = 1;
      if (snap.last_ack_len >= 7) {
        direction = snap.last_ack_data[6]; // 1: 상승, 2: 하강, 0: 정지
      }
    }

    RouteEndpoint ep{1, -1, 0};
    uint8_t ch = 1;
    if (g_route_registry.lookupRoute(snap.dev_id, snap.sub1, snap.sub2, ep)) {
      ch = ep.channel_id;
    }

    if (locked_count > 0) out.append(",");
    out.appendFormat("{\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"%s\",\"name\":\"%s\",\"channel\":%u,\"power\":%d",
                     snap.dev_id, snap.sub1, snap.sub2, cls_str, name_buf, ch, power);

    if (dc == DeviceClass::THERMOSTAT) {
      out.appendFormat(",\"target_temp\":%d,\"current_temp\":%d", target_temp, current_temp);
    } else if (dc == DeviceClass::VENT) {
      out.appendFormat(",\"fan_speed\":%d,\"vent_mode\":%d", fan_speed, vent_mode);
    } else if (dc == DeviceClass::GAS) {
      out.appendFormat(",\"valve\":\"%s\"", valve_state);
    } else if (is_outlet) {
      out.appendFormat(",\"power_w\":%.1f", power_w);
    } else if (dc == DeviceClass::MOMENTARY) {
      out.appendFormat(",\"floor\":%d,\"direction\":%d", floor, direction);
    }
    out.append("}");
    locked_count++;
  }

  out.appendFormat("],\"count\":%u}\n", static_cast<unsigned>(locked_count));
}
void Mgmt_BroadcastDoorphoneEvent(bool front_bell, bool lobby_bell) {
  char buf[128];
  int len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"doorphone\",\"front_bell\":%s,\"lobby_bell\":%s}\n",
                     front_bell ? "true" : "false", lobby_bell ? "true" : "false");
  if (len <= 0 || !g_mgmt_mutex) return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDeviceState(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                               DeviceClass dev_class, int power,
                               int target_temp, int current_temp,
                               int speed, const char *valve_state,
                               float power_w, int floor, int direction,
                               int ho, int vent_mode) {
  char buf[256];
  int len = 0;
  switch (dev_class) {
    case DeviceClass::THERMOSTAT:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"thermostat\",\"power\":%d,\"target_temp\":%d,\"current_temp\":%d}\n",
                     dev_id, sub1, sub2, power, target_temp, current_temp);
      break;
    case DeviceClass::VENT:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"vent\",\"power\":%d,\"fan_speed\":%d,\"vent_mode\":%d}\n",
                     dev_id, sub1, sub2, power, speed, vent_mode);
      break;
    case DeviceClass::GAS:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"gas\",\"valve\":\"%s\"}\n",
                     dev_id, sub1, sub2, valve_state ? valve_state : "closed");
      break;
    case DeviceClass::OUTLET:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"outlet\",\"power\":%d,\"power_w\":%.1f}\n",
                     dev_id, sub1, sub2, power, power_w);
      break;
    case DeviceClass::MOMENTARY:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"momentary\",\"power\":%d,\"floor\":%d,\"direction\":%d,\"ho\":%d}\n",
                     dev_id, sub1, sub2, power, floor, direction, ho);
      break;
    default:
      len = snprintf(buf, sizeof(buf),
                     "{\"event\":\"device_state\",\"dev_id\":%u,\"sub1\":%u,\"sub2\":%u,\"class\":\"switch\",\"power\":%d}\n",
                     dev_id, sub1, sub2, power);
      break;
  }

  if (len <= 0 || !g_mgmt_mutex) return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, buf, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Mgmt_BroadcastDevicesUpdated() {
  const char *msg = "{\"event\":\"devices_updated\"}\n";
  size_t len = strlen(msg);
  if (!g_mgmt_mutex) return;

  MutexLocker lock(g_mgmt_mutex);
  for (int i = 0; i < Config::TCP::MAX_MGMT_CLIENTS; i++) {
    if (g_mgmt_sessions[i].sock >= 0) {
      send(g_mgmt_sessions[i].sock, msg, len, MSG_DONTWAIT);
      g_pkt_stats.ch6.tx_pkts.fetch_add(1, std::memory_order_relaxed);
    }
  }
}
