# M5Stack AtomS3 Lite RS485 Gateway
## CH6 RAW 전송 제거 + CH7→CH6 변경 + 힙 최적화 계획서

> **참조 문서**:
> - [MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md](MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md)
> - [REFACTORING_PLAN_V2.md](REFACTORING_PLAN_V2.md)

---

## 0. 현황 파악

### 채널 구조 (현재 As-Is)

| 채널 | 포트 | 역할 | 방향 |
| :--- | :--- | :--- | :--- |
| CH1 | UART0 (RS485) | IoT 마스터 폴링/제어 | 양방향 |
| CH2/3 | UART1/2 (RS485) | 월패드 슬레이브 가상 응답 | 양방향 |
| CH4 | SoftwareSerial | 도어폰 통신 | 양방향 |
| CH5 | TCP 8898/8891~8894 | EW11 TCP 허브 클라이언트 연결 | 양방향 |
| **CH6 (삭제)** | TCP 8899 | 허브 RAW 패킷 수신/송신 서버 | 양방향 |
| **CH7 (→CH6 변경)** | TCP 8900 | SmartThings JSON-RPC 관리 서버 | 양방향 |

### 목표 (To-Be)

| 채널 | 포트 | 역할 |
| :--- | :--- | :--- |
| CH1~5 | 동일 | 변경 없음 |
| **CH6 (신)** | TCP 8900 | 구 CH7. SmartThings JSON-RPC 관리 서버 |

> [!IMPORTANT]
> TCP 포트 8899(구 CH6 Hub 서버)는 완전 폐쇄. 포트 8900(구 CH7 Mgmt 서버)은 **채널 번호만 CH6으로 변경**, 기능/포트/프로토콜 동일.

---

## 1. 삭제 대상 (CH6 RAW 전송 관련)

### 1.1 삭제 대상 함수

| 함수 | 위치 | 설명 |
| :--- | :--- | :--- |
| `Ch6_SendAck_Direct()` | `main.cpp` L710~735 | Hub 세션 직접 send (TokenBucket 기반 Rate Limit) |
| `Ch6_SendAck()` | `main.cpp` L738~742 | `g_ch6_to_tcp_queue`에 enqueue |
| `Ch6_Data()` | `main.cpp` L744~836 | Hub TCP 세션 수신 파싱 + 제어 dispatch |
| `hub_server_fd` 초기화 블록 | `main.cpp` L1098~1120 | TCP 8899 서버 소켓 bind/listen |
| `hub_sessions` select 루프 | `main.cpp` L1280~1340 | Hub 세션 accept/poll/receive |
| CH6 Queue drain 루프 | `main.cpp` L1383~1386 | `g_ch6_to_tcp_queue` → `Ch6_SendAck_Direct` |
| `Ch6_SendAck` 호출부 2곳 | `Engine.cpp` L1150, L1152 | ch1_HandleCtrl 내 ch6 ACK 전송 |

### 1.2 삭제 대상 전역 변수/버퍼

| 변수 | 위치 | 타입 | 크기 |
| :--- | :--- | :--- | ---: |
| `g_ch6_to_tcp_queue_buf` | `main.cpp` L396 | `StaticQueue_t` | ~88B |
| `g_ch6_to_tcp_storage[]` | `main.cpp` L404 | `uint8_t[32 * 66]` | **~2,112B** |
| `g_ch6_to_tcp_queue` | `main.cpp` L411 | `QueueHandle_t` | 4B |
| `g_ch6_mutex` | `main.cpp` L489 | `SemaphoreHandle_t` | 4B + FreeRTOS heap ~80B |
| `s_ch6_bucket` | `main.cpp` L550 | `TokenBucket` | ~16B |
| `hub_sessions[]` | `main.cpp` L488 | `TcpFragSession[3]` | **~780B** (`(4+256+4+4)*3`) |
| `hub_server_fd` | Task_Network 지역 | `int` | 4B |

**삭제로 인한 정적 메모리 해제: ~2,200B (약 2.1KB) + FreeRTOS 힙 ~80B (Semaphore)**

### 1.3 삭제 대상 Config 상수

| 상수 | 위치 | 값 |
| :--- | :--- | :--- |
| `Config::TCP::HUB_PORT` | `Common.h` L302~304 | `8899` |
| `Config::TCP::MAX_HUB_CLIENTS` | `Common.h` L310~311 | `3` |
| `Config::TCP::CH6_TOKEN_BURST` | `Common.h` L325 | `32` |
| `Config::TCP::CH6_TOKEN_REFILL_MS` | `Common.h` L327 | `100` |
| `static_assert` on `MAX_HUB_CLIENTS` | `Common.h` L652~654 | 삭제 |

### 1.4 삭제 대상 통계/Snapshot 구조 멤버

| 항목 | 구조체 | 변경 내용 |
| :--- | :--- | :--- |
| `PacketStatistics::ch6` | `Common.h` L1057 | `TcpSocketStats ch6` → 삭제 후 신규 ch6 = 구 ch7로 대체 |
| `PktSnapshot::ch6` | `Common.h` L1133 | 동일 처리 |
| `ch6.reset()` | `PacketStatistics::reset()` | 수정 |

### 1.5 삭제 대상 Tracer/CLI 참조

| 항목 | 위치 | 내용 |
| :--- | :--- | :--- |
| `TelnetTracer::passesFilter` 내 `channel <= 6` | `TelnetCli.cpp` L25 | → `<= 5` 또는 실제 채널 수로 수정 |
| `entry.channel == 6` 분기 | `TelnetCli.cpp` L208, L220 | 삭제 또는 CH6→5로 이동 |
| `"CMD_CH6"` 태그 | `TelnetCli.cpp` L214 | 삭제 |
| `"CH6"` Tracer 채널 태그 | `CliCommands.cpp` L1042 | 삭제 |
| `CH5에서 스니핑된 CH6 패스스루` 주석/코드 | `TelnetCli.cpp` L219~257 | 동작 확인 후 정리 |
| `FormatNetworkStats` 내 `CH#6_Hub` 블록 | `Engine.cpp` L269~279 | 삭제 |
| `[보안 4.2] channel_id == 6` 분기 | `Engine.cpp` L779~829 | CH6 제어 패킷 인젝션 방어 로직 — 신규 CH6(구 CH7)에 맞게 수정 |
| `req.channel_id == 6` VIP 큐 분기 | `Engine.cpp` L829 | 삭제 (Hub에서 오는 제어 패킷 경로 없어짐) |
| `MgmtRpc.cpp` 내 `"ch6"` JSON 키 | `MgmtRpc.cpp` L417 | `"ch6"` → 삭제 또는 ch7→ch6 로 재매핑 |

---

## 2. CH7 → CH6 변경 (이름 변경만, 기능 동일)

### 2.1 변경 대상 심볼 매핑

| 변경 전 (CH7) | 변경 후 (CH6) | 위치 |
| :--- | :--- | :--- |
| `g_pkt_stats.ch7` | `g_pkt_stats.ch6` | 전체 |
| `pkt.ch7` (PktSnapshot) | `pkt.ch6` | 전체 |
| `PacketStatistics::ch7` | `PacketStatistics::ch6` | `Common.h` |
| `PktSnapshot::ch7` | `PktSnapshot::ch6` | `Common.h` |
| `ch7.reset()` | `ch6.reset()` | `Common.h` |
| `"CH7 실시간 ..."` 주석 | `"CH6 ..."` | `MgmtRpc.cpp` |
| `\"ch7\"` JSON 키 | `\"ch6\"` | `MgmtRpc.cpp` L418 |
| `ch7_rx`, `ch7_tx` 변수 | `ch6_rx`, `ch6_tx` | `MgmtRpc.cpp` L315~316 |
| `req.channel_id = 7` | `req.channel_id = 6` | `MgmtRpc.cpp` L1135 |
| `ch7.tx_pkts.fetch_add(...)` | `ch6.tx_pkts.fetch_add(...)` | `MgmtRpc.cpp` 전체 |
| `ch7.rx_pkts.fetch_add(...)` | `ch6.rx_pkts.fetch_add(...)` | `MgmtRpc.cpp` L1162 |
| `g_pkt_stats.ch7.is_connected` | `g_pkt_stats.ch6.is_connected` | `main.cpp` L1434 |
| `"CH7 \u0026 CH7 TCP server..."` | `"CH6 TCP server..."` | `main.cpp` L1179 |
| `"CH6 \u0026 CH7 TCP server..."` | `"CH6 TCP server..."` | `main.cpp` L1179 (구 CH6 삭제 후) |
| Tracer `channel_id == 7` | `channel_id == 6` | `TelnetCli.cpp` L238 |
| `"CH7/RPC"` 주석 | `"CH6/RPC"` | `TelnetCli.cpp` L238 |
| `FormatNetworkStats` 미존재 → 신규 추가 | `CH#6_Mgmt` 행 추가 | `Engine.cpp` |

### 2.2 `PacketStatistics` 구조체 변경

```cpp
// BEFORE
struct PacketStatistics {
  SingleChannelStats ch1, ch2, ch3, ch4;
  TcpSocketStats ch5;
  TcpSocketStats ch6;  // 구 Hub (삭제)
  TcpSocketStats ch7;  // 구 Mgmt → ch6으로 이동
  void reset() { ...; ch6.reset(); ch7.reset(); }
};

// AFTER
struct PacketStatistics {
  SingleChannelStats ch1, ch2, ch3, ch4;
  TcpSocketStats ch5;
  TcpSocketStats ch6;  // 신 CH6 = 구 CH7 Mgmt
  void reset() { ...; ch5.reset(); ch6.reset(); }
};
```

---

## 3. 힙 메모리 최적화 — 추가 발견 항목

### 3.1 `DpTaskArgs new` — 스택 기반으로 교체 가능성 낮음

**현황**: `MgmtRpc.cpp` L994 — `new DpTaskArgs{...}` 로 5바이트 구조체 힙 할당 후 xTaskCreate에 전달.

**분석**: xTaskCreate의 pvParameters는 태스크 생명주기 동안 유효해야 하므로 스택 할당 불가. 그러나 구조체가 5바이트(`uint8_t` 5개)로 매우 소형.

**대안**: 태스크에 struct 대신 `uintptr_t` 하나에 5바이트를 패킹하거나, `static` 구조체 풀 사용. 단, 태스크가 완료 전 재호출될 경우 재진입 문제 발생.

**결정**: 이 부분은 Cold Path (Management), 빈도 낮음, 5바이트 힙 할당으로 영향 무시 가능. **현행 유지.**

---

### 3.2 `AutoProbingEngine::analyzeCacheMatrix()` — STL 컨테이너 Cold Path 한정

**현황**: `WallpadParser.cpp` L658~1092에서 `std::vector<PktPair>`, `std::set<uint8_t>`, `std::map<uint8_t, uint8_t>` 등 다수 동적 할당.

**분석**: `analyzeCacheMatrix()`는 프로토콜 수렴(Lock) 완료 이전에만 호출되며, 수렴 후에는 `isLocked()` 체크로 조기 종료됨. 즉, **런타임 정상 동작 중에는 거의 호출되지 않는 Cold Path.**

**현황**: `pairs.reserve(64)` — `PktPair`(2×`StaticPacket` = 2×66B = 132B) × 64개 = **약 8.5KB 힙 피크**. 수렴 후에는 해제됨.

**결정**: 수렴 전 1회성 분석에서 순간 피크 8.5KB는 허용 범위 내. 프로파일 변경 시에만 재실행. **현행 유지.** 단, `reserve(64)` → 실제 최대 폴링 대상 수 기준으로 줄이는 것을 검토.

```cpp
// AFTER (개선안): 실측 폴링 대상 수 기반으로 reserve
size_t target_count = g_polling_targets.count();
pairs.reserve(std::min(target_count, size_t(32))); // 최대 32개로 제한
```

**커밋**: `optimize: cap AutoProbingEngine pair vector reserve to actual target count`

---

### 3.3 `xEventGroupCreate` 이중 초기화 버그 (찌꺼기)

**현황**: `main.cpp` 에서 `g_system_event_group`이 두 곳에서 생성됨:

```cpp
// main.cpp L245 (함수 외부, 전역 초기화 시점?)
g_system_event_group = xEventGroupCreate();

// main.cpp L2141 (Boot_InitSyncPrimitives 내부)
g_system_event_group = xEventGroupCreate();
```

**문제**: 첫 번째 생성으로 만들어진 EventGroup이 두 번째 호출에 의해 덮어씌워지면 첫 번째 핸들이 **메모리 누수**. FreeRTOS에서 EventGroup은 힙 할당이므로 vEventGroupDelete 없이 덮어쓰면 약 ~72B 힙 누수.

**수정 내용**:
```cpp
// BEFORE: main.cpp L245 부근 (전역/조기 초기화)
g_system_event_group = xEventGroupCreate(); // ← 중복 초기화, 삭제

// KEEP: main.cpp Boot_InitSyncPrimitives 내 (L2141)
if (!g_system_event_group)
    g_system_event_group = xEventGroupCreate();
```

**위험도**: 낮음
**커밋**: `fix: remove duplicate xEventGroupCreate causing heap leak`

---

### 3.4 `g_ch6_mutex` xSemaphoreCreate 이중 초기화 패턴 검토

**현황**: `Boot_InitSyncPrimitives` (L2133~2134):
```cpp
if (!g_ch6_mutex)
    g_ch6_mutex = xSemaphoreCreateMutex();
```
CH6 삭제 시 이 라인도 함께 제거. (`g_ch6_mutex` 전체 삭제)

---

### 3.5 `TokenBucket s_ch6_bucket` — static 객체 크기

**현황**: `main.cpp` L550~551 전역 `static TokenBucket s_ch6_bucket(32, 100)`. CH6 삭제 시 자동 제거됨.

**TokenBucket 구조체 크기**: `uint32_t _capacity` + `uint32_t _tokens` + `uint32_t _refill_ms` + `uint32_t _last_refill_ms` + `portMUX_TYPE _mux` ≈ **약 24B** 절약.

---

### 3.6 `TcpFragSession hub_sessions[3]` — static 배열 해제

`static TcpFragSession hub_sessions[Config::TCP::MAX_HUB_CLIENTS]` 삭제.

- `TcpFragSession` 크기: `int(4)` + `uint8_t[256]` + `size_t(4)` + `uint32_t(4)` = **268B**
- 3개: **804B** 정적 메모리 해제

---

### 3.7 `WallpadParser.cpp` — `#include <vector>` `<set>` `<map>` 제거 가능 여부

`analyzeCacheMatrix()`를 유지하는 한 `<vector>`, `<set>`, `<map>` 모두 필요. **삭제 불가.**

단, 향후 수렴이 완료된 뒤 `analyzeCacheMatrix()`를 완전 비활성화하는 컴파일 플래그(`#if ENABLE_AUTO_PROBING`)를 추가하면 이 헤더들도 제거 가능 — **향후 검토.**

---

## 4. 전체 메모리 절약 요약

| 항목 | 절약량 | 분류 |
| :--- | ---: | :--- |
| `g_ch6_to_tcp_storage[]` (StaticQueue 백킹 버퍼) | **~2,112B** | 정적 RAM |
| `hub_sessions[3]` | **~804B** | 정적 RAM |
| `StaticQueue_t g_ch6_to_tcp_queue_buf` | **~88B** | 정적 RAM |
| `TokenBucket s_ch6_bucket` | **~24B** | 정적 RAM |
| `SemaphoreHandle_t g_ch6_mutex` (포인터) | **4B** | 정적 RAM |
| xSemaphoreCreateMutex (FreeRTOS 힙) | **~80B** | 동적 RAM |
| xEventGroupCreate 중복 누수 수정 | **~72B** | 동적 RAM 누수 방지 |
| `AutoProbingEngine` vector reserve 최적화 | 피크 최대 **~4KB** 감소 | 동적 RAM 피크 |
| **정적 RAM 합계** | **~3,032B (~3.0KB)** | |
| **동적 RAM 합계** | **~152B + 피크 4KB** | |

---

## 5. 작업 순서 및 커밋 계획

> [!IMPORTANT]
> 삭제 → CH7→CH6 심볼 변경 → 통계 구조체 수정 → CLI/Tracer 수정 → 힙 최적화 순으로 진행.
> 각 단계마다 `pio run` 빌드 확인 필수. 삭제 커밋과 rename 커밋은 반드시 분리.

### Step 1: CH6 RAW 전송 관련 코드 삭제

```
commit A: remove CH6 Hub server (hub_server_fd, bind/listen/accept/poll block)
commit B: remove Ch6_SendAck_Direct, Ch6_SendAck, Ch6_Data functions
commit C: remove g_ch6_to_tcp_queue + g_ch6_to_tcp_storage + g_ch6_mutex + s_ch6_bucket
commit D: remove Ch6_SendAck calls in Engine.cpp Ch1_HandleCtrl (ch6_ack 변수도 제거)
commit E: remove Config::TCP::HUB_PORT, MAX_HUB_CLIENTS, CH6_TOKEN_* constants
commit F: remove hub_sessions[] static array declaration
```

**각 커밋 후 검증**: `pio run` 0 error, undefined reference 없음.

### Step 2: CH7 → CH6 심볼 일괄 변경

```
commit G: rename PacketStatistics::ch7 → ch6, PktSnapshot::ch7 → ch6 (Common.h)
commit H: rename MgmtRpc.cpp ch7 → ch6 (변수명, JSON 키, channel_id, 주석)
commit I: rename main.cpp ch7 → ch6 (is_connected 업데이트, Rescue 메시지)
commit J: rename TelnetCli.cpp CH7/RPC → CH6/RPC (Tracer 채널 참조)
commit K: add CH#6_Mgmt row to FormatNetworkStats in Engine.cpp
```

### Step 3: Tracer/CLI 정리

```
commit L: fix TelnetTracer channel range (ch <= 6 → ch <= 5 또는 유효 채널만)
          remove CMD_CH6 tag, remove CH6 sniffer comment blocks
commit M: remove CH#6_Hub row from FormatNetworkStats (already done in Step 1)
          remove ch6 from MgmtRpc telemetry JSON
```

### Step 4: 힙 최적화

```
commit N: fix duplicate xEventGroupCreate (g_system_event_group leak)
commit O: optimize AutoProbingEngine::analyzeCacheMatrix vector reserve cap
```

### Step 5: 최종 검증 (Phase 2 기준과 동일)

```bash
# 잔존 심볼 확인
grep -rn "ch6_to_tcp\|Ch6_SendAck\|Ch6_Data\|g_ch6_mutex\|s_ch6_bucket\|hub_sessions\|HUB_PORT\|CH6_TOKEN" src/ include/
# → 0 결과 기대 (단, 신규 CH6=구CH7 심볼은 존재)

grep -rn "\.ch7\|ch7\." src/ include/
# → 0 결과 기대

# 빌드
pio run --environment m5stack-atoms3 2>&1 | grep -E "error:|warning:"
```

---

## 6. 영향받는 SmartThings Edge Driver

> [!IMPORTANT]
> SmartThings Edge Driver (`Gateway-edge-driver/`) 에서 TCP 포트 및 채널 ID를 하드코딩하고 있을 경우 함께 변경 필요.

**확인 필요 항목**:
- 포트 `8899` 참조 → 삭제 또는 폐기
- 포트 `8900` 참조 → 유지 (기존 CH7 = 신 CH6)
- `channel_id == 6` JSON 필드 → 신 CH6(구 CH7)로 의미 변경됨, 드라이버가 채널 ID를 사용하는지 확인
- `channel_id == 7` → 삭제

```bash
grep -rn "8899\|8900\|channel_id.*6\|channel_id.*7\|\"ch6\"\|\"ch7\"" \
  Gateway-edge-driver/ 2>/dev/null | head -20
```

---

## 7. 체크리스트

### CH6 삭제 완료 기준
- [ ] 포트 8899 TCP 서버 소켓 미생성
- [ ] `Ch6_SendAck`, `Ch6_Data` 미존재
- [ ] `g_ch6_to_tcp_queue` 미존재
- [ ] `hub_sessions[]` 미존재
- [ ] `TokenBucket s_ch6_bucket` 미존재
- [ ] `Config::TCP::HUB_PORT`, `CH6_TOKEN_*` 미존재
- [ ] `PacketStatistics::ch6` = 구 ch7 (Mgmt) 통계로 교체됨

### CH7→CH6 변경 완료 기준
- [ ] `g_pkt_stats.ch7` 참조 0건
- [ ] `req.channel_id = 7` 참조 0건
- [ ] `"ch7"` JSON 키 0건
- [ ] `"CH7"` 주석/문자열 0건
- [ ] `FormatNetworkStats`에 `CH#6_Mgmt` 행 존재

### 힙 최적화 완료 기준
- [ ] `xEventGroupCreate` 이중 호출 제거 (`g_system_event_group`)
- [ ] Free heap Baseline 대비 **+3.0KB 이상** 증가
- [ ] `pio run` 0 error, 0 new warning
- [ ] WDT 24h 무발생
- [ ] SmartThings 장치 통신 정상

