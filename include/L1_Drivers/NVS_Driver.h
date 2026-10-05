#pragma once

#include "L0_Base/System_Buffer.h"
#include "L0_Base/System_Config.h"
#include "L0_Base/System_Platform.h"
#include <Arduino.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x434C4E52; // 'CLNR'
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343;        // 'RESC'
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431;           // 'WDT1'

// ── RTC Fast SRAM Retention Variables ──

extern uint32_t rtc_magic;
extern uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
extern volatile uint32_t g_telnet_stage;
#define TSTAGE(n)                                                              \
  (g_telnet_stage = (0xA5A50000u | (static_cast<uint32_t>(n) & 0xFFFFu)))
extern uint32_t rtc_rescue_magic;
extern uint32_t rtc_crash_counter;
extern uint32_t rtc_clean_restart_magic;

// ── System Boot & Rescue Status ──

extern std::atomic<bool> g_rescue_mode;
extern bool g_rollback_detected;
