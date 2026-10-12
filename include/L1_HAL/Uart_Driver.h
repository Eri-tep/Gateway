#pragma once

// ============================================================================
// Uart_Driver.h — L1 Physical / HAL Drivers
// HW UART (UART_NUM_0~2) & Doorphone SW Serial Unified HAL
// Canonical 4+1 Layer: L1 — no #include of L2+ headers allowed here.
// ============================================================================
//
// All UART hardware state (SoftwareSerial instance, HW UART port mappings)
// is 100% static-sealed inside Uart_Driver.cpp. Zero extern leaks.
//
// API naming: Domain_VerbNoun per MODERN_CPP_GUIDELINES.md §1.1
// ============================================================================

#include "L0_Foundation/System_Platform.h"
#include "L0_Foundation/System_Config.h"
#include <driver/uart.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

// ── HW UART Channel Identifiers ─────────────────────────────────────────────
// CH1 = UART_NUM_0  (Wallpad main RS-485 bus)
// CH2 = UART_NUM_1  (Sub-device RS-485 bus)
// CH3 = UART_NUM_2  (Ventilation / Temp RS-485 bus)
// CH4 = SW Serial   (Software serial isolated line)

// ── Strongly-typed Framing Enums (Aligned with NVS storage values) ──────────
enum class DataBits : uint8_t { Five = 5, Six = 6, Seven = 7, Eight = 8 };
enum class Parity   : uint8_t { None = 0, Even = 1, Odd = 2 };
enum class StopBits : uint8_t { One = 1, Two = 2 };

struct UartFraming {
  DataBits data_bits{DataBits::Eight};
  Parity   parity{Parity::None};
  StopBits stop_bits{StopBits::One};
};

struct UartHwConfig {
  uint32_t    baud{115200};
  UartFraming framing{};
};

// ── IDF Hardware Register Conversions (Explicit Mapping) ────────────────────
[[nodiscard]] constexpr uart_parity_t toIdf(Parity p) noexcept {
  switch (p) {
  case Parity::None: return UART_PARITY_DISABLE;
  case Parity::Even: return UART_PARITY_EVEN;
  case Parity::Odd:  return UART_PARITY_ODD;
  }
  return UART_PARITY_DISABLE;
}

[[nodiscard]] constexpr uart_stop_bits_t toIdf(StopBits s) noexcept {
  switch (s) {
  case StopBits::One: return UART_STOP_BITS_1;
  case StopBits::Two: return UART_STOP_BITS_2;
  }
  return UART_STOP_BITS_1;
}

[[nodiscard]] constexpr uart_word_length_t toIdf(DataBits d) noexcept {
  switch (d) {
  case DataBits::Five:  return UART_DATA_5_BITS;
  case DataBits::Six:   return UART_DATA_6_BITS;
  case DataBits::Seven: return UART_DATA_7_BITS;
  case DataBits::Eight: return UART_DATA_8_BITS;
  }
  return UART_DATA_8_BITS;
}

static_assert(toIdf(Parity::None) == UART_PARITY_DISABLE);
static_assert(toIdf(Parity::Even) == UART_PARITY_EVEN);
static_assert(toIdf(Parity::Odd) == UART_PARITY_ODD);
static_assert(toIdf(StopBits::One) == UART_STOP_BITS_1);
static_assert(toIdf(StopBits::Two) == UART_STOP_BITS_2);
static_assert(toIdf(DataBits::Eight) == UART_DATA_8_BITS);
static_assert(toIdf(DataBits::Seven) == UART_DATA_7_BITS);

// ── Config Construction Helpers ─────────────────────────────────────────────
[[nodiscard]] constexpr UartHwConfig makeUartConfig(uint32_t baud,
                                                     DataBits data_bits,
                                                     Parity parity,
                                                     StopBits stop_bits) noexcept {
  return UartHwConfig{
      .baud = baud,
      .framing = { .data_bits = data_bits, .parity = parity, .stop_bits = stop_bits }
  };
}

[[nodiscard]] constexpr UartHwConfig makeUartConfig(uint32_t baud,
                                                     uint8_t data_bits,
                                                     uint8_t parity,
                                                     uint8_t stop_bits) noexcept {
  auto d = toEnum<DataBits>(data_bits, DataBits::Five, DataBits::Eight).value_or(DataBits::Eight);
  auto p = toEnum<Parity>(parity, Parity::None, Parity::Odd).value_or(Parity::None);
  auto s = toEnum<StopBits>(stop_bits, StopBits::One, StopBits::Two).value_or(StopBits::One);
  return makeUartConfig(baud, d, p, s);
}

// ── Thread Safety Contract ───────────────────────────────────────────────────
// Uart_Write* and Uart_Read* must be synchronized by caller if accessed from
// multiple FreeRTOS tasks.
// Uart_Reconfig* MUST NOT be called concurrently with active Write/Read operations.
// ─────────────────────────────────────────────────────────────────────────────

// ── Initialisation ───────────────────────────────────────────────────────────

/// Initialise HW UART port with full framing parameters.
/// @param port       UART_NUM_0 / UART_NUM_1 / UART_NUM_2
/// @param tx_pin     GPIO pin number for TX
/// @param rx_pin     GPIO pin number for RX
/// @param cfg        Baud rate and framing configuration
/// @param event_queue_out  Optional FreeRTOS queue handle pointer for UART events
[[nodiscard]] esp_err_t Uart_InitHw(uart_port_t port, int tx_pin, int rx_pin,
                                    const UartHwConfig &cfg,
                                    QueueHandle_t *event_queue_out = nullptr) noexcept;

[[nodiscard, gnu::always_inline]] inline esp_err_t
Uart_InitHw(uart_port_t port, int tx_pin, int rx_pin,
            uint32_t baud, uint8_t data_bits, uint8_t parity,
            uint8_t stop_bits, QueueHandle_t *event_queue_out = nullptr) noexcept {
  return Uart_InitHw(port, tx_pin, rx_pin,
                     makeUartConfig(baud, data_bits, parity, stop_bits),
                     event_queue_out);
}

/// Initialise CH4 SW Serial line (SoftwareSerial — static sealed).
[[nodiscard]] esp_err_t Uart_InitSwSerial(int rx_pin, int tx_pin,
                                          const UartHwConfig &cfg) noexcept;

[[nodiscard, gnu::always_inline]] inline esp_err_t
Uart_InitSwSerial(uint32_t baud, uint8_t data_bits, uint8_t parity,
                  uint8_t stop_bits, int rx_pin, int tx_pin) noexcept {
  return Uart_InitSwSerial(rx_pin, tx_pin,
                           makeUartConfig(baud, data_bits, parity, stop_bits));
}

// ── Dynamic Reconfiguration ───────────────────────────────────────────────────

/// Apply new baud + framing to a live HW UART port without reinstalling driver.
[[nodiscard]] esp_err_t Uart_ReconfigHw(uart_port_t port, const UartHwConfig &cfg) noexcept;

[[nodiscard, gnu::always_inline]] inline esp_err_t
Uart_ReconfigHw(uart_port_t port, uint32_t baud,
                uint8_t data_bits, uint8_t parity, uint8_t stop_bits) noexcept {
  return Uart_ReconfigHw(port, makeUartConfig(baud, data_bits, parity, stop_bits));
}

/// Reconfigure CH4 SW Serial with new framing.
[[nodiscard]] esp_err_t Uart_ReconfigSwSerial(const UartHwConfig &cfg) noexcept;

[[nodiscard, gnu::always_inline]] inline esp_err_t
Uart_ReconfigSwSerial(uint32_t baud, uint8_t data_bits,
                      uint8_t parity, uint8_t stop_bits) noexcept {
  return Uart_ReconfigSwSerial(makeUartConfig(baud, data_bits, parity, stop_bits));
}

// ── Write ─────────────────────────────────────────────────────────────────────

/// Write bytes to a HW UART port TX FIFO (blocks if TX ring buffer / FIFO is full).
/// Hot-path safe: zero heap allocation.
[[nodiscard]] int Uart_WriteHw(uart_port_t port, std::span<const uint8_t> data) noexcept;

[[nodiscard, gnu::always_inline]] inline int
Uart_WriteHw(uart_port_t port, const uint8_t *buf, size_t len) noexcept {
  return buf ? Uart_WriteHw(port, std::span<const uint8_t>{buf, len}) : 0;
}

/// Write bytes to CH4 SW Serial.
/// Hot-path safe: zero heap allocation.
void Uart_WriteSwSerial(std::span<const uint8_t> data) noexcept;

[[gnu::always_inline]] inline void
Uart_WriteSwSerial(const uint8_t *buf, size_t len) noexcept {
  if (buf) {
    Uart_WriteSwSerial(std::span<const uint8_t>{buf, len});
  }
}

// ── Read ──────────────────────────────────────────────────────────────────────

/// Read available bytes from HW UART RX buffer into caller-supplied buf.
/// Returns number of bytes actually read (0 if nothing available).
/// Hot-path safe: zero heap allocation, non-blocking.
[[nodiscard]] size_t Uart_ReadHw(uart_port_t port, std::span<uint8_t> out_buf) noexcept;

[[nodiscard, gnu::always_inline]] inline size_t
Uart_ReadHw(uart_port_t port, uint8_t *buf, size_t max_len) noexcept {
  return buf ? Uart_ReadHw(port, std::span<uint8_t>{buf, max_len}) : 0;
}

/// Read available bytes from CH4 SW Serial.
/// Returns number of bytes actually read.
[[nodiscard]] size_t Uart_ReadSwSerial(std::span<uint8_t> out_buf) noexcept;

[[nodiscard, gnu::always_inline]] inline size_t
Uart_ReadSwSerial(uint8_t *buf, size_t max_len) noexcept {
  return buf ? Uart_ReadSwSerial(std::span<uint8_t>{buf, max_len}) : 0;
}

// ── Poll ──────────────────────────────────────────────────────────────────────

/// Returns number of bytes available in HW UART RX ring buffer.
[[nodiscard]] size_t Uart_AvailableHw(uart_port_t port) noexcept;

/// Returns number of bytes available in CH4 SW Serial RX buffer.
[[nodiscard]] size_t Uart_AvailableSwSerial() noexcept;

// ── Flush ─────────────────────────────────────────────────────────────────────

/// Flush HW UART RX ring buffer (discard pending bytes).
void Uart_FlushHw(uart_port_t port) noexcept;
