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

void Uart_InitSwSerial(uint32_t baud, uint8_t data_bits, uint8_t parity,
                        uint8_t stop_bits, int rx_pin, int tx_pin) {
  s_doorphone_serial.begin(baud,
                           priv_doorSerialConfig(data_bits, parity, stop_bits),
                           rx_pin, tx_pin);
  pinMode(rx_pin, INPUT_PULLUP);
}

// ── Dynamic Reconfiguration ───────────────────────────────────────────────────

void Uart_ReconfigHw(uart_port_t port, const UartHwConfig &cfg) {
  Uart_ReconfigHw(port, cfg.baud, cfg.data_bits, cfg.parity, cfg.stop_bits);
}

void Uart_ReconfigHw(uart_port_t port, uint32_t baud,
                     uint8_t data_bits, uint8_t parity, uint8_t stop_bits) {
  uart_set_baudrate(port, baud);
  uart_set_word_length(port, priv_toWordLen(data_bits));
  uart_set_parity(port, priv_toParity(parity));
  uart_set_stop_bits(port, priv_toStopBits(stop_bits));
  uart_flush_input(port);
}

void Uart_ReconfigSwSerial(const UartHwConfig &cfg) {
  Uart_ReconfigSwSerial(cfg.baud, cfg.data_bits, cfg.parity, cfg.stop_bits);
}

void Uart_ReconfigSwSerial(uint32_t baud, uint8_t data_bits,
                            uint8_t parity, uint8_t stop_bits) {
  s_doorphone_serial.begin(baud,
                           priv_doorSerialConfig(data_bits, parity, stop_bits),
                           Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
  pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
}

// ── Write ─────────────────────────────────────────────────────────────────────

int Uart_WriteHw(uart_port_t port, const uint8_t *buf, size_t len) {
  return uart_write_bytes(port, reinterpret_cast<const char *>(buf),
                          static_cast<int>(len));
}

int Uart_WriteHw(uart_port_t port, std::span<const uint8_t> data) {
  return Uart_WriteHw(port, data.data(), data.size());
}

void Uart_WriteSwSerial(const uint8_t *buf, size_t len) {
  s_doorphone_serial.write(buf, len);
}

void Uart_WriteSwSerial(std::span<const uint8_t> data) {
  Uart_WriteSwSerial(data.data(), data.size());
}

// ── Read ──────────────────────────────────────────────────────────────────────

size_t Uart_ReadHw(uart_port_t port, uint8_t *buf, size_t max_len) {
  int n = uart_read_bytes(port, buf, static_cast<uint32_t>(max_len), 0);
  return (n > 0) ? static_cast<size_t>(n) : 0u;
}

size_t Uart_ReadHw(uart_port_t port, std::span<uint8_t> out_buf) {
  return Uart_ReadHw(port, out_buf.data(), out_buf.size());
}

size_t Uart_ReadSwSerial(uint8_t *buf, size_t max_len) {
  size_t count = 0;
  while (count < max_len && s_doorphone_serial.available() > 0) {
    buf[count++] = static_cast<uint8_t>(s_doorphone_serial.read());
  }
  return count;
}

size_t Uart_ReadSwSerial(std::span<uint8_t> out_buf) {
  return Uart_ReadSwSerial(out_buf.data(), out_buf.size());
}

// ── Poll ──────────────────────────────────────────────────────────────────────

size_t Uart_AvailableHw(uart_port_t port) {
  size_t avail = 0;
  uart_get_buffered_data_len(port, &avail);
  return avail;
}

size_t Uart_AvailableSwSerial() {
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
