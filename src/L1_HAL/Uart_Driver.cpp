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

// Forward declaration of internal SW serial framing mapper.
static SoftwareSerialConfig toSwSerialConfig(const UartFraming &f) noexcept;

// ── Initialisation ────────────────────────────────────────────────────────────

esp_err_t Uart_InitHw(uart_port_t port, int tx_pin, int rx_pin,
                      const UartHwConfig &cfg,
                      QueueHandle_t *event_queue_out) noexcept {
  if (port < 0 || port >= UART_NUM_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  uart_config_t uart_cfg = {
      .baud_rate  = static_cast<int>(cfg.baud),
      .data_bits  = toIdf(cfg.framing.data_bits),
      .parity     = toIdf(cfg.framing.parity),
      .stop_bits  = toIdf(cfg.framing.stop_bits),
      .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      .source_clk = UART_SCLK_DEFAULT,
#else
      .source_clk = UART_SCLK_APB,
#endif
  };
  esp_err_t err = uart_param_config(port, &uart_cfg);
  if (err != ESP_OK) return err;

  err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) return err;

  return uart_driver_install(port,
                             Config::Packet::UART_HW_RX_BUF_SIZE, 0,
                             Config::Queue::UART_EVENT_QUEUE_SIZE,
                             event_queue_out, 0);
}

esp_err_t Uart_InitSwSerial(int rx_pin, int tx_pin,
                            const UartHwConfig &cfg) noexcept {
  if (rx_pin < 0 || tx_pin < 0) {
    return ESP_ERR_INVALID_ARG;
  }
  s_doorphone_serial.begin(cfg.baud, toSwSerialConfig(cfg.framing), rx_pin, tx_pin);
  pinMode(rx_pin, INPUT_PULLUP);
  return ESP_OK;
}

// ── Dynamic Reconfiguration ───────────────────────────────────────────────────

esp_err_t Uart_ReconfigHw(uart_port_t port, const UartHwConfig &cfg) noexcept {
  if (port < 0 || port >= UART_NUM_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t err = uart_set_baudrate(port, cfg.baud);
  if (err != ESP_OK) return err;
  err = uart_set_word_length(port, toIdf(cfg.framing.data_bits));
  if (err != ESP_OK) return err;
  err = uart_set_parity(port, toIdf(cfg.framing.parity));
  if (err != ESP_OK) return err;
  err = uart_set_stop_bits(port, toIdf(cfg.framing.stop_bits));
  if (err != ESP_OK) return err;
  return uart_flush_input(port);
}

esp_err_t Uart_ReconfigSwSerial(const UartHwConfig &cfg) noexcept {
  s_doorphone_serial.begin(cfg.baud, toSwSerialConfig(cfg.framing),
                           Config::GPIO::RX_GPIO, Config::GPIO::TX_GPIO);
  pinMode(Config::GPIO::RX_GPIO, INPUT_PULLUP);
  return ESP_OK;
}

// ── Write ─────────────────────────────────────────────────────────────────────

int Uart_WriteHw(uart_port_t port, std::span<const uint8_t> data) noexcept {
  if (data.empty() || !data.data())
    return 0;
  return uart_write_bytes(port, reinterpret_cast<const char *>(data.data()),
                          static_cast<int>(data.size()));
}

void Uart_WriteSwSerial(std::span<const uint8_t> data) noexcept {
  if (data.empty() || !data.data())
    return;
  s_doorphone_serial.write(data.data(), data.size());
}

// ── Read ──────────────────────────────────────────────────────────────────────

size_t Uart_ReadHw(uart_port_t port, std::span<uint8_t> out_buf) noexcept {
  if (out_buf.empty() || !out_buf.data())
    return 0u;
  int n = uart_read_bytes(port, out_buf.data(), static_cast<uint32_t>(out_buf.size()), 0);
  return (n > 0) ? static_cast<size_t>(n) : 0u;
}

size_t Uart_ReadSwSerial(std::span<uint8_t> out_buf) noexcept {
  if (out_buf.empty() || !out_buf.data())
    return 0u;
  size_t count = 0;
  while (count < out_buf.size() && s_doorphone_serial.available() > 0) {
    out_buf[count++] = static_cast<uint8_t>(s_doorphone_serial.read());
  }
  return count;
}

// ── Poll ──────────────────────────────────────────────────────────────────────

size_t Uart_AvailableHw(uart_port_t port) noexcept {
  size_t avail = 0;
  if (uart_get_buffered_data_len(port, &avail) != ESP_OK)
    return 0u;
  return avail;
}

size_t Uart_AvailableSwSerial() noexcept {
  int n = s_doorphone_serial.available();
  return (n > 0) ? static_cast<size_t>(n) : 0u;
}

// ── Flush ─────────────────────────────────────────────────────────────────────

void Uart_FlushHw(uart_port_t port) noexcept {
  uart_flush_input(port);
}

// ── Utility (internal) ────────────────────────────────────────────────────────

namespace {
struct SwSerialConfigMap {
  DataBits data_bits;
  StopBits stop_bits;
  Parity parity;
  SoftwareSerialConfig cfg;
};

static constexpr SwSerialConfigMap kSwSerialTable[] = {
    {DataBits::Seven, StopBits::One, Parity::Even, SWSERIAL_7E1},
    {DataBits::Seven, StopBits::One, Parity::Odd,  SWSERIAL_7O1},
    {DataBits::Eight, StopBits::One, Parity::Even, SWSERIAL_8E1},
    {DataBits::Eight, StopBits::One, Parity::Odd,  SWSERIAL_8O1},
    {DataBits::Eight, StopBits::One, Parity::None, SWSERIAL_8N1},
    {DataBits::Eight, StopBits::Two, Parity::None, SWSERIAL_8N2},
};
} // namespace

static SoftwareSerialConfig toSwSerialConfig(const UartFraming &f) noexcept {
  for (const auto &entry : kSwSerialTable) {
    if (entry.data_bits == f.data_bits &&
        entry.stop_bits == f.stop_bits &&
        entry.parity == f.parity) {
      return entry.cfg;
    }
  }
  return SWSERIAL_8N1;  // safe default
}

