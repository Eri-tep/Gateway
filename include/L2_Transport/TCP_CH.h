#pragma once

// ============================================================================
// TCP_CH.h — L2 Transport / Data Link Channels
// TCP Socket Channel Abstraction (CH5 EW11 + CH6 SmartThings/REST)
// Canonical 4+1 Layer: L2 — depends only on L1 Drivers and L0 Base.
// ============================================================================
//
// Channel Mapping:
//   CH5 = EW11 Virtual RS-485 TCP bridge  (Core 0 TCP Reactor)
//   CH6 = SmartThings Hub / REST / Mgmt   (Core 0 TCP Reactor)
//
// Design invariants:
//   - IPFilter (subnet whitelist) is embedded here — NOT exposed to L3+.
//   - All socket file descriptor management is sealed inside TcpReactor.cpp.
//   - Reactor participant registration (BridgeService, RemoteService) is the
//     only coupling point between L4 services and L2 TCP_CH.
//   - No upward includes ($L2 → L3+$): zero violation of AGENTS.md Rule 17.
//
// This header aggregates and re-exports the canonical L2 TCP channel surface:
//   - Transport::TcpReactor  (Core 0 select() event loop)
//   - Transport::ReactorParticipant  (zero-vtable SPI for socket owners)
//   - IP whitelist filter functions  (moved from NetworkRouter.h)
// ============================================================================

#include "L0_Foundation/System_Buffer.h"
#include "L0_Foundation/System_Platform.h"
#include <IPAddress.h>
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

// ── Task Entry Point (Core 0) ─────────────────────────────────────────────────

/// Core 0 TCP event reactor task. Drives the select() loop for all TCP channels.
/// Registered participants (BridgeService, RemoteService) are called from here.
/// Task naming: Task_<Domain> per MODERN_CPP_GUIDELINES.md §1.2.
/// Implementation: Transport::TcpReactor::runTask()
inline void Task_TcpCore0(void *pvParameters) {
  Transport::TcpReactor::runTask(pvParameters);
}
