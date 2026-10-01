#pragma once

// ============================================================================
// From include/core/Platform.h
// ============================================================================

// ============================================================================
// PLATFORM & COMPLIANCE DEFINITIONS (Extracted from Common.h SECTION 1)
// ============================================================================

#include "driver/uart.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <Arduino.h>
#include <Preferences.h>
#include <SoftwareSerial.h>
#include <WiFi.h>
#include <array>
#include <atomic>
#include <cctype>
#include <string_view>
#include <sys/time.h>
#include <time.h>
#include <type_traits>

struct CoreDumpInfo {
  bool valid{false};
  char task_name[16]{};
  uint32_t exc_pc{0};
  uint32_t exc_cause{0};
  uint32_t bt[16]{};
  uint8_t bt_depth{0};
  bool bt_corrupted{false};
};

struct DoorphoneSpec {
  uint32_t baud_rate; // 통신 속도
  uint8_t stx;        // 시작 바이트
  uint8_t etx;        // 종료 바이트
  uint8_t len;        // 프레임 길이
  const char *desc;   // 규격 명칭
  uint8_t bell_front; // 현관 벨 호출 수신
  uint8_t bell_lobby; // 로비 벨 호출 수신
  uint8_t call_front; // 현관 통화 시작 송신
  uint8_t call_lobby; // 로비 통화 시작 송신
  uint8_t open_front; // 현관 문열림 송신
  uint8_t open_lobby; // 로비 문열림 송신
  uint8_t end_front;  // 현관 통화 종료 송신
  uint8_t end_lobby;  // 로비 통화 종료 송신
};

extern CoreDumpInfo g_coredump_info;
void System_CheckCoreDump();

#ifndef LIKELY
#define LIKELY(x) __builtin_expect(!!(x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

#if __has_include(<span>)
#include <span>
using std::span;
#else
namespace Gateway {
template <typename T> class span {
  T *ptr_{nullptr};
  size_t len_{0};

public:
  constexpr span() noexcept = default;
  constexpr span(T *ptr, size_t len) noexcept : ptr_(ptr), len_(len) {}
  constexpr span(T *first, T *last) noexcept
      : ptr_(first), len_(last - first) {}
  template <size_t N>
  constexpr span(
      std::array<typename std::remove_const<T>::type, N> &arr) noexcept
      : ptr_(arr.data()), len_(N) {}
  template <size_t N>
  constexpr span(
      const std::array<typename std::remove_const<T>::type, N> &arr) noexcept
      : ptr_(arr.data()), len_(N) {}
  template <size_t N>
  constexpr span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
  constexpr T *data() const noexcept { return ptr_; }
  constexpr size_t size() const noexcept { return len_; }
  constexpr bool empty() const noexcept { return len_ == 0; }
  constexpr T &operator[](size_t idx) const noexcept { return ptr_[idx]; }
  constexpr T *begin() const noexcept { return ptr_; }
  constexpr T *end() const noexcept { return ptr_ + len_; }
  constexpr span<T>
  subspan(size_t offset,
          size_t count = static_cast<size_t>(-1)) const noexcept {
    if (offset >= len_)
      return span<T>();
    size_t actual_count =
        (count == static_cast<size_t>(-1) || offset + count > len_)
            ? (len_ - offset)
            : count;
    return span<T>(ptr_ + offset, actual_count);
  }
};
} // namespace Gateway
using Gateway::span;
#endif

using std::string_view;

inline uint32_t FastCrc32(const uint8_t *data, size_t len) noexcept {
  return ~esp_rom_crc32_le(~0U, data, len);
}

template <typename T> struct NvsEnvelope {
  uint32_t magic{0x4757484D}; // "GWHM"
  uint16_t version{1};
  uint16_t data_len{sizeof(T)};
  uint32_t crc32{0};
  T payload{};

  void seal() noexcept {
    crc32 = FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
  }

  bool verify() const noexcept {
    if (magic != 0x4757484D || data_len != sizeof(T))
      return false;
    uint32_t computed =
        FastCrc32(reinterpret_cast<const uint8_t *>(&payload), sizeof(T));
    return (computed == crc32);
  }
};

inline SoftwareSerialConfig Door_SerialConfig(uint8_t data_bits, uint8_t parity,
                                              uint8_t stop_bits) {
  if (data_bits == 7 && stop_bits == 1) {
    if (parity == 1)
      return SWSERIAL_7E1;
    if (parity == 2)
      return SWSERIAL_7O1;
  } else if (data_bits == 8) {
    if (stop_bits == 1) {
      if (parity == 1)
        return SWSERIAL_8E1;
      if (parity == 2)
        return SWSERIAL_8O1;
    } else if (stop_bits == 2 && parity == 0) {
      return SWSERIAL_8N2;
    }
  }
  return SWSERIAL_8N1;
}

namespace HexLUT {
constexpr auto generateLUT() {
  std::array<std::array<char, 2>, 256> lut{};
  constexpr char hexDigits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < 256; ++i) {
    lut[i][0] = hexDigits[(i >> 4) & 0x0F];
    lut[i][1] = hexDigits[i & 0x0F];
  }
  return lut;
}
alignas(16) inline constexpr auto LUT = generateLUT();
} // namespace HexLUT

namespace TimeUtils {
[[nodiscard]] inline bool isElapsed(uint32_t start_ms,
                                    uint32_t duration_ms) noexcept {
  return (millis() - start_ms) >= duration_ms;
}
} // namespace TimeUtils

void Tcp_EnableKeepalive(int sock, int idle, int intvl, int cnt);

// ============================================================================
// From include/core/Config.h
// ============================================================================

// ============================================================================
// SYSTEM CONFIGURATION CONSTANTS & NAMESPACES (Extracted from Common.h SECTION
// 2)
// ============================================================================

namespace Config {
// [시스템] 펌웨어 버전 문자열 (CLI/Log/OTA)
constexpr const char *FIRMWARE_VERSION = "v1.5.8";
} // namespace Config

namespace Config::Task {
// [Task] Core1 RS-485 마스터(CH1) 통신 스택 크기 (실측 3.0KB + 3배 마진 3.0KB =
// 6KB)
constexpr size_t STACK_SIZE_CORE1 = 6144;
// [Task] Core1 RS-485 슬레이브(CH2, CH3) 통신 스택 크기 (실측 3.6KB + 2배
// 마진 2.0KB = 5.5KB)
constexpr size_t STACK_SIZE_SLAVE = 5632;
// [Task] Core1 도어폰(CH4 SoftwareSerial) 통신 스택 크기 (실측 1.9KB + 2배
// 마진 2.2KB = 4KB)
constexpr size_t STACK_SIZE_CH4 = 4096;
// [Task] Core0 네트워크 스택 크기 (실측 4.1KB + 4배 마진 4.1KB = 8KB 보존)
constexpr size_t STACK_SIZE_CORE0 = 8192;
// [Task] Telnet CLI 스택 크기 (실측 2.9KB + 3배 마진 3.2KB = 6KB)
constexpr size_t STACK_SIZE_TELNET = 6144;
// [Task] 총 감시 대상 태스크 개수 및 텔넷 WDT ID
constexpr size_t TASK_COUNT = 6;
constexpr uint8_t WDT_ID_TELNET = 5;
static_assert(WDT_ID_TELNET < TASK_COUNT, "WDT_ID_TELNET out of range");
} // namespace Config::Task

namespace Config::Queue {
// [Queue] CH1 일반 제어 명령 큐 용량 (기본: 32개)
constexpr size_t POOL_SIZE_CONTROL = 32;
// [Queue] CH1 VIP 긴급 제어 명령 큐 용량 (16개)
constexpr size_t POOL_SIZE_VIP = 16;
// [Queue] CH4 도어폰 패스스루 큐 용량 (16개)
constexpr size_t POOL_SIZE_CH4_PASS = 16;
// [Queue] UART 이벤트 이중화 큐 (기본: 16개)
constexpr size_t UART_EVENT_QUEUE_SIZE = 16;
} // namespace Config::Queue

namespace Config::Timing {
// [CH1 마스터] RS-485 응답 타임아웃 (기본: 200ms)
constexpr uint32_t CH1_POLL_TIMEOUT_MS = 200;
// [동기화] 공유자원 뮤텍스 대기 타임아웃 (기본: 300ms)
constexpr uint32_t MAX_LOCK_HOLD_MS = 300;
// [시스템] 24시간 업타임 ms 환산값
constexpr uint32_t UPTIME_24H_MS = 86400000;
// [CH1 버스] 패킷 간 최소 안정 지연 (기본: 15ms)
constexpr uint32_t CH1_INTER_PACKET_DELAY_MS = 15;
// [UART] TX 전송 완료 대기 타임아웃 (기본: 20ms, 9600bps 14B 기준 이론치 ~16ms
// + 마진)
constexpr uint32_t UART_TX_DONE_TIMEOUT_MS = 20;
// [CH1 마스터] 정기 폴링 간격 (기본: 1000ms)
constexpr uint32_t CH1_POLL_INTERVAL_MS = 1000;
// [기기 헬스] 오프라인(Stale) 판정 임계치 (기본: 3분)
constexpr uint32_t STALE_DEVICE_THRESHOLD_MS = 180000;
// [CH1 오프라인] 기기 재접속 확인 폴링 주기 (기본: 10초)
constexpr uint32_t CH1_STALE_POLL_INTERVAL_MS = 10000;
// [부팅] 초기 캐싱 완료 유예 대기 시간 (기본: 5초)
constexpr uint32_t INITIAL_CACHING_GRACE_PERIOD_MS = 5000;
// [헬스 모니터] CPU/Heap/온도 샘플링 주기 (기본: 15초)
constexpr uint32_t SYSTEM_MONITOR_INTERVAL_MS = 15000;
// [CH4 도어폰] 버튼 신호 디바운스 대기 (기본: 500ms)
constexpr uint32_t DOORPHONE_DEBOUNCE_MS = 500;
// [월패드 Auto 학습] 범용 인터패킷 갭 감지 타이머 (기본: 20ms 침묵 = 1프레임
// 캡처, 9600bps 기준 패킷 분할 방지)
constexpr uint32_t WALLPAD_AUTO_IPG_MS = 20;
// [CH4 도어폰] 범용 인터패킷 갭 감지 타이머 (기본: 25ms 침묵 = 1프레임 종료
// 판정)
constexpr uint32_t DOORPHONE_IPG_MS = 25;
// [CH4 도어폰] 보레이트 기반 바이트 간 최대 허용 연속 지연 타이머 동적 계산
constexpr uint32_t DEFAULT_DOORPHONE_INTER_BYTE_TIMEOUT_MS = 16;
inline uint32_t getDoorphoneInterByteTimeoutMs(uint32_t baud) noexcept {
  if (baud == 0)
    return DEFAULT_DOORPHONE_INTER_BYTE_TIMEOUT_MS;
  // 3860 baud 기준 16ms 보장, 고속 보레이트(9600 등) 시 비례 축소 (최소 6ms,
  // 최대 20ms)
  uint32_t timeout = (60000UL + baud - 1) / baud;
  return (timeout < 6) ? 6 : (timeout > 20 ? 20 : timeout);
}
// [CH2 월패드] 가상 응답(Virtual ACK) 지연 (기본: 30ms)
constexpr uint32_t CH2_CACHE_DELAY_MS = 30;
// [CH3 월패드] 가상 응답(Virtual ACK) 지연 (기본: 240ms)
constexpr uint32_t CH3_CACHE_DELAY_MS = 240;
// [OTA 무결성] 펌웨어 정상 확정 및 롤백 해제 유예 시간 (기본: 120초)
constexpr uint32_t OTA_VALIDATION_PERIOD_MS = 120000;
// [비상 복구] 전면 버튼 길게 누름 판정 시간 (기본: 2.5초)
constexpr uint32_t RESCUE_BUTTON_HOLD_MS = 2500;
// [WiFi 복구] SoftAP 비상 모드 중 백그라운드 STA 재연결 탐색 주기 (기본: 60초)
constexpr uint32_t WIFI_BACKGROUND_RETRY_INTERVAL_MS = 60000;
// [웜캐싱] NVS 스냅샷 저장 디바운스 시간 (신규 장치 발견 후 60초 대기)
constexpr uint32_t WARM_CACHE_NVS_DEBOUNCE_MS = 60000;
// [웜캐싱] 복원 기기 ACK 검증 타임아웃 (60초 이내 미응답 시 자동 퇴출)
constexpr uint32_t WARM_CACHE_VERIFY_TIMEOUT_MS = 60000;
// [1차 캐시] EXPIRED 엔트리 완전 영구 삭제 유예 시간 (기본: 10분)
} // namespace Config::Timing

namespace Config::Network {
// [WiFi] AP 접속 시도 타임아웃 (기본: 30초)
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;
} // namespace Config::Network

namespace Config::OTA {
// [OTA] 다운로드 전체 제한 시간 (기본: 5분)
constexpr uint32_t DOWNLOAD_DEADLINE_MS = 300000;
// [OTA] 소켓 침묵 스톨 타임아웃 (기본: 25초)
constexpr uint32_t STALL_TIMEOUT_MS = 25000;
// [OTA] 데이터 미수신/대기 시 CPU 양보 시간 (기본: 2ms)
constexpr uint32_t IDLE_DELAY_MS = 2;
} // namespace Config::OTA

namespace Config::Metrics {
// [메트릭] 링버퍼 샘플링 주기 (기본: 5초) - Core0 태스크와 모니터링 공통 참조
constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
} // namespace Config::Metrics

namespace Config::Memory {
// [메모리] 힙 최소 안전 경고 임계치 (기본: 25KB)
constexpr uint32_t MIN_HEAP_THRESHOLD_KB = 25;
} // namespace Config::Memory

namespace Config::Packet {
// [RS-485] 검증 패킷 최소 길이 (기본: 3B)
constexpr uint8_t MIN_LEN = 3;
// [RS-485] 검증 패킷 최대 길이 (기본: 64B)
constexpr uint8_t MAX_LEN = 64;
// [UART] 하드웨어 드라이버 RX 링버퍼 크기 (기본: 2048B)
constexpr uint16_t UART_HW_RX_BUF_SIZE = 2048;
// [UART] RX 단일 읽기 청크 버퍼 크기 (64B)
constexpr size_t UART_READ_CHUNK = 64;
// [UART] RX 패킷 누적 스트림 버퍼 크기 (128B)
constexpr size_t MAX_STREAM_BUF = 128;
} // namespace Config::Packet

namespace Config::GPIO {
// [GPIO] M5 AtomS3 전면 화면 물리 버튼 (GPIO 41, Active LOW)
constexpr int BTN_PIN = 41;
// [GPIO] 도어폰 SoftwareSerial TX 핀 (GPIO 39)
constexpr int TX_GPIO = 39;
// [GPIO] 도어폰 SoftwareSerial RX 핀 (GPIO 38)
constexpr int RX_GPIO = 38;
} // namespace Config::GPIO

namespace Config::TCP {
// [포트] Telnet CLI 접속 포트 (23)
constexpr uint16_t TELNET_PORT = 23;
// [포트] CH5 EW11 수신 전용 TCP 서버 포트 (8898)
constexpr uint16_t EW11_PORT = 8898;
// [포트] CH6 SmartThings & 관리 JSON-RPC TCP 서버 포트 (8900)
constexpr uint16_t MGMT_PORT = 8900;

// [접속 제한] Telnet CLI 동시 클라이언트 최대 수 (3대)
constexpr uint8_t MAX_TELNET_CLIENTS = 3;
// [접속 제한] CH6 관리 TCP 동시 클라이언트 최대 수 (3대)
constexpr uint8_t MAX_MGMT_CLIENTS = 3;

// [CH5 EW11 멀티 TCP 클라이언트 슬롯 풀 (최대 5대: Slot 0: EV, Slot 1~4: AC)]
constexpr uint8_t MAX_EW11_SLOTS = 5;
// [포트] CH5 EW11 슬롯별 전용 TCP 서버 포트 (Slot 0: 8898, Slot 1~4: 8891~8894)
constexpr uint16_t EW11_SLOT_PORTS[MAX_EW11_SLOTS] = {8898, 8891, 8892, 8893,
                                                      8894};

// [소켓 버퍼] TCP SO_RCVBUF / SO_SNDBUF 크기 (4096B = 4KB)
constexpr int SOCKET_BUFFER_SIZE = 4096;
// [수신 버퍼] CH5 Hub 클라이언트 슬롯 수신 누적 버퍼 (1024B)
constexpr size_t HUB_RX_BUFFER_SIZE = 1024;
// [세션 버퍼] CH6 관리 TCP JSON 수신 버퍼 (512B)
constexpr size_t MGMT_BUFFER_SIZE = 512;
// [폴링 청크] TCP 스트림 1회 수신 청크 버퍼 (128B)
constexpr size_t POLL_RX_CHUNK_SIZE = 128;

// [세션] Telnet 세션 자동 정리 타임아웃 (기본: 10분)
constexpr uint32_t TELNET_SESSION_TIMEOUT_MS = 600000;
// [세션] 좀비 세션 감지 및 정리 주기 (기본: 30초)
constexpr uint32_t CLEANUP_INTERVAL_MS = 30000;

// [기본 Keepalive] 최초 아이들 (기본: 60초)
constexpr uint32_t DEFAULT_KEEPALIVE_IDLE_SEC = 60;
// [기본 Keepalive] 프로브 간격 (기본: 5초)
constexpr uint32_t DEFAULT_KEEPALIVE_INTVL_SEC = 5;
// [기본 Keepalive] 허용 횟수 (기본: 3회)
constexpr uint32_t DEFAULT_KEEPALIVE_CNT = 3;
} // namespace Config::TCP

namespace Config::FCU {
// [CH5 FCU] Modbus-RTU 폴링 및 제어 파라미터
constexpr uint32_t POLL_INTERVAL_MS =
    1000; // 슬롯별 독립 폴링 주기 (1초, 실시간 상태 추적)
constexpr uint32_t RX_TIMEOUT_MS =
    500; // 쿼리 응답 대기 타임아웃 (TCP/WiFi 지터 방어 마진 포함 500ms)
constexpr uint32_t INTER_PACKET_DELAY_MS =
    50; // 버스 유휴 감지 및 안정 지연 (EW11 버퍼 플러시 확보 50ms)
constexpr uint8_t MAX_TIMEOUT_COUNT = 3; // 연속 타임아웃 허용 횟수
constexpr uint8_t TEMP_MIN = 18;         // 최저 희망 설정 온도 (18℃)
constexpr uint8_t TEMP_MAX = 30;         // 최고 희망 설정 온도 (30℃)
constexpr uint8_t DEV_ID = 0x2C; // ST 연동 디바이스 ID (에어컨/FCU 계열)
} // namespace Config::FCU

namespace Config::Serial {
// [도어폰 Serial] 보레이트 (기본: 3860)
constexpr uint32_t DEFAULT_DOORPHONE_BAUD = 3860;
// [도어폰 Serial] 패리티 (기본: 1 = Even)
constexpr uint8_t DEFAULT_DOORPHONE_PARITY = 1;
// [도어폰 Serial] 데이터 비트 (기본: 8B)
constexpr uint8_t DEFAULT_DOORPHONE_DATABITS = 8;
// [도어폰 Serial] 스톱 비트 (기본: 1B)
constexpr uint8_t DEFAULT_DOORPHONE_STOPBITS = 1;
} // namespace Config::Serial

namespace Config::Doorphone {
// [도어폰 통신 프레임] 프레임 헤더 시작 표시 바이트 (STX: 0x7F)
constexpr uint8_t STX = 0x7F;
// [도어폰 통신 프레임] 프레임 종동 종료 표시 바이트 (ETX: 0xEE)
constexpr uint8_t ETX = 0xEE;
// [도어폰 통신 프레임] 도어폰 패킷 기본 길이 (5 Bytes)
constexpr uint8_t PKT_LEN = 5;

enum class FramingStatus : uint8_t {
  WAITING = 0,
  LEARNING = 1,
  LOCKED = 2,
  NOISY = 3
};

struct FramingTracker {
  std::atomic<FramingStatus> status{FramingStatus::WAITING};
  std::atomic<uint8_t> candidate_stx{0};
  std::atomic<uint8_t> candidate_etx{0};
  std::atomic<uint8_t> candidate_len{0};
  std::atomic<uint8_t> consecutive_matches{0};
  std::atomic<uint8_t> consecutive_mismatches{0};
  std::atomic<bool> is_custom_fixed{false};

  void setFixedLock(uint8_t stx, uint8_t etx, uint8_t len) noexcept {
    candidate_stx.store(stx, std::memory_order_relaxed);
    candidate_etx.store(etx, std::memory_order_relaxed);
    candidate_len.store(len, std::memory_order_relaxed);
    consecutive_matches.store(10, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    is_custom_fixed.store(true, std::memory_order_relaxed);
    status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
  }

  void reset() noexcept {
    is_custom_fixed.store(false, std::memory_order_relaxed);
    candidate_stx.store(0, std::memory_order_relaxed);
    candidate_etx.store(0, std::memory_order_relaxed);
    candidate_len.store(0, std::memory_order_relaxed);
    consecutive_matches.store(0, std::memory_order_relaxed);
    consecutive_mismatches.store(0, std::memory_order_relaxed);
    status.store(FramingStatus::WAITING, std::memory_order_relaxed);
  }

  void clearNvs(const char *nvs_ns, const char *tag = "DOORPHONE") noexcept;

  void processFrame(uint8_t stx, uint8_t etx, uint8_t len, const char *nvs_ns,
                    const char *tag = "DOORPHONE") noexcept;

  inline static void getNvsNamespace(uint8_t prof_idx, char *out_ns,
                                     size_t max_len) noexcept {
    snprintf(out_ns, max_len, "dp_frame_p%u",
             static_cast<unsigned int>(prof_idx & 0x03));
  }

  void restoreFromNvs(const char *nvs_ns = "dp_frame_p0",
                      const char *tag = "DOORPHONE") noexcept;

  void saveToNvs(const char *nvs_ns = "dp_frame_p0",
                 const char *tag = "DOORPHONE") noexcept;

  [[nodiscard]] bool isConsistent(uint8_t stx, uint8_t etx) const noexcept {
    FramingStatus cur = status.load(std::memory_order_relaxed);
    if (cur != FramingStatus::LOCKED)
      return true;
    return (stx == candidate_stx.load(std::memory_order_relaxed) &&
            etx == candidate_etx.load(std::memory_order_relaxed));
  }
};

} // namespace Config::Doorphone

namespace Config::Doorphone {
struct DoorphoneState {
  std::atomic<bool> front_bell{false};
  std::atomic<bool> lobby_bell{false};
  std::atomic<uint32_t> last_bell_ms{0};
};
} // namespace Config::Doorphone

extern Config::Doorphone::DoorphoneState g_doorphone_state;

enum class HubDeviceType : uint8_t {
  WALLPAD_COMPATIBLE = 0, // 엘리베이터 (월패드 0xF7/0xEE 규격)
  AIR_CONDITIONER = 1     // 에어컨 1~4대
};

struct HubClientSlot {
  bool enabled{false};
  char name[16]{""};
  char target_ip[16]{""};
  uint16_t target_port{8898};
  HubDeviceType dev_type{HubDeviceType::WALLPAD_COMPATIBLE};
  Config::Doorphone::FramingTracker
      tracker; // 슬롯별 독립 프레이밍 자율 학습기 (STX/ETX/길이 수렴)
  int sock{-1};
  bool is_connected{false};
  uint32_t last_reconnect_ms{0};
  uint8_t rx_buf[Config::TCP::HUB_RX_BUFFER_SIZE];
  size_t rx_len{0};
  uint32_t last_rx_ms{0};
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint8_t last_query_data[64]{0}; // 1차 캐시(질문) 보관용 버퍼
  uint8_t last_query_len{0};      // 1차 캐시(질문) 길이
};

extern HubClientSlot g_hub_slots[Config::TCP::MAX_EW11_SLOTS];

// ----------------------------------------------------------------------------
// Compile-time Configuration Integrity Checks
// ----------------------------------------------------------------------------
static_assert(Config::Packet::MAX_LEN >= Config::Packet::MIN_LEN,
              "Config error: Packet::MAX_LEN must be >= Packet::MIN_LEN");
static_assert(
    Config::Packet::MAX_LEN <= 64,
    "Config error: Packet::MAX_LEN cannot exceed StaticPacket capacity (64B)");
static_assert(Config::TCP::MAX_TELNET_CLIENTS > 0 &&
                  Config::TCP::MAX_TELNET_CLIENTS <= 8,
              "Config error: TCP::MAX_TELNET_CLIENTS must be between 1 and 8");
static_assert(Config::TCP::MAX_MGMT_CLIENTS > 0 &&
                  Config::TCP::MAX_MGMT_CLIENTS <= 8,
              "Config error: TCP::MAX_MGMT_CLIENTS must be between 1 and 8");
static_assert(Config::TCP::MAX_EW11_SLOTS > 0 &&
                  Config::TCP::MAX_EW11_SLOTS <= 8,
              "Config error: TCP::MAX_EW11_SLOTS must be between 1 and 8");
static_assert(Config::Queue::POOL_SIZE_CONTROL > 0,
              "Config error: Queue::POOL_SIZE_CONTROL must be > 0");
static_assert(Config::Queue::UART_EVENT_QUEUE_SIZE > 0,
              "Config error: Queue::UART_EVENT_QUEUE_SIZE must be > 0");
static_assert(Config::Timing::MAX_LOCK_HOLD_MS > 0,
              "Config error: Timing::MAX_LOCK_HOLD_MS must be > 0");
static_assert(Config::TCP::TELNET_SESSION_TIMEOUT_MS > 0,
              "Config error: TCP::TELNET_SESSION_TIMEOUT_MS must be > 0");

// ----------------------------------------------------------------------------
// Profile Index Enum (4 Slots: Auto + Custom1~3)
// ----------------------------------------------------------------------------
enum class WallpadProfileIndex : uint8_t {
  ADAPTIVE = 0,
  CUSTOM1 = 1,
  CUSTOM2 = 2,
  CUSTOM3 = 3,
  COUNT = 4
};

// ----------------------------------------------------------------------------
// Runtime Configuration & System Snapshots
// ----------------------------------------------------------------------------
struct RuntimeConfig {
  uint32_t uart_baud_rate{9600};
  uint32_t ch2_baud_rate{9600};
  uint32_t ch3_baud_rate{9600};
  uint32_t doorphone_baud_rate{Config::Serial::DEFAULT_DOORPHONE_BAUD};
  char wifi_ssid[64]{0};
  char wifi_password[64]{0};
  char ap_ssid[64]{0};
  char ap_password[64]{0};
  char telnet_pass_hash[68]{0};
  uint16_t wifi_connect_timeout_s{30};
  uint8_t uart_data_bits{8};
  uint8_t uart_parity{0};
  uint8_t uart_stop_bits{1};
  uint8_t ch2_data_bits{8};
  uint8_t ch2_parity{0};
  uint8_t ch2_stop_bits{1};
  uint8_t ch3_data_bits{8};
  uint8_t ch3_parity{0};
  uint8_t ch3_stop_bits{1};
  uint8_t doorphone_data_bits{Config::Serial::DEFAULT_DOORPHONE_DATABITS};
  uint8_t doorphone_parity{Config::Serial::DEFAULT_DOORPHONE_PARITY};
  uint8_t doorphone_stop_bits{Config::Serial::DEFAULT_DOORPHONE_STOPBITS};
  uint8_t wallpad_profile{0};
};

inline const char *formatFramingStr(uint8_t data_bits, uint8_t parity,
                                    uint8_t stop_bits) noexcept {
  if (data_bits == 8) {
    if (parity == 0 && stop_bits == 1)
      return "8N1";
    if (parity == 1 && stop_bits == 1)
      return "8E1";
    if (parity == 2 && stop_bits == 1)
      return "8O1";
    if (parity == 0 && stop_bits == 2)
      return "8N2";
  }
  return "8N1";
}

inline bool parseFramingStr(const char *str, uint8_t &data_bits,
                            uint8_t &parity, uint8_t &stop_bits) noexcept {
  if (!str)
    return false;
  if (strcasecmp(str, "8N1") == 0) {
    data_bits = 8;
    parity = 0;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8E1") == 0) {
    data_bits = 8;
    parity = 1;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8O1") == 0) {
    data_bits = 8;
    parity = 2;
    stop_bits = 1;
    return true;
  } else if (strcasecmp(str, "8N2") == 0) {
    data_bits = 8;
    parity = 0;
    stop_bits = 2;
    return true;
  }
  return false;
}

bool System_ApplyUartConfig(uint8_t ch, uint32_t baud, const char *format);

struct SysSnapshot {
  uint32_t free_heap;
  uint32_t min_free_heap;
  uint32_t total_heap;
  uint32_t sketch_size_kb;
  uint32_t flash_total_kb;
  uint32_t uptime_ms;
  bool wifi_connected;
  int8_t wifi_rssi;
  char wifi_ip[16];
};

// ============================================================================
// From include/core/Buffers.h
// ============================================================================

// ============================================================================
// OUTPUT FORMATTERS & BUFFER UTILITIES (Extracted from Common.h SECTION 3)
// ============================================================================

#include <WiFi.h>

namespace Fmt {
constexpr char DIV80[] = "------------------------------------------------"
                         "--------------------------------\r\n";
constexpr size_t DIV80_LEN = sizeof(DIV80) - 1;

constexpr char DIV80EQ[] = "=============================================="
                           "==================================\r\n";
constexpr size_t DIV80EQ_LEN = sizeof(DIV80EQ) - 1;

inline void FormatHex(const uint8_t *data, size_t len, char *out,
                      size_t out_len) noexcept {
  size_t idx = 0;
  for (size_t k = 0; k < len && idx + 3 < out_len; ++k) {
    const auto &hex_chars = HexLUT::LUT[data[k]];
    out[idx++] = hex_chars[0];
    out[idx++] = hex_chars[1];
    out[idx++] = ' ';
  }
  if (out_len > 0) {
    out[idx] = '\0';
  }
}

inline void FormatElapsed(uint32_t now, uint32_t timestamp, char *out,
                          size_t out_len) noexcept {
  if (timestamp == 0 || out_len == 0) {
    if (out_len > 0)
      snprintf(out, out_len, "Never");
    return;
  }
  uint32_t el_ms = (now >= timestamp) ? (now - timestamp) : 0;
  float el = el_ms / 1000.0f;
  if (el < 60.0f)
    snprintf(out, out_len, "%.1fs", el);
  else
    snprintf(out, out_len, "%lum", static_cast<unsigned long>(el / 60));
}
} // namespace Fmt

struct AppendBuf {
  char *buf;
  size_t cap;
  size_t offset = 0;

private:
  void appendFormatV(const char *fmt, va_list a) {
    if (offset >= cap)
      return;
    int n = vsnprintf(buf + offset, cap - offset, fmt, a);
    if (n > 0)
      offset = std::min(offset + static_cast<size_t>(n), cap - 1);
  }

public:
  void appendFormat(const char *fmt, ...)
      __attribute__((format(printf, 2, 3))) {
    va_list a;
    va_start(a, fmt);
    appendFormatV(fmt, a);
    va_end(a);
  }

  template <size_t N> void append(const char (&str)[N]) noexcept {
    if (offset >= cap)
      return;
    size_t copy_len = std::min(N - 1, cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, str, copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }

  void append(std::string_view sv) noexcept {
    if (sv.empty() || offset >= cap)
      return;
    size_t copy_len = std::min(sv.size(), cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, sv.data(), copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }

  void append(const char *str) noexcept {
    if (!str || offset >= cap)
      return;
    size_t len = strlen(str);
    size_t copy_len = std::min(len, cap - 1 - offset);
    if (copy_len > 0) {
      memcpy(buf + offset, str, copy_len);
      offset += copy_len;
      buf[offset] = '\0';
    }
  }
};

[[nodiscard]] inline bool Tcp_IsAllowedIP(IPAddress ip) {
  // 1. Local Loopback
  if (ip == IPAddress(127, 0, 0, 1))
    return true;

  // 2. Narrowed Private Subnets (172.30.1.0/24, 172.30.2.0/24)
  if (ip[0] == 172 && ip[1] == 30 && (ip[2] == 1 || ip[2] == 2))
    return true;

  // 3. Dynamic STA Subnet Match
  if (WiFi.isConnected()) {
    IPAddress sta_ip = WiFi.localIP();
    IPAddress sta_mask = WiFi.subnetMask();
    if ((ip & sta_mask) == (sta_ip & sta_mask))
      return true;
  }

  // 4. Dynamic SoftAP Subnet Match
  if (WiFi.getMode() == WIFI_MODE_AP || WiFi.getMode() == WIFI_MODE_APSTA) {
    IPAddress ap_ip = WiFi.softAPIP();
    IPAddress ap_mask = WiFi.softAPSubnetMask();
    if ((ip & ap_mask) == (ap_ip & ap_mask))
      return true;
  }

  return false;
}

[[nodiscard]] inline bool Telnet_IsAllowedIP(IPAddress ip) {
  // Option B: Specific external public IP allowed exclusively for Telnet CLI
  if (ip == IPAddress(115, 91, 242, 69))
    return true;

  return Tcp_IsAllowedIP(ip);
}

// ============================================================================
// From include/core/Metrics.h
// ============================================================================

// ============================================================================
// CONCURRENCY LOCKERS & METRICS TRACKERS (Extracted from Common.h SECTION 4)
// ============================================================================

class CriticalSectionLocker {
private:
  portMUX_TYPE *_mux{nullptr};

public:
  explicit CriticalSectionLocker(portMUX_TYPE *mux) noexcept : _mux(mux) {
    if (_mux)
      portENTER_CRITICAL(_mux);
  }
  explicit CriticalSectionLocker(portMUX_TYPE &mux) noexcept : _mux(&mux) {
    portENTER_CRITICAL(_mux);
  }
  ~CriticalSectionLocker() noexcept {
    if (_mux)
      portEXIT_CRITICAL(_mux);
  }
  CriticalSectionLocker(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker &operator=(const CriticalSectionLocker &) = delete;
  CriticalSectionLocker(CriticalSectionLocker &&) = delete;
  CriticalSectionLocker &operator=(CriticalSectionLocker &&) = delete;
};

class [[nodiscard]] MutexLocker {
private:
  SemaphoreHandle_t _mutex{nullptr};
  bool _locked{false};
  uint32_t _acquired_ms{0};

public:
  explicit MutexLocker(SemaphoreHandle_t mutex,
                       TickType_t timeout = portMAX_DELAY) noexcept
      : _mutex(mutex) {
    if (_mutex) {
      _locked = (xSemaphoreTake(_mutex, timeout) == pdTRUE);
      if (_locked) {
        _acquired_ms = millis();
      }
    }
  }
  ~MutexLocker() noexcept {
    if (_mutex && _locked) {
      uint32_t hold_ms = millis() - _acquired_ms;
      if (hold_ms >= Config::Timing::MAX_LOCK_HOLD_MS) {
        ESP_LOGW("LOCK", "Mutex held for %u ms (>= %u ms threshold)",
                 static_cast<unsigned>(hold_ms),
                 static_cast<unsigned>(Config::Timing::MAX_LOCK_HOLD_MS));
      }
      xSemaphoreGive(_mutex);
    }
  }
  [[nodiscard]] bool isLocked() const noexcept { return _locked; }
  explicit operator bool() const noexcept { return _locked; }
  MutexLocker(const MutexLocker &) = delete;
  MutexLocker &operator=(const MutexLocker &) = delete;
  MutexLocker(MutexLocker &&) = delete;
  MutexLocker &operator=(MutexLocker &&) = delete;
};

struct MetricSample {
  uint8_t cpu0_pct;
  uint8_t cpu1_pct;
  uint16_t ram_kb;
  uint16_t flash_kb;
  int8_t temp_c;
};

struct MetricBucket {
  uint32_t cpu0_sum{0};
  uint32_t cpu1_sum{0};
  uint8_t cpu0_peak{0};
  uint8_t cpu1_peak{0};
  uint32_t ram_sum{0};
  uint16_t ram_peak{0};
  int32_t temp_sum{0};
  int8_t temp_peak{-127};
  uint16_t count{0};
};

struct StatSummary {
  uint8_t cpu0_avg{0}, cpu0_peak{0};
  uint8_t cpu1_avg{0}, cpu1_peak{0};
  uint16_t ram_avg{0}, ram_peak{0};
  uint16_t flash_avg{0}, flash_peak{0};
  int8_t temp_avg{0}, temp_peak{0};
  uint16_t count{0};
};

class SystemMetricsTracker {
public:
  static constexpr size_t SAMPLES_15M = 180;
  static constexpr size_t BUCKETS_24H = 96;

private:
  MetricSample _ring15[SAMPLES_15M]{};
  size_t _ring15_head = 0;
  size_t _ring15_count = 0;

  MetricBucket _ring24[BUCKETS_24H]{};
  size_t _ring24_head = 0;
  size_t _ring24_count = 0;

  MetricBucket _cur_bucket{};
  uint16_t _bucket_sample_count = 0;

  MetricSample _current{};
  uint16_t _cached_flash_kb{0};
  mutable SemaphoreHandle_t _metrics_mutex = nullptr;

public:
  SystemMetricsTracker() noexcept = default;

  void init();
  void reset();
  void addSample(uint8_t cpu0_pct, uint8_t cpu1_pct, uint16_t ram_kb,
                 int8_t temp_c);
  MetricSample getCurrent() const noexcept {
    MutexLocker lock(_metrics_mutex);
    return _current;
  }

  StatSummary get15m() const;
  StatSummary get24h() const;
};

struct SingleChannelStats {
  std::atomic<uint32_t> rx_pkts{0};
  std::atomic<uint32_t> tx_pkts{0};
  std::atomic<uint32_t> crc_errors{0};
  std::atomic<uint32_t> invalid_frames{0};
  std::atomic<uint32_t> timeouts{0};
  std::atomic<uint32_t> uncached_pkts{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    crc_errors.store(0, std::memory_order_relaxed);
    invalid_frames.store(0, std::memory_order_relaxed);
    timeouts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
  }
};

struct TcpSocketStats {
  std::atomic<bool> is_connected{false};
  std::atomic<uint32_t> connection_count{0};
  std::atomic<uint32_t> rx_pkts{0};
  std::atomic<uint32_t> tx_pkts{0};
  std::atomic<uint32_t> dropped_pkts{0};
  std::atomic<uint32_t> uncached_pkts{0};

  void reset() {
    rx_pkts.store(0, std::memory_order_relaxed);
    tx_pkts.store(0, std::memory_order_relaxed);
    dropped_pkts.store(0, std::memory_order_relaxed);
    uncached_pkts.store(0, std::memory_order_relaxed);
  }
};

struct PacketStatistics {
  SingleChannelStats ch1;
  SingleChannelStats ch2;
  SingleChannelStats ch3;
  SingleChannelStats ch4;
  TcpSocketStats ch5;
  TcpSocketStats ch6;

  void resetAll() {
    ch1.reset();
    ch2.reset();
    ch3.reset();
    ch4.reset();
    ch5.reset();
    ch6.reset();
  }
};

struct ChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t crc_errors{0};
  uint32_t invalid_frames{0};
  uint32_t timeouts{0};
  uint32_t uncached_pkts{0};

  ChanStats() = default;
  ChanStats(const SingleChannelStats &s) noexcept
      : rx_pkts(s.rx_pkts.load(std::memory_order_relaxed)),
        tx_pkts(s.tx_pkts.load(std::memory_order_relaxed)),
        crc_errors(s.crc_errors.load(std::memory_order_relaxed)),
        invalid_frames(s.invalid_frames.load(std::memory_order_relaxed)),
        timeouts(s.timeouts.load(std::memory_order_relaxed)),
        uncached_pkts(s.uncached_pkts.load(std::memory_order_relaxed)) {}

  ChanStats &operator=(const SingleChannelStats &s) noexcept {
    rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
    tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
    crc_errors = s.crc_errors.load(std::memory_order_relaxed);
    invalid_frames = s.invalid_frames.load(std::memory_order_relaxed);
    timeouts = s.timeouts.load(std::memory_order_relaxed);
    uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
    return *this;
  }
};

struct TcpChanStats {
  uint32_t rx_pkts{0};
  uint32_t tx_pkts{0};
  uint32_t dropped_pkts{0};
  uint32_t uncached_pkts{0};
  uint16_t connection_count{0};
  bool is_connected{false};

  TcpChanStats() = default;
  TcpChanStats(const TcpSocketStats &s) noexcept
      : rx_pkts(s.rx_pkts.load(std::memory_order_relaxed)),
        tx_pkts(s.tx_pkts.load(std::memory_order_relaxed)),
        dropped_pkts(s.dropped_pkts.load(std::memory_order_relaxed)),
        uncached_pkts(s.uncached_pkts.load(std::memory_order_relaxed)),
        connection_count(s.connection_count.load(std::memory_order_relaxed)),
        is_connected(s.is_connected.load(std::memory_order_relaxed)) {}

  TcpChanStats &operator=(const TcpSocketStats &s) noexcept {
    is_connected = s.is_connected.load(std::memory_order_relaxed);
    connection_count = s.connection_count.load(std::memory_order_relaxed);
    rx_pkts = s.rx_pkts.load(std::memory_order_relaxed);
    tx_pkts = s.tx_pkts.load(std::memory_order_relaxed);
    dropped_pkts = s.dropped_pkts.load(std::memory_order_relaxed);
    uncached_pkts = s.uncached_pkts.load(std::memory_order_relaxed);
    return *this;
  }
};

struct PktSnapshot {
  ChanStats ch1;
  ChanStats ch2;
  ChanStats ch3;
  ChanStats ch4;
  TcpChanStats ch5;
  TcpChanStats ch6;
};

// ============================================================================
// From include/core/Devices.h
// ============================================================================

// ============================================================================
// PACKET STRUCTURES & DEVICE REPOSITORY (Extracted from Common.h SECTION 5)
// ============================================================================

#include <shared_mutex>

constexpr uint8_t PKT_STX = 0xF7;
constexpr uint8_t PKT_ETX = 0xEE;

struct StaticPacket {
  uint8_t channel_id;
  uint8_t length;
  std::array<uint8_t, 64> data;
};

struct GroupControlTemplate;
struct DecodedDeviceState;

struct TimestampedPacket {
  uint32_t due_ms;
  StaticPacket pkt;
};

template <size_t Capacity = 8> class TimestampedPacketQueue {
private:
  TimestampedPacket _elements[Capacity]{};
  size_t _head = 0;
  size_t _tail = 0;
  size_t _size = 0;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  bool enqueue(const StaticPacket &pkt, uint32_t due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size >= Capacity) {
      return false;
    }
    _elements[_tail] = {due_ms, pkt};
    _tail = (_tail + 1) % Capacity;
    _size++;
    return true;
  }

  bool dequeue(StaticPacket &out_pkt, uint32_t &out_due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size == 0) {
      return false;
    }
    out_due_ms = _elements[_head].due_ms;
    out_pkt = _elements[_head].pkt;
    _head = (_head + 1) % Capacity;
    _size--;
    return true;
  }

  bool peek(StaticPacket &out_pkt, uint32_t &out_due_ms) noexcept {
    CriticalSectionLocker lock(&_mux);
    if (_size == 0) {
      return false;
    }
    out_due_ms = _elements[_head].due_ms;
    out_pkt = _elements[_head].pkt;
    return true;
  }

  size_t size() const noexcept {
    CriticalSectionLocker lock(const_cast<portMUX_TYPE *>(&_mux));
    return _size;
  }
};

struct DeviceStateEntry {
  uint8_t dev_id;
  uint8_t sub1, sub2;
  std::array<uint8_t, 64> last_ack_data;
  uint8_t last_ack_len{0};
  uint8_t last_target_temp{0};
  uint8_t last_current_temp{0};
  uint32_t last_updated_ms{0};
  mutable uint32_t last_stale_poll_ms{0};
  uint8_t timeout_count{0};
  bool is_online{false};

  [[nodiscard]] bool isStale() const noexcept {
    return last_updated_ms > 0 &&
           TimeUtils::isElapsed(last_updated_ms,
                                Config::Timing::STALE_DEVICE_THRESHOLD_MS);
  }
};

// ============================================================================
// 1ST TIER WARM-START CACHE (RTC FAST SRAM & NVS SNAPSHOT)
// ============================================================================

struct RtcWarmCacheEntry {
  uint8_t dev_id;
  uint8_t sub1;
  uint8_t sub2;
  uint8_t source_channels;
  uint8_t raw_len;
  uint8_t raw_query[32];
};

struct RtcWarmCache {
  uint32_t magic; // 0x57415243 ('WARC')
  uint8_t count;
  uint8_t reserved[3];
  RtcWarmCacheEntry entries[48];
  uint32_t crc32;
};

constexpr uint32_t RTC_MAGIC_WARM_CACHE = 0x57415243; // 'WARC'

extern SemaphoreHandle_t g_ctrl_queue_mutex;

[[nodiscard]] inline bool
Queue_EnqueueDropHead(QueueHandle_t queue,
                      const StaticPacket &packet) noexcept {
  if (UNLIKELY(!queue))
    return false;
  MutexLocker lock(g_ctrl_queue_mutex);
  if (xQueueSend(queue, &packet, 0) == pdTRUE)
    return true;
  StaticPacket dummy;
  xQueueReceive(queue, &dummy, 0);
  return (xQueueSend(queue, &packet, 0) == pdTRUE);
}

class TokenBucket {
private:
  const uint32_t _capacity;
  const uint32_t _refill_ms;
  std::atomic<uint32_t> _tokens;
  std::atomic<uint32_t> _last_refill_ms;

public:
  explicit TokenBucket(uint32_t capacity, uint32_t refill_ms)
      : _capacity(capacity), _refill_ms(refill_ms), _tokens(capacity),
        _last_refill_ms(0) {}

  [[nodiscard]] bool consume(uint32_t count = 1) noexcept {
    refill();
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    while (current >= count) {
      if (_tokens.compare_exchange_weak(current, current - count,
                                        std::memory_order_acquire,
                                        std::memory_order_relaxed))
        return true;
    }
    return false;
  }
  void restore(uint32_t count = 1) noexcept {
    uint32_t current = _tokens.load(std::memory_order_relaxed);
    uint32_t target;
    do {
      target = std::min(current + count, _capacity);
    } while (!_tokens.compare_exchange_weak(
        current, target, std::memory_order_release, std::memory_order_relaxed));
  }
  void refill() noexcept {
    const uint32_t now = millis();
    uint32_t last = _last_refill_ms.load(std::memory_order_relaxed);
    if (now - last >= _refill_ms) {
      const uint32_t elapsed = now - last;
      const uint32_t new_tokens = elapsed / _refill_ms;
      if (new_tokens > 0) {
        const uint32_t next_last = now - (elapsed % _refill_ms);
        if (_last_refill_ms.compare_exchange_strong(
                last, next_last, std::memory_order_release,
                std::memory_order_relaxed)) {
          uint32_t current = _tokens.load(std::memory_order_relaxed);
          uint32_t target;
          do {
            target = std::min(current + new_tokens, _capacity);
          } while (!_tokens.compare_exchange_weak(current, target,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));
        }
      }
    }
  }
};

enum class TraceType : uint8_t {
  ALL = 0,
  QRY,
  CTL,
  ACK,
  DRP,
  RMT,
  MSG,
  CH,
  DEVID
};

struct WallpadChannelConfig {
  uart_port_t uart_num;
  QueueHandle_t *event_queue_ptr;
  uint8_t channel_id;
};

class DeviceRepository {
private:
  // 1차 캐시(MAX_TARGETS=48)와 1:1 일치 정렬 (실제 28대 + 20대 여유)
  static constexpr size_t MAX_DEVICES = 48;
  DeviceStateEntry cache[MAX_DEVICES]{};
  int8_t dev_lookup_map[256]{};
  size_t device_count = 0;
  SemaphoreHandle_t _cache_mutex = nullptr;

public:
  DeviceStateEntry *findMutable(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                bool auto_create = false) noexcept;
  void initDevices();
  void clear();
  [[nodiscard]] const DeviceStateEntry *find(uint8_t dev_id, uint8_t sub1,
                                             uint8_t sub2) const noexcept;
  [[nodiscard]] const DeviceStateEntry *getAt(size_t index) const noexcept;
  [[nodiscard]] bool getSnapshot(size_t index,
                                 DeviceStateEntry &out_copy) noexcept;
  [[nodiscard]] size_t count() const noexcept;
  [[nodiscard]] size_t getOnlineCount() const noexcept;
  void setLastStalePollMs(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                          uint32_t ms) noexcept;
  void setLastStalePollMsByIndex(size_t index, uint32_t ms) noexcept;
  bool setTargetTemp(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                     uint8_t temp) noexcept;
  void updateFromBus(StaticPacket &ack);
  static void decodeDeviceState(const GroupControlTemplate &grp,
                                const StaticPacket &ack,
                                const DeviceStateEntry *dev,
                                DecodedDeviceState &out);
  void handlePollingTimeout(const DeviceStateEntry *dev);
  void handlePollingTimeout(uint8_t dev_id, uint8_t sub1, uint8_t sub2);
  [[nodiscard]] bool copyVirtualAck(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                                    StaticPacket &out) noexcept;
};

namespace PacketCodec {
uint8_t calculateChecksum(const uint8_t *data, size_t len) noexcept;
} // namespace PacketCodec

class ControlDispatcher {
public:
  bool dispatch(StaticPacket &req, StaticPacket &virtual_ack_out);
};

struct HwSnapshot {
  uint8_t cpu0_cur, cpu0_15m_avg, cpu0_15m_peak, cpu0_24h_avg, cpu0_24h_peak;
  uint8_t cpu1_cur, cpu1_15m_avg, cpu1_15m_peak, cpu1_24h_avg, cpu1_24h_peak;
  uint16_t ram_cur, ram_15m_avg, ram_15m_peak, ram_24h_avg, ram_24h_peak;
  int8_t temp_cur, temp_15m_avg, temp_15m_peak, temp_24h_avg, temp_24h_peak;
};

struct StackSnapshot {
  uint16_t ch1_stack, ch2_stack, ch3_stack, ch4_stack, net_stack, telnet_stack;
};

struct LogEntry {
  uint32_t timestamp;
  char reason[32];
  SysSnapshot stats_snapshot;
  HwSnapshot hw_snapshot;
  StackSnapshot stack_snapshot;
  PktSnapshot packet_stats_snapshot;
};

class LogManager {
public:
  static constexpr size_t MAX_LOG_ENTRIES = 20;
  static void writeRebootLog(const char *reason);
  static size_t getLogCount();
  static bool getLogEntry(size_t index, LogEntry &out_entry);
  static void readRebootLog(char *out_buf, size_t max_len, size_t index = 0);
  static void clearRebootLog();
};

struct TracePacketEntry {
  struct timeval tv;
  uint8_t channel;
  bool is_tx;
  TraceType type;
  uint8_t len;
  std::array<uint8_t, 64> data;
};

class TelnetManager;
class TelnetTracer;

enum class Ch1State : uint8_t {
  IDLE,
  VIP_CONTROL,
  NORMAL_CONTROL,
  POLL_DEVICE
};

struct Ch1StateMetrics {
  std::atomic<uint32_t> poll_cnt{0}, vip_cnt{0}, normal_cnt{0},
      stale_poll_cnt{0};
  std::atomic<Ch1State> last_from_state{Ch1State::IDLE};
  std::atomic<Ch1State> last_to_state{Ch1State::IDLE};
  std::atomic<uint32_t> last_transition_ms{0};
};

extern Ch1StateMetrics g_ch1_state_metrics;
extern DeviceRepository g_device_repo;
extern ControlDispatcher g_control_dispatcher;
extern QueueHandle_t g_ch1_control_queue, g_ch1_vip_queue;
extern StaticQueue_t g_ch1_ctrl_queue_buf, g_ch4_pass_queue_buf,
    g_ch1_vip_queue_buf;
extern uint8_t
    g_ch1_ctrl_storage[Config::Queue::POOL_SIZE_CONTROL * sizeof(StaticPacket)];
extern uint8_t g_ch4_pass_storage[Config::Queue::POOL_SIZE_CH4_PASS *
                                  sizeof(StaticPacket)];
extern uint8_t
    g_ch1_vip_storage[Config::Queue::POOL_SIZE_VIP * sizeof(StaticPacket)];

extern StaticTask_t g_task_core1_ch1_buf, g_task_core1_slave_buf,
    g_task_core1_slave2_buf, g_task_core1_ch4_buf, g_task_core0_net_buf,
    g_telnet_task_buf;
extern StackType_t stackCore1Ch1[Config::Task::STACK_SIZE_CORE1],
    stackCore1Slave[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Slave2[Config::Task::STACK_SIZE_SLAVE],
    stackCore1Ch4[Config::Task::STACK_SIZE_CH4],
    stackCore0Net[Config::Task::STACK_SIZE_CORE0],
    telnetTaskStack[Config::Task::STACK_SIZE_TELNET];
extern EventGroupHandle_t g_wifi_event_group;
extern QueueSetHandle_t g_ch1_queue_set;
extern QueueHandle_t g_uart0_event_queue, g_uart1_event_queue,
    g_uart2_event_queue;
extern QueueHandle_t g_ch4_passthrough_queue;
extern SemaphoreHandle_t g_ch5_mutex;
extern SemaphoreHandle_t g_mgmt_mutex;
extern RuntimeConfig g_config;
extern std::shared_mutex g_config_rw;
extern portMUX_TYPE g_config_mux;
extern std::atomic<uint32_t> g_ch1_bus_ms;
extern std::atomic<bool> g_config_dirty;
extern std::atomic<bool> g_ota_in_progress;
extern std::atomic<bool> g_initial_caching_complete;
// ★ wallpad reset 시 수렴 상태를 재초기화하여 재학습·재락을 허용하는 신호
// 플래그
extern std::atomic<bool> g_probe_convergence_reset;
extern EventGroupHandle_t g_system_event_group;
constexpr EventBits_t SYS_EVT_OTA_IDLE = (1 << 0);
constexpr EventBits_t SYS_EVT_CACHE_READY = (1 << 1);
constexpr EventBits_t SYS_EVT_SYSTEM_RUNNING = (1 << 2);
extern PacketStatistics g_pkt_stats;
extern uint32_t g_boot_start_ms;
struct WifiFallbackGuard {
  std::atomic<bool> testing{false};
  uint32_t start_ms{0};
  char prev_ssid[64]{0};
  char prev_pass[64]{0};
};
extern WifiFallbackGuard g_wifi_guard;
extern SystemMetricsTracker g_metrics;

extern SoftwareSerial g_doorphone_serial;

void Task_Ch1(void *pvParameters);
void Task_Ch2Ch3(void *pvParameters);
void Task_Ch4(void *pvParameters);
void Task_Network(void *pvParameters);
void Task_Telnet(void *pvParameters);

extern TelnetManager g_telnet_manager;
extern SemaphoreHandle_t g_tracer_sem;
void System_TakeSnapshot(SysSnapshot &sys_snapshot, HwSnapshot &hw_snapshot,
                         StackSnapshot &stack_snapshot,
                         PktSnapshot &pkt_snapshot);

extern uint32_t rtc_magic;
extern uint32_t rtc_last_alive_ms[Config::Task::TASK_COUNT];
extern volatile uint32_t g_telnet_stage;
#define TSTAGE(n)                                                              \
  (g_telnet_stage = (0xA5A50000u | (static_cast<uint32_t>(n) & 0xFFFFu)))
constexpr uint32_t RTC_MAGIC_WDT = 0x57445431;
extern uint32_t rtc_rescue_magic;
constexpr uint32_t RTC_MAGIC_RESCUE = 0x52455343;
extern uint32_t rtc_clean_restart_magic;
constexpr uint32_t RTC_MAGIC_CLEAN_RESTART = 0x5AA55AA5;
extern RtcWarmCache rtc_warm_cache;
extern uint32_t rtc_crash_counter;

struct TaskWdtMetrics {
  std::atomic<uint32_t> last_feed_ms{0};
  std::atomic<uint32_t> max_interval_ms{0};
  std::atomic<uint32_t> feed_count{0};
};

class TaskWdtMonitor {
public:
  static constexpr size_t TASK_COUNT = 6;
  TaskWdtMetrics tasks[TASK_COUNT];

  inline void feed(size_t index) noexcept {
    if (index >= TASK_COUNT)
      return;
    uint32_t now = millis();
    rtc_last_alive_ms[index] = now;
    uint32_t prev =
        tasks[index].last_feed_ms.exchange(now, std::memory_order_relaxed);
    if (prev > 0) {
      uint32_t gap = (now >= prev) ? (now - prev) : 0;
      uint32_t cur_max =
          tasks[index].max_interval_ms.load(std::memory_order_relaxed);
      while (gap > cur_max &&
             !tasks[index].max_interval_ms.compare_exchange_weak(
                 cur_max, gap, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }
    }
    tasks[index].feed_count.fetch_add(1, std::memory_order_relaxed);
    esp_task_wdt_reset();
  }
};

extern TaskWdtMonitor g_wdt_monitor;

namespace Fmt {
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw);
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt);
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt);
void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st,
                      const TaskWdtMonitor &wdt);
} // namespace Fmt

extern TaskHandle_t g_telnet_task_handle, g_ch1_task_handle, g_ch2_task_handle,
    g_ch3_task_handle, g_ch4_task_handle, g_network_task_handle;

void Config_Load();
void Config_Save();
void Config_ResetDefaults();
void System_Restart(const char *reason);
void System_Sha256ToHex(const char *input, char *output);
void System_ReadCpuPct(uint8_t &cpu0_out, uint8_t &cpu1_out);
int8_t System_ReadTempC();
void System_EnterRescueMode(const char *reason);
void System_CheckOtaHealth();
void System_LogResetReason();
void System_DiagnoseStuck();
void System_CheckCoreDump();
extern const char *s_pending_reboot_reason;
[[nodiscard]] bool System_IsOtaPendingVerify();

void Hub_LoadConfig();
void Hub_SaveConfig();
bool Hub_SetSlot(uint8_t slot_idx, bool enabled, const char *ip, uint16_t port,
                 const char *name = nullptr);
bool Hub_SendPacket(uint8_t slot_idx, const StaticPacket &pkt);

struct RouteEndpoint {
  uint8_t channel_id{1}; // 기본 채널: CH1 (메인 물리 RS-485)
  int8_t slot_idx{-1};   // CH5인 경우 슬롯 인덱스 (0~4), 그 외 -1
  uint32_t last_seen_ms{0};
};

struct DeviceRouteEntry {
  uint8_t dev_id{0};
  uint8_t sub1{0};
  uint8_t sub2{0};
  RouteEndpoint endpoint{};
};

class DeviceRouteRegistry {
public:
  static constexpr size_t MAX_ROUTES = 64;

private:
  DeviceRouteEntry _entries[MAX_ROUTES]{};
  size_t _count{0};
  mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

public:
  void recordRoute(uint8_t channel_id, int8_t slot_idx, uint8_t dev_id,
                   uint8_t sub1, uint8_t sub2);
  bool lookupRoute(uint8_t dev_id, uint8_t sub1, uint8_t sub2,
                   RouteEndpoint &out_ep) const;
  size_t getRoutes(DeviceRouteEntry *out_buf, size_t max_count) const;
  void clear();
};

extern DeviceRouteRegistry g_route_registry;

extern std::atomic<bool> g_rescue_mode;
extern bool g_rollback_detected;
extern SemaphoreHandle_t g_uart0_mutex, g_uart1_mutex, g_uart2_mutex;
extern Config::Doorphone::FramingTracker g_doorphone_tracker;

// 1st-Tier Warm Cache externs
extern bool g_warm_cache_loaded;
extern uint8_t g_warm_cache_source; // 0: Cold, 1: RTC SRAM, 2: NVS Flash
extern uint8_t g_warm_cache_restored_count;
extern std::atomic<bool> g_warm_cache_dirty;
extern std::atomic<uint32_t> g_warm_cache_dirty_ms;

void Cache_SaveToRtc();
void Cache_SaveToNvs();
void Cache_RestoreOnBoot();
void Cache_CheckNvsDebounce();
