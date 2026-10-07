# ESP32-S3 상용 제품급 시스템 안정성 및 CPU 벤치마크 사양서
# (ESP32-S3 Embedded Stability & Benchmark Specification)

> **문서 버전**: v1.1.0  
> **개정일**: 2026-10-07  
> **대상 플랫폼**: M5Stack AtomS3 Lite (ESP32-S3FN8 Dual-Core 240MHz, FreeRTOS, Arduino-ESP32 Core v3.1.x / ESP-IDF v5.3.2)  
> **적용 표준**: C++23 (`-std=gnu++23`), Exceptions Disabled (`-fno-exceptions`)  
> **빌드 격리**: `#if defined(BENCHMARK_BUILD)` 기반 컴파일 타임 100% 바이너리 격리 (Release 빌드 링크 시 0 Bytes Overhead)

---

## 0. 제정 목적 및 배경

본 사양서는 스마트홈 RS-485/TCP 게이트웨이의 C++23 리팩토링 과정에서 관측된 **CPU 점유율 변화(6% ➡️ 9%)**의 물리적/소프트웨어적 원인을 계측 기반으로 정밀 분리하고, 상용 완제품(Production-Grade, 365일 24시간 무중단) 수준의 초저지연·고신뢰성 실시간 제어를 보증하기 위한 **하드웨어 네이티브 벤치마크 하네스 설계 표준 및 런타임 안정성 거버넌스 규격**이다.

### 0.1 핵심 규명 원칙
1. **On-Chip Real Hardware Execution**: PC 환경 시뮬레이션이나 단순 목업을 철저히 배제하고, ESP32-S3 듀얼코어 하드웨어 온칩 상에서 직접 계측한다.
2. **No Measurement Contamination**: 벤치마크 코드가 프로덕션 펌웨어 상시 루프에 상주하여 CPU/메모리를 잠식하지 않도록 컴파일 타임(`BENCHMARK_BUILD`) 분리 및 샌드박스를 강제한다.
3. **Decoupled Root Cause Isolation**: 전체 CPU 9%라는 거시적 지표 하나에 의존하지 않고, Core0/Core1 독립 분리, Task별 런타임, 어셈블리 생성 차이, 인터럽트/동기화 비용, 캐시 미스를 계층별로 독립 분리하여 검증한다.

---

## 1. C++23 전환 CPU 점유율 증가(6% ➡️ 9%) 5대 물리적 가설 재정의

| 가설 ID | 물리적 요인 | 세부 메커니즘 및 검증 대상 | 검증 및 차별화 방법 |
|---|---|---|---|
| **H1** | **`std::span` 및 추상화 코드 생성 회귀 (Codegen Regression)** | `std::span` 자체의 ABI 문제가 아닌, Pass-by-value vs Reference 불일치, 컴파일러 인라인화(Inlining) 누락, 임시 객체(Temporary) 생성, 경계 검사(Bounds Check) 코드 삽입 여부 | 포인터+길이 vs `std::span` 순수 어셈블리(`objdump`) 및 100만 회 사이클 대조 |
| **H2** | **I-Cache / D-Cache & Flash SPI 미스** | C++23 템플릿 인스턴스화, `constexpr` 룩업 테이블, Enum 변환 맵 등의 확장이 Flash `.rodata` / `.text`에 배치되어 SPI 버스 접근 지연(Cache Line Miss, Instruction Fetch Stall) 발생 | Flash 코드/데이터 상주 vs IRAM/DRAM 상주 실행 사이클 대조 |
| **H3** | **동기화 및 메모리 오더 비용 (Sync & Memory Ordering)** | `std::atomic` 기본값(`seq_cst`) 적용으로 인한 불필요한 DMB(Data Memory Barrier) 파이프라인 스톨 누적, 또는 `portENTER_CRITICAL` 스핀락 경합 증가 | `seq_cst` vs `acq_rel` vs `relaxed` 사이클 분해 및 임계 구역 누적/최대 체류 시간 실측 |
| **H4** | **인터럽트(ISR) 및 컨텍스트 스위칭 지연** | UART RX ISR ➡️ FreeRTOS Queue ➡️ `Task_Ch1` 수직 경로에서 인터럽트 비활성화 시간 증가 또는 인터럽트 핸들러 오버헤드 | ISR 호출 빈도, 인터럽트 비활성화 지속 시간, 컨텍스트 스위치 횟수 계측 |
| **H5** | **계측 무결성 왜곡 (Measurement Artifacts)** | 실제 부하 증가가 아닌, FreeRTOS IDLE 태스크 런타임 통계 계산 방식, Wi-Fi/LwIP 백그라운드 지터, 샘플링 윈도우 차이로 인한 수치적 왜곡 | Core0/Core1 독립 IDLE 카운트 실측 및 계측 자체 오버헤드 보정 |

---

## 2. 상용 제품급 벤치마크 & 런타임 안정성 12대 불변식 (The 12 Pillars)

```
+==================================================================================================+
|                        ESP32-S3 PRODUCTION-GRADE 12 STABILITY PILLARS                           |
+==================================================================================================+
| [P0]  Measurement Integrity    : Core0/Core1 독립 IDLE 검증, 통계 샘플링 일관성, 계측 오버헤드 보정   |
| [P1]  Non-Blocking / WDT       : Task WDT 보호(10k 회 리셋), Core 0/1 기아 방지, 백그라운드 통신 보장  |
| [P2]  Zero-Heap & Stack Guard  : 0-Byte Heap 변동(FAIL 판정), Task Stack 여유 1.5KB 이상 영구 보증   |
| [P3]  Cycle-Accurate Timing    : esp_cpu_get_cycle_count() (4.16ns 정밀도), 1,000회 Warm-up 버림     |
| [P4]  Bus-Isolated Sandbox     : 물리 UART/GPIO/TCP 전기적 출력 완전 침묵(Null Sink), NVS RAM 우회    |
| [P5]  Frequency & Thermal Guard: CPU 240MHz 고정 검증(불일치 시 즉시 무효화), 온칩 온도 로깅(열 왜곡 감시)|
| [P6]  Synchronization & Order  : lock_count, contention, total/max_hold_cycles 실측, 임계구역 6대 금지 |
| [P7]  Code Generation / Binary : C++17 vs C++23 objdump 어셈블리 diff, .text/.rodata/IRAM 배치 분석  |
| [P8]  Cache & Memory Locality  : I-Cache/D-Cache 미스, Flash Fetch Stall 감시, Hot-Path IRAM 배치    |
| [P9]  Interrupt & ISR Budget   : UART RX ISR -> Queue -> Task 파이프라인 사이클, 인터럽트 마스킹 한도  |
| [P10] Real-Time Determinism    : Avg/Median/P95/P99/P99.9/WCET(Max) 지터 분석, 꼬리 지연(Tail Latency)|
| [P11] Queue & Backpressure     : Drop-Head vs Drop-Tail 메트릭 실측, 큐 포화 및 백프레셔 복원력 검증   |
+==================================================================================================+
```

### Pillar 0: Measurement Integrity (계측 무결성)
- **규칙 0.1 (Core 분리 계측)**: 
  - 통합 CPU 점유율 단일 수치만으로 평가하지 않으며, **Core 0(시스템/네트워크: IDLE0, TcpReactor, Telnet, WiFi)**과 **Core 1(실시간 통신: IDLE1, Task_Ch1, Task_Ch2Ch3, Task_Ch4)**을 완전히 독립된 런타임 윈도우로 분리 측정한다.
- **규칙 0.2 (동일 환경 조건 통제)**:
  - 측정 윈도우(예: 1,000ms), 등록된 태스크 세트, CPU 클록(240MHz), Wi-Fi 연결 상태(동일 AP 접속 또는 오프라인 고정), 인터럽트 백그라운드 활성도를 동일 조건으로 통제한다.
- **규칙 0.3 (계측 오버헤드 자체 보정)**:
  - 사이클 카운터 조회(`esp_cpu_get_cycle_count()`), 메모리 조회, 런타임 통계 추출 함수 자체의 실행 사이클(보통 15~30 사이클)을 사전에 캘리브레이션하여 실측 데이터에서 차감 보정한다.

### Pillar 1: Non-Blocking / WDT / Starvation Prevention (비차단 및 기아 방어)
- **규칙 1.1 (Task Watchdog Timer 보호)**:
  - 10만~100만 회 벤치마크 루프는 단일 블로킹으로 실행되어서는 안 된다.
  - 매 10,000회 루프마다 `esp_task_wdt_reset()`을 호출하거나 `taskYIELD()`를 수행해야 한다.
- **규칙 1.2 (IDLE 태스크 기아 방지)**:
  - 매 50,000회마다 `vTaskDelay(pdMS_TO_TICKS(1))` 체크포인트를 두어 FreeRTOS IDLE 태스크 및 Wi-Fi/LwIP 백그라운드 태스크의 정상 동작을 보장한다.

### Pillar 2: Zero-Heap & Stack Watermark Invariant (메모리 무결성)
- **규칙 2.1 (0-Byte Heap 변동 엄수)**:
  - 벤치마크 시작 직전 `esp_get_free_heap_size()`와 완료 후의 힙 크기는 **오차 0바이트**여야 한다.
  - 단 1바이트라도 동적 할당 누수가 감지될 경우 해당 테스트는 즉시 `FAIL` 처리한다.
- **규칙 2.2 (스택 고수위 감시)**:
  - `uxTaskGetStackHighWaterMark()`를 매 단계 계측하여, 태스크 스택 잔여량이 **1,536 Bytes (1.5KB)** 이하로 떨어질 경우 실행을 즉시 강제 중단하여 Stack Overflow를 원천 차단한다.

### Pillar 3: Cycle-Accurate Timing & Warmup Invariant (사이클 정밀도 및 결과 무결성)
- **규칙 3.1 (하드웨어 사이클 계측)**:
  - `millis()`나 `micros()`와 같은 OS 타이머를 배제하고, ESP32-S3 전용 하드웨어 사이클 카운터인 `esp_cpu_get_cycle_count()`를 사용한다. (240MHz 기준 1 사이클 = 약 4.167ns 정밀도)
- **규칙 3.2 (캐시 워밍업 및 콜드 스타트 격리)**:
  - Flash 캐시 라인 콜드 스타트 노이즈를 제거하기 위해, **최초 1,000회는 Warm-up으로 실행한 후 결과를 버린다(Discard)**.
  - 이후 완전히 캐시된 웜(Warm) 상태에서 본 측정을 수행한다.
- **규칙 3.3 (Benchmark Result Integrity & DCE 방어)**:
  - 컴파일러의 Dead-Code Elimination(DCE)에 의해 루프 연산이 증발하는 것을 방지하기 위해, 최종 파싱 결과나 체크섬을 **Observable Sink(누적 체크섬 정적 전역 변수)**에 기록한다.
  - 단, 루프 내부 변수에 `volatile`을 남용하여 실제 프로덕션 레지스터 최적화를 왜곡하는 행위는 엄격히 금지한다.

### Pillar 4: Bus-Isolated Sandbox Invariant (물리 버스 완전 격리)
- **규칙 4.1 (물리적 송출 침묵)**:
  - 벤치마크 하네스는 실제 하드웨어 UART 0~2, SoftwareSerial 핀, TCP 소켓으로 단 1바이트의 전기적 신호도 출력해서는 안 된다.
  - 출력 패킷은 메모리 상의 정적 널 싱크(Null Sink) 버퍼 또는 인메모리 링버퍼로 흡수된다.
- **규칙 4.2 (NVS 플래시 쓰기 우회)**:
  - 테스트 패킷에 설정 변경 또는 학습 커맨드가 포함되더라도, 물리 플래시 메모리의 수명 마모 및 지연을 방지하기 위해 NVS 커밋은 RAM 임시 구조체로 우회되어야 한다.

### Pillar 5: Frequency & Thermal Guard (주파수 고정 및 열 왜곡 방어)
- **규칙 5.1 (CPU 주파수 240MHz 고정 및 검증)**:
  - Dynamic Frequency Scaling(DFS)으로 인한 주파수 변동(80/160/240MHz)을 차단한다.
  - 벤치마크 시작 및 종료 시 CPU 클록이 **240MHz가 아닌 경우 즉시 해당 테스트를 무효(INVALID)** 처리한다.
- **규칙 5.2 (온칩 온도 텔레메트리 로깅)**:
  - ESP32-S3 내장 온도 센서(`temperature_sensor_get_celsius()`)를 통해 시작/종료 온도를 기록하여 열 왜곡(Thermal Drift) 여부를 진단 메트릭으로 보존한다.

### Pillar 6: Synchronization & Memory Order Invariant (동기화 비용 정밀 분리)
- **규칙 6.1 (Lock 4대 핵심 지표 분리 계측)**:
  - 락 획득 횟수 (`lock_count`)
  - 스핀락 경합 횟수 (`lock_contention_count`)
  - 임계 구역 총 체류 사이클 (`total_hold_cycles`)
  - 임계 구역 최대 체류 사이클 (`max_hold_cycles`)
- **규칙 6.2 (임계 구역 내부 6대 절대 금지 조항)**:
  - `portENTER_CRITICAL` 및 스핀락 내부에서 다음 연산은 절대 금지되며, 위반 시 런타임 Assertion으로 트랩한다:
    1. Queue 차단/대기 (`xQueueSend`, `xQueueReceive` 등)
    2. 소켓/네트워크 I/O 연산
    3. NVS Flash 읽기/쓰기
    4. 동적 메모리 할당 (`malloc`, `new`)
    5. 콜백 함수 호출 (역호출 데드락 방지)
    6. 가변 길이 대규모 패킷 파싱 루프
- **규칙 6.3 (원자적 연산 메모리 오더 감사)**:
  - `std::atomic` 변수 접근 시 `relaxed`, `acquire/release`, `seq_cst`의 실행 사이클을 단계별로 대조한다.

### Pillar 7: Code Generation & Binary Integrity (어셈블리 및 바이너리 무결성)
- **규칙 7.1 (바이너리 섹션 크기 Diff)**:
  - C++17 vs C++23 빌드의 `.text`, `.rodata`, `.bss`, `IRAM` 사용량을 섹션별로 분해 비교한다.
- **규칙 7.2 (핫패스 어셈블리 직접 비교 - objdump)**:
  - 주요 핫패스 심볼에 대해 역어셈블(Disassembly)을 수행하여 명령어 수, 인라인화 여부, 분기문(Branch) 수, 메모리 로드/스토어(Load/Store) 빈도를 직접 대조한다.
  - **직접 비교 대상 8대 심볼**:
    1. `CH1 RX parser` (`Wallpad_ExtractLength`, `Wallpad_ValidatePacket`)
    2. `DeviceCatalog::find()`
    3. `Firewall` 규칙 판정기
    4. `Protocol_Router` 디스패처
    5. `PacketBuilder`
    6. `Queue Enqueue/Dequeue` (`Task_Ch1` 내부)
    7. `CriticalSectionLocker`
    8. `std::atomic` 상태 읽기/쓰기

### Pillar 8: Cache & Memory Locality Invariant (캐시 및 메모리 국소성)
- **규칙 8.1 (Flash vs IRAM 실행 배치 검증)**:
  - 핫패스 루프 및 인터럽트 서비스 루틴이 SPI Flash에 머물러 Flash Cache Miss를 유발하는지, 아니면 IRAM(`IRAM_ATTR`)에 상주하는지 검증한다.
- **규칙 8.2 (상수 테이블 메모리 배치)**:
  - C++23 현대화로 도입된 `constexpr` 룩업 테이블들이 D-Cache 라인을 효율적으로 활용하는지 또는 캐시 스톨을 유발하는지 배치 섹션(`.rodata` in Flash vs DRAM)을 확인한다.

### Pillar 9: Interrupt & ISR Budget Invariant (인터럽트 예산 엄수)
- **규칙 9.1 (CH1 RS-485 수직 경로 단계별 분리)**:
  - `UART RX ISR` ➡️ `RingBuffer/Queue` ➡️ `Task_Ch1 Context Switch` 각 구간의 사이클을 분리하여 계측한다.
- **규칙 9.2 (인터럽트 마스킹 최대 시간 제한)**:
  - 임계 구역으로 인해 전체 인터럽트가 비활성화되는 최대 지속 시간(`max_interrupt_disabled_cycles`)이 **500 사이클 (약 2.08µs)**을 초과하지 않도록 보증한다.

### Pillar 10: Real-Time Determinism & Jitter Invariant (실시간 결정성 및 지터)
- **규칙 10.1 (통계적 분포 프로파일링)**:
  - 단순 평균(Average)에 속지 않고, 백분위수 분포를 필히 산출한다:
    - **Mean / Median / P95 / P99 / P99.9 / Max (Worst-Case Execution Time, WCET)**
- **규칙 10.2 (꼬리 지연(Tail Latency) 규제)**:
  - 평균 사이클이 400 사이클이더라도, P99.9 또는 Max가 10,000 사이클을 초과하는 스파이크가 발생할 경우 '실시간성 결함'으로 판정하고 원인을 추적한다.

### Pillar 11: Queue & Backpressure Invariant (큐 및 백프레셔 복원력)
- **규칙 11.1 (큐 조작 사이클 계측)**:
  - Enqueue 및 Dequeue 순수 소요 사이클을 실측한다.
- **규칙 11.2 (백프레셔 드롭 정책 무결성 검증)**:
  - 과부하 상황(Queue Full)에서 아키텍처 규격이 정의한 백프레셔 정책이 올바르게 동작하는지 검증한다:
    - **상태/폴링 패킷**: Drop-Head 정상 작동 여부 및 드롭 카운트 기록
    - **제어(VIP) 패킷**: Drop-Tail 차단 및 동기적 에러 리턴 여부 검증
  - Peak Queue Depth 및 Sustained Queue Depth를 기록한다.

---

## 3. 6단계 벤치마크 하네스 아키텍처 (6-Phase Benchmark Harness Topology)

```
+-----------------------------------------------------------------------------------------+
| Phase 0: Measurement Validation & Baseline Calibration                                  |
|  - Core0 vs Core1 IDLE Task Runtime 독립 검증                                            |
|  - 하드웨어 사이클 카운터(esp_cpu_get_cycle_count) 오버헤드 캘리브레이션                      |
|  - CPU 주파수(240MHz), 힙 오차 0바이트, 스택 워터마크 기준선 수립                           |
+-----------------------------------------------------------------------------------------+
                                             │
                                             ▼
+-----------------------------------------------------------------------------------------+
| Phase 1: Primitive & Parser Benchmark (1,000,000 runs)                                  |
|  - Raw Pointer+Len vs std::span (Pass-by-value vs Reference)                            |
|  - std::optional vs std::expected vs Status Enum                                        |
|  - Checksum 계산: Legacy 반복문 루프 vs C++23 Table Dispatch                            |
+-----------------------------------------------------------------------------------------+
                                             │
                                             ▼
+-----------------------------------------------------------------------------------------+
| Phase 2: CH1 Hot-Path Packet Flow Sandbox (100,000 runs)                                |
|  - Step A: RX Raw Stream Extraction (STX/ETX Framer)                                    |
|  - Step B: Wallpad Packet Parsing & Validation                                          |
|  - Step C: Device Catalog Match & State Decode                                          |
|  - Step D: Firewall / Routing Lookup                                                    |
|  - Step E: Action Dispatch & Downlink Packet Builder                                    |
+-----------------------------------------------------------------------------------------+
                                             │
                                             ▼
+-----------------------------------------------------------------------------------------+
| Phase 3: Synchronization, ISR & Queue Profiler (500,000 runs)                           |
|  - portENTER_CRITICAL (lock_count, contention, total/max hold cycles)                   |
|  - std::atomic 메모리 오더 (seq_cst vs acq_rel vs relaxed)                              |
|  - FreeRTOS Queue Push/Pop vs Direct Lockless RingBuffer                                |
|  - Drop-Head / Drop-Tail 백프레셔 모의 부하 테스트                                      |
+-----------------------------------------------------------------------------------------+
                                             │
                                             ▼
+-----------------------------------------------------------------------------------------+
| Phase 4: Code Generation, Cache & Assembly Diff                                         |
|  - C++17 vs C++23 objdump 역어셈블리 분기문/인라인/로드-스토어 카운트 비교                |
|  - .text / .rodata / IRAM 섹션 크기 및 Flash 코드 배치 분석                              |
|  - Cache Miss 유발 함수 IRAM 재배치 실험                                                |
+-----------------------------------------------------------------------------------------+
                                             │
                                             ▼
+-----------------------------------------------------------------------------------------+
| Phase 5: Full Firmware A/B Differential Profiling & Soak Test                           |
|  - A = C++23 Base + Legacy Hot-path Implementation                                      |
|  - B = C++23 Base + Modern Hot-path Implementation                                      |
|  - 10분간 연속 트래픽 주입 후 Core0/Core1 CPU 점유율, 힙/스택 누수, 지터 최종 비교       |
+-----------------------------------------------------------------------------------------+
```

---

## 4. CPU 점유율 6% ➡️ 9% 원인 추적 우선순위 매트릭스

| 우선순위 | 계측 대상 영역 | 관련 Pillar | 가설 검증 핵심 질문 |
|:---:|---|---|---|
| **★★★★★** | **Measurement Integrity** | **Pillar 0** | Core0/Core1의 IDLE 태스크 런타임이 통계 계산 방식 변경으로 인해 왜곡되었는가? |
| **★★★★★** | **Task / Core별 Runtime** | **Pillar 0, 1** | 3%p 증가는 Core0(시스템/네트워크)인가, Core1(`Task_Ch1` 통신)인가? |
| **★★★★★** | **Codegen / Assembly Diff** | **Pillar 7** | `std::span` 및 C++23 래퍼가 인라인되지 않고 함수 호출/스택 프레임을 추가 생성했는가? |
| **★★★★★** | **Lock / Atomic Profiling** | **Pillar 6** | `portENTER_CRITICAL`의 체류 시간 및 `seq_cst` 원자적 연산 DMB 지연이 증가했는가? |
| **★★★★★** | **ISR / Interrupt Budget** | **Pillar 9** | UART RX 인터럽트 처리 시간 및 인터럽트 비활성화 시간이 늘어났는가? |
| **★★★★☆** | **Cache Profiling** | **Pillar 8** | `constexpr` 테이블과 확장된 `.text` 코드가 Flash Cache Miss를 유발하는가? |
| **★★★★☆** | **Jitter / WCET** | **Pillar 10** | 평균이 아닌 P99.9나 최대 지연(Worst-Case)에 비정상적인 지연 스파이크가 있는가? |
| **★★★☆☆** | **Queue & Backpressure** | **Pillar 11** | 큐 인큐/디큐 오버헤드 및 백프레셔 정책 처리 비용이 증가했는가? |
| **★★☆☆☆** | **Thermal & Frequency** | **Pillar 5** | 벤치마크 중 CPU 주파수가 240MHz로 유지되고 온도가 정상 범위 내에 있는가? |

---

## 5. CH1 Hot-Path 패킷 처리 파이프라인 계측 명세

실제 월패드 트래픽(현대통신 HT 11바이트 쿼리/응답 및 0x28 난방 제어 패킷)을 고정 데이터셋으로 사용하여 파이프라인의 각 구간별 사이클을 마이크로초 단위로 분해 계측한다.

### 5.1 표준 입력 테스트 벡터 (Golden Test Packets)
1. **Query Frame (11 Bytes)**:
   `F7 0B 01 18 01 01 00 00 00 12 EE` (조명 1번 상태 조회)
2. **ACK Response Frame (11 Bytes)**:
   `F7 0B 01 18 04 01 01 00 00 16 EE` (조명 1번 전원 ON 응답)
3. **Control Frame (14 Bytes)**:
   `F7 0E 01 28 00 01 01 16 00 00 00 00 3B EE` (난방 희망온도 22도 설정 제어)

### 5.2 단계별 예산 및 프로파일링 타겟

| 파이프라인 단계 | 함수 / 로직 | 예산 사이클 (240MHz 기준) | 목표 시간 (µs) |
|---|---|:---:|:---:|
| **1. Stream Extract** | `Wallpad_ExtractLength` / STX 탐색 | < 240 사이클 | < 1.0 µs |
| **2. Packet Validation** | `Wallpad_ValidatePacket` / 체크섬 검증 | < 480 사이클 | < 2.0 µs |
| **3. Device Decode** | `ControlTemplate_DecodeDeviceState` | < 720 사이클 | < 3.0 µs |
| **4. Route Lookup** | `Protocol_LookupDeviceChannel` | < 360 사이클 | < 1.5 µs |
| **5. Action Dispatch** | `Router_DispatchControl` / `buildControlPacket` | < 960 사이클 | < 4.0 µs |
| **6. Sync Overhead** | Mutex / Spinlock / Atomic 갱신 | < 360 사이클 | < 1.5 µs |
| **합계 (Total Pipeline)** | **RX ➡️ State Update ➡️ TX Build** | **< 3,120 사이클** | **< 13.0 µs** |

---

## 6. 컴파일 타임 하네스 격리 및 빌드 규칙 (Compile-Time Feature Isolation)

### 6.1 프로덕션 바이너리 무결성 규칙
- **규칙 6.1.1 (100% 릴리스 분리)**:
  - 벤치마크 하네스 코드 및 테스트 벡터는 반드시 `#if defined(BENCHMARK_BUILD)` 전처리기 지시자로 격리되어야 한다.
  - 일반 릴리스 펌웨어(`pio run -e release`)에서는 벤치마크 관련 코드가 **단 1바이트도 링크되지 않아야 한다(0 Bytes Binary Overhead)**.
- **규칙 6.1.2 (전용 빌드 환경 플래그)**:
  - PlatformIO 환경에 `[env:m5stack-atoms3-benchmark]` 전용 환경을 구성하고, 빌드 플래그 `-D BENCHMARK_BUILD=1`을 적용하여 격리 컴파일한다.

---

## 7. 결과 리포트 출력 규격 (CLI 포맷 표준)

하네스 실행 완료 시 CLI 콘솔에 80컬럼 유니파이드 포맷으로 다음과 같이 출력되어야 한다.

```text
+==============================================================================+
|             ESP32-S3 C++23 HOT-PATH BENCHMARK DIFFERENTIAL REPORT            |
+==============================================================================+
| Pipeline Component        | Legacy (C++17) | Modern (C++23) |  Delta (Cycles)|
+---------------------------+----------------+----------------+----------------+
| 1. Stream & Framing       |     182 cycles |     185 cycles |  +1.6% (  +3)  |
| 2. Checksum / Validate    |     340 cycles |     342 cycles |  +0.6% (  +2)  |
| 3. Device Catalog Lookup  |     410 cycles |     415 cycles |  +1.2% (  +5)  |
| 4. Route Lookup           |     220 cycles |     222 cycles |  +0.9% (  +2)  |
| 5. Action Dispatch/Build  |     520 cycles |     530 cycles |  +1.9% ( +10)  |
| 6. Synchronization/Atomic |     210 cycles |     580 cycles | +176.2% (+370) |
+---------------------------+----------------+----------------+----------------+
| Total Latency per Packet  |   1,882 cycles |   2,274 cycles | +20.8% (+392)  |
| Measured Throughput       | 127,523 pkt/s  | 105,540 pkt/s  | -17.2%         |
+---------------------------+----------------+----------------+----------------+
| Latency Distribution (Modern C++23):                                         |
|  - Median: 2,240 cycles | P95: 2,310 cycles | P99: 2,420 cycles | Max: 2,890  |
+------------------------------------------------------------------------------+
| Core Utilization Breakdown:                                                  |
|  - Core 0 (System/Net) IDLE: 94.2% (Active: 5.8%)                            |
|  - Core 1 (RS-485 Hot) IDLE: 96.8% (Active: 3.2%)                            |
+------------------------------------------------------------------------------+
| Heap Delta: 0 Bytes [PASS]| Min Stack Left: 3,420 Bytes [PASS]| Temp: 48.2°C |
+==============================================================================+
```

---

## 8. 결론 및 향후 계획

본 안정성 및 벤치마크 사양서에 정의된 규칙에 따라:
1. 하드웨어 물리 통신을 일절 방해하지 않는 **독립 샌드박스 벤치마크 모듈**을 구성한다.
2. 각 구간별 사이클을 측정하여 CPU 6% ➡️ 9%의 원인이 **(1) 언어 추상화 비용(span/codegen regression)**인지, **(2) 동기화/메모리 오더 비용**인지, **(3) 계측 방식 왜곡**인지 확정한다.
3. 규명된 병목 지점을 집중 최적화(IRAM 배치, 메모리 오더 완화, 인라인 강제 등)하여 **C++23 표준 준수와 6% 미만의 극저 CPU 점유율**을 동시에 달성한다.
