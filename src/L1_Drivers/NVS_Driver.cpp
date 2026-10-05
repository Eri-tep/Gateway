#include "L1_Drivers/NVS_Driver.h"
#include <Arduino.h>

RTC_NOINIT_ATTR uint32_t rtc_magic;
RTC_NOINIT_ATTR uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
RTC_NOINIT_ATTR volatile uint32_t g_telnet_stage = 0;
RTC_NOINIT_ATTR uint32_t rtc_rescue_magic;
RTC_NOINIT_ATTR uint32_t rtc_crash_counter;
RTC_NOINIT_ATTR uint32_t rtc_clean_restart_magic;

std::atomic<bool> g_rescue_mode{false};
bool g_rollback_detected = false;
