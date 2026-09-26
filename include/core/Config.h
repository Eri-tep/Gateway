#pragma once

// ============================================================================
// SYSTEM CONFIGURATION CONSTANTS & NAMESPACES (Extracted from Common.h SECTION
// 2)
// ============================================================================

#include "Platform.h"

namespace Config {
// [시스템] 펌웨어 버전 문자열 (CLI/Log/OTA)
constexpr const char *FIRMWARE_VERSION = "v1.1.5";
} // namespace Config

namespace Config::Task {
// [Task] Core1 RS-485 마스터/슬레이브(CH1, CH2, CH3) 통신 스택 크기 (기본:
// 8192B)
constexpr size_t STACK_SIZE_CORE1 = 8192;
// [Task] Core1 도어폰(CH4 SoftwareSerial) 통신 스택 크기 (기존 8192B -> 6144B
// 최적화)
constexpr size_t STACK_SIZE_CH4 = 6144;
// [Task] Core0 네트워크 스택 크기 (기본: 8192B)
constexpr size_t STACK_SIZE_CORE0 = 8192;
// [Task] Telnet CLI 스택 크기 (기존 8192B -> 9216B 확장)
constexpr size_t STACK_SIZE_TELNET = 9216;
} // namespace Config::Task

namespace Config::Queue {
// [Queue] CH1 제어 명령 큐 용량 (기본: 32개)
constexpr size_t POOL_SIZE_CONTROL = 32;
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

  void clearNvs(const char *nvs_ns = "dp_frame",
                const char *tag = "DOORPHONE") noexcept {
    reset();
    Preferences prefs;
    if (prefs.begin(nvs_ns, false)) {
      prefs.clear();
      prefs.end();
      ::Serial.printf("[%s] Cleared framing NVS storage (%s).\r\n", tag,
                      nvs_ns);
    }
  }

  void processFrame(uint8_t stx, uint8_t etx, uint8_t len = 0,
                    const char *nvs_ns = "dp_frame",
                    const char *tag = "DOORPHONE") noexcept {
    if (is_custom_fixed.load(std::memory_order_relaxed)) {
      // Custom 고정 락 모드: 노이즈나 외래 패킷으로 인한 상태 변경 불가 (영구
      // 락)
      return;
    }

    FramingStatus cur = status.load(std::memory_order_relaxed);

    // 도어폰 전용 1-Shot 카탈로그 즉시 잠금: STX=0x7F, ETX=0xEE 5바이트 프레임
    // 1회 감지 즉시 LOCKED
    if (stx == 0x7F && etx == 0xEE && (len == 0 || len == 5)) {
      setFixedLock(0x7F, 0xEE, 5);
      saveToNvs(nvs_ns, tag);
      return;
    }

    if (cur == FramingStatus::WAITING) {
      candidate_stx.store(stx, std::memory_order_relaxed);
      candidate_etx.store(etx, std::memory_order_relaxed);
      if (len > 0)
        candidate_len.store(len, std::memory_order_relaxed);
      consecutive_matches.store(1, std::memory_order_relaxed);
      consecutive_mismatches.store(0, std::memory_order_relaxed);
      status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
      return;
    }

    uint8_t cand_s = candidate_stx.load(std::memory_order_relaxed);
    uint8_t cand_e = candidate_etx.load(std::memory_order_relaxed);

    if (stx == cand_s && etx == cand_e) {
      if (len > 0)
        candidate_len.store(len, std::memory_order_relaxed);
      consecutive_mismatches.store(0, std::memory_order_relaxed);
      uint8_t m =
          consecutive_matches.fetch_add(1, std::memory_order_relaxed) + 1;
      if (m >= 3) {
        status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
        saveToNvs(nvs_ns, tag);
      } else {
        status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
      }
    } else {
      consecutive_matches.store(0, std::memory_order_relaxed);
      uint8_t m =
          consecutive_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
      if (cur == FramingStatus::LOCKED) {
        // Auto 프로파일 상태: 연속 10회 이상 새로운 프레임 패턴이 지속될 때만
        // 안전하게 자동 언락
        if (m >= 10) {
          status.store(FramingStatus::WAITING, std::memory_order_relaxed);
          consecutive_mismatches.store(0, std::memory_order_relaxed);
        }
        // 단발성 노이즈(m < 10)에서는 LOCKED 상태 유지 (NOISY 등으로 강등 금지)
      } else {
        // LEARNING 상태: 불일치 5회 누적 시 새 후보로 교체
        if (m >= 5) {
          candidate_stx.store(stx, std::memory_order_relaxed);
          candidate_etx.store(etx, std::memory_order_relaxed);
          if (len > 0)
            candidate_len.store(len, std::memory_order_relaxed);
          consecutive_matches.store(1, std::memory_order_relaxed);
          consecutive_mismatches.store(0, std::memory_order_relaxed);
          status.store(FramingStatus::LEARNING, std::memory_order_relaxed);
        }
      }
    }
  }

  inline static void getNvsNamespace(uint8_t prof_idx, char *out_ns,
                                     size_t max_len) noexcept {
    snprintf(out_ns, max_len, "dp_frame_p%u", prof_idx & 0x03);
  }

  void restoreFromNvs(const char *nvs_ns = "dp_frame_p0",
                      const char *tag = "DOORPHONE") noexcept {
    if (!nvs_ns)
      nvs_ns = "dp_frame_p0";

    Preferences prefs;
    bool found = false;
    if (prefs.begin(nvs_ns, true)) {
      if (prefs.isKey("stx") && prefs.isKey("etx")) {
        found = true;
      }
      prefs.end();
    }

    // 레거시 "dp_frame" 1회 마이그레이션 (슬롯 네임스페이스가 아직 비어있을 때)
    if (!found && strcmp(nvs_ns, "dp_frame") != 0) {
      Preferences leg;
      if (leg.begin("dp_frame", true)) {
        if (leg.isKey("stx") && leg.isKey("etx")) {
          uint8_t ls = leg.getUChar("stx", 0);
          uint8_t le = leg.getUChar("etx", 0);
          uint8_t ll = leg.getUChar("len", 0);
          bool llocked = leg.getBool("locked", false);
          bool lfixed = leg.getBool("fixed", false);
          leg.end();

          if (llocked && ls != 0 && le != 0) {
            Preferences dest;
            if (dest.begin(nvs_ns, false)) {
              dest.putUChar("stx", ls);
              dest.putUChar("etx", le);
              dest.putUChar("len", (ls == 0x7F && le == 0xEE) ? 5 : ll);
              dest.putBool("locked", true);
              dest.putBool("fixed", (ls == 0x7F && le == 0xEE) ? true : lfixed);
              dest.end();
              ::Serial.printf("[%s] Migrated legacy dp_frame to %s\r\n", tag,
                              nvs_ns);
            }
          }
        } else {
          leg.end();
        }
      }
    }

    if (prefs.begin(nvs_ns, true)) {
      uint8_t s = prefs.getUChar("stx", 0);
      uint8_t e = prefs.getUChar("etx", 0);
      uint8_t l = prefs.getUChar("len", 0);
      bool locked = prefs.getBool("locked", false);
      bool fixed = prefs.getBool("fixed", false);
      prefs.end();
      if (locked && s != 0 && e != 0) {
        if (s == 0x7F && e == 0xEE && l != 5) {
          l = 5;
          fixed = true;
          Preferences wr_pref;
          if (wr_pref.begin(nvs_ns, false)) {
            wr_pref.putUChar("len", 5);
            wr_pref.putBool("fixed", true);
            wr_pref.end();
          }
        }
        candidate_stx.store(s, std::memory_order_relaxed);
        candidate_etx.store(e, std::memory_order_relaxed);
        candidate_len.store(l, std::memory_order_relaxed);
        consecutive_matches.store(3, std::memory_order_relaxed);
        is_custom_fixed.store(fixed, std::memory_order_relaxed);
        status.store(FramingStatus::LOCKED, std::memory_order_relaxed);
        ::Serial.printf("[%s] Restored framing from NVS (%s): STX 0x%02X, "
                        "ETX 0x%02X, Len %u%s\r\n",
                        tag, nvs_ns, s, e, l, fixed ? " (FIXED)" : "");
      }
    }
  }

  void saveToNvs(const char *nvs_ns = "dp_frame_p0",
                 const char *tag = "DOORPHONE") noexcept {
    if (!nvs_ns)
      nvs_ns = "dp_frame_p0";

    uint8_t s = candidate_stx.load(std::memory_order_relaxed);
    uint8_t e = candidate_etx.load(std::memory_order_relaxed);
    uint8_t l = candidate_len.load(std::memory_order_relaxed);
    bool fixed = is_custom_fixed.load(std::memory_order_relaxed);
    if (s == 0 || e == 0)
      return;
    Preferences prefs;
    if (prefs.begin(nvs_ns, false)) {
      prefs.putUChar("stx", s);
      prefs.putUChar("etx", e);
      prefs.putUChar("len", l);
      prefs.putBool("locked", true);
      prefs.putBool("fixed", fixed);
      prefs.end();
      ::Serial.printf("[%s] Saved framing to NVS (%s): STX 0x%02X, ETX "
                      "0x%02X, Len %u%s\r\n",
                      tag, nvs_ns, s, e, l, fixed ? " (FIXED)" : "");
    }
  }

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
