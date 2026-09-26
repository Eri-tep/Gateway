#include "core/Platform.h"
#include "core/Config.h"
#include "core/Buffers.h"
#include "core/Metrics.h"
#include "core/Devices.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>

void System_Sha256ToHex(const char *input, char *output) {
  if (!input || !output)
    return;

  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, (const unsigned char *)input, strlen(input));
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    output[i * 2] = hex[hash[i] >> 4];
    output[i * 2 + 1] = hex[hash[i] & 0xF];
  }
  output[64] = '\0';
}

void Config_Load() {
  Preferences p;
  p.begin("runtime-config", true);
  auto &c = g_config;

  c.uart_baud_rate = p.getULong("uart_baud", 9600);
  c.ch2_baud_rate = p.getULong("ch2_baud", 9600);
  c.ch3_baud_rate = p.getULong("ch3_baud", 9600);
  c.doorphone_baud_rate =
      p.getULong("door_baud", Config::Serial::DEFAULT_DOORPHONE_BAUD);
  p.getString("wifi_ssid", "").toCharArray(c.wifi_ssid, sizeof(c.wifi_ssid));
  p.getString("wifi_pass", "")
      .toCharArray(c.wifi_password, sizeof(c.wifi_password));
  p.getString("ap_ssid", "").toCharArray(c.ap_ssid, sizeof(c.ap_ssid));
  p.getString("ap_pass", "").toCharArray(c.ap_password, sizeof(c.ap_password));
  p.getString("telnet_hash", "")
      .toCharArray(c.telnet_pass_hash, sizeof(c.telnet_pass_hash));

  c.uart_parity = p.getUChar("u_parity", 0);
  c.uart_stop_bits = p.getUChar("u_sbits", 1);
  c.uart_data_bits = p.getUChar("u_dbits", 8);
  c.ch2_parity = p.getUChar("ch2_parity", 0);
  c.ch2_stop_bits = p.getUChar("ch2_sbits", 1);
  c.ch2_data_bits = p.getUChar("ch2_dbits", 8);
  c.ch3_parity = p.getUChar("ch3_parity", 0);
  c.ch3_stop_bits = p.getUChar("ch3_sbits", 1);
  c.ch3_data_bits = p.getUChar("ch3_dbits", 8);
  c.doorphone_data_bits =
      p.getUChar("d_dbits", Config::Serial::DEFAULT_DOORPHONE_DATABITS);
  c.doorphone_parity =
      p.getUChar("d_parity", Config::Serial::DEFAULT_DOORPHONE_PARITY);
  c.doorphone_stop_bits =
      p.getUChar("d_sbits", Config::Serial::DEFAULT_DOORPHONE_STOPBITS);
  c.wifi_connect_timeout_s = p.getUShort("w_tout", 30);
  c.wallpad_profile =
      p.getUChar("w_prof", static_cast<uint8_t>(WallpadProfileIndex::ADAPTIVE));
  p.end();

  uint16_t mac_suffix = static_cast<uint16_t>(ESP.getEfuseMac() >> 32);

  if (strlen(c.wifi_ssid) == 0) {
#ifdef WIFI_SSID
    strncpy(c.wifi_ssid, WIFI_SSID, sizeof(c.wifi_ssid) - 1);
    c.wifi_ssid[sizeof(c.wifi_ssid) - 1] = '\0';
#endif
  }

  if (strlen(c.wifi_password) == 0) {
#ifdef WIFI_PASSWORD
    strncpy(c.wifi_password, WIFI_PASSWORD, sizeof(c.wifi_password) - 1);
    c.wifi_password[sizeof(c.wifi_password) - 1] = '\0';
#endif
  }

  if (strlen(c.ap_ssid) == 0) {
    snprintf(c.ap_ssid, sizeof(c.ap_ssid), "Gateway-Setup-%04X", mac_suffix);
  }

  if (strlen(c.ap_password) < 8) {
#ifdef EMERGENCY_AP_PASS
    strncpy(c.ap_password, EMERGENCY_AP_PASS, sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#else
    strncpy(c.ap_password, "9dnjf1!DLF", sizeof(c.ap_password) - 1);
    c.ap_password[sizeof(c.ap_password) - 1] = '\0';
#endif
  }

  if (strlen(c.telnet_pass_hash) == 0) {
#ifdef DEFAULT_TELNET_PASS
    System_Sha256ToHex(DEFAULT_TELNET_PASS, c.telnet_pass_hash);
#endif
  }
}

void Config_Save() {
  if (!g_config_dirty.load(std::memory_order_acquire))
    return;

  Preferences p;
  p.begin("runtime-config", false);
  RuntimeConfig snapshot;
  {
    CriticalSectionLocker lock(&g_config_mux);
    snapshot = g_config;
    g_config_dirty.store(false, std::memory_order_release);
  }

  p.putULong("uart_baud", snapshot.uart_baud_rate);
  p.putULong("ch2_baud", snapshot.ch2_baud_rate);
  p.putULong("ch3_baud", snapshot.ch3_baud_rate);
  p.putULong("door_baud", snapshot.doorphone_baud_rate);
  p.putString("wifi_ssid", snapshot.wifi_ssid);
  p.putString("wifi_pass", snapshot.wifi_password);
  p.putString("ap_ssid", snapshot.ap_ssid);
  p.putString("ap_pass", snapshot.ap_password);
  p.putString("telnet_hash", snapshot.telnet_pass_hash);

  p.putUChar("u_parity", snapshot.uart_parity);
  p.putUChar("u_sbits", snapshot.uart_stop_bits);
  p.putUChar("u_dbits", snapshot.uart_data_bits);
  p.putUChar("ch2_parity", snapshot.ch2_parity);
  p.putUChar("ch2_sbits", snapshot.ch2_stop_bits);
  p.putUChar("ch2_dbits", snapshot.ch2_data_bits);
  p.putUChar("ch3_parity", snapshot.ch3_parity);
  p.putUChar("ch3_sbits", snapshot.ch3_stop_bits);
  p.putUChar("ch3_dbits", snapshot.ch3_data_bits);
  p.putUChar("d_dbits", snapshot.doorphone_data_bits);
  p.putUChar("d_parity", snapshot.doorphone_parity);
  p.putUChar("d_sbits", snapshot.doorphone_stop_bits);
  p.putUShort("w_tout", snapshot.wifi_connect_timeout_s);
  p.putUChar("w_prof", snapshot.wallpad_profile);

  p.end();
}

void Config_ResetDefaults() {
  CriticalSectionLocker lock(&g_config_mux);
  g_config = RuntimeConfig{};
}

void System_TakeSnapshot(SysSnapshot &sys, HwSnapshot &hw, StackSnapshot &st,
                         PktSnapshot &pkt) {
  sys.uptime_ms = millis();
  sys.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  sys.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
  sys.total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
  sys.sketch_size_kb = ESP.getSketchSize() / 1024;
  sys.flash_total_kb = ESP.getFlashChipSize() / 1024;
  sys.wifi_connected = WiFi.isConnected();
  sys.wifi_rssi = static_cast<int8_t>(WiFi.RSSI());

  if (sys.wifi_connected) {
    strncpy(sys.wifi_ip, WiFi.localIP().toString().c_str(),
            sizeof(sys.wifi_ip) - 1);
  } else {
    strncpy(sys.wifi_ip, "0.0.0.0", sizeof(sys.wifi_ip));
  }

  auto s15 = g_metrics.get15m();
  auto s24 = g_metrics.get24h();

  uint8_t c0 = 0, c1 = 0;
  System_ReadCpuPct(c0, c1);
  hw.cpu0_cur = c0;
  hw.cpu0_15m_avg = s15.count ? s15.cpu0_avg : hw.cpu0_cur;
  hw.cpu0_15m_peak = s15.count ? s15.cpu0_peak : hw.cpu0_cur;
  hw.cpu0_24h_avg = s24.count ? s24.cpu0_avg : hw.cpu0_cur;
  hw.cpu0_24h_peak = s24.count ? s24.cpu0_peak : hw.cpu0_cur;

  hw.cpu1_cur = c1;
  hw.cpu1_15m_avg = s15.count ? s15.cpu1_avg : hw.cpu1_cur;
  hw.cpu1_15m_peak = s15.count ? s15.cpu1_peak : hw.cpu1_cur;
  hw.cpu1_24h_avg = s24.count ? s24.cpu1_avg : hw.cpu1_cur;
  hw.cpu1_24h_peak = s24.count ? s24.cpu1_peak : hw.cpu1_cur;

  hw.ram_cur = (sys.total_heap - sys.free_heap) / 1024;
  hw.ram_15m_avg = s15.count ? s15.ram_avg : hw.ram_cur;
  hw.ram_15m_peak = s15.count ? s15.ram_peak : hw.ram_cur;
  hw.ram_24h_avg = s24.count ? s24.ram_avg : hw.ram_cur;
  hw.ram_24h_peak = s24.count ? s24.ram_peak : hw.ram_cur;

  hw.temp_cur = System_ReadTempC();
  hw.temp_15m_avg = s15.count ? s15.temp_avg : hw.temp_cur;
  hw.temp_15m_peak = s15.count ? s15.temp_peak : hw.temp_cur;
  hw.temp_24h_avg = s24.count ? s24.temp_avg : hw.temp_cur;
  hw.temp_24h_peak = s24.count ? s24.temp_peak : hw.temp_cur;

  st.ch1_stack = g_ch1_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch1_task_handle))
                     : 0;
  st.ch2_stack = g_ch2_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch2_task_handle))
                     : 0;
  st.ch3_stack = g_ch3_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch3_task_handle))
                     : 0;
  st.ch4_stack = g_ch4_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_ch4_task_handle))
                     : 0;
  st.net_stack = g_network_task_handle
                     ? static_cast<uint16_t>(
                           uxTaskGetStackHighWaterMark(g_network_task_handle))
                     : 0;
  st.telnet_stack = g_telnet_task_handle
                         ? static_cast<uint16_t>(
                               uxTaskGetStackHighWaterMark(g_telnet_task_handle))
                         : 0;

  pkt.ch1 = g_pkt_stats.ch1;
  pkt.ch2 = g_pkt_stats.ch2;
  pkt.ch3 = g_pkt_stats.ch3;
  pkt.ch4 = g_pkt_stats.ch4;
  pkt.ch5 = g_pkt_stats.ch5;
  pkt.ch6 = g_pkt_stats.ch6;
}

bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format) {
  uint8_t db = 8, pr = 0, sb = 1;
  if (!parseFramingStr(format, db, pr, sb))
    return false;
  if (baud < 1200 || baud > 921600)
    return false;

  auto to_uart_parity = [](uint8_t p) -> uart_parity_t {
    return (p == 1)   ? UART_PARITY_EVEN
           : (p == 2) ? UART_PARITY_ODD
                      : UART_PARITY_DISABLE;
  };
  auto to_uart_stopbits = [](uint8_t s) -> uart_stop_bits_t {
    return s == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  };

  {
    CriticalSectionLocker lock(&g_config_mux);
    if (ch == 1) {
      g_config.uart_baud_rate = baud;
      g_config.uart_data_bits = db;
      g_config.uart_parity = pr;
      g_config.uart_stop_bits = sb;
    } else if (ch == 2) {
      g_config.ch2_baud_rate = baud;
      g_config.ch2_data_bits = db;
      g_config.ch2_parity = pr;
      g_config.ch2_stop_bits = sb;
    } else if (ch == 3) {
      g_config.ch3_baud_rate = baud;
      g_config.ch3_data_bits = db;
      g_config.ch3_parity = pr;
      g_config.ch3_stop_bits = sb;
    } else if (ch == 4) {
      g_config.doorphone_baud_rate = baud;
      g_config.doorphone_data_bits = db;
      g_config.doorphone_parity = pr;
      g_config.doorphone_stop_bits = sb;
    } else {
      return false;
    }
    g_config_dirty.store(true, std::memory_order_release);
  }

  if (ch >= 1 && ch <= 3) {
    uart_port_t port = (ch == 1)   ? UART_NUM_0
                       : (ch == 2) ? UART_NUM_1
                                   : UART_NUM_2;
    uart_set_baudrate(port, baud);
    uart_set_word_length(port, (db == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS);
    uart_set_parity(port, to_uart_parity(pr));
    uart_set_stop_bits(port, to_uart_stopbits(sb));
    uart_flush_input(port);
  } else if (ch == 4) {
    g_doorphone_serial.begin(baud, Door_SerialConfig(db, pr, sb),
                             Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
    pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
  }

  Config_Save();
  ::Serial.printf("[UART] CH%u reconfigured: %u bps, %s\r\n", ch, baud, format);
  return true;
}
