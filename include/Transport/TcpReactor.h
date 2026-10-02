#pragma once

// ============================================================================
// TcpReactor: Level 2 Transport Network Event Reactor (Core 0 Event Loop)
// ============================================================================

#include <sys/select.h>
#include <cstddef>
#include <cstdint>

namespace Transport {

// ── Reactor Participant Interface (High-Performance Zero-vtable SPI) ──
struct ReactorParticipant {
  const char *name{nullptr};

  // 1. 감시할 소켓 fd 수집 (select() 호출 전)
  void (*populateFds)(fd_set &readfds, fd_set &errorfds, int &max_fd) noexcept {nullptr};

  // 2. 소켓 I/O 이벤트 발생 시 디스패치 (select() 복귀 후)
  void (*processEvents)(fd_set &readfds, fd_set &errorfds,
                        bool ota_now) noexcept {nullptr};

  // 3. 주기적 타이머 틱 (10~20ms 주기)
  void (*tick)(bool ota_now, uint32_t now_ms) noexcept {nullptr};
};

class TcpReactor {
public:
  static constexpr size_t MAX_PARTICIPANTS = 4;

  // 정적 참가자 등록 (부트 시점 등록)
  static bool registerParticipant(const ReactorParticipant &p) noexcept;

  // Core 0 단일 네트워크 이벤트 루프 진입점
  static void runTask(void *pvParameters);

  // 긴급 정지 / 소켓 일괄 셧다운
  static void shutdownAll() noexcept;
};

} // namespace Transport
