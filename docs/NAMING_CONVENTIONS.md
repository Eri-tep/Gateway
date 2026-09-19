# GW Home Architecture & Codebase Naming Conventions

본 문서는 `GW Home` 프로젝트의 코드 품질, 직관성, 일관성 및 장기 유지보수성을 보장하기 위한 공식 명명 규칙(Naming Conventions) 표준입니다. 모든 기여자와 에이전트는 본 명명 규칙을 엄격히 준수해야 합니다.

---

## 1. 전역 및 프리(Free) 함수 명명 규칙

전역 함수는 소속 도메인과 동작을 명확히 알 수 있도록 **`Domain_VerbNoun`** (PascalCase + 언더스코어 구분자) 형식을 사용합니다.

| 도메인 접두사 | 대상 영역 | 예시 |
| :--- | :--- | :--- |
| `System_` | 하드웨어 측정, 재부팅, 코어덤프, 스냅샷 | `System_ReadTempC()`, `System_Restart()`, `System_TakeSnapshot()` |
| `Config_` | NVS 설정 로드/저장/리셋 | `Config_Load()`, `Config_Save()`, `Config_ResetDefaults()` |
| `Cache_` | RTC 메모리 및 NVS 기반 영구 캐시/부팅 상태 동기화 | `Cache_SaveToRtc()`, `Cache_SaveToNvs()`, `Cache_RestoreOnBoot()` |
| `Hub_` | CH5 멀티 슬롯 TCP 허브 클라이언트 관리 및 통신 | `Hub_LoadConfig()`, `Hub_SaveConfig()`, `Hub_SetSlot()`, `Hub_SendPacket()` |
| `Queue_` | RTOS 큐 특수 동작 | `Queue_EnqueueDropHead()` |
| `Tcp_` | TCP 소켓 유틸리티, IP 필터링 | `Tcp_IsAllowedIP()`, `Tcp_EnableKeepalive()` |
| `Door_` | 도어폰 (CH4/CH5) 프로토콜/시리얼 | `Door_SerialConfig()`, `Door_IsValidOpcode()` |
| `Uart_` | UART 물리 계층 I/O | `Uart_RecvPacket()` |
| `Ch1_` ~ `Ch6_` | 각 RS-485 및 TCP 채널 전용 핸들러 | `Ch1_BuildQueryPacket()`, `Ch6_SendAck()`, `Ch6_Data()` |
| `Boot_` | `main.cpp` 내부 부팅 시퀀스 (`static void` 한정) | `Boot_InitHardwareAndDevices()`, `Boot_StartTasks()` |

> **규칙 및 금지 사항**:
> - 포맷팅 유틸리티 함수(`FormatHwMetrics` 등)는 전역이 아닌 `namespace Fmt { ... }` 내부에 선언 및 구현합니다.
> - `Boot_` 접두사는 부팅 시퀀스를 담당하는 `main.cpp` 내부 `static` 전용 함수에만 한정하여 사용합니다.
> - 접두사 없는 순수 camelCase 전역 함수 (예: `readChipTempC()`, `isAllowedClientIP()`) 금지
> - PascalCase 단독 전역 함수 (예: `CheckAndLogLastResetReason()`) 금지

---

## 2. FreeRTOS 태스크 함수 명명 규칙

태스크 진입 함수는 래퍼 없이 **`Task_<Domain>`** 단일 명칭으로 정의하고 외부로 export합니다.

| 태스크 명칭 | 소속 코어 | 역할 |
| :--- | :---: | :--- |
| `Task_Ch1` | Core 1 | RS-485 CH1 IoT 장비 마스터 폴링 및 제어 |
| `Task_Ch2Ch3` | Core 1 | RS-485 CH2/CH3 월패드 슬레이브 가상 응답 |
| `Task_Ch4` | Core 1 | CH4 현관/공동 도어폰 양방향 통신 |
| `Task_Network` | Core 0 | Wi-Fi, OTA, TCP 소켓 서버 (CH5, CH6) 총괄 |
| `Task_Telnet` | Core 0 | Telnet CLI 관리 및 진단 콘솔 |

> **금지 사항**:
> - `Core1_Ch1Task`와 같은 불필요한 래퍼 함수 정의 금지
> - `Task_IoTChannel1 = Core1_Ch1Task`와 같은 중복 별칭(Alias) 정의 금지

---

## 3. 전역 변수 및 별칭 규칙

모든 전역 변수는 `g_` 접두사를 사용하며, **단 하나의 명확한 표준 이름**만 선언합니다.

| 표준 전역 변수명 | 타입 | 설명 |
| :--- | :--- | :--- |
| `g_config` | `RuntimeConfig` | 시스템 런타임 설정 인스턴스 |
| `g_metrics` | `SystemMetricsTracker` | 시스템 하드웨어 링버퍼 메트릭 수집기 |
| `g_ch1_bus_ms` | `std::atomic<uint32_t>` | CH1 버스 마지막 활동 타임스탬프 (ms) |
| `g_pkt_stats` | `PacketStatistics` | 실시간 패킷/에러 카운터 |
| `g_polling_targets` | `PollingTargetRegistry` | 1차 동적 폴링 타깃 레지스트리 |
| `g_auto_probing_engine` | `AutoProbingEngine` | 범용 오토 프로빙 학습 엔진 |
| `g_wdt_monitor` | `TaskWdtMonitor` | 태스크별 WDT 헬스 모니터 |
| `g_hub_slots` | `HubClientSlot[]` | CH5 멀티 클라이언트 슬롯 풀 |

> **금지 사항**:
> - 동일 인스턴스에 대한 참조 별칭 선언 금지 (`inline auto &g_metrics = g_metrics_tracker;` 등)
> - 동일 타입에 대한 중복 typedef/using 선언 금지 (`using DeviceRepo = DeviceRepository;` 등)
> - 단순 래퍼 네임스페이스 선언 금지 (`namespace Kernel = SystemOrchestrator;` 등)

---

## 4. 상수 및 네임스페이스 규칙

- 모든 설정 상수는 소속된 `namespace Config::<Group>` 내부에 `UPPER_SNAKE_CASE`로 정의합니다.
- 네임스페이스 외부에서 `using` 및 `inline constexpr`로 중복 선언하지 않고, 호출부에서 `Config::<Group>::<CONST>`로 명확하게 참조합니다.

---

## 5. 스냅샷(Snapshot) 데이터 구조체 명명 규칙

Atomic 변수를 배제하고 memcpy/스냅샷 복사 용도로 사용하는 구조체는 **`Snapshot`** 또는 **`Stats`** 접미사를 사용합니다.

- `SysSnapshot` (구 `PlainSystemStats`)
- `HwSnapshot` (구 `PlainHardwareMetrics`)
- `StackSnapshot` (구 `PlainTaskStackStats`)
- `ChanStats` (구 `PlainSingleChannelStats`)
- `TcpChanStats` (구 `PlainTcpSocketStats`)
- `PktSnapshot` (구 `PlainPacketStatistics`)
