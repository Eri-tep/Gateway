// ============================================================================
// NetworkRouter: Level 2 IP Routing, Subnet Whitelist & Port Mapping Implementation
// ============================================================================

#include "Transport/NetworkRouter.h"
#include "Base/SystemConfig.h"
#include "Base/SystemPlatform.h"
#include "System/LockUtils.h"

#include <WiFi.h>
#include <mutex>
#include <shared_mutex>

extern SoftwareSerial g_doorphone_serial;

// ── IP Subnet Whitelist Filters ──

bool Tcp_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

bool Telnet_IsAllowedIP(IPAddress ip) {
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}

// ── Physical UART Dynamic Configuration ──

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
    return (s == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
  };

  {
    std::unique_lock lock(g_config_rw);
    switch (ch) {
    case 1:
      g_config.uart_baud_rate = baud;
      g_config.uart_data_bits = db;
      g_config.uart_parity = pr;
      g_config.uart_stop_bits = sb;
      break;
    case 2:
      g_config.ch2_baud_rate = baud;
      g_config.ch2_data_bits = db;
      g_config.ch2_parity = pr;
      g_config.ch2_stop_bits = sb;
      break;
    case 3:
      g_config.ch3_baud_rate = baud;
      g_config.ch3_data_bits = db;
      g_config.ch3_parity = pr;
      g_config.ch3_stop_bits = sb;
      break;
    case 4:
      g_config.doorphone_baud_rate = baud;
      g_config.doorphone_data_bits = db;
      g_config.doorphone_parity = pr;
      g_config.doorphone_stop_bits = sb;
      break;
    default:
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

// ── Routing Table & Multi-Channel Dispatch Registry ──

DeviceRouteRegistry g_route_registry;

namespace {
template <class E>
int findRouteIdx(const E *e, size_t n, uint8_t d, uint8_t s1, uint8_t s2) {
  for (size_t i = 0; i < n; ++i)
    if (e[i].dev_id == d && e[i].sub1 == s1 && e[i].sub2 == s2)
      return static_cast<int>(i);
  return -1;
}
} // anonymous namespace

void DeviceRouteRegistry::recordRoute(uint8_t channel_id, int8_t slot_idx,
                                      uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2) {
  CriticalSectionLocker lock(&_mux);
  int i = findRouteIdx(&_entries[0], _count, dev_id, sub1, sub2);
  if (i < 0) {
    if (_count >= MAX_ROUTES)
      return;
    i = static_cast<int>(_count++);
    _entries[i].dev_id = dev_id;
    _entries[i].sub1 = sub1;
    _entries[i].sub2 = sub2;
  }
  _entries[i].endpoint.channel_id = channel_id;
  _entries[i].endpoint.slot_idx = slot_idx;
  _entries[i].endpoint.last_seen_ms = millis();
}

bool DeviceRouteRegistry::lookupRoute(uint8_t dev_id, uint8_t sub1,
                                      uint8_t sub2,
                                      RouteEndpoint &out_ep) const {
  CriticalSectionLocker lock(&_mux);
  const int i = findRouteIdx(&_entries[0], _count, dev_id, sub1, sub2);
  if (i < 0)
    return false;
  out_ep = _entries[i].endpoint;
  return true;
}

size_t DeviceRouteRegistry::getRoutes(DeviceRouteEntry *out_buf,
                                      size_t max_count) const {
  CriticalSectionLocker lock(&_mux);
  const size_t n = std::min(_count, max_count);
  for (size_t i = 0; i < n; i++)
    out_buf[i] = _entries[i];
  return n;
}

void DeviceRouteRegistry::clear() {
  CriticalSectionLocker lock(&_mux);
  _count = 0;
  memset(_entries, 0, sizeof(_entries));
}
