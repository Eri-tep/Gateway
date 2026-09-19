# M5Stack AtomS3 Lite RS485 Gateway
## 모듈 분리 리팩터링 세부 계획서 v2.0

> **참조 문서**: [MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md](MODULARIZATION_AND_OPTIMIZATION_SPEC_V2.md)

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

> **절대 불변**: Task 수, Core affinity, Priority, 함수 시그니처

---

## 2. 목표 구조 (To-Be)

```
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
│   ├── PollingTargetRegistry.cpp   ← WallpadParser BLOCK 1 (L11~290)
│   ├── AutoProbingEngine.cpp       ← WallpadParser BLOCK 2 (L291~1124)
│   ├── ProfileRepository.cpp       ← WallpadParser BLOCK 3 (L1125~1449)
│   └── UniversalProtocolEngine.cpp ← WallpadParser BLOCK 4+5 (L1450~1775)
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
│   └── TelnetCli.cpp           ← (기존) CLI 공통 프레임워크/bindCommands/writeCharToClient만 잔류
│
├── CLI/
│   ├── CliCommands.cpp         ← (기존) 등록/디스패치/공통 헬퍼만 잔류
│   ├── CliConfig.cpp           ← CliCommands.cpp namespace ConfigCli (L37~452) [신규]
│   ├── CliSystem.cpp           ← CliCommands.cpp namespace SystemCli (L457~956) [신규]
│   ├── CliControl.cpp          ← CliCommands.cpp namespace WallpadCli (L957~2893) [신규]
│   └── CliNetwork.cpp          ← CliCommands.cpp namespace WifiCli (L2894~3029) [신규]
│
├── Management/
│   ├── MgmtRpc.cpp             ← (기존) RPC dispatch + 세션 lifecycle만 잔류
│   ├── HttpOta.cpp             ← MgmtRpc.cpp Task_HttpOta + Mgmt_StartHttpOta [신규]
│   └── Telemetry.cpp           ← MgmtRpc.cpp Mgmt_SerializeTelemetry + Broadcast [신규]
│
└── main.cpp                    ← Boot_Start~setup() 글루 코드만 잔류
```

---

## 3. 파일별 상세 분리 계획

### 3.1 Phase 1-A: Telnet 도메인 분리 (위험도: 낮음)

**원본**: [`src/TelnetCli.cpp`](../src/TelnetCli.cpp) (1,496줄)
**헤더**: `TelnetManager`, `TelnetTracer` 선언은 기존 [`include/TelnetCli.h`](../include/TelnetCli.h)에 유지

#### `src/Telnet/TelnetTracer.cpp` [신규]
- **출처**: `TelnetCli.cpp` SECTION 1 (L19~361)
- **포함 내용**:
  - `TelnetTracer::passesFilter()`
  - `TelnetTracer::trace()` (2 오버로드)
  - `TelnetTracer::flushToClient()`
  - 내부 static 변수: `s_ch1_tracker`, `s_wp_tracker[]` — **함수 내부 static 유지**
- **헤더 변경**: 없음 (기존 `TelnetCli.h` 내 선언 유지)
- **주의**: Hot Path에서 호출되는 trace 이벤트 생성부 — 분리 후 인라인화 가능성 검토

#### `src/Telnet/TelnetAuth.cpp` [신규]
- **출처**: `TelnetCli.cpp` SECTION 3 앞부분
  - `Tcp_ConstantTimeStrcmp()` (L532~588)
  - `TelnetManager::handlePassword()` (L589~689)
- **헤더 변경**: 없음

#### `src/Telnet/TelnetWizard.cpp` [신규]
- **출처**: `TelnetCli.cpp` SECTION 4
  - `struct WizardTargetDef` (L714~733) — 해당 `.cpp` 내 `static`으로 유지
  - `TelnetManager::handleWizardStepAdvance()` (L734~811)
  - `TelnetManager::handleWizardInput()` (L812~872)
  - `TelnetManager::notifyControlTransaction()` (L873~1178)
- **헤더 변경**: 없음

#### `src/Telnet/TelnetServer.cpp` [신규]
- **출처**: `TelnetCli.cpp` SECTION 3 후반
  - `TelnetManager::onClientConnect()` (L1179~1241)
  - `TelnetManager::handleClientDisconnect()` (L1242~1252)
  - `TelnetManager::shutdownForReboot()` (L1253~1265)
  - `TelnetManager::startServer()` (L1266~1293)
  - `TelnetManager::tick()` (L1294~1406)
  - `Task_Telnet()` (L1407~1435)
- **헤더 변경**: 없음

#### `src/Telnet/TelnetCli.cpp` [슬림화]
- **잔류 내용**:
  - SECTION 2 전체: `sendTelnetMsg*()` 헬퍼 (L362~409)
  - `TelnetManager::writeCharToClient()` (L410~425)
  - `TelnetManager::bindCommands()` (L426~465)
  - `TelnetManager::onClientData()` (L466~527)
  - `TelnetManager::sendScanResult()` (L690~701)
  - `TelnetManager::cmdExit()` (L702~709)
- **예상 잔류 라인**: ~200줄

---

### 3.2 Phase 1-B: CLI 도메인 분리 (위험도: 낮음~중간)

**원본**: [`src/CliCommands.cpp`](../src/CliCommands.cpp) (3,029줄)
**헤더**: 기존 [`include/CliCommands.h`](../include/CliCommands.h) 유지 (등록 함수 선언만 존재)

각 namespace는 자체 `.cpp` 파일에 `namespace XXXCli { ... }` 블록 그대로 이동. 별도 헤더 신설 없음.

#### `src/CLI/CliConfig.cpp` [신규]
- **출처**: `namespace ConfigCli { ... }` (L37~452, ~415줄)
- **포함**:
  - `printConfig()`, `setConfig()`, `cmdConfig()`, `cmdSave()`, `cmdEw11()`, `cmdRoutes()`
  - `struct ConfigParamDef` — 해당 `.cpp` 내부로 이동

#### `src/CLI/CliSystem.cpp` [신규]
- **출처**: `namespace SystemCli { ... }` (L457~956, ~500줄)
- **포함**:
  - `printSystemOverview()`, `printStats()`, `cmdStats()`, `cmdReboot()`
  - `cmdLogView()`, `cmdCoreDump()`
  - `otaPrintStatus()`, `otaTriggerRollback()`, `otaValidate()`, `cmdOta()`
  - `cmdHelp()`

#### `src/CLI/CliControl.cpp` [신규]
- **출처**: `namespace WallpadCli { ... }` (L957~2893, ~1,936줄, 최대 분리 효과)
- **포함**:
  - `cmdTrace()`, `cmdStop()`, `cmdDevs()`, `wallpadPrintStatus()`
  - `wallpadListProfiles()`, `wallpadSaveProfile()`, `wallpadDeleteProfile()`, `wallpadSetProfile()`
  - `wallpadPrintControlTable()`, `wallpadPrintControlDetail()`
  - `wallpadControlLearnInteractive()`, `wallpadControlAbort()`, `wallpadControlReset()`
  - `cmdWallpad()`, `cmdCtl()`
- **주의**: `ControlTemplate.h`, `WallpadParser.h` 인클루드 필요 — 기존대로 유지

#### `src/CLI/CliNetwork.cpp` [신규]
- **출처**: `namespace WifiCli { ... }` (L2894~3029, ~135줄)
- **포함**:
  - `AsyncWifiScanTask()` — 이 파일 내 `static`으로 유지
  - `cmdWifi()`

#### `src/CLI/CliCommands.cpp` [슬림화]
- **잔류 내용**: 등록 함수 `Cli_RegisterCommands()`, 공통 헬퍼 (파일 상단 전처리 블록 L1~36)
- **예상 잔류 라인**: ~50줄

---

### 3.3 Phase 1-C: Parser 도메인 분리 (위험도: 낮음)

**원본**: [`src/WallpadParser.cpp`](../src/WallpadParser.cpp) (1,775줄)
**헤더**: 기존 [`include/WallpadParser.h`](../include/WallpadParser.h) 유지

> [!NOTE]
> WallpadParser.cpp는 이미 5개 블록으로 내부 구분되어 있어 분리 경계가 명확하다. 각 클래스의 멤버 함수를 각 파일로 이동하며 헤더는 그대로 유지한다.

#### `src/Parser/PollingTargetRegistry.cpp` [신규]
- **출처**: BLOCK 1 "1ST TIER CACHE: POLLING TARGET REGISTRY" (L11~290, ~280줄)
- **포함**: `PollingTargetRegistry::*` 전체 멤버함수, 전역 `g_polling_targets`

#### `src/Parser/AutoProbingEngine.cpp` [신규]
- **출처**: BLOCK 2 "UNIVERSAL AUTO-PROBING PROTOCOL ENGINE" (L291~1124, ~834줄)
- **포함**: `AutoProbingEngine::*` 전체 멤버함수

#### `src/Parser/ProfileRepository.cpp` [신규]
- **출처**: BLOCK 3 "PROFILE REPOSITORY" (L1125~1449, ~325줄)
- **포함**: `ProfileRepository::*` 전체 멤버함수, `static s_profiles_initialized`

#### `src/Parser/UniversalProtocolEngine.cpp` [신규]
- **출처**: BLOCK 4+5 (L1450~1775, ~325줄)
  - "DATA-DRIVEN UNIVERSAL PROTOCOL ENGINE IMPLEMENTATION": `UniversalProtocolEngine::*`
  - "WALLPAD PARSER FACTORY": `WallpadParserFactory::*`
- **전역**: `g_auto_probing_engine` 등 전역 인스턴스 → 어느 한 파일에 집약 (WallpadParser.h 선언 유지)

---

### 3.4 Phase 1-D: Management 도메인 분리 (위험도: 낮음)

**원본**: [`src/MgmtRpc.cpp`](../src/MgmtRpc.cpp) (1,275줄)
**헤더**: 기존 [`include/MgmtRpc.h`](../include/MgmtRpc.h) 유지

#### `src/Management/HttpOta.cpp` [신규]
- **출처**: MgmtRpc.cpp 앞부분
  - `Task_HttpOta()` (L27~185) — `static` 유지
  - `Mgmt_StartHttpOta()` (L186~207)
- **전역**: `g_http_ota_state` 이 파일로 이동, `MgmtRpc.h` 또는 `Common.h` extern 선언
- **인클루드**: `HTTPClient.h`, `Update.h`, `WiFiClientSecure.h`만 여기서 포함

#### `src/Management/Telemetry.cpp` [신규]
- **출처**:
  - `Mgmt_SerializeTelemetry()` (L267~480)
  - `Mgmt_SerializeLockedDevices()` (L484~623)
  - `Mgmt_BroadcastDoorphoneEvent()` (L1195~1210)
  - `Mgmt_BroadcastDeviceState()` (L1214~1257)
  - `Mgmt_BroadcastDevicesUpdated()` (L1261~1275)

#### `src/Management/MgmtRpc.cpp` [슬림화]
- **잔류 내용**:
  - 전역 인스턴스 및 세션 풀 (L16~22)
  - `TimingConfig_Load/Save()` (L211~251)
  - `Mgmt_Init()` (L252~263)
  - `Mgmt_DispatchJsonRpc()` (L660~1155) — RPC 핵심 로직
  - `Mgmt_Data()` (L1159~1191)
- **예상 잔류 라인**: ~700줄

---

### 3.5 Phase 1-E: Engine/Network 도메인 분리 (위험도: 중간)

**원본**: [`src/Engine.cpp`](../src/Engine.cpp) (1,915줄)

> [!IMPORTANT]
> Engine.cpp 분리는 Task 함수 자체를 파일 이동하는 작업이다. Task 재설계가 아니라 구현 코드를 파일로 분리하는 것이며, 함수 시그니처·호출 경로·Task 핸들은 완전히 동일하게 유지한다.

#### `src/Engine/Ch1Engine.cpp` [신규]
- **출처**: Engine.cpp SECTION 2 일부
  - `DeviceRepository::*` 전체 멤버함수 (L436~671)
  - `Ch1_BuildQueryPacket()` (L679~689)
  - `Ch1_HandleCtrl()` (L1044~1160) — `static` 유지
  - `Ch1_SetState()` (L1329~1348) — `static` 유지
  - `calculateChecksum()` (L672~678)

#### `src/Engine/Ch1Polling.cpp` [신규]
- **출처**:
  - `Ch1_PollNext()` (L1161~1328) — `static` 유지
  - `Task_Ch1()` (L1353~1498)
- **주의**: `Task_Ch1` 선언은 `Common.h`에서 `extern` 유지, 파일만 이동

#### `src/Engine/Ch23Engine.cpp` [신규]
- **출처**:
  - `Task_Ch2Ch3()` (L1499~1621)
  - `Task_Ch4()` (L1622~1915)

#### `src/Engine/ControlRegistry.cpp` → `src/Control/ControlRegistry.cpp` [신규]
- **출처**: Engine.cpp SECTION 2 앞부분
  - `DeviceRouteRegistry::*` (L690~746)
  - `ControlDispatcher::*` (L747~1043)

#### `src/Engine/UartRx.cpp` [신규] — Phase 3 최적화 대상
- **출처**: Engine.cpp 내 RX 처리 함수들 (Ch1, Ch2/3 공통 수신 경로)
- **Phase 1 작업**: 파일 이동만 (로직 변경 없음)
- **Phase 3 작업**: Hot Path 최적화 적용

#### `src/Engine/UartTx.cpp` [신규] — Phase 3 최적화 대상
- **출처**: Engine.cpp 내 TX 처리 함수들 (`Ch6_SendAck*`, `Ew11_SendPacket` 등)
- **Phase 1 작업**: 파일 이동만
- **Phase 3 작업**: TxGuard/Priority Queue 유지하며 불필요한 복사 제거

#### `src/Engine/Engine.cpp` [슬림화]
- **잔류 내용** (SECTION 1 전체 유지):
  - `System_ReadTempC()`, `System_ReadCpuPct()`
  - `SystemMetricsTracker::*` 전체
  - `FormatHwMetrics()`, `FormatNetworkStats()`, `FormatRs485Stats()`, `FormatTaskStacks()`
- **예상 잔류 라인**: ~350줄

---

### 3.6 Phase 1-F: main.cpp 슬림화 (위험도: 중간~높음)

**원본**: [`src/main.cpp`](../src/main.cpp) (2,421줄)

> [!IMPORTANT]
> main.cpp 분리는 전역 변수(RTC, Atomic 등)의 정의 위치 변경을 수반한다. `extern` 선언과 정의 파일 간 일관성을 정밀하게 관리해야 한다. Telnet/Config/WarmCache 순서대로 한 블록씩 이동한다.

#### `src/Core/Globals.cpp` [신규]
- **출처**: main.cpp SECTION 1
  - RTC_NOINIT_ATTR 전역 변수들 (L18~30)
  - `std::atomic<bool> g_rescue_mode`, `g_rollback_detected` (L31~35)
  - `g_warm_cache_loaded`, `g_warm_cache_source`, `g_warm_cache_restored_count` (L278~280)
  - `g_wdt_monitor`, 각 채널 스토리지 버퍼 (L394~417)
  - `g_telnet_task_handle`, `g_ch1_task_handle` 등 Task 핸들 (L562~565)
  - `g_boot_start_ms` (L566)
- **헤더**: `Common.h`의 `extern` 선언으로 공개

#### `src/Core/SystemHealth.cpp` [신규]
- **출처**: main.cpp SECTION 1 진단 함수들
  - `System_DiagnoseStuck()` (L44~91)
  - `System_LogResetReason()` (L95~150)
  - `System_CheckCoreDump()` (L151~170)
  - `System_IsOtaPendingVerify()` (L171~182)
  - `System_CheckOtaHealth()` (L183~210)
  - `System_EnterRescueMode()` (L211~273)

#### `src/Network/HubManager.cpp` [신규]
- **출처**: main.cpp TCP/Ew11 관련
  - `struct TcpFragSession`, `Tcp_CloseAllSessions()`, `Tcp_PollAndReceive()` (L482~561)
  - `Tcp_EnableKeepalive()` (L577~587)
  - `Tcp_AcceptAndAssignSlot()` (L588~644)
  - `Ew11_AcceptClient()` (L645~709)
  - `Ch6_SendAck_Direct()`, `Ch6_SendAck()` (L710~743)
  - `Ch6_Data()` (L744~837)
  - `Ew11_ProcessPacket()` (L838~944)
  - `Ew11_Data()` (L945~1079)
  - `Ew11_LoadConfig()`, `Ew11_SaveConfig()`, `Ew11_SetSlot()`, `Ew11_SendPacket()` (L1578~1711)

#### `src/Network/NetworkManager.cpp` [신규]
- **출처**: main.cpp
  - `onWifiEvent()` (L419~481)
  - `Task_Network()` (L1084~1440)

#### `src/main.cpp` [슬림화]
- **잔류 내용**: Boot 시퀀스 글루 코드만
  - `Config_Load()`, `Config_Save()`, `Config_ResetDefaults()` (L1461~1713)
  - `System_TakeSnapshot()` (L1714~1793)
  - `LogManager::*` (SECTION 3, L1798~1954)
  - `System_Restart()` (L1955~1987)
  - `Boot_CheckCrashLoop()`, `Boot_InitSyncPrimitives()`, `Boot_InitHardwareAndDevices()` (L2003~2259)
  - `System_ApplyUartConfig()` (L2195~2259)
  - `Boot_InitWifiAndOta()`, `Boot_StartTasks()` (L2260~2397)
  - `setup()`, `loop()` (L2398~2421)
- **예상 잔류 라인**: ~700줄

---

## 4. 전역 인스턴스 및 헤더 관리

### 4.1 전역 인스턴스 소유권 정의

| 전역 변수 | 현재 위치 | 이동 위치 | extern 선언 위치 |
| :--- | :--- | :--- | :--- |
| `g_polling_targets` | `WallpadParser.cpp` L15 | `Parser/PollingTargetRegistry.cpp` | `WallpadParser.h` |
| `g_telnet_manager` | `TelnetCli.cpp` | `Telnet/TelnetCli.cpp` (잔류) | `TelnetCli.h` |
| `g_mgmt_sessions[]` | `MgmtRpc.cpp` L18 | `Management/MgmtRpc.cpp` (잔류) | `MgmtRpc.h` |
| `g_http_ota_state` | `MgmtRpc.cpp` L17 | `Management/HttpOta.cpp` | `MgmtRpc.h` |
| `g_timing_config` | `MgmtRpc.cpp` L16 | `Management/MgmtRpc.cpp` (잔류) | `MgmtRpc.h` |
| `g_wdt_monitor` | `main.cpp` L394 | `Core/Globals.cpp` | `Common.h` |
| `g_rescue_mode` | `main.cpp` L31 | `Core/Globals.cpp` | `Common.h` |
| `g_pkt_stats` | `Common.h` (전역) | 변경 없음 | `Common.h` |
| `rtc_warm_cache` | `main.cpp` L29 | `Core/Globals.cpp` | `Common.h` |

### 4.2 헤더 분리 계획

> [!NOTE]
> 헤더는 인터페이스 제공 목적으로만 존재. 파일 분리를 위한 기계적 헤더 신설 금지.

| 조치 | 대상 | 내용 |
| :--- | :--- | :--- |
| **유지** | `include/Common.h` | 공통 타입/전역/유틸 선언 — 대규모 변경 없음 |
| **유지** | `include/TelnetCli.h` | `TelnetManager`, `TelnetTracer` 선언 그대로 유지 |
| **유지** | `include/WallpadParser.h` | Parser 클래스 선언 그대로 유지 |
| **유지** | `include/ControlTemplate.h` | 변경 없음 |
| **유지** | `include/CliCommands.h` | 변경 없음 |
| **유지** | `include/MgmtRpc.h` | extern 선언 추가 가능 (HttpOta 전역) |
| **신규 검토** | `include/Config.h` | Common.h의 `namespace Config { ... }` 분리 (선택적) |

---

## 5. PlatformIO 빌드 설정 변경

### 5.1 `platformio.ini` src_filter 추가

현재 `src_filter`가 없으면 `src/` 루트만 스캔. 하위 디렉터리 추가 시 명시 필요.

```ini
[env:m5stack-atoms3]
...
build_flags =
    -I include
    -I src/Core
    ; 기존 build_flags 유지...

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

> [!IMPORTANT]
> `src_filter` 설정 후 즉시 빌드 테스트. 누락 파일 또는 중복 심볼 에러 발생 시 해당 Phase에서 즉시 수정.

---

## 6. 단계별 실행 계획 (Phase-by-Phase)

### Phase 0: Baseline 고정

```bash
# 빌드 후 기록
pio run --environment m5stack-atoms3 2>&1 | grep -E "RAM:|Flash:|error|warning" > docs/baseline.txt

# 실행 후 시리얼로 기록해야 할 항목:
# - Free heap
# - Min free heap
# - Task별 stack watermark (CH1, CH2/3, CH4, Network, Telnet)
# - Reset reason
# - Firmware 바이너리 크기 (bin/firmware.bin)
```

**커밋**: `commit 00: baseline snapshot`

---

### Phase 1-A: Telnet 분리 (예상 소요: 2~3h)

**순서** (낮은 위험도 → 높은 순):
1. `TelnetTracer.cpp` 생성: SECTION 1 이동
2. `TelnetAuth.cpp` 생성: handlePassword 이동
3. `TelnetWizard.cpp` 생성: Wizard 함수들 이동
4. `TelnetServer.cpp` 생성: Server/Session lifecycle 이동
5. `TelnetCli.cpp` 정리: 잔류 코드 확인

**각 파일 이동 후**:
- `pio run` → 0 error 확인
- `static void` 내부 함수 → 해당 파일 내 `static` 또는 익명 namespace 유지
- `_cli_mutex` 등 공유 mutex → 어느 한 파일에서 정의, extern으로 공유

**커밋**: 각 파일 이동마다 1 커밋
- `commit 01: split TelnetTracer.cpp`
- `commit 02: split TelnetAuth.cpp`
- `commit 03: split TelnetWizard.cpp`
- `commit 04: split TelnetServer.cpp`
- `commit 05: slim TelnetCli.cpp`

---

### Phase 1-B: CLI 분리 (예상 소요: 2~3h)

**순서**:
1. `CliConfig.cpp` — `namespace ConfigCli` 블록 이동
2. `CliSystem.cpp` — `namespace SystemCli` 블록 이동
3. `CliControl.cpp` — `namespace WallpadCli` 블록 이동 (최대 규모)
4. `CliNetwork.cpp` — `namespace WifiCli` 블록 이동
5. `CliCommands.cpp` 정리

**주의사항**:
- 각 `namespace` 클로징 `}` 위치 정밀 확인 (파서 구분자 없음)
- `CliControl.cpp` 내 `static void formatSources()` → 해당 파일 내 `static` 유지
- `CliNetwork.cpp` 내 `static void AsyncWifiScanTask()` → 해당 파일 내 `static` 유지

**커밋**:
- `commit 06: split CliConfig.cpp`
- `commit 07: split CliSystem.cpp`
- `commit 08: split CliControl.cpp`
- `commit 09: split CliNetwork.cpp`

---

### Phase 1-C: Parser 분리 (예상 소요: 1~2h)

**순서** (경계가 명확하여 위험도 낮음):
1. `PollingTargetRegistry.cpp` — BLOCK 1 이동
2. `AutoProbingEngine.cpp` — BLOCK 2 이동
3. `ProfileRepository.cpp` — BLOCK 3 이동
4. `UniversalProtocolEngine.cpp` — BLOCK 4+5 이동

**주의사항**:
- `#include <vector>`, `<set>`, `<map>` — 각 파일에 필요한 것만 포함
- `g_auto_probing_engine`, `g_wallpad_factory` 등 전역 인스턴스 → `UniversalProtocolEngine.cpp` 또는 최초 사용 파일에 집약

**커밋**:
- `commit 10: split Parser/PollingTargetRegistry.cpp`
- `commit 11: split Parser/AutoProbingEngine.cpp`
- `commit 12: split Parser/ProfileRepository.cpp`
- `commit 13: split Parser/UniversalProtocolEngine.cpp`

---

### Phase 1-D: Management 분리 (예상 소요: 1h)

**순서**:
1. `HttpOta.cpp` — OTA 태스크 및 API 이동
2. `Telemetry.cpp` — Telemetry/Broadcast 함수들 이동
3. `MgmtRpc.cpp` 정리

**커밋**:
- `commit 14: split Management/HttpOta.cpp`
- `commit 15: split Management/Telemetry.cpp`

---

### Phase 1-E: Engine 분리 (예상 소요: 3~4h)

**순서** (의존성 낮은 것부터):
1. `Control/ControlRegistry.cpp` — DeviceRouteRegistry + ControlDispatcher
2. `Engine/Ch1Engine.cpp` — DeviceRepository + Control 핸들러
3. `Engine/Ch1Polling.cpp` — 폴링 루프 + Task_Ch1
4. `Engine/Ch23Engine.cpp` — Task_Ch2Ch3 + Task_Ch4
5. `Engine/UartRx.cpp` — RX 경로 함수들
6. `Engine/UartTx.cpp` — TX 경로 함수들
7. `Engine/Engine.cpp` 정리

**커밋**:
- `commit 16: split Control/ControlRegistry.cpp`
- `commit 17: split Engine/Ch1Engine.cpp`
- `commit 18: split Engine/Ch1Polling.cpp`
- `commit 19: split Engine/Ch23Engine.cpp`
- `commit 20: split Engine/UartRx.cpp + UartTx.cpp`

---

### Phase 1-F: main.cpp 슬림화 (예상 소요: 2~3h)

> [!CAUTION]
> 전역 변수 정의 위치 변경 작업. `RTC_NOINIT_ATTR` 변수들은 링커 섹션 배치에 영향을 줄 수 있으므로 빌드 후 반드시 Flash/RAM 맵 비교.

**순서**:
1. `Core/SystemHealth.cpp` — 진단 함수들 (전역 변수 미포함)
2. `Network/NetworkManager.cpp` — Task_Network + WiFi 이벤트
3. `Network/HubManager.cpp` — Ew11/TCP/Ch6 함수들
4. `Core/Globals.cpp` — 전역 변수 정의 집약 (마지막에 처리)
5. `main.cpp` 정리

**커밋**:
- `commit 21: split Core/SystemHealth.cpp`
- `commit 22: split Network/NetworkManager.cpp`
- `commit 23: split Network/HubManager.cpp`
- `commit 24: split Core/Globals.cpp (global var relocation)`
- `commit 25: slim main.cpp`

---

### Phase 2: 분리 무결성 검증 (Delta = 0)

**빌드 검증**:
```bash
pio run --environment m5stack-atoms3 2>&1 | grep -E "error|warning"
# Warning 수: Baseline 이하 유지
```

**런타임 검증 항목**:
1. UART RX/TX (RS485 9600bps 정상 수신/송신)
2. RS485 Polling 주기 유지
3. TCP 소켓 연결/재연결 정상
4. Telnet 접속, 인증, CLI 명령 응답 정상
5. SmartThings Hub (Ew11) 연결 정상
6. WDT Reset 미발생 (24h 이상)
7. Free heap, Min free heap — Baseline 대비 감소 없음
8. Stack watermark 6개 Task 모두 — Baseline 대비 악화 없음

**커밋**: `commit 26: Phase 2 verification pass (no delta)`

---

### Phase 3~8: 최적화 (분리 완료 후 진행)

| Phase | 작업 | 대상 파일 |
| :--- | :--- | :--- |
| **Phase 3** | Hot Path 최적화 | `Engine/UartRx.cpp`, `Engine/UartTx.cpp` |
| **Phase 4** | 반복 Lookup 제거 | `Engine/Ch1Engine.cpp` (`DeviceDescriptor` 캐시) |
| **Phase 5** | memcpy 최소화 | `Engine/UartRx.cpp`, `Engine/Ch1Engine.cpp` |
| **Phase 6** | Lock hold time 단축 | `Telnet/TelnetTracer.cpp`, `Engine/Ch1Engine.cpp` |
| **Phase 7** | Logging 경로 격리 | `Engine/UartRx.cpp` → LogManager Queue |
| **Phase 8** | Management Path 정리 | `Management/Telemetry.cpp`, `Network/HubManager.cpp` |

> Phase 3~8는 Phase 2 검증 완료 후 별도 세부 계획서로 작성.

---

## 7. 신규 디렉터리 생성 명령

```bash
cd src
mkdir -p Core Engine Parser Network Control Telnet CLI Management
```

---

## 8. 검증 체크리스트

### Phase 별 공통 체크 (매 커밋 전)

- [ ] `pio run` 0 error
- [ ] 이전 대비 신규 warning 없음
- [ ] 이동된 함수의 `static` / 익명 namespace 속성 보존 확인
- [ ] 공유 mutex가 파일 분리로 인해 복제되지 않았는지 확인
- [ ] 전역 변수 중복 정의(ODR violation) 없음

### Phase 2 최종 체크

- [ ] UART 패킷 TX/RX 동일성 (Packet format, checksum)
- [ ] RS485 Polling 응답 시간 Baseline ±5% 이내
- [ ] SmartThings 이벤트 전달 정상
- [ ] Telnet 세션 최대 연결수 정상
- [ ] WDT 24h 무발생
- [ ] Free heap Baseline ≥ 현재
- [ ] 6개 Task stack watermark Baseline ≤ 현재

---

## 9. 위험 관리

| 위험 요소 | 가능성 | 영향 | 대응 |
| :--- | :---: | :---: | :--- |
| ODR (중복 정의) 빌드 에러 | 높음 | 빌드 불가 | 전역 변수 이동 시 `extern` 선언 확인 |
| `static` 함수 노출로 인한 링크 에러 | 중간 | 빌드 불가 | 내부 static 함수 → 해당 .cpp 내 익명 namespace |
| RTC 전역 변수 링커 섹션 배치 변경 | 낮음 | 런타임 오작동 | Flash 맵 비교, WDT 진단 로직 검증 |
| 공유 mutex 분산 복제 | 낮음 | 데드락/레이스 | 분리 후 mutex 정의 위치 일관성 검증 |
| 파일 이동 중 기능 실수 변경 | 낮음~중간 | 기능 회귀 | Phase 1은 로직 변경 0%, copy-only 원칙 |
| `CliControl.cpp` 크기 (1,936줄) | — | 유지보수 우려 | Phase 1에서는 이대로 유지, 세분화는 Phase 8 이후 검토 |

---

## 10. 최종 파일 수 예측

| 디렉터리 | 현재 | 목표 |
| :--- | :---: | :---: |
| `src/` (루트) | 7 .cpp | 1 (`main.cpp`) |
| `src/Core/` | 0 | 2 `.cpp` |
| `src/Engine/` | 0 | 6 `.cpp` |
| `src/Parser/` | 0 | 4 `.cpp` |
| `src/Network/` | 0 | 2 `.cpp` |
| `src/Control/` | 0 | 2 `.cpp` |
| `src/Telnet/` | 0 | 5 `.cpp` |
| `src/CLI/` | 0 | 5 `.cpp` |
| `src/Management/` | 0 | 3 `.cpp` |
| **합계** | **7** | **30 .cpp** |

> 헤더 수는 현행 6개에서 1~2개 추가 정도로 최소화.

