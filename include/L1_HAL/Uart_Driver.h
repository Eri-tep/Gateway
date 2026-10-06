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
#include <driver/uart.h>
#include <cstddef>
#include <cstdint>
#include <span>

// ── HW UART Channel Identifiers ─────────────────────────────────────────────
// CH1 = UART_NUM_0  (Wallpad main RS-485 bus)
// CH2 = UART_NUM_1  (Sub-device RS-485 bus)
// CH3 = UART_NUM_2  (Ventilation / Temp RS-485 bus)
// CH4 = SW Serial   (Software serial isolated line)

// ── Initialisation ───────────────────────────────────────────────────────────

/// Initialise HW UART port with full framing parameters.
/// @param port       UART_NUM_0 / UART_NUM_1 / UART_NUM_2
/// @param tx_pin     GPIO pin number for TX
/// @param rx_pin     GPIO pin number for RX
/// @param baud       Baud rate (1200 ~ 921600)
/// @param data_bits  7 or 8
/// @param parity     0=None  1=Even  2=Odd
/// @param stop_bits  1 or 2
/// @param event_queue_out  Optional FreeRTOS queue handle pointer for UART events
void Uart_InitHw(uart_port_t port, int tx_pin, int rx_pin,
                 uint32_t baud, uint8_t data_bits, uint8_t parity,
                 uint8_t stop_bits, QueueHandle_t *event_queue_out);

/// Initialise CH4 SW Serial line (SoftwareSerial — static sealed).
/// @param baud       Baud rate
/// @param data_bits  7 or 8
/// @param parity     0=None  1=Even  2=Odd
/// @param stop_bits  1 or 2
/// @param rx_pin     GPIO RX pin (defaults to Config::GPIO::RX_GPIO)
/// @param tx_pin     GPIO TX pin (defaults to Config::GPIO::TX_GPIO)
void Uart_InitSwSerial(uint32_t baud, uint8_t data_bits, uint8_t parity,
                       uint8_t stop_bits, int rx_pin, int tx_pin);

// ── Dynamic Reconfiguration ───────────────────────────────────────────────────

struct UartHwConfig {
  uint32_t baud{115200};
  uint8_t data_bits{8};
  uint8_t parity{0};
  uint8_t stop_bits{1};
};

/// Apply new baud + framing to a live HW UART port without reinstalling driver.
void Uart_ReconfigHw(uart_port_t port, const UartHwConfig &cfg);
void Uart_ReconfigHw(uart_port_t port, uint32_t baud,
                     uint8_t data_bits, uint8_t parity, uint8_t stop_bits);

/// Reconfigure CH4 SW Serial with new framing (re-calls begin()).
void Uart_ReconfigSwSerial(const UartHwConfig &cfg);
void Uart_ReconfigSwSerial(uint32_t baud, uint8_t data_bits,
                           uint8_t parity, uint8_t stop_bits);

// ── Write ─────────────────────────────────────────────────────────────────────

/// Write bytes to a HW UART port TX FIFO (non-blocking, returns bytes written).
/// Hot-path safe: zero heap allocation.
int Uart_WriteHw(uart_port_t port, const uint8_t *buf, size_t len);
int Uart_WriteHw(uart_port_t port, std::span<const uint8_t> data);

/// Write bytes to CH4 SW Serial.
/// Hot-path safe: zero heap allocation.
void Uart_WriteSwSerial(const uint8_t *buf, size_t len);
void Uart_WriteSwSerial(std::span<const uint8_t> data);

// ── Read ──────────────────────────────────────────────────────────────────────

/// Read available bytes from HW UART RX buffer into caller-supplied buf.
/// Returns number of bytes actually read (0 if nothing available).
/// Hot-path safe: zero heap allocation, non-blocking.
size_t Uart_ReadHw(uart_port_t port, uint8_t *buf, size_t max_len);
size_t Uart_ReadHw(uart_port_t port, std::span<uint8_t> out_buf);

/// Read available bytes from CH4 SW Serial.
/// Returns number of bytes actually read.
size_t Uart_ReadSwSerial(uint8_t *buf, size_t max_len);
size_t Uart_ReadSwSerial(std::span<uint8_t> out_buf);

// ── Poll ──────────────────────────────────────────────────────────────────────

/// Returns number of bytes available in HW UART RX ring buffer.
size_t Uart_AvailableHw(uart_port_t port);

/// Returns number of bytes available in CH4 SW Serial RX buffer.
size_t Uart_AvailableSwSerial();

// ── Flush ─────────────────────────────────────────────────────────────────────

/// Flush HW UART RX ring buffer (discard pending bytes).
void Uart_FlushHw(uart_port_t port);

// ── Backward-Compatibility Inline Wrappers (Step 2 Domain Neutralization) ──────

inline void Uart_InitDoorphone(uint32_t baud, uint8_t data_bits, uint8_t parity,
                               uint8_t stop_bits, int rx_pin, int tx_pin) {
  Uart_InitSwSerial(baud, data_bits, parity, stop_bits, rx_pin, tx_pin);
}

inline void Uart_ReconfigDoorphone(const UartHwConfig &cfg) {
  Uart_ReconfigSwSerial(cfg);
}

inline void Uart_ReconfigDoorphone(uint32_t baud, uint8_t data_bits,
                                    uint8_t parity, uint8_t stop_bits) {
  Uart_ReconfigSwSerial(baud, data_bits, parity, stop_bits);
}

inline void Uart_WriteDoorphone(const uint8_t *buf, size_t len) {
  Uart_WriteSwSerial(buf, len);
}

inline void Uart_WriteDoorphone(std::span<const uint8_t> data) {
  Uart_WriteSwSerial(data);
}

inline size_t Uart_ReadDoorphone(uint8_t *buf, size_t max_len) {
  return Uart_ReadSwSerial(buf, max_len);
}

inline size_t Uart_ReadDoorphone(std::span<uint8_t> out_buf) {
  return Uart_ReadSwSerial(out_buf);
}

inline size_t Uart_AvailableDoorphone() {
  return Uart_AvailableSwSerial();
}
