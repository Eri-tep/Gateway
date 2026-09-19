# M5Stack AtomS3 Lite RS485 Gateway
## 최적화 및 명명 변경 세부 계획서 v2.0

> **참조 문서**:
> - [MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md](MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md)
> - [REFACTORING_PLAN_V2.md](REFACTORING_PLAN_V2.md)
> - [NAMING_CONVENTIONS.md](NAMING_CONVENTIONS.md)
> - [MODERN_CPP_GUIDELINES.md](MODERN_CPP_GUIDELINES.md)

---

## 0. 범위 및 전제

이 계획서는 Phase 2 (분리 무결성 검증 완료) 이후, **Phase 3~9** 에 해당하는 실제 최적화 및 명명 변경 작업을 다룬다.

**적용 원칙**:
- 명명 변경은 기능 동작 변경이 아니다. 심볼 이름만 변경하고 동작은 100% 동일하게 유지.
- 최적화는 실측 또는 코드 분석으로 확인된 낭비 요소만 제거한다.
- 안정성(1~4순위) > 최적화(5순위) > 명명 규칙(6~8순위).
- 명명 변경과 최적화는 절대 같은 커밋에 섞지 않는다.

---

## 1. 명명 변경 계획

### 1.1 현황 진단 요약

NAMING_CONVENTIONS.md 기준으로 현재 코드베이스의 명명 규칙 준수 현황:

| 카테고리 | 현재 상태 | 위반 건수 |
| :--- | :--- | :---: |
| 전역 함수 `Domain_VerbNoun` | 대부분 준수. `Format*` 계열이 `namespace Fmt` 내부로 이미 이동됨 | 0 |
| Task 함수 `Task_<Domain>` | 완전 준수 | 0 |
| 전역 변수 `g_` 접두사 | 완전 준수 | 0 |
| `WarmCache_` 접두사 | **규칙서 미등록 도메인** 접두사 사용 중 | 4건 |
| `Boot_` 접두사 | **규칙서 미등록 도메인** 접두사 사용 중 (모두 `static`) | 5건 |
| `FormatXxx()` 전역 선언 | `namespace Fmt` 내부로 이미 wrapping됨. 구현만 Fmt:: 밖에 위치 | 4건 |
| `Ew11_` 접두사 | 규칙서 미등록 (Hub 도메인으로 흡수 필요 검토) | 6건 |

> [!NOTE]
> 실제 조사 결과 대부분의 공개 전역 함수는 이미 `Domain_VerbNoun` 또는 `namespace` 규칙을 따르고 있다. 명명 위반보다는 **구현과 선언의 위치 불일치** 및 **미등록 도메인 접두사** 문제가 주된 이슈다.

---

### 1.2 명명 변경 대상 상세

#### [R-01] `FormatXxx()` 구현을 `namespace Fmt { }` 안으로 이동

**현황**:
- `Common.h` L1523~1529: `namespace Fmt { ... }` 안에 선언
- `Engine.cpp` L234~353: 구현이 `namespace Fmt { }` **밖**에 위치
- 호출부(`CliCommands.cpp`, `main.cpp`)는 `Fmt::FormatHwMetrics(...)` 로 정상 접근

**문제**: 선언은 `Fmt::` 안, 구현은 전역 → 링커가 연결하므로 동작은 정상이나, 코드 의도가 불명확하고 향후 이름 충돌 위험.

**변경 내용** (`src/Engine/Engine.cpp` 분리 후):
```cpp
// BEFORE: Engine.cpp (전역 공간에 구현)
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw) { ... }

// AFTER: src/Engine/Engine.cpp (namespace 안으로 이동)
namespace Fmt {
void FormatHwMetrics(AppendBuf &out, const HwSnapshot &hw) { ... }
void FormatNetworkStats(AppendBuf &out, const PktSnapshot &pkt) { ... }
void FormatRs485Stats(AppendBuf &out, const PktSnapshot &pkt) { ... }
void FormatTaskStacks(AppendBuf &out, const StackSnapshot &st, const TaskWdtMonitor &wdt) { ... }
} // namespace Fmt
```
**위험도**: 낮음 (호출부 변경 없음, 링커 심볼 동일)
**커밋**: `rename: move FormatXxx implementations into namespace Fmt`

---

#### [R-02] `WarmCache_` 도메인 접두사 → `Cache_` 로 표준화

**현황**: `WarmCache_SaveToRtc()`, `WarmCache_SaveToNvs()`, `WarmCache_RestoreOnBoot()`, `WarmCache_CheckNvsDebounce()`

**문제**: `NAMING_CONVENTIONS.md`에 등록되지 않은 접두사. `WarmCache`는 구현 세부사항 (Warm vs Cold Cache)을 접두사로 노출.

**변경 내용**:
| 변경 전 | 변경 후 | 이유 |
| :--- | :--- | :--- |
| `WarmCache_SaveToRtc()` | `Cache_SaveToRtc()` | 도메인 표준화 |
| `WarmCache_SaveToNvs()` | `Cache_SaveToNvs()` | 도메인 표준화 |
| `WarmCache_RestoreOnBoot()` | `Cache_RestoreOnBoot()` | 도메인 표준화 |
| `WarmCache_CheckNvsDebounce()` | `Cache_CheckNvsDebounce()` | 도메인 표준화 |

**영향 파일**: `src/main.cpp` (구현), `include/Common.h` (선언), 호출부 (`Engine.cpp`, `main.cpp` 내 Task_Network)
**위험도**: 낮음 (단순 심볼 rename, 기능 변경 없음)
**커밋**: `rename: WarmCache_ → Cache_ domain prefix`

> **NAMING_CONVENTIONS.md 업데이트**: `Cache_` 접두사를 공식 도메인 접두사로 추가 (영구 부팅 상태 캐시 관리).

---

#### [R-03] `Boot_` 접두사 — 유지 (모두 `static`, 외부 노출 없음)

**현황**: `Boot_CheckCrashLoop()`, `Boot_InitSyncPrimitives()`, `Boot_InitHardwareAndDevices()`, `Boot_InitWifiAndOta()`, `Boot_StartTasks()` — 모두 `static void`

**결정**: 이미 `static`으로 `main.cpp` 내부에만 한정되어 있어 외부 인터페이스에 영향 없음. 명명 규칙 문서에 `Boot_` 접두사를 `static`-only 내부 함수 규칙으로 명시화하는 것으로 충분.

**조치**: 변경 없음. NAMING_CONVENTIONS.md에 `Boot_` 참조 추가.

---

#### [R-04] `Ew11_` 접두사 → `Hub_` 로 표준화

**현황**: `Ew11_LoadConfig()`, `Ew11_SaveConfig()`, `Ew11_SetSlot()`, `Ew11_SendPacket()`, `Ew11_Data()`, `Ew11_AcceptClient()`

**문제**: `Ew11`은 특정 하드웨어 모델명(EW-11) 기반 접두사. 설계서 v2.0의 `HubManager` 도메인과 명칭 불일치. 향후 다른 Hub 클라이언트 확장 시 혼란.

**변경 내용**:
| 변경 전 | 변경 후 |
| :--- | :--- |
| `Ew11_LoadConfig()` | `Hub_LoadConfig()` |
| `Ew11_SaveConfig()` | `Hub_SaveConfig()` |
| `Ew11_SetSlot()` | `Hub_SetSlot()` |
| `Ew11_SendPacket()` | `Hub_SendPacket()` |
| `Ew11_Data()` | `Hub_Data()` |
| `Ew11_AcceptClient()` | `Hub_AcceptClient()` |
| `static Ew11_ProcessPacket()` | `static Hub_ProcessPacket()` |
| 타입 `Ew11ClientSlot` | `HubClientSlot` |
| 전역 `g_ew11_slots[]` | `g_hub_slots[]` |

**영향 파일**: `src/main.cpp`→`src/Network/HubManager.cpp` (구현), `include/Common.h` (타입/extern 선언), 호출부 (main.cpp, Engine.cpp, MgmtRpc.cpp, CliCommands.cpp)
**위험도**: 중간 (다수 파일 변경, grep/replace 필수)
**전제**: Phase 1-F HubManager.cpp 분리 완료 후 진행
**커밋**: `rename: Ew11 → Hub domain prefix and type names`

> **NAMING_CONVENTIONS.md 업데이트**: `Hub_` 접두사를 공식 도메인 접두사로 추가.

---

#### [R-05] `Uart_RecvPacket` — static 유지, 분리 후 배치 확인

**현황**: `static UartRxStatus Uart_RecvPacket(...)` — Engine.cpp L866, `static`으로 내부 한정

**결정**: `UartRx.cpp`로 이동 시 `static` 속성 유지. 이름은 `Uart_` 접두사로 이미 규칙 준수. 변경 없음.

단, `Uart_GetEventQueue()` 등 연관 헬퍼도 같은 파일로 함께 이동하여 논리적 응집도 유지.

---

#### [R-06] `Mgmt_Serialize*` 계열 명명 — 유지

**현황**: `Mgmt_SerializeTelemetry()`, `Mgmt_SerializeLockedDevices()`, `Mgmt_BroadcastXxx()`

**결정**: 이미 `Domain_VerbNoun` 규칙 완전 준수. 변경 없음.

---

### 1.3 명명 변경 커밋 순서

```
commit 27: rename: move FormatXxx() into namespace Fmt { }      [R-01]
commit 28: rename: WarmCache_ → Cache_ prefix                   [R-02]
commit 29: rename: Ew11 → Hub (type, function, global)          [R-04]
commit 30: docs: update NAMING_CONVENTIONS.md (Cache_, Hub_, Boot_)
```

---

## 2. 최적화 계획

### 2.1 Phase 3 — Hot Path: UART RX/TX 최적화

**대상 파일**: `src/Engine/UartRx.cpp`, `src/Engine/UartTx.cpp`
**대상 함수**: `static Uart_RecvPacket()`, TX 관련 함수들

#### [O-01] `Uart_RecvPacket` — `WallpadParserFactory::getActiveParser()` 호출 제거

**현황**: `Uart_RecvPacket` 내부에서 매번 `WallpadParserFactory::getActiveParser()` 호출 (함수 진입마다 실행, 9600bps에서 최대 수백 회/초).

```cpp
// BEFORE: 매 호출마다 parser 포인터 재획득
static UartRxStatus Uart_RecvPacket(...) {
  auto *parser = WallpadParserFactory::getActiveParser(); // ← 매번 호출
  ...
  while (millis() - start_ms < tout_ms) {
    ...
    if (parser && parser->isAutoMode() && !parser->isLocked()) { ... }
    int len_res = parser ? parser->extractPacketLength(...) : -1;
  }
}
```

**변경 내용**: 호출 1회로 고정, `const` 포인터 캐싱. 단, 프로파일이 런타임에 변경될 수 있으므로 `UartRx` 진입점(Task 루프)에서 획득하고 함수 파라미터로 전달하는 방식 검토.

```cpp
// AFTER: Task 루프에서 1회 획득 → 파라미터 전달
static UartRxStatus Uart_RecvPacket(uart_port_t u_num, StaticPacket &out,
                                    uint32_t tout_ms,
                                    IWallpadParser *parser,       // ← 파라미터로
                                    UartPollCallback on_poll = nullptr, ...) {
  // parser 재획득 없음
  while (...) {
    if (parser && parser->isAutoMode() && !parser->isLocked()) { ... }
  }
}
```

**효과**: 함수 호출 오버헤드 제거, 루프 내 동적 dispatch 감소
**위험도**: 낮음 (프로파일 변경은 Management Path에서만 발생하여 Task 루프 주기 내에서 획득해도 충분)
**커밋**: `optimize: cache parser pointer in Uart_RecvPacket call site`

---

#### [O-02] `Uart_RecvPacket` — `stream` 스택 버퍼 크기 검토

**현황**: `uint8_t temp[64], stream[128]` — 스택 할당. RS485 9600bps 패킷 최대 길이 기준 충분하나, `stream[128]` 이 두 번 선언되어 있는 경우(Ch1/Ch23 각자 스택에) 확인 필요.

**작업**: 실제 최대 패킷 길이를 `Config::Protocol::MAX_PACKET_LEN` 상수로 명시화하고 배열 크기를 상수 기반으로 교체. `static` 버퍼화는 재진입 불가 이슈로 하지 않음.

```cpp
// AFTER
uint8_t temp[Config::Protocol::UART_READ_CHUNK];  // 64
uint8_t stream[Config::Protocol::MAX_STREAM_BUF]; // 128 (상수화)
```

**커밋**: `optimize: replace magic numbers in Uart_RecvPacket buffers with constants`

---

#### [O-03] TX 경로 — 불필요한 `uart_wait_tx_done` 대기 시간 검토

**현황**: `Ch1_HandleCtrl` 내 `uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(20))` — 20ms 고정 대기.

**작업**: 실제 TX 완료 시간 측정 (9600bps × 최대 패킷 길이 기준 이론값 계산) 후 `Config::Timing` 상수와 일치하는지 확인. 과도하면 단축, 부족하면 유지.

- 9600bps, 11bit/byte 기준: 1바이트 = ~1.15ms, 최대 14바이트 패킷 = ~16ms → 20ms 여유 적절
- **결론**: 현행 유지. 상수명만 `Config::Timing::UART_TX_DONE_TIMEOUT_MS`로 명시화.

**커밋**: `optimize: replace uart_wait_tx_done magic value with Config constant`

---

### 2.2 Phase 4 — 중복 Lookup 제거

**대상 파일**: `src/Engine/Ch1Polling.cpp`, `src/Engine/Ch1Engine.cpp`

#### [O-04] `Ch1_PollNext` 내 `g_device_repo.find()` 3중 중복 호출 제거

**현황** (Engine.cpp L1185~1300 분석):
```cpp
// Tier 1 선택 경로
const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2); // 1번
...
// Tier 2 경로 (라우트 미일치)
const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2); // 2번 (같은 tgt)
...
// Stale 폴링 경로
const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2); // 3번 (같은 tgt)
```

`DeviceRepository::find()` 내부는 선형 탐색(O(n)). 동일 `(dev_id, sub1, sub2)` 에 대해 단일 이터레이션 내에서 3회 호출됨.

**변경 내용**: 각 분기 진입 전 1회 호출 후 포인터 재사용. 단, `g_device_repo.updateFromBus()` 호출 이후에는 포인터 무효화될 수 있으므로 갱신 이전 스냅샷만 재사용.

```cpp
// AFTER: 단일 이터레이션 내 1회 획득
const auto *cached_dev = g_device_repo.find(tgt.dev_id, tgt.sub1, tgt.sub2);
bool has_fresh_data = (tgt.raw_ack_len > 0 && cached_dev && cached_dev->last_updated_ms > 0);

if (!has_fresh_data) {
    // Tier 1: 직접 폴링
    poll_dev_id = tgt.dev_id;
    ...
} else {
    // 라우트 확인 후 Tier 2 또는 Stale 처리 — cached_dev 재사용
    ...
    g_device_repo.setLastStalePollMs(tgt.dev_id, tgt.sub1, tgt.sub2, now); // 포인터 무효화 없음
}
```

**효과**: 루프 1회당 `DeviceRepository::find()` 2회 감소 (최대 폴링 대상 수 × 절약)
**위험도**: 낮음~중간 (논리 동일, 포인터 수명 주의)
**커밋**: `optimize: eliminate redundant g_device_repo.find() calls in Ch1_PollNext`

---

#### [O-05] `Ch1_PollNext` 내 `poll_raw_data` memcpy 3중 중복 제거

**현황**: Tier 1/2/Stale 각 분기에서 동일한 패턴 반복:
```cpp
poll_raw_len = tgt.raw_query_len;
if (poll_raw_len > 0)
    memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len); // 3곳에서 반복
```

**변경 내용**: 분기 전 공통 코드를 분기 밖으로 추출 (hoisting):
```cpp
// AFTER: 분기 외부에서 1회만
poll_dev_id = tgt.dev_id;
poll_sub1   = tgt.sub1;
poll_sub2   = tgt.sub2;
poll_raw_len = tgt.raw_query_len;
if (poll_raw_len > 0)
    memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len);
// 이후 각 분기는 poll_* 변수를 그대로 사용
```

**효과**: Hot Path에서 `memcpy` 2회 절약, 코드 중복 제거
**위험도**: 낮음 (로직 동일)
**커밋**: `optimize: hoist redundant poll_raw_data copy out of Ch1_PollNext branches`

---

#### [O-06] `Ch1_HandleCtrl` 내 `getActiveParser()` 재호출 제거

**현황**: `Ch1_HandleCtrl` 진입 시 `getActiveParser()` 호출 후, 컨트롤 처리 완료 후 다시 `isQueryPacket()` 호출 시 재획득 패턴.

**변경 내용**: 함수 최상단에서 1회 획득 후 재사용. [O-01]과 동일 패턴.

**커밋**: `optimize: cache parser pointer in Ch1_HandleCtrl`

---

### 2.3 Phase 5 — Memory Path 최적화

**대상 파일**: `src/Engine/Ch1Engine.cpp`, `src/Engine/UartRx.cpp`

#### [O-07] `DeviceRepository::updateFromBus` 내 memcpy 검토

**현황** (Engine.cpp L505~637): `updateFromBus(ack)` 내부에서 `ack.data` → `dev->last_ack_data` memcpy.
```cpp
memcpy(dev->last_ack_data.data(), ack.data.data(), ack.length); // L530
```

**분석**: `StaticPacket`은 고정 크기 배열(`std::array<uint8_t, N>`) 기반. `ack.length` ≤ 최대 패킷 크기(14바이트 수준). **9600bps 환경에서 14바이트 memcpy는 ~1µs 미만으로 무시 가능한 비용.**

**결정**: 현행 유지. Zero-copy 소유권 모델 도입으로 복잡성 증가를 정당화할 만한 이득 없음. 설계서 v2.0 §6.1 원칙("RS485 9600bps 환경에서는 CPU보다 안정성이 중요") 준수.

---

#### [O-08] `getSnapshot()`, `copyVirtualAck()` — 이중 복사 검토

**현황**:
```cpp
memcpy(out_copy.state_data.data(), cache[index].state_data.data(), len);  // L447
memcpy(out_copy.last_ack_data.data(), cache[index].last_ack_data.data(), ...); // L452
```

**분석**: `getSnapshot()`은 CLI/Telnet 에서만 호출 (Cold Path). Hot Path 외부. 최적화 불필요.

**결정**: 현행 유지.

---

#### [O-09] `Ch1_PollNext` — `q_pkt` 빌드 시 memcpy 최소화

**현황**:
```cpp
// 1. poll_raw_data에 복사 (분기에서 이미 복사)
memcpy(poll_raw_data, tgt.raw_query_data.data(), poll_raw_len);
// 2. q_pkt에 다시 복사
q_pkt.length = poll_raw_len;
memcpy(q_pkt.data.data(), poll_raw_data, poll_raw_len); // ← 2번째 복사
```

**변경 내용**: [O-05] 구현 시 `poll_raw_data` 중간 버퍼를 제거하고 `tgt.raw_query_data.data()`를 직접 `q_pkt.data.data()`로 복사 1회만:
```cpp
// AFTER: poll_raw_data 중간 버퍼 제거
q_pkt.length = tgt.raw_query_len;
memcpy(q_pkt.data.data(), tgt.raw_query_data.data(), tgt.raw_query_len);
```

**효과**: memcpy 1회 제거, 스택 변수(`poll_raw_data[Config::Protocol::MAX_PACKET_LEN]`) 제거
**위험도**: 낮음~중간 (poll_raw_len/poll_raw_data를 참조하는 다른 코드 없는지 확인 필수)
**주의**: [O-04], [O-05]와 함께 구현할 것 — 연계 변경
**커밋**: `optimize: remove intermediate poll_raw_data copy buffer in Ch1_PollNext`

---

### 2.4 Phase 6 — Lock Hold Time 최적화

**대상 파일**: `src/Telnet/TelnetTracer.cpp`, `src/Engine/Ch1Engine.cpp`

#### [O-10] `TelnetTracer::flushToClient()` — Lock 보호 범위 축소

**현황**: `TelnetTracer::flushToClient()`가 소켓 send까지 Lock 내부에서 실행하는 패턴이 있는지 확인 필요.

```cpp
// 확인 필요 패턴
xSemaphoreTake(g_tracer_sem, ...);
{
    format_output();     // OK
    send(sock, ...);     // ← 소켓 I/O가 Lock 내부에 있으면 개선 대상
}
xSemaphoreGive(g_tracer_sem);
```

**변경 방향**: 
- Lock → 링 버퍼에서 데이터 추출 → Unlock → 소켓 send (Lock 외부)
- 기존 Lock 범위 확인 후 소켓 I/O가 포함되어 있을 때만 적용

**커밋**: `optimize: narrow lock scope in TelnetTracer::flushToClient`

---

#### [O-11] `DeviceRepository::updateFromBus()` — Lock 범위 검토

**현황**: 전체 `updateFromBus` 가 단일 Lock 하에 실행되는지, 또는 내부적으로 세분화되어 있는지 확인.

**변경 방향**: Lock → 최소 상태 갱신 → Unlock → (필요시) Notification 발행. Notification/Broadcast (느린 I/O)를 Lock 외부로 이동.

**주의**: 실제 Lock 구조 확인 없이 변경 금지. 분석 후 이득이 확인될 때만 적용.

**커밋**: `optimize: narrow lock scope in DeviceRepository::updateFromBus`

---

### 2.5 Phase 7 — Logging 경로 격리

**대상 파일**: `src/Engine/UartRx.cpp`, `src/Engine/Ch1Engine.cpp`, `src/Engine/Ch1Polling.cpp`

#### [O-12] Hot Path 내 `g_telnet_tracer.trace()` 직접 호출 → 이벤트 큐 경유로 전환

**현황**: `Uart_RecvPacket` 및 `Ch1_HandleCtrl` 내에서 `g_telnet_tracer.trace(...)` 직접 호출.

```cpp
// BEFORE: Hot Path 직접 호출
g_telnet_tracer.trace(1, false, TraceType::ACK, ack);
g_telnet_tracer.trace("[WARN] Dropped CH1 ctrl packet, mutex timed out.\r\n");
```

**분석**: 현재 `TelnetTracer::trace()`가 내부적으로 링 버퍼에만 쓰고 Semaphore로 즉시 소켓 전송을 트리거하는 구조인지 확인 필요.

- `trace()` → 링 버퍼에 쓰기만 (소켓 I/O 없음) → OK, 유지 가능
- `trace()` → 즉시 `flushToClient()` 호출 (소켓 I/O 동기 포함) → **격리 필요**

**변경 방향 (후자인 경우)**:
```cpp
// AFTER: 최소 이벤트만 링 버퍼에 기록, flush는 Tracer Task/Semaphore에 위임
// Hot Path: 링 버퍼에 raw event push만
g_telnet_tracer.pushEvent(TraceEvent{...}); // non-blocking, lock-free

// Cold Path (TelnetTracer::flushToClient 또는 Telnet Task):
// 링 버퍼에서 이벤트 poll → format → send
```

**커밋**: `optimize: decouple Hot Path trace from socket I/O in TelnetTracer`

---

#### [O-13] `g_pkt_stats.ch1.timeouts.fetch_add()` — 정상 패턴 유지

**현황**: Hot Path 내 `std::atomic::fetch_add(std::memory_order_relaxed)` 사용.

**결론**: `memory_order_relaxed`는 이미 최적화된 패턴. 변경 없음.

---

### 2.6 Phase 8 — Management Path 정리

**대상 파일**: `src/Management/Telemetry.cpp`, `src/Network/HubManager.cpp`

#### [O-14] `Mgmt_SerializeTelemetry()` — 호출 빈도 및 버퍼 크기 검토

**현황**: Telemetry 직렬화가 얼마나 자주 호출되는지, 중간 버퍼 크기가 과도하지 않은지 확인.

**변경 방향**: 호출 주기가 1초 미만이면 Cold Path 분류로 적절. 현행 유지.

---

#### [O-15] `Hub_Data()` (구 `Ew11_Data`) — TCP fragmentation 조립 버퍼 재사용

**현황**: `TcpFragSession` 구조체가 세션당 할당되어 있고 조립 버퍼 크기 검토.

**변경 방향**: 버퍼 크기가 고정 상수 기반인지 확인. 매직 넘버가 있으면 `Config::TCP::FRAG_BUF_SIZE` 등으로 상수화.

**커밋**: `optimize: replace TCP frag session magic buffer sizes with Config constants`

---

## 3. 최적화 작업 커밋 순서 (전체)

```
# Phase 3: Hot Path
commit 31: optimize: cache parser pointer in Uart_RecvPacket [O-01]
commit 32: optimize: replace Uart buffer magic numbers with Config constants [O-02]
commit 33: optimize: replace uart_wait_tx_done magic value with Config constant [O-03]

# Phase 4: Lookup 제거
commit 34: optimize: eliminate redundant find() + hoist memcpy in Ch1_PollNext [O-04+05]
commit 35: optimize: remove intermediate poll_raw_data buffer [O-09]
commit 36: optimize: cache parser pointer in Ch1_HandleCtrl [O-06]

# Phase 6: Lock
commit 37: optimize: narrow lock scope in TelnetTracer::flushToClient [O-10]
commit 38: optimize: narrow lock scope in DeviceRepository::updateFromBus [O-11]

# Phase 7: Logging isolation
commit 39: optimize: decouple Hot Path trace from socket I/O [O-12]

# Phase 8: Management
commit 40: optimize: replace TCP frag session magic buffer sizes with Config constants [O-15]
```

---

## 4. 명명 변경 + 최적화 통합 커밋 맵

### 전체 커밋 시퀀스 (Phase 2 이후)

| 커밋 | 분류 | 설명 |
| :--- | :---: | :--- |
| `commit 27` | Rename | `FormatXxx()` → `namespace Fmt { }` 내부로 이동 |
| `commit 28` | Rename | `WarmCache_` → `Cache_` |
| `commit 29` | Rename | `Ew11` → `Hub` (타입, 함수, 전역) |
| `commit 30` | Docs | `NAMING_CONVENTIONS.md` 업데이트 |
| `commit 31` | Optimize | `Uart_RecvPacket` parser 포인터 캐싱 |
| `commit 32` | Optimize | UART 버퍼 크기 상수화 |
| `commit 33` | Optimize | `uart_wait_tx_done` timeout 상수화 |
| `commit 34` | Optimize | `Ch1_PollNext` `find()` 중복 제거 + memcpy hoisting |
| `commit 35` | Optimize | `poll_raw_data` 중간 버퍼 제거 |
| `commit 36` | Optimize | `Ch1_HandleCtrl` parser 포인터 캐싱 |
| `commit 37` | Optimize | `TelnetTracer::flushToClient` Lock 범위 축소 |
| `commit 38` | Optimize | `DeviceRepository::updateFromBus` Lock 범위 축소 |
| `commit 39` | Optimize | Hot Path trace → Tracer 분리 (검증 후) |
| `commit 40` | Optimize | TCP frag 버퍼 크기 상수화 |

---

## 5. 최종 검증 기준 (Phase 10)

### 5.1 명명 변경 검증

```bash
# 구 이름 잔존 여부 확인
grep -rn "WarmCache_\|Ew11_\|Ew11Client\|g_ew11_slots" src/ include/
# → 0 결과 기대
```

### 5.2 최적화 효과 검증

| 지표 | 측정 방법 | 목표 |
| :--- | :--- | :--- |
| `g_device_repo.find()` 호출 횟수 | 카운터 atomic 추가 → 1000 폴링 주기 측정 후 제거 | 기존 대비 33% 감소 |
| 폴링 루프 1회 실행 시간 | `esp_timer_get_time()` 측정 | Baseline 대비 개선 |
| Hot Path `memcpy` 횟수 | 코드 리뷰 카운트 | `Ch1_PollNext` 2회 → 1회 |
| Free heap | `esp_get_free_heap_size()` | Baseline 대비 감소 없음 |
| Stack watermark | `uxTaskGetStackHighWaterMark()` | Baseline 대비 악화 없음 |
| WDT Reset | 시리얼 로그 모니터링 (24h) | 0건 |
| RS485 패킷 처리 시간 | Tracer timestamp 분석 | Baseline 대비 유지 또는 개선 |

---

## 6. 향후 검토 항목 (이번 범위 외)

아래는 이번 Phase에서 다루지 않고 별도로 검토할 사항:

| 항목 | 이유 | 시점 |
| :--- | :--- | :--- |
| `DeviceRepository::find()` O(n) → O(1) 해시 인덱싱 | 아키텍처 변경에 해당, 현재 디바이스 수 소규모 | 별도 검토 |
| `Config::Timing` 전체 상수 재검토 | 안전 타이밍에 영향 | 별도 검토 |
| `ControlTemplate` 내부 최적화 | 고응집 모듈, Cold Path 위주 | 필요시 별도 |
| `AutoProbingEngine` 학습 알고리즘 최적화 | 별도 도메인 전문성 필요 | 별도 검토 |
| `TelnetTracer` 링 버퍼 → Lock-free Queue 전환 | 현재 Semaphore 기반이 안정적 | 실측 병목 확인 후 |

