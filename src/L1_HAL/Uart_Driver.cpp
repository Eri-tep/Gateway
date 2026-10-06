// ============================================================================
// Uart_Driver.cpp — L1 Physical / HAL Drivers
// HW UART (UART_NUM_0~2) & Doorphone SW Serial — 100% static-sealed
// ============================================================================
//
// INVARIANTS (AGENTS.md):
//   - g_doorphone_serial is static to this TU. Zero extern leak.
//   - No dynamic allocation (new / malloc / String) in any path.
//   - No #include of L2+ headers (Channels / Routing / Services).
// ============================================================================

#include "L1_HAL/Uart_Driver.h"
#include "L0_Foundation/System_Config.h"

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <driver/uart.h>

// ── Static-sealed Doorphone SW Serial instance (L1 private) ─────────────────
// Previously: `extern SoftwareSerial g_doorphone_serial;` in NetworkRouter.h
// Now: completely hidden inside this translation unit. Rule 17 compliant.
static SoftwareSerial s_doorphone_serial;

// ── Internal helpers ──────────────────────────────────────────────────────────

// Forward declaration — defined at end of file.
static SoftwareSerialConfig priv_doorSerialConfig(uint8_t data_bits,
                                                   uint8_t parity,
                                                   uint8_t stop_bits);

static uart_parity_t priv_toParity(uint8_t p) {
  return (p == 1) ? UART_PARITY_EVEN
       : (p == 2) ? UART_PARITY_ODD
                  : UART_PARITY_DISABLE;
}

static uart_stop_bits_t priv_toStopBits(uint8_t s) {
  return (s == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static uart_word_length_t priv_toWordLen(uint8_t d) {
  return (d == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;
}

// ── Initialisation ────────────────────────────────────────────────────────────

void Uart_InitHw(uart_port_t port, int tx_pin, int rx_pin,
                 uint32_t baud, uint8_t data_bits, uint8_t parity,
                 uint8_t stop_bits, QueueHandle_t *event_queue_out) {
  uart_config_t cfg = {
      .baud_rate  = static_cast<int>(baud),
      .data_bits  = priv_toWordLen(data_bits),
      .parity     = priv_toParity(parity),
      .stop_bits  = priv_toStopBits(stop_bits),
      .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      .source_clk = UART_SCLK_DEFAULT,
#else
      .source_clk = UART_SCLK_APB,
#endif
  };
  uart_param_config(port, &cfg);
  uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  uart_driver_install(port,
                      Config::Packet::UART_HW_RX_BUF_SIZE, 0,
                      Config::Queue::UART_EVENT_QUEUE_SIZE,
                      event_queue_out, 0);
}

void Uart_InitDoorphone(uint32_t baud, uint8_t data_bits, uint8_t parity,
                        uint8_t stop_bits, int rx_pin, int tx_pin) {
  s_doorphone_serial.begin(baud,
                           priv_doorSerialConfig(data_bits, parity, stop_bits),
                           rx_pin, tx_pin);
  pinMode(rx_pin, INPUT_PULLUP);
}

// ── Dynamic Reconfiguration ───────────────────────────────────────────────────

void Uart_ReconfigHw(uart_port_t port, uint32_t baud,
                     uint8_t data_bits, uint8_t parity, uint8_t stop_bits) {
  uart_set_baudrate(port, baud);
  uart_set_word_length(port, priv_toWordLen(data_bits));
  uart_set_parity(port, priv_toParity(parity));
  uart_set_stop_bits(port, priv_toStopBits(stop_bits));
  uart_flush_input(port);
}

void Uart_ReconfigDoorphone(uint32_t baud, uint8_t data_bits,
                             uint8_t parity, uint8_t stop_bits) {
  s_doorphone_serial.begin(baud,
                           priv_doorSerialConfig(data_bits, parity, stop_bits),
                           Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
  pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
}

bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format) {
  uint8_t db = 8, pr = 0, sb = 1;
  if (!parseFramingStr(format, db, pr, sb))
    return false;
  if (baud < 1200 || baud > 921600)
    return false;

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
    Uart_ReconfigHw(port, baud, db, pr, sb);
  } else if (ch == 4) {
    Uart_ReconfigDoorphone(baud, db, pr, sb);
  }

  Config_Save();
  ::Serial.printf("[UART] CH%u reconfigured: %u bps, %s\r\n", ch, baud, format);
  return true;
}

// ── Write ─────────────────────────────────────────────────────────────────────

int Uart_WriteHw(uart_port_t port, const uint8_t *buf, size_t len) {
  return uart_write_bytes(port, reinterpret_cast<const char *>(buf),
                          static_cast<int>(len));
}

void Uart_WriteDoorphone(const uint8_t *buf, size_t len) {
  s_doorphone_serial.write(buf, len);
}

// ── Read ──────────────────────────────────────────────────────────────────────

size_t Uart_ReadHw(uart_port_t port, uint8_t *buf, size_t max_len) {
  int n = uart_read_bytes(port, buf, static_cast<uint32_t>(max_len), 0);
  return (n > 0) ? static_cast<size_t>(n) : 0u;
}

size_t Uart_ReadDoorphone(uint8_t *buf, size_t max_len) {
  size_t count = 0;
  while (count < max_len && s_doorphone_serial.available() > 0) {
    buf[count++] = static_cast<uint8_t>(s_doorphone_serial.read());
  }
  return count;
}

// ── Poll ──────────────────────────────────────────────────────────────────────

size_t Uart_AvailableHw(uart_port_t port) {
  size_t avail = 0;
  uart_get_buffered_data_len(port, &avail);
  return avail;
}

size_t Uart_AvailableDoorphone() {
  int n = s_doorphone_serial.available();
  return (n > 0) ? static_cast<size_t>(n) : 0u;
}

// ── Flush ─────────────────────────────────────────────────────────────────────

void Uart_FlushHw(uart_port_t port) {
  uart_flush_input(port);
}

// ── Utility (internal) ────────────────────────────────────────────────────────

static SoftwareSerialConfig priv_doorSerialConfig(uint8_t data_bits,
                                                   uint8_t parity,
                                                   uint8_t stop_bits) {
  if (data_bits == 7 && stop_bits == 1) {
    if (parity == 1) return SWSERIAL_7E1;
    if (parity == 2) return SWSERIAL_7O1;
  } else if (data_bits == 8) {
    if (stop_bits == 1) {
      if (parity == 1) return SWSERIAL_8E1;
      if (parity == 2) return SWSERIAL_8O1;
      return SWSERIAL_8N1;
    } else if (stop_bits == 2 && parity == 0) {
      return SWSERIAL_8N2;
    }
  }
  return SWSERIAL_8N1;  // safe default
}
