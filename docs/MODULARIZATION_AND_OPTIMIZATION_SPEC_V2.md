# M5Stack AtomS3 Lite RS485 Gateway
## 모듈 분리 + 실행경로 최적화 통합 설계서 v2.0

---

## 0. 통합 배경

기존에 제시되었던 두 가지 아키텍처 및 리팩터링 방향을 하나로 통합 정규화한다.

* **v1.0 (안정성 우선)**: 책임별 물리적 모듈 분리. 파일 분리 자체는 성능에 직접적인 영향을 주지 않는다는 전제 하에 유지보수성 확보를 1차 목표로 설정.
* **v1.x (최적화 우선)**: Hot Path / Management Path를 물리적으로 분리하고, Lock·`memcpy`·Task·Queue 관점에서 실행 경로 자체를 최적화.

### 통합 원칙
두 방향은 상호 배타적이지 않으며, 모듈 분리는 최적화를 가능하게 하는 전제조건이고 최적화는 모듈 경계를 결정하는 기준이 된다.

```
[구조 확보] 책임별 물리적 분리 
   └──> [분석] Hot / Warm / Cold Path 식별 
           └──> [최적화] Hot Path 중심 복사·락·할당·깨우기 횟수 최소화 
                   └──> [검증] 365/24/7 무중단 기준 전체 무결성 검증
```

---

## 1. 우선순위 원칙

> [!IMPORTANT]
> 최적화(5순위)는 **안정성(1~4순위)**을 절대 침해하지 않는 범위 내에서만 수행한다. 파일 분리 자체는 5순위를 달성하기 위한 수단으로 정의한다.

| 우선순위 | 목표 | 설명 |
| :---: | :--- | :--- |
| **1** | **365/24/7 런타임 안정성** | 무중단 지속 가동 보장, WDT 리셋 0건 |
| **2** | **기존 동작 완전 보존** | 기능적 동작 회귀(Regression) 절대 불가 |
| **3** | **동시성/락 구조 보존** | 레이스 컨디션 및 데드락 방지 |
| **4** | **메모리 사용 및 스택 안정성 보존** | 힙 단편화 방지, 스택 오버플로우 방지 |
| **5** | **실행 경로(Hot Path) 최적화** | 복사·락 점유시간·할당·컨텍스트 스위칭 최소화 |
| **6** | **코드 책임 분리** | 도메인별/역할별 명확한 소유권 확립 |
| **7** | **컴파일/디버깅 범위 축소** | 모듈화로 인한 빌드 격리 및 문제 추적성 향상 |
| **8** | **향후 유지보수성 향상** | 코드베이스 가독성 및 확장성 확보 |

---

## 2. 절대 변경 금지 영역 (Invariant Core)

모듈 분리 및 최적화 전 과정에서 아래 항목은 절대 임의 수정·재설계하지 않는다.

* **Architecture 변경 금지** ❌
* **Behavior / State Machine 변경 금지** ❌
* **Feature 신규 추가 금지** ❌
* **Queue / QueueSet 재설계 금지** ❌
* **Task 구조 / 스케줄링 재설계 금지** ❌

### 고정 대상 목록
1. **Task & 스케줄링**: Task 생성 구조, Core affinity, 기존 Priority
2. **동기화 & IPC**: Queue 구조, QueueSet, Mutex 개수 및 기본 보호 범위, Semaphore, Atomic 구조
3. **통신 프로토콜**: RS485 프로토콜, 패킷 포맷 (Packet format), UART 설정 (9600bps 등), TCP 포트
4. **타이밍 & 정책**: 기존 timeout, WDT 정책, Retry 정책, Rate limit, Hub failover 정책
5. **외부 인터페이스**: Telnet 인증 방식, 기존 CLI 명령어 세트, 기존 SmartThings 채널 및 Device 매핑, 기존 ControlTemplate
6. **메모리 & 객체**: 기존 전송 우선순위, 기존 메모리 풀(Pool), 기존 전역 객체 및 Public API

> 이번 작업은 **Physical Modularization + Hot Path Tightening**이며, 신규 아키텍처를 도입하는 작업이 아니다.

---

## 3. 도메인 기준 물리 분리 아키텍처

Realtime Domain(Core 1 위주)과 Management Domain(Core 0 위주)을 코드 레벨과 코어 배치에서 명확히 분리한다.

```
                              main.cpp
                                 │
                    ┌────────────┴────────────┐
                    ▼                          ▼
             REALTIME DOMAIN              MANAGEMENT DOMAIN
              (Core 1 위주)                (Core 0 위주)
                    │                          │
          ┌─────────┼─────────┐        ┌───────┼────────┐
          ▼         ▼         ▼        ▼       ▼        ▼
        UartRx     CH1      CH23    Telnet    CLI    Mgmt/RPC
          │         │         │        │       │        │
          ▼         ▼         ▼        ▼       ▼        ▼
        Parser   Control   Forward  Server  Commands   OTA
          │       Polling     │     Auth               Telemetry
          │         │         │     Tracer
          └─────────┼─────────┘     Wizard
                     ▼
              기존 Queue/QueueSet
                     │
                     ▼
                   UartTx  →  TCP/Hub(Network)
```

* **Realtime Domain 목표**: 최저 Latency, 최저 Copy, 최소 Lock 점유, Zero Heap Allocation
* **Management Domain 목표**: Realtime Domain 간섭 원천 차단 (성능보다 격리와 비간섭이 우선)

---

## 4. 최종 디렉터리 및 파일 구조

> [!NOTE]
> 파일 분리의 기준은 단순 라인 수가 아닌 **책임과 Path의 성격**이다. 응집도가 높은 모듈(예: `ControlTemplate.cpp`)은 라인 수가 많더라도 분리하지 않고 유지한다.

```
src/
│
├── Core/                       ← 핵심 설정 및 시스템 상태
│   ├── Common.h
│   ├── Config.h
│   ├── Globals.cpp
│   └── SystemHealth.cpp
│
├── Engine/                     ← Hot Path (최우선 최적화 대상)
│   ├── Engine.cpp              (Task 생성/초기화/Glue 전용, 비즈니스 로직 배제)
│   ├── UartRx.cpp              (수신 Hot Path)
│   ├── UartTx.cpp              (송신 Hot Path)
│   ├── Ch1Engine.cpp           (CH1 상태 관리 및 제어)
│   ├── Ch1Polling.cpp          (CH1 폴링 루프, 동일 Task 내부 유지)
│   └── Ch23Engine.cpp          (CH2/CH3 바이패스/포워딩, Warm Path)
│
├── Network/                    ← Warm Path
│   ├── HubManager.cpp          (SmartThings Hub 연결/Failover)
│   └── NetworkManager.cpp      (이더넷/Wi-Fi 네트워크 생명주기)
│
├── Control/                    ← 고응집 도메인 (기존 구조 유지)
│   ├── ControlTemplate.cpp
│   └── ControlRegistry.cpp
│
├── Telnet/                     ← Cold Path
│   ├── TelnetCli.cpp           (CLI 공통 프레임워크 및 헬퍼)
│   ├── TelnetServer.cpp        (TCP 소켓 수명주기)
│   ├── TelnetAuth.cpp          (패스워드/인증 관리)
│   ├── TelnetTracer.cpp        (Packet Trace, 내부 Ring Buffer 상태 유지)
│   └── TelnetWizard.cpp        (대화형 설정 마법사 FSM)
│
├── CLI/                        ← Cold Path
│   ├── CliCommands.cpp         (커맨드 등록/디스패치/공통 유틸)
│   ├── CliSystem.cpp           (시스템 제어/상태 확인 명령)
│   ├── CliNetwork.cpp          (네트워크/허브 설정 명령)
│   ├── CliConfig.cpp           (NVS 설정 및 조회 명령)
│   ├── CliControl.cpp          (장치 제어 및 테스트 명령)
│   └── CliOta.cpp              (OTA 펌웨어 업데이트 명령)
│
├── Management/                 ← Cold Path
│   ├── MgmtRpc.cpp             (RPC 원격 관리)
│   ├── HttpOta.cpp             (HTTP 기반 OTA)
│   └── Telemetry.cpp           (원격 텔레메트리 전송)
│
└── main.cpp                    (진입점)
```

---

## 5. Hot / Warm / Cold Path 정의 및 처리 기준

| 구분 | 대상 모듈 | 최적화 목표 | 허용되지 않는 것 (금지 사항) |
| :---: | :--- | :--- | :--- |
| **Hot** | `UartRx`<br>`UartTx`<br>`Ch1Engine`<br>`Ch1Polling` | • 최저 지연 시간 (Latency)<br>• 최소 복사 (`memcpy`)<br>• 최소 락 (Lock hold time 축소)<br>• 무할당 (Zero dynamic allocation) | • `String` 객체 사용 금지<br>• 동적 힙 할당 (`new`, `malloc`) 금지<br>• `printf`, Logging 직접 I/O 호출 금지<br>• 패킷 처리 중 동일 심볼 반복 Lookup 금지 |
| **Warm** | `Ch23Engine`<br>`HubManager`<br>`NetworkManager` | • 처리량(Throughput) 확보<br>• 연결 안정성 보장 | • 불필요한 패킷 이중 복사<br>• 과도한 런타임 Logging |
| **Cold** | `Telnet`<br>`CLI`<br>`Wizard`<br>`Tracer`<br>`OTA`<br>`RPC`<br>`Telemetry` | • **Realtime Domain 간섭 최소화**<br>• 백그라운드 안전성 | • Hot Path에 대한 동기 블로킹(Blocking) 호출<br>• 긴 Critical Section 점유 |

---

## 6. 최적화 우선순위 및 세부 원칙

### 최적화 우선순위
1. 실시간 데이터 경로(UART → Parser → CH1 → Control/Polling → Queue → TCP) 최적화
2. 불필요한 `memcpy` 및 중복 Parsing 제거
3. Lock Contention 최소화 (순서 변경이 아닌 Hold Time 축소)
4. Queue / Task 불필요한 Wake-up 최소화
5. Heap Allocation / Fragmentation 제거
6. Logging / Telnet I/O를 Hot Path에서 완전 격리
7. CPU 사용량 최적화
8. 코드 모듈화 및 가독성 확보

### 6.1 세부 원칙
* **`UartRx.cpp`**: 수신 바이트가 가장 짧은 경로로 검증된 유효 패킷이 되도록 처리. Telnet, Syslog, `String`, 동적 할당, 무거운 Logging 호출 일체 금지.
* **`UartTx.cpp`**: 기존 `TxGuard`, `Priority Queue`, `AdaptiveBusController`를 온전히 유지하며 불필요한 복사 및 대기 지연만 축소.
* **`Ch1Engine.cpp`**: `DeviceDescriptor* desc = DeviceCatalog::find(...)`는 최초 1회만 획득 후 재사용. 동일 패킷 처리 루프 내 반복 검색 금지.
* **`Ch1Polling.cpp`**: 제어 로직과 소스 코드는 분리하되, 반드시 **동일 Task 내부**에서 실행 (신규 Task 생성으로 인한 컨텍스트 스위칭 오버헤드 금지).
* **`TelnetTracer.cpp`**: 패킷 포맷팅, 디바이스 파싱, 딜레이 계산, 트래커 상태 연산은 Hot Path에서 제외. Hot Path는 최소한의 Trace 이벤트만 생성하며, 실제 포맷 및 소켓 전송은 Tracer가 전담. 기존 트래커(`s_ch1_tracker`, `s_wp_tracker[]` 등) 상태는 함수 내부 `static`으로 유지(전역 오염 방지).
* **Logging 경로**: `Realtime` → `최소 이벤트` → `Log/Trace Queue/Buffer` → `LogManager` → `Format` → `Syslog/Telnet`. 기존 `LogManager` 파이프라인을 재사용하고 독자 서브시스템 신설을 금지.
* **Lock 관리**: `Lock` → `공유 상태 변경` → `Unlock` → `(필요시) 느린 I/O` 순서를 준수하되, 실제 보호 대상 상태를 검증한 후 Hold Time만 단축 (무단 Unlock 위치 변경 금지).
* **`memcpy` 최소화**: RX → Queue → Processing → TCP 경로 중 불필요한 중복 복사만 제거. 안정성을 해치는 무리한 Zero-copy 소유권 모델 도입 지양.
* **PacketCodec / Builder**: 고정 길이 패킷 구조에 맞춘 Static 고정 버퍼 및 Direct Field Access 유지 (`std::vector`, 범용 직렬화 도입 금지).
* **Queue & Task/Core**: 실측 병목이 확인되지 않은 단순 단계 축소 리팩터링 금지. `Core 1 = RS485/CH1 Realtime`, `Core 0 = TCP/Telnet/Syslog/Management` 배치 불변.

---

## 7. Header 및 Linkage 원칙

* **헤더 기계적 신설 금지**: `.cpp` 분리마다 매번 `.h`를 만들지 않는다. 기존 클래스(예: `TelnetManager`)가 `TelnetCli.h`에 선언되어 있다면 유지하고 분리된 `.cpp`에서 메서드만 구현한다.
* **인터페이스 전용 헤더**: Header는 모듈 간 공개 인터페이스 제공을 위해서만 존재하며 파일 분리 편의를 위해 남발하지 않는다.
* **내부 함수 격리**: 타 모듈에서 참조되지 않는 내부 함수는 `static` 또는 익명 네임스페이스(`namespace { ... }`)로 한정하여 심볼 오염을 방지한다.
* **기존 전역 객체 보존**: `g_telnet_manager`, `g_restart_pending` 등의 기존 전역 객체는 싱글톤(Singleton)이나 DI 구조로 재설계하지 않는다.
* **공유 Mutex 보존**: 여러 모듈이 공유하는 Mutex(예: `_cli_mutex`)는 분리 후에도 단일 Mutex로 공유하며 모듈별 분할 생성을 금지한다.

---

## 8. 이번 프로젝트에서 하지 않을 것 (Explicit Anti-Patterns)

* ❌ **신규 Manager 남발**: `Ch1Manager`, `PollingManager`, `UartManager` 등 불필요한 관리자 클래스 신설 금지 (기존 `TelnetManager` 등은 유지)
* ❌ **Interface 계층 도입**: `IEngine`, `ITelnet`, `IUart`, `IControl` 등 가상 함수 기반 추상화 금지
* ❌ **Dependency Injection**: 의존성 주입 프레임워크 및 동적 바인딩 도입 금지
* ❌ **Event Bus 도입**: 기존 검증된 FreeRTOS Queue를 대체하는 신규 이벤트 버스 도입 금지
* ❌ **Queue 구조 재설계**: 특히 검증 완료된 CH1 Queue 토폴로지 변경 금지
* ❌ **Task 재배치 및 신규 Task 생성**: 스케줄링 간섭 유발 금지
* ❌ **무리한 양방향 의존성 제거**: 실익 없는 구조 리팩터링 배제

---

## 9. 통합 작업 단계 (Phase 0 ~ Phase 10)

```
                    기존 안정 구조
                         │
                         ▼
              ┌────────────────────┐
              │ Phase 0  Baseline 고정  │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 1  책임별 물리 분리 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 2  분리 검증 (Delta=0) │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 3  Hot Path 분석·최적화 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 4  중복 연산/lookup 제거 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 5  Memory Path 최적화 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 6  Lock 최적화 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 7  Logging 경로 격리 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 8  Management Path 정리 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 9  Git 단위 커밋/역추적 │
              └─────────┬──────────┘
              ┌─────────▼──────────┐
              │ Phase 10 365/24/7 최종 검증 │
              └─────────┬──────────┘
                         ▼
                    최종 안정화 버전
```

### Phase 세부 실행 절차
* **Phase 0 — Baseline 고정**: 현재 안정 빌드의 Firmware size, Free/Min free heap, Task별 Stack watermark, Task count, Queue 상태, UART/TCP/Telnet 동작 기준 기록.
* **Phase 1 — 물리 분리 (로직 변경 0%, 파일만 이동)**:
  1. `TelnetTracer.cpp` 분리 (효과 높음, 위험 낮음)
  2. `TelnetWizard.cpp` 분리
  3. `TelnetAuth.cpp` 분리
  4. `TelnetServer.cpp` 분리
  5. `TelnetCli.cpp` 정리 (공통 CLI 인터페이스만 잔류)
  6. `UartRx.cpp` / `UartTx.cpp` 분리
  7. `Ch1Engine.cpp` 분리
  8. `Ch1Polling.cpp` 분리
  9. `Ch23Engine.cpp` 분리
  10. `CliCommands.cpp` → `CliSystem`, `CliNetwork`, `CliConfig`, `CliControl`, `CliOta` 분리
  11. `MgmtRpc.cpp` → `HttpOta`, `Telemetry` 분리
* **Phase 2 — 분리 무결성 검증**: 동작 Delta 0 확인 (패킷, 타이밍, 큐, 태스크, 뮤텍스, 메모리, WDT 전수 동일 확인).
* **Phase 3~8 — 단계별 최적화 (Hot → Warm → Cold)**: 6장의 세부 원칙을 UART RX/TX → CH1 → CH1 Polling → CH2/CH3 → Tracer/Logging → Telnet/CLI 순서로 적용.
* **Phase 9 — Git 단위 커밋 및 역추적성 확보**: 각 분리 및 최적화 단계마다 1 커밋 원칙 적용 (이슈 발생 시 즉각 롤백 및 역추적 가능 보장).
* **Phase 10 — 365/24/7 장기 런타임 최종 검증**: UART/TCP/SmartThings Polling/Telnet 재연결/CLI/Syslog/WDT 대상 장시간 실가동 후 리소스 지표 비교.

---

## 10. 단계별 공통 검증 기준

| 구분 | 검증 항목 | 합격 기준 |
| :--- | :--- | :--- |
| **Compile** | 빌드 무결성 | 0 Error, Baseline 대비 신규 Warning 0건 |
| **Runtime** | 기능 가동 | UART RX/TX, RS485 Polling, TCP RX/TX, Telnet, SmartThings 통신 100% 정상 |
| **Stability** | 시스템 안정성 | WDT Reset 0건, Heap Leak 0건, Stack Watermark 악화 없음, Queue Overflow 증가 없음, Socket Leak 0건 |
| **Functional** | 프로토콜 일치성 | Packet 포맷, 전송 타이밍, 내부 State Machine 전이, Response 처리, Timeout, Retry 정책이 Baseline과 완벽 일치 |

---

## 11. 최종 정량 평가 기준 (가중치)

| 평가 항목 | 중요도 | 비고 |
| :--- | :---: | :--- |
| **UART Latency** | ★★★★★ | RS485 수신/송신 즉시성 |
| **CH1 Processing Latency** | ★★★★★ | 핵심 채널 패킷 디코딩 및 처리 반응성 |
| **Lock Contention** | ★★★★★ | Critical section 점유 시간 최소화 |
| **`memcpy` 감소** | ★★★★★ | Hot Path 내 불필요한 데이터 복사 축소 |
| **Heap 안정성** | ★★★★★ | 런타임 동적 할당 0 및 힙 단편화 방지 |
| **Queue 효율** | ★★★★☆ | Queue 복사 및 Wake-up 최적화 |
| **Task Scheduling** | ★★★★☆ | 코어별 Task 간섭 배제 |
| **Logging Isolation** | ★★★★☆ | Cold Path 로깅의 Hot Path 비간섭 |
| **TCP 효율** | ★★★☆☆ | 허브 및 소켓 버퍼 핸들링 |
| **Telnet/CLI 응답성** | ★★☆☆☆ | 운용자 편의 기능 |
| **코드 파일 크기 / 분할 수** | ★★☆☆☆ | 최적화를 위한 보조 지표 (단순 분할 목적 배제) |

---

## 12. 대상별 분리 및 최적화 우선순위 요약표

| 모듈 대상 | 분리 필요성 | 리팩터링 위험도 | 처리 시점 |
| :--- | :---: | :---: | :--- |
| **TelnetTracer** | 매우 높음 | 낮음 | Phase 1 (최우선 분리) |
| **TelnetWizard** | 매우 높음 | 중간 | Phase 1 (최우선 분리) |
| **TelnetAuth** | 높음 | 낮음 | Phase 1 |
| **TelnetServer** | 높음 | 중간 | Phase 1 |
| **UartRx / UartTx** | 매우 높음 | 중간 | Phase 1 분리 → Phase 3 최우선 최적화 |
| **Ch1Engine** | 매우 높음 | 중간 | Phase 1 분리 → Phase 3 최적화 |
| **Ch1Polling** | 높음 | 중간 | Phase 1 분리 → Phase 3 최적화 (동일 Task 내 유지) |
| **Ch23Engine** | 중~높음 | 낮음 | Phase 1 분리 → Phase 3 최적화 |
| **CliCommands 계열** | 매우 높음 | 낮음~중간 | Phase 1 후반 분리 |
| **MgmtRpc / OTA** | 중간 | 낮음 | Phase 1 후반 분리 |
| **ControlTemplate** | 낮음 | — | **기존 유지 (분리 대상 아님)** |

---

## 13. 결론

1. **구조와 최적화는 하나의 단일 파이프라인**: 먼저 안전하게 물리적으로 분리하고(Phase 1~2), 명확해진 도메인 경계 위에서 Hot Path부터 체계적으로 최적화한다(Phase 3~8).
2. **Hot Path 심장부 보호**: `UART → Parser → CH1 → Polling → UART TX` 경로에서는 **최소 Latency·Copy·Lock·Allocation** 원칙을 철저히 고수한다.
3. **Cold Path 완전 격리**: Telnet, CLI, Tracer, Syslog, OTA 등은 Realtime Domain에 절대 블로킹 영향을 주지 않도록 격리한다.
4. **검증된 기반 구조의 100% 보존**: 기존의 Queue, State Machine, TokenBucket, PacketCodec, Mutex/Atomic, Static Buffer는 온전히 유지하며 그 위에서 낭비 요소(중복 처리, 복사, 락 대기 지연)만 걷어낸다.
5. **안전한 변경 프로세스**: 매 단계마다 Baseline과 1:1 비교 검증을 수행하고 단위 커밋을 통해 즉각적인 역추적성을 보장한다.
