#include "L0_Foundation/System_Platform.h"
#include <cstring>
#include <lwip/sockets.h>

// ── System Lifecycle & Synchronization Primitives ──
EventGroupHandle_t g_system_event_group = nullptr;
std::atomic<bool> g_ota_in_progress{false};

// ── Global Decoupled Diagnostic Trace Message Sink ──
static std::atomic<SystemTraceMessageFn> s_trace_msg_sink{nullptr};
static std::atomic<SystemTracePacketFn> s_trace_pkt_sink{nullptr};

void System_RegisterTraceMessageSink(SystemTraceMessageFn fn) noexcept {
  s_trace_msg_sink.store(fn, std::memory_order_release);
}

void System_RegisterTracePacketSink(SystemTracePacketFn fn) noexcept {
  s_trace_pkt_sink.store(fn, std::memory_order_release);
}

void System_TraceMessage(const char *msg) noexcept {
  auto sink = s_trace_msg_sink.load(std::memory_order_acquire);
  if (sink && msg) {
    sink(msg);
  }
}

void System_TracePacket(uint8_t channel, bool is_tx, TraceType type,
                        const StaticPacket &pkt) noexcept {
  auto sink = s_trace_pkt_sink.load(std::memory_order_acquire);
  if (sink) {
    sink(channel, is_tx, type, pkt);
  }
}



// ── Watchdog Feeding Hook Bridge ──────────────────────────────────────────────
static std::atomic<WdtFeedHook> s_wdt_feed_hook{nullptr};

void System_RegisterWdtHook(WdtFeedHook hook) noexcept {
  s_wdt_feed_hook.store(hook, std::memory_order_release);
}

void System_FeedWdt(size_t index) noexcept {
  auto hook = s_wdt_feed_hook.load(std::memory_order_relaxed);
  if (hook) {
    hook(index);
  }
}

// ── Socket Keepalive Utility ─────────────────────────────────────────────────
void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt) {
  if (sock < 0)
    return;
  int keepalive = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

const char *System_ResetReasonToString(esp_reset_reason_t rr) noexcept {
  switch (rr) {
  case ESP_RST_UNKNOWN:    return "Unknown Reset";
  case ESP_RST_POWERON:    return "Power-On Reset";
  case ESP_RST_EXT:        return "Hardware Reset Pin (EXT)";
  case ESP_RST_SW:         return "Software Restart";
  case ESP_RST_PANIC:      return "CPU Panic / Crash Exception";
  case ESP_RST_INT_WDT:    return "Interrupt Watchdog Reset";
  case ESP_RST_TASK_WDT:   return "Task Watchdog Reset";
  case ESP_RST_WDT:        return "Other Watchdog Reset";
  case ESP_RST_DEEPSLEEP:  return "Deep Sleep Reset";
  case ESP_RST_BROWNOUT:   return "HW: Brownout Reset (Low Voltage)";
  case ESP_RST_SDIO:       return "HW: SDIO Reset";
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  case ESP_RST_USB:        return "HW: USB Reset";
  case ESP_RST_JTAG:       return "HW: JTAG Reset";
  case ESP_RST_EFUSE:      return "HW: eFuse Reset";
  case ESP_RST_PWR_GLITCH: return "HW: Power Glitch Reset";
  case ESP_RST_CPU_LOCKUP: return "HW: CPU Lockup Reset";
#endif
  }
  return "Unknown Hardware Reset";
}

bool System_NvsPutUInt(const char *ns, const char *key, uint32_t val) noexcept {
  if (!ns || !key)
    return false;
  SystemNvs p;
  if (!p.begin(ns, false))
    return false;
  const size_t wr = p.putUInt(key, val);
  p.end();
  return wr > 0;
}

// ── SystemNvs RAII Wrapper (Zero-Heap NVS Storage) ───────────────────────────
SystemNvs::~SystemNvs() noexcept {
  end();
}

bool SystemNvs::begin(const char *name, bool readOnly) noexcept {
  if (_started)
    end();
  if (!name)
    return false;
  _readOnly = readOnly;
  nvs_handle_t h = 0;
  esp_err_t err = nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &h);
  if (err != ESP_OK)
    return false;
  _handle = static_cast<uint32_t>(h);
  _started = true;
  return true;
}

void SystemNvs::end() noexcept {
  if (_started) {
    nvs_close(static_cast<nvs_handle_t>(_handle));
    _handle = 0;
    _started = false;
  }
}

bool SystemNvs::clear() noexcept {
  if (!_started || _readOnly)
    return false;
  esp_err_t err = nvs_erase_all(static_cast<nvs_handle_t>(_handle));
  if (err != ESP_OK)
    return false;
  return nvs_commit(static_cast<nvs_handle_t>(_handle)) == ESP_OK;
}

bool SystemNvs::remove(const char *key) noexcept {
  if (!_started || _readOnly || !key)
    return false;
  esp_err_t err = nvs_erase_key(static_cast<nvs_handle_t>(_handle), key);
  if (err != ESP_OK)
    return false;
  return nvs_commit(static_cast<nvs_handle_t>(_handle)) == ESP_OK;
}

NvsValueType SystemNvs::getType(const char *key) noexcept {
  if (!_started || !key)
    return NvsValueType::INVALID;
  nvs_type_t nt = NVS_TYPE_ANY;
  if (nvs_find_key(static_cast<nvs_handle_t>(_handle), key, &nt) != ESP_OK)
    return NvsValueType::INVALID;
  switch (nt) {
  case NVS_TYPE_I8:   return NvsValueType::I8;
  case NVS_TYPE_U8:   return NvsValueType::U8;
  case NVS_TYPE_I16:  return NvsValueType::I16;
  case NVS_TYPE_U16:  return NvsValueType::U16;
  case NVS_TYPE_I32:  return NvsValueType::I32;
  case NVS_TYPE_U32:  return NvsValueType::U32;
  case NVS_TYPE_I64:  return NvsValueType::I64;
  case NVS_TYPE_U64:  return NvsValueType::U64;
  case NVS_TYPE_STR:  return NvsValueType::STR;
  case NVS_TYPE_BLOB: return NvsValueType::BLOB;
  default:            return NvsValueType::INVALID;
  }
}

bool SystemNvs::isKey(const char *key) noexcept {
  return getType(key) != NvsValueType::INVALID;
}

size_t SystemNvs::putChar(const char *key, int8_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_i8(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 1 : 0;
}

size_t SystemNvs::putUChar(const char *key, uint8_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_u8(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 1 : 0;
}

size_t SystemNvs::putShort(const char *key, int16_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_i16(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 2 : 0;
}

size_t SystemNvs::putUShort(const char *key, uint16_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_u16(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 2 : 0;
}

size_t SystemNvs::putLong(const char *key, int32_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_i32(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 4 : 0;
}

size_t SystemNvs::putULong(const char *key, uint32_t value) noexcept {
  if (!_started || _readOnly || !key) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  return (nvs_set_u32(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK) ? 4 : 0;
}

size_t SystemNvs::putUInt(const char *key, uint32_t value) noexcept {
  return putULong(key, value);
}

size_t SystemNvs::putBool(const char *key, bool value) noexcept {
  return putUChar(key, value ? 1u : 0u);
}

size_t SystemNvs::putString(const char *key, const char *value) noexcept {
  if (!_started || _readOnly || !key || !value) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  if (nvs_set_str(h, key, value) != ESP_OK || nvs_commit(h) != ESP_OK)
    return 0;
  return strlen(value);
}

size_t SystemNvs::putBytes(const char *key, const void *value, size_t len) noexcept {
  if (!_started || _readOnly || !key || !value || len == 0) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  if (nvs_set_blob(h, key, value, len) != ESP_OK || nvs_commit(h) != ESP_OK)
    return 0;
  return len;
}

int8_t SystemNvs::getChar(const char *key, int8_t defaultValue) noexcept {
  int8_t v = defaultValue;
  if (_started && key) nvs_get_i8(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

uint8_t SystemNvs::getUChar(const char *key, uint8_t defaultValue) noexcept {
  uint8_t v = defaultValue;
  if (_started && key) nvs_get_u8(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

int16_t SystemNvs::getShort(const char *key, int16_t defaultValue) noexcept {
  int16_t v = defaultValue;
  if (_started && key) nvs_get_i16(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

uint16_t SystemNvs::getUShort(const char *key, uint16_t defaultValue) noexcept {
  uint16_t v = defaultValue;
  if (_started && key) nvs_get_u16(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

int32_t SystemNvs::getLong(const char *key, int32_t defaultValue) noexcept {
  int32_t v = defaultValue;
  if (_started && key) nvs_get_i32(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

uint32_t SystemNvs::getULong(const char *key, uint32_t defaultValue) noexcept {
  uint32_t v = defaultValue;
  if (_started && key) nvs_get_u32(static_cast<nvs_handle_t>(_handle), key, &v);
  return v;
}

uint32_t SystemNvs::getUInt(const char *key, uint32_t defaultValue) noexcept {
  return getULong(key, defaultValue);
}

bool SystemNvs::getBool(const char *key, bool defaultValue) noexcept {
  return getUChar(key, defaultValue ? 1u : 0u) != 0u;
}

size_t SystemNvs::getString(const char *key, char *value, size_t maxLen) noexcept {
  if (!_started || !key || !value || maxLen == 0) return 0;
  const auto h = static_cast<nvs_handle_t>(_handle);
  size_t len = 0;
  if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 || len > maxLen)
    return 0;
  if (nvs_get_str(h, key, value, &len) != ESP_OK)
    return 0;
  return (len > 0) ? (len - 1) : 0;
}

size_t SystemNvs::getBytesLength(const char *key) noexcept {
  if (!_started || !key) return 0;
  size_t len = 0;
  if (nvs_get_blob(static_cast<nvs_handle_t>(_handle), key, nullptr, &len) != ESP_OK)
    return 0;
  return len;
}

size_t SystemNvs::getBytes(const char *key, void *buf, size_t maxLen) noexcept {
  if (!_started || !key || !buf || maxLen == 0) return 0;
  size_t len = maxLen;
  if (nvs_get_blob(static_cast<nvs_handle_t>(_handle), key, buf, &len) != ESP_OK)
    return 0;
  return len;
}




