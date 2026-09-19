# M5Stack AtomS3 Lite RS485 Gateway
## 리팩터링 마스터 계획서 v2.0

> **참조 문서**: `MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md`, `NAMING_CONVENTIONS.md`, `MODERN_CPP_GUIDELINES.md`

---

## 1. 현황 분석 (As-Is)

### 1.1 현재 파일 구조 및 규모

| 파일 | 라인 수 | 비고 |
| :--- | ---: | :--- |
| `src/main.cpp` | 2,421 | System/Boot/Network/Config 혼재 |
| `src/Engine.cpp` | 1,915 | Metrics/RS485 Engine/Task(CH1,2,3,4) 혼재 |
| `src/CliCommands.cpp` | 3,029 | 4개 namespace 분리됨 (Config/System/Wallpad/Wifi) |
| `src/TelnetCli.cpp` | 1,496 | Tracer/Auth/Wizard/Server/Lifecycle 혼재 |
| `src/WallpadParser.cpp` | 1,775 | 5개 블록 혼재 |
| `src/MgmtRpc.cpp` | 1,275 | OTA/Timing/Telemetry/RPC/Broadcast 혼재 |
| `src/ControlTemplate.cpp` | 1,697 | 고응집, 분리 대상 아님 |
| **합계** | **13,608** | (헤더 제외) |

### 1.2 현재 헤더 구조

| 파일 | 라인 수 | 주요 내용 |
| :--- | ---: | :--- |
| `include/Common.h` | 1,599 | 전체 공유 타입/전역/설정 상수/유틸 |
| `include/TelnetCli.h` | 283 | `TelnetManager`, `TelnetTracer`, `TelnetSession` 선언 |
| `include/WallpadParser.h` | 325 | Parser 관련 클래스 전체 선언 |
| `include/ControlTemplate.h` | 245 | ControlTemplate 클래스 선언 |
| `include/CliCommands.h` | 56 | CLI 등록 함수 선언 |
| `include/MgmtRpc.h` | 59 | Mgmt API 선언 |

### 1.3 현재 Task 구조 (불변)

| Task 이름 | Core | 우선순위 | 함수 | 경로 분류 |
| :--- | :---: | :---: | :--- | :--- |
| `CH#1_IoT` | 1 | 높음 | `Task_Ch1()` | **Hot Path** |
| `CH#2_WP#1` | 1 | 높음 | `Task_Ch2Ch3()` | Warm Path |
| `CH#3_WP#2` | 1 | 높음 | `Task_Ch2Ch3()` | Warm Path |
| `CH#4_WP#3` | 1 | 중간 | `Task_Ch4()` | Warm Path |
| `Network` | 0 | 중간 | `Task_Network()` | Warm Path |
| `Telnet_CLI` | 0 | 낮음 | `Task_Telnet()` | Cold Path |

> [!IMPORTANT]
> 절대 불변: Task 수, Core affinity, Priority, 함수 시그니처

### 1.4 현재 채널 구조

| 채널 | 포트 | 역할 | 방향 |
| :--- | :--- | :--- | :--- |
| CH1 | UART0 (RS485) | IoT 마스터 폴링/제어 | 양방향 |
| CH2/3 | UART1/2 (RS485) | 월패드 슬레이브 가상 응답 | 양방향 |
| CH4 | SoftwareSerial | 도어폰 통신 | 양방향 |
| CH5 | TCP 8898/8891~8894 | EW11 TCP 허브 클라이언트 연결 | 양방향 |
| **CH6 (삭제 대상)** | TCP 8899 | 허브 RAW 패킷 수신/송신 서버 | 양방향 |
| **CH7 (변경 대상)** | TCP 8900 | SmartThings JSON-RPC 관리 서버 | 양방향 |

---

## 2. 목표 구조 (To-Be)

### 2.1 목표 디렉터리 트리

```text
src/
├── Core/
│   ├── Common.h                ← 기존 include/Common.h 이동
│   ├── Config.h                ← Common.h에서 Config namespace 분리 [신규]
│   ├── Globals.cpp             ← main.cpp SECTION 1 (전역 변수/RTC/Atomic) [신규]
│   └── SystemHealth.cpp        ← main.cpp Boot 진단 함수들 [신규]
│
├── Engine/
│   ├── Engine.cpp              ← (기존) Task 생성/초기화/Glue 전용으로 슬림화
│   ├── UartRx.cpp              ← Engine.cpp 내 RX 경로 함수들 [신규]
│   ├── UartTx.cpp              ← Engine.cpp 내 TX 경로 함수들 [신규]
│   ├── Ch1Engine.cpp           ← Engine.cpp SECTION 2 DeviceRepository/Control [신규]
│   ├── Ch1Polling.cpp          ← Engine.cpp Ch1_PollNext/Ch1_HandleCtrl [신규]
│   └── Ch23Engine.cpp          ← Engine.cpp Task_Ch2Ch3/Task_Ch4 [신규]
│
├── Parser/                     ← WallpadParser.cpp 분리 [신규 디렉터리]
│   ├── PollingTargetRegistry.cpp   ← WallpadParser BLOCK 1
│   ├── AutoProbingEngine.cpp       ← WallpadParser BLOCK 2
│   ├── ProfileRepository.cpp       ← WallpadParser BLOCK 3
│   └── UniversalProtocolEngine.cpp ← WallpadParser BLOCK 4+5
│
├── Network/
│   ├── HubManager.cpp          ← main.cpp Ew11 관련 함수들 [신규]
│   └── NetworkManager.cpp      ← main.cpp Task_Network + WiFi 이벤트 [신규]
│
├── Control/
│   ├── ControlTemplate.cpp     ← 기존 유지 (응집도 높음, 분리 안 함)
│   └── ControlRegistry.cpp     ← Engine.cpp DeviceRouteRegistry+ControlDispatcher [신규]
│
├── Telnet/
│   ├── TelnetTracer.cpp        ← TelnetCli.cpp SECTION 1 [신규]
│   ├── TelnetServer.cpp        ← TelnetCli.cpp SECTION 3 Server/Session lifecycle [신규]
│   ├── TelnetAuth.cpp          ← TelnetCli.cpp handlePassword/Tcp_ConstantTimeStrcmp [신규]
│   ├── TelnetWizard.cpp        ← TelnetCli.cpp handleWizardInput/StepAdvance [신규]
│   └── TelnetCli.cpp           ← (기존) CLI 공통 프레임워크/bindCommands 등 잔류
│
├── CLI/
│   ├── CliCommands.cpp         ← (기존) 등록/디스패치/공통 헬퍼만 잔류
│   ├── CliConfig.cpp           ← CliCommands.cpp namespace ConfigCli [신규]
│   ├── CliSystem.cpp           ← CliCommands.cpp namespace SystemCli [신규]
│   ├── CliControl.cpp          ← CliCommands.cpp namespace WallpadCli [신규]
│   └── CliNetwork.cpp          ← CliCommands.cpp namespace WifiCli [신규]
│
├── Management/
│   ├── MgmtRpc.cpp             ← (기존) RPC dispatch + 세션 lifecycle만 잔류
│   ├── HttpOta.cpp             ← MgmtRpc.cpp Task_HttpOta + Mgmt_StartHttpOta [신규]
│   └── Telemetry.cpp           ← MgmtRpc.cpp Mgmt_SerializeTelemetry + Broadcast [신규]
│
└── main.cpp                    ← Boot_Start~setup() 글루 코드만 잔류
```

### 2.2 목표 채널 구조

| 채널 | 포트 | 역할 |
| :--- | :--- | :--- |
| CH1~5 | 동일 | 변경 없음 |
| **CH6 (신)** | TCP 8900 | 구 CH7. SmartThings JSON-RPC 관리 서버 |

> [!NOTE]
> TCP 포트 8899(구 CH6 Hub 서버)는 완전 폐쇄되며, 포트 8900(구 CH7 Mgmt 서버)은 **채널 번호만 CH6으로 변경**된다.

---

## 3. 사전 작업: CH6 삭제 + CH7→CH6 변경

이 작업은 모듈 분리 이전에 수행해야 불필요한 코드 이동을 막을 수 있다. 가장 먼저 수행하는 파괴적 변경이다.

### 3.1 삭제 대상 (CH6 RAW 전송 관련)
- **함수**: `Ch6_SendAck_Direct()`, `Ch6_SendAck()`, `Ch6_Data()`, `hub_server_fd` 초기화 블록, `hub_sessions` select 루프, Queue drain 루프 등
- **전역 변수/버퍼**: `g_ch6_to_tcp_queue_buf`, `g_ch6_to_tcp_storage[]`, `g_ch6_to_tcp_queue`, `g_ch6_mutex`, `s_ch6_bucket`, `hub_sessions[]`, `hub_server_fd`
- **상수**: `Config::TCP::HUB_PORT`, `MAX_HUB_CLIENTS`, `CH6_TOKEN_*`
- **구조체/호출부**: `PacketStatistics::ch6`, `PktSnapshot::ch6` 삭제, Tracer 내 `"CMD_CH6"` 태그 및 스니핑 패스스루 로직

### 3.2 CH7 → CH6 변경 (이름 변경)
- 구 CH7이었던 SmartThings JSON-RPC 서버 관련 변수(`g_pkt_stats.ch7`, `req.channel_id = 7`, `\"ch7\"` 등)를 모두 `ch6`으로 매핑.
- `PacketStatistics` 내부에서 기존 `ch6` 삭제 후 `ch7`을 `ch6`으로 변경.
- Tracer와 로그 문자열 내 `"CH7"` 문자열을 `"CH6"`으로 치환.

---

## 4. 모듈 분리 계획 (Phase 1)

모듈 분리는 파괴적 변경이 완료된 상태에서 순수하게 논리적 구조만 나누는 작업이다.  
**원칙**: 파일 이동은 100% Copy-only. 로직 변경 0%, `static` / 익명 namespace 속성 보존.

---

### 4.1 Phase 1-A: Telnet 도메인 — `src/TelnetCli.cpp` (1,496줄)

| 신규 파일 | 출처 라인 | 주요 함수/내용 |
| :--- | :--- | :--- |
| `Telnet/TelnetTracer.cpp` | L19~361 | `TelnetTracer::passesFilter()`, `trace()` (×2), `flushToClient()` / 내부 static: `s_ch1_tracker`, `s_wp_tracker[]` — 함수 내 static 유지 |
| `Telnet/TelnetAuth.cpp` | L532~689 | `Tcp_ConstantTimeStrcmp()`, `TelnetManager::handlePassword()` |
| `Telnet/TelnetWizard.cpp` | L714~1178 | `WizardTargetDef` (해당 .cpp 내 static), `handleWizardStepAdvance()`, `handleWizardInput()`, `notifyControlTransaction()` |
| `Telnet/TelnetServer.cpp` | L1179~1435 | `onClientConnect()`, `handleClientDisconnect()`, `shutdownForReboot()`, `startServer()`, `tick()`, `Task_Telnet()` |
| `Telnet/TelnetCli.cpp` (잔류) | L362~527, L690~709 | `sendTelnetMsg*()`, `writeCharToClient()`, `bindCommands()`, `onClientData()`, `sendScanResult()`, `cmdExit()` (~200줄) |

**헤더**: 기존 `include/TelnetCli.h` 유지. 공유 `_cli_mutex`는 어느 한 파일에서만 정의, 나머지는 extern.

커밋: `commit 11` split TelnetTracer.cpp / `commit 12` TelnetAuth.cpp / `commit 13` TelnetWizard.cpp / `commit 14` TelnetServer.cpp / `commit 15` slim TelnetCli.cpp

---

### 4.2 Phase 1-B: CLI 도메인 — `src/CliCommands.cpp` (3,029줄)

각 `namespace` 블록을 별도 `.cpp`로 이동. 별도 헤더 신설 없음.

| 신규 파일 | 출처 라인 | 주요 함수/내용 |
| :--- | :--- | :--- |
| `CLI/CliConfig.cpp` | L37~452 | `printConfig()`, `cmdConfig()`, `cmdSave()`, `cmdEw11()`, `cmdRoutes()` / `ConfigParamDef` → 해당 .cpp 내부 |
| `CLI/CliSystem.cpp` | L457~956 | `printSystemOverview()`, `printStats()`, `cmdStats()`, `cmdReboot()`, `cmdLogView()`, `cmdCoreDump()`, `otaPrintStatus()`, `cmdOta()`, `cmdHelp()` |
| `CLI/CliControl.cpp` | L957~2893 | `cmdTrace()`, `cmdStop()`, `cmdDevs()`, `wallpadPrintStatus()`, `wallpadListProfiles()`, `wallpadSave/Delete/SetProfile()`, `wallpadPrintControlTable/Detail()`, `wallpadControlLearnInteractive()`, `cmdWallpad()`, `cmdCtl()` / 내부 `static formatSources()` 유지 |
| `CLI/CliNetwork.cpp` | L2894~3029 | `AsyncWifiScanTask()` (static 유지), `cmdWifi()` |
| `CLI/CliCommands.cpp` (잔류) | L1~36 | `Cli_RegisterCommands()`, 공통 헬퍼 (~50줄) |

커밋: `commit 16~19`

---

### 4.3 Phase 1-C: Parser 도메인 — `src/WallpadParser.cpp` (1,775줄)

| 신규 파일 | 출처 라인 | 주요 내용 |
| :--- | :--- | :--- |
| `Parser/PollingTargetRegistry.cpp` | L11~290 | `PollingTargetRegistry::*`, 전역 `g_polling_targets` |
| `Parser/AutoProbingEngine.cpp` | L291~1124 | `AutoProbingEngine::*` 전체 (`analyzeCacheMatrix` 포함) / `#include <vector><set><map>` 이 파일에 집중 |
| `Parser/ProfileRepository.cpp` | L1125~1449 | `ProfileRepository::*`, `static s_profiles_initialized` |
| `Parser/UniversalProtocolEngine.cpp` | L1450~1775 | `UniversalProtocolEngine::*`, `WallpadParserFactory::*`, `g_auto_probing_engine` 정의 |

**헤더**: 기존 `include/WallpadParser.h` 유지. 커밋: `commit 20~23`

---

### 4.4 Phase 1-D: Management 도메인 — `src/MgmtRpc.cpp` (1,275줄)

| 신규 파일 | 출처 라인 | 주요 내용 |
| :--- | :--- | :--- |
| `Management/HttpOta.cpp` | L27~207 | `Task_HttpOta()` (static), `Mgmt_StartHttpOta()`, 전역 `g_http_ota_state` |
| `Management/Telemetry.cpp` | L267~623, L1195~1275 | `Mgmt_SerializeTelemetry()`, `Mgmt_SerializeLockedDevices()`, `Mgmt_Broadcast*()` 3종 |
| `Management/MgmtRpc.cpp` (잔류) | L16~22, L211~263, L660~1191 | 전역 인스턴스, `TimingConfig_*`, `Mgmt_Init()`, `Mgmt_DispatchJsonRpc()`, `Mgmt_Data()` (~700줄) |

커밋: `commit 24~25`

---

### 4.5 Phase 1-E: Engine 도메인 — `src/Engine.cpp` (1,915줄)

> [!IMPORTANT]
> Task 함수의 파일 이동만 수행. Task 재설계, 함수 시그니처 변경, 호출 경로 변경 없음.

| 신규 파일 | 출처 라인 | 주요 내용 |
| :--- | :--- | :--- |
| `Control/ControlRegistry.cpp` | L690~1043 | `DeviceRouteRegistry::*`, `ControlDispatcher::*` |
| `Engine/Ch1Engine.cpp` | L436~689, L1044~1160, L1329~1348 | `DeviceRepository::*`, `Ch1_BuildQueryPacket()`, `Ch1_HandleCtrl()` (static), `Ch1_SetState()` (static), `calculateChecksum()` |
| `Engine/Ch1Polling.cpp` | L1161~1328, L1353~1498 | `Ch1_PollNext()` (static), `Task_Ch1()` |
| `Engine/Ch23Engine.cpp` | L1499~1915 | `Task_Ch2Ch3()`, `Task_Ch4()` |
| `Engine/UartRx.cpp` | L866~1042 | `Uart_RecvPacket()` (static) — **Phase 4 최적화 대상** |
| `Engine/Engine.cpp` (잔류) | L17~352 | `SystemMetricsTracker::*`, `FormatHwMetrics()`, `FormatNetworkStats()`, `FormatRs485Stats()`, `FormatTaskStacks()` (~350줄) |

커밋: `commit 26~30`

---

### 4.6 Phase 1-F: main.cpp 슬림화 — `src/main.cpp` (2,421줄)

> [!CAUTION]
> 전역 변수 정의 위치 변경 작업. `RTC_NOINIT_ATTR` 변수 이동 후 `.map` 파일 비교 필수. **`Core/Globals.cpp` 는 마지막에 처리.**

| 신규 파일 | 출처 라인 | 주요 내용 |
| :--- | :--- | :--- |
| `Core/SystemHealth.cpp` | L44~273 | `System_DiagnoseStuck()`, `System_LogResetReason()`, `System_CheckCoreDump()`, `System_CheckOtaHealth()`, `System_EnterRescueMode()` |
| `Network/HubManager.cpp` | L482~709, L1578~1711 | `TcpFragSession` 구조체, `Tcp_CloseAllSessions/PollAndReceive/AcceptAndAssignSlot()`, `Hub_AcceptClient()`, `Hub_Data()`, `Hub_ProcessPacket()` (static), `Hub_LoadConfig()`, `Hub_SaveConfig()`, `Hub_SetSlot()`, `Hub_SendPacket()` |
| `Network/NetworkManager.cpp` | L419~481, L1084~1440 | `onWifiEvent()`, `Task_Network()` |
| `Core/Globals.cpp` | L18~35, L278~280, L394~566 | `RTC_NOINIT_ATTR` 전역, `std::atomic` 전역들, WDT/Task 핸들들, Queue 스토리지 버퍼 |
| `main.cpp` (잔류) | L1461~2421 | `Config_Load/Save/Reset()`, `System_TakeSnapshot()`, `LogManager::*`, `System_Restart()`, `Boot_*()` (static), `setup()`, `loop()` (~700줄) |

커밋: `commit 31~35`

---

### 4.7 전역 인스턴스 소유권 정의

| 전역 변수 | 현재 위치 | 이동 위치 | extern 선언 |
| :--- | :--- | :--- | :--- |
| `g_polling_targets` | `WallpadParser.cpp` L15 | `Parser/PollingTargetRegistry.cpp` | `WallpadParser.h` |
| `g_auto_probing_engine` | `WallpadParser.cpp` | `Parser/UniversalProtocolEngine.cpp` | `WallpadParser.h` |
| `g_telnet_manager` | `TelnetCli.cpp` | `Telnet/TelnetCli.cpp` (잔류) | `TelnetCli.h` |
| `g_mgmt_sessions[]` | `MgmtRpc.cpp` L18 | `Management/MgmtRpc.cpp` (잔류) | `MgmtRpc.h` |
| `g_http_ota_state` | `MgmtRpc.cpp` L17 | `Management/HttpOta.cpp` | `MgmtRpc.h` |
| `g_wdt_monitor` | `main.cpp` L394 | `Core/Globals.cpp` | `Common.h` |
| `g_rescue_mode` | `main.cpp` L31 | `Core/Globals.cpp` | `Common.h` |
| `rtc_warm_cache` | `main.cpp` L29 | `Core/Globals.cpp` | `Common.h` |
| `g_pkt_stats` | `Common.h` (전역) | 변경 없음 | `Common.h` |

> [!NOTE]
> 헤더는 인터페이스 제공 목적으로만 유지. 파일 분리를 위해 기계적으로 헤더를 신설하지 않는다. 기존 6개 헤더를 최대한 그대로 유지하고 `extern` 선언만 필요 시 추가한다.

---

### 4.8 최종 파일 수 예측

| 디렉터리 | 현재 | 목표 |
| :--- | :---: | :---: |
| `src/` (루트) | 7 `.cpp` | 1 (`main.cpp`) |
| `src/Core/` | 0 | 2 `.cpp` |
| `src/Engine/` | 0 | 6 `.cpp` |
| `src/Parser/` | 0 | 4 `.cpp` |
| `src/Network/` | 0 | 2 `.cpp` |
| `src/Control/` | 0 | 2 `.cpp` |
| `src/Telnet/` | 0 | 5 `.cpp` |
| `src/CLI/` | 0 | 5 `.cpp` |
| `src/Management/` | 0 | 3 `.cpp` |
| **합계** | **7** | **30 `.cpp`** |

### 4.9 PlatformIO 설정

```bash
# 신규 디렉터리 생성
cd src && mkdir -p Core Engine Parser Network Control Telnet CLI Management
```

`platformio.ini`의 `src_filter`에 신규 디렉터리 경로 명시:
```ini
src_filter =
    +<*.cpp>
    +<Core/*.cpp>
    +<Engine/*.cpp>
    +<Parser/*.cpp>
    +<Network/*.cpp>
    +<Control/*.cpp>
    +<Telnet/*.cpp>
    +<CLI/*.cpp>
    +<Management/*.cpp>
```

---

## 5. 검증 (Phase 2)

모듈 분리가 올바르게 수행되었는지 런타임/빌드 검증.

- [ ] `pio run` 에러 및 신규 워닝 없음
- [ ] UART 통신 및 TCP 소켓 정상 동작
- [ ] ODR(중복 정의) 빌드 에러 없음
- [ ] 이동된 함수의 `static` / 익명 namespace 속성 보존
- [ ] **Delta = 0**: 기능상 어떠한 변화도 없어야 함.

---

## 6. 명명 변경 계획 (Phase 3)

명명 변경은 최적화 이전에 진행하며, 어떠한 동작 변경도 수반해서는 안 된다.

| 변경 전 | 변경 후 | 비고 |
| :--- | :--- | :--- |
| `FormatXxx()` 전역 | `namespace Fmt { }` 내부 | 선언/구현 위치 통일 |
| `WarmCache_` 접두사 | `Cache_` 접두사 | 표준 도메인 접두사 적용 |
| `Ew11_` 접두사 | `Hub_` 접두사 | 하드웨어 종속성 제거 (함수, 타입, 변수 모두) |
| `Boot_` 접두사 | 유지 | `static` 함수로 규칙서 참고만 추가 |

*작업 후 `NAMING_CONVENTIONS.md` 문서 업데이트.*

---

## 7. 최적화 계획 (Phase 4~9)

**최적화 원칙**: 안정성(1~4순위) > 최적화(5순위). 낭비되는 요소(Lock 타임, 중복 룩업 등) 위주로 제거.

### 7.1 Hot Path: UART RX/TX (Phase 3)
- `Uart_RecvPacket` 내 매번 호출되는 `getActiveParser()`를 루프 바깥/진입점에서 1회 호출하여 인자로 캐싱.
- 버퍼 매직 넘버(`64`, `128`)를 `Config::Protocol` 상수로 교체.
- TX `uart_wait_tx_done` 대기 시간을 20ms에서 실측 기반 상수로 관리(`Config::Timing::UART_TX_DONE_TIMEOUT_MS`).

### 7.2 반복 Lookup 제거 (Phase 4)
- `Ch1_PollNext` 내 동일 대상에 대한 `g_device_repo.find()` 다중 호출을 1회 호출 후 재사용으로 개선.
- Tier 1/2 분기 내 중복 발생하는 `memcpy` (tgt.raw_query_data)를 조건 분기 밖으로 추출(Hoisting).

### 7.3 Memory Path (Phase 5)
- Hot Path 내 중간 버퍼(poll_raw_data)를 제거하고 직접 복사하여 `memcpy` 1회 단축.
- *참고: `DeviceRepository::updateFromBus` 내 14바이트 `memcpy`는 성능 영향 미미하므로 유지.*

### 7.4 Lock Hold Time (Phase 6)
- `TelnetTracer::flushToClient` 내 소켓 I/O(send)가 Mutex 보호 구역 내에 있다면, 버퍼 추출부까지만 Lock을 걸고 I/O는 Lock 밖에서 수행.
- `DeviceRepository::updateFromBus` 등 무거운 콜백/Broadcast 가 락 안에 들어있지 않은지 점검.

### 7.5 Logging 경로 격리 (Phase 7)
- Hot Path에서 `g_telnet_tracer.trace()`가 동기적 소켓 I/O를 발생시키지 않도록 버퍼 큐잉 방식인지 재확인 (필요시 비동기 구조로 개선).

### 7.6 Management (Phase 8)
- TCP frag session 등의 할당 버퍼 크기를 상수로 교체 (`Config::TCP::FRAG_BUF_SIZE`).

---

## 8. 힙 메모리 최적화

불필요한 구조체와 메모리 누수 요소를 제거한다.

### 8.1 주요 작업
1. **`xEventGroupCreate` 누수 수정**: `main.cpp` 최상단과 `Boot_InitSyncPrimitives` 양쪽에서 두 번 호출되어 발생하는 `g_system_event_group` 누수(~72B) 방지.
2. **`AutoProbingEngine` 벡터 Reserve 축소**: `analyzeCacheMatrix()`에서 과도하게 고정 할당된 `reserve(64)`를 `std::min(g_polling_targets.count(), 32)` 형태로 최적화.

### 8.2 통합 메모리 절약 표

| 제거 항목 | 절약량 | 분류 | 비고 |
| :--- | ---: | :--- | :--- |
| `g_ch6_to_tcp_storage[]` | **~2,112B** | 정적 RAM | CH6 Queue 백킹 버퍼 |
| `hub_sessions[3]` | **~804B** | 정적 RAM | CH6 Hub 클라이언트 배열 |
| `g_ch6_to_tcp_queue_buf` | **~88B** | 정적 RAM | Queue 제어 블록 |
| `TokenBucket s_ch6_bucket` | **~24B** | 정적 RAM | |
| `g_ch6_mutex` 포인터 | **4B** | 정적 RAM | |
| `xSemaphoreCreateMutex` | **~80B** | 동적 RAM | CH6 Mutex 힙 |
| `xEventGroupCreate` 중복 | **~72B** | 동적 누수 방지 | |
| `vector reserve` 축소 | 최대 **~4KB** | 동적 RAM 피크 | 프로빙 시 순간 피크 억제 |
| **정적 RAM 총합** | **~3,032B (3.0KB)** | | |
| **동적 RAM 총합** | **~152B + 피크 4KB** | | |

---

## 9. 전체 커밋 맵

전체 작업은 선형적으로 진행되며 아래와 같이 커밋을 구성한다.

| 번호 | 레이블 | 설명 |
| ---: | :--- | :--- |
| `commit 00` | `[docs]` | Baseline snapshot 및 상태 기록 |
| **사전 작업** | | |
| `commit 01` | `[remove]` | remove CH6 Hub server (bind/listen/accept/poll block) |
| `commit 02` | `[remove]` | remove Ch6_SendAck_Direct, Ch6_SendAck, Ch6_Data functions |
| `commit 03` | `[remove]` | remove g_ch6_to_tcp_queue + storage + g_ch6_mutex + bucket |
| `commit 04` | `[remove]` | remove Ch6_SendAck calls in Engine.cpp Ch1_HandleCtrl |
| `commit 05` | `[remove]` | remove Config::TCP::HUB_PORT, MAX_HUB_CLIENTS, hub_sessions[] |
| `commit 06` | `[rename]` | rename PacketStatistics/PktSnapshot ch7 → ch6 |
| `commit 07` | `[rename]` | rename MgmtRpc.cpp and main.cpp ch7 → ch6 (keys, variables) |
| `commit 08` | `[rename]` | fix TelnetTracer channel range and FormatNetworkStats for CH6_Mgmt |
| `commit 09` | `[fix]` | remove duplicate xEventGroupCreate (g_system_event_group leak) |
| `commit 10` | `[optimize]` | optimize AutoProbingEngine::analyzeCacheMatrix vector reserve cap |
| **Phase 1: 파일 분리** | | |
| `commit 11` | `[split]` | split TelnetTracer.cpp |
| `commit 12` | `[split]` | split TelnetAuth.cpp |
| `commit 13` | `[split]` | split TelnetWizard.cpp |
| `commit 14` | `[split]` | split TelnetServer.cpp |
| `commit 15` | `[split]` | slim TelnetCli.cpp |
| `commit 16` | `[split]` | split CliConfig.cpp |
| `commit 17` | `[split]` | split CliSystem.cpp |
| `commit 18` | `[split]` | split CliControl.cpp |
| `commit 19` | `[split]` | split CliNetwork.cpp |
| `commit 20` | `[split]` | split Parser/PollingTargetRegistry.cpp |
| `commit 21` | `[split]` | split Parser/AutoProbingEngine.cpp |
| `commit 22` | `[split]` | split Parser/ProfileRepository.cpp |
| `commit 23` | `[split]` | split Parser/UniversalProtocolEngine.cpp |
| `commit 24` | `[split]` | split Management/HttpOta.cpp |
| `commit 25` | `[split]` | split Management/Telemetry.cpp |
| `commit 26` | `[split]` | split Control/ControlRegistry.cpp |
| `commit 27` | `[split]` | split Engine/Ch1Engine.cpp |
| `commit 28` | `[split]` | split Engine/Ch1Polling.cpp |
| `commit 29` | `[split]` | split Engine/Ch23Engine.cpp |
| `commit 30` | `[split]` | split Engine/UartRx.cpp + UartTx.cpp |
| `commit 31` | `[split]` | split Core/SystemHealth.cpp |
| `commit 32` | `[split]` | split Network/NetworkManager.cpp |
| `commit 33` | `[split]` | split Network/HubManager.cpp |
| `commit 34` | `[split]` | split Core/Globals.cpp (global var relocation) |
| `commit 35` | `[split]` | slim main.cpp |
| **Phase 2: 검증** | | |
| `commit 36` | `[docs]` | Phase 2 verification pass (no delta) |
| **Phase 3: 명명 변경** | | |
| `commit 37` | `[rename]` | move FormatXxx() into namespace Fmt |
| `commit 38` | `[rename]` | WarmCache_ → Cache_ prefix |
| `commit 39` | `[rename]` | Ew11 → Hub (type, function, global) |
| `commit 40` | `[docs]` | update NAMING_CONVENTIONS.md (Cache_, Hub_, Boot_) |
| **Phase 4~9: 최적화** | | |
| `commit 41` | `[optimize]` | cache parser pointer in Uart_RecvPacket |
| `commit 42` | `[optimize]` | replace Uart buffer magic numbers with Config constants |
| `commit 43` | `[optimize]` | replace uart_wait_tx_done magic value with Config constant |
| `commit 44` | `[optimize]` | eliminate redundant find() + hoist memcpy in Ch1_PollNext |
| `commit 45` | `[optimize]` | remove intermediate poll_raw_data buffer in Ch1_PollNext |
| `commit 46` | `[optimize]` | cache parser pointer in Ch1_HandleCtrl |
| `commit 47` | `[optimize]` | narrow lock scope in TelnetTracer::flushToClient |
| `commit 48` | `[optimize]` | narrow lock scope in DeviceRepository::updateFromBus |
| `commit 49` | `[optimize]` | decouple Hot Path trace from socket I/O in TelnetTracer |
| `commit 50` | `[optimize]` | replace TCP frag session magic buffer sizes with Config constants |

---

## 10. 검증 체크리스트

각 주요 Phase 완료 후, 그리고 최종 작업 후 아래 항목을 체크한다.

### 코드 레벨 검증
- [ ] `grep -rn "ch6_to_tcp\|Ch6_SendAck\|Ch6_Data\|g_ch6_mutex\|HUB_PORT" src/ include/` → **0건** 기대
- [ ] `grep -rn "\.ch7\|ch7\." src/ include/` → **0건** 기대
- [ ] `grep -rn "WarmCache_\|Ew11_\|Ew11Client\|g_ew11_slots" src/ include/` → **0건** 기대
- [ ] `xEventGroupCreate` 중복 호출 여부 점검

### 시스템 동작 검증
- [ ] `pio run` 0 error, 0 new warning
- [ ] UART 패킷 TX/RX 동일성 (Packet format, checksum)
- [ ] RS485 Polling 주기가 Baseline 대비 ±5% 이내 또는 개선
- [ ] Telnet 접속 및 CLI 명령 응답 정상
- [ ] WDT 리셋 24h 무발생
- [ ] Free heap Size가 Baseline 대비 **+3.0KB 이상 증가**
- [ ] 6개 Task stack watermark가 Baseline 대비 유지 또는 개선

---

## 11. 위험 관리

| 위험 요소 | 가능성 | 영향 | 대응 방안 |
| :--- | :---: | :---: | :--- |
| ODR (중복 정의) 빌드 에러 | 높음 | 빌드 불가 | 전역 변수 이동 시 `extern` 선언 정밀 체크 |
| `static` 함수 노출로 인한 링크 에러 | 중간 | 빌드 불가 | 내부 static 함수는 해당 `.cpp` 내 익명 namespace에 유지 |
| 파일 이동 중 기능 회귀 | 낮음 | 버그 | Phase 1 모듈 분리는 "Copy-only" 100% 로직 무변경 원칙 고수 |
| 최적화로 인한 타이밍 붕괴 | 낮음 | 통신 장애 | UART_TX 등의 타이밍 상수는 실측값 검증 후 적용 |

---

## 12. Edge Driver 연동 확인

SmartThings Edge Driver (`Gateway-edge-driver/`) 에서 삭제된 CH6 및 변경된 CH7에 대한 하드코딩 값 의존성을 제거한다.

- 포트 `8899` (구 CH6 Hub) 통신 로직 제거
- 포트 `8900` (신 CH6 Mgmt) 참조는 유지
- JSON Payload 내 `channel_id == 6` 수신부가 과거 Hub 데이터가 아닌 RPC Mgmt 데이터로 처리되도록 드라이버 코드 업데이트
- JSON Payload 내 `channel_id == 7` 처리 로직 삭제

```bash
# 드라이버 코드 내 하드코딩 검증을 위한 커맨드
grep -rn "8899\|8900\|channel_id.*6\|channel_id.*7\|\"ch6\"\|\"ch7\"" Gateway-edge-driver/
```
