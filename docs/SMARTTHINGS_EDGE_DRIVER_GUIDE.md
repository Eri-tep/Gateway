# SmartThings Edge Driver 개발자 가이드 및 아키텍처 레퍼런스

> **문서 목적**: SmartThings 공식 개발자 아키텍처 문서 및 `~/Home/SmartThings` 저장소의 핵심 규칙/스킬을 기반으로, ESP32 RS-485 LAN Gateway Edge 드라이버의 아키텍처, 6-Card 멀티 컴포넌트 구조, 마스터 티커(Master Ticker) 패턴, 수명 주기(Lifecycle), 코딩 컨벤션 및 CLI 배포 절차를 정규화하여 정리한 영구 기술 문서입니다.

---

## 1. 아키텍처 및 런타임 원칙

SmartThings Edge 드라이버는 SmartThings Hub 로컬 환경에서 실행되는 임베디드 드라이버로, 클라우드 의존성 없이 로컬 네트워크 내에서 기기를 감시 및 제어합니다.

### 1.1 협력형 멀티태스킹 (`cosock`)
* **단일 스레드 비선점형 코루틴**: 허브의 Lua 5.3 런타임은 `cosock` 기반 코루틴으로 동작합니다.
* **비블로킹(Non-blocking) 규칙**:
  * ❌ `os.execute("sleep")`, `while true do end` 등 CPU를 독점하는 블로킹 루프는 절대 금지 (허브 내 전체 드라이버 동결 유발).
  * ✅ `cosock.socket` TCP 소켓 사용 시 반드시 타임아웃(`sock:settimeout(3)`)을 지정하여 통신 지연 시 즉시 제어를 반환.
  * ✅ 지연이 필요한 경우 `device.thread:call_with_delay(sec, func)` 또는 `socket.sleep(sec)` 사용.

### 1.2 단일 진실 공급원 (Single Source of Truth) 원칙
* 드라이버 내부 상태를 모듈 레벨 전역 변수(예: `local is_device_created = false`)에 의존하지 않습니다.
* 디바이스 인스턴스 검색 및 중복 검사는 항상 `driver:get_devices()` 컬렉션과 기기의 고유 식별자(`device.device_network_id`)를 기준으로 수행합니다.

### 1.3 불변성 및 이벤트 중복 방지 우회 (`state_change = true`)
* 순회 롤링(Master Ticker) 시 동일한 문자열이 연속해서 방출될 경우, SmartThings 플랫폼의 기본 중복 이벤트 필터에 의해 이벤트가 드롭되어 앱 UI가 갱신되지 않을 수 있습니다.
* 주기적으로 순회하는 텔레메트리 이벤트는 반드시 `cap_event.state_change = true`를 설정하여 방출합니다.

---

## 2. Gateway Ultra 6-Card UI 컴포넌트 아키텍처

`gateway-ultra` 프로필은 총 6개의 독립 컴포넌트(Component)로 구성되며, SmartThings 모바일 앱에서 각 컴포넌트가 하나의 카드(Card)로 렌더링됩니다.

```
┌─────────────────────────────────────────────────────────────┐
│ 1. main (Devices)        : 활성 디바이스 수 (Active Devices)    │
│                            [새로고침: refresh]               │
├─────────────────────────────────────────────────────────────┤
│ 2. wallpad (Wallpad)     : 월패드 프로토콜 규격 및 정보          │
│                            [스위치: Auto-Probing 리셋]        │
├─────────────────────────────────────────────────────────────┤
│ 3. diagnostics (Gateway) : 시스템 가동시간, 리소스, 트래픽 통계  │
│                            [스위치: 시스템 원격 리부트]         │
├─────────────────────────────────────────────────────────────┤
│ 4. logs (Logs)           : NVS 리부트 로그 & Panic 코어덤프     │
│                            [스위치: 로그 & 덤프 초기화]        │
├─────────────────────────────────────────────────────────────┤
│ 5. network (Network)     : Wi-Fi AP 스캔 결과, IP/모드 정보     │
│                            [스위치: Wi-Fi 주변 스캔 트리거]    │
├─────────────────────────────────────────────────────────────┤
│ 6. ota (Firmware)        : 펌웨어 버전 (현재 ➔ 최신), 빌드 상태│
│                            [스위치: GitHub 클라우드 OTA 실행]  │
└─────────────────────────────────────────────────────────────┘
```

### 2.1 컴포넌트 및 캐패빌리티 매핑

| 카드 ID | 카드 레이블 | 내장 스위치 액션 | 연동 Capability | 설명 |
|---|---|---|---|---|
| `main` | Devices | 수동 새로고침 (`refresh`) | `digituniverse06711.activeDevice`, `refresh` | 온라인 연결 기기 수 현황 (예: `23 / 23 Online`) |
| `wallpad` | Wallpad | Auto-Probing 리셋 | `switch`, `profile`, `frame`, `checksum`, `opcodes` | RS-485 프레임 구조(`F7 [LN] [SA] [DT] [OC] [CD] [ID] [PL] [CS] EE`), 프로토콜 파라미터 |
| `diagnostics` | Gateway | 게이트웨이 원격 재부팅 | `switch`, `uptime`, `resource`, `channel` | 시스템 Uptime, CPU/RAM/Flash 순회, CH1~4 패킷 통계 순회 |
| `logs` | Logs | 로그 및 코어덤프 삭제 | `switch`, `history` | 최근 리부트 사유 및 Crash CoreDump 10초 순회 |
| `network` | Network | Wi-Fi AP 재스캔 | `switch`, `scan`, `wifi` | 발견된 SSID 스캔 결과 순회, Wi-Fi SSID/RSSI & IP/Mode 순회 |
| `ota` | Firmware | Cloud OTA 업데이트 | `switch`, `version`, `build` | 펌웨어 버전(`v1.1.5 ➔ v1.1.6`), 빌드 안정성 상태(`Stable (Idle)`) |

### 2.2 모멘터리 액션 스위치 (Momentary Action Switch) 패턴
* 상위 5개 카드(wallpad, diagnostics, logs, network, ota)의 헤더 스위치는 누름 즉시 명령을 수행하고, **1.5초 후 자동으로 OFF 상태로 원복**되는 모멘터리 펄스 버튼 구조를 취합니다.
* 구현:
  ```lua
  device.thread:call_with_delay(1.5, function()
    if comp then
      device:emit_component_event(comp, capabilities.switch.switch.off())
    end
  end)
  ```

---

## 3. 마스터 티커 (Master Ticker) 디자인 패턴

### 3.1 문제점 및 설계 배경
* 각 카드마다 별도의 `call_with_delay` 재귀 타이머를 구동할 경우:
  1. 각 카드 간 순회 타이밍이 제각각 어긋나 화면 갱신이 산만해짐.
  2. 매 틱마다 새로운 익명 클로저가 생성되어 허브 Lua 런타임에 불필요한 GC 메모리 부하 발생.
  3. Wi-Fi 스캔 등 일시적 순회 항목 추가 시 기존 타이머와 충돌/누수 위험 발생.

### 3.2 정규 마스터 티커 구조
* **단일 정주기 타이머 (`call_on_schedule`)**:
  ```lua
  local TICKER_INTERVAL = 10 -- 10초 주기 정규화

  function TelemetryHandler.ensure_master_ticker(device)
    if device:get_field("master_roll_timer") then return end

    local timer = device.thread:call_on_schedule(TICKER_INTERVAL, function()
      local registry = device:get_field("ticker_registry") or {}
      for _, entry in pairs(registry) do
        if entry.items and #entry.items > 1 and entry.cap and entry.comp then
          entry.idx = (entry.idx % #entry.items) + 1
          emit_event(device, entry.comp, entry.cap[entry.attr_name]({ value = entry.items[entry.idx] }))
        end
      end
    end, "master_roll_timer")
    device:set_field("master_roll_timer", timer)
  end
  ```
* **장점**:
  1. 허브 상에서 단 하나의 타이머 스레드만 동작하므로 리소스 점유 최소화.
  2. 모든 카드의 항목이 **동일한 10초 정각 타이밍에 완벽히 동기화**되어 회전.
  3. 동적 리스트(Wi-Fi AP 스캔 결과)도 `register_ticker` 호출만으로 마스터 틱 주기에 자연스럽게 합류.

---

## 4. 드라이버 수명 주기 (Lifecycle) 모범 사례

```
[Discovery / Join] ──> added ──(자동 합성 이벤트)──> init ──> [정상 동작]
                                                       │
[드라이버 재시작 / Hub 재부팅] ───────────────────────────┘
                                                       │
[기기 환경설정 변경] ─────────────────────────> infoChanged
                                                       │
[기기 삭제] ───────────────────────────────────> removed
```

### 4.1 콜백 역할 분담
1. **`init`**:
   * 허브 재부팅, 드라이버 업데이트, 기기 페어링 직후 등 **항상 실행되는 핵심 진입점**.
   * 프로필 메타데이터 검증(`try_update_metadata`), 주기적 폴링 타이머 등록(`schedule_polling_timer`), 최초 텔레메트리 조회가 이루어져야 함.
2. **`added`**:
   * 기기가 최초 페어링되었을 때 **단 1회만 호출**.
   * SmartThings Edge 프레임워크는 `added` 완료 직후 **자동으로 합성 `init` 이벤트를 큐잉**하므로, `added`에서 폴링 타이머나 중복 텔레메트리 조회를 실행하지 않음.
3. **`infoChanged`**:
   * 모바일 앱의 기기 설정(Preferences)이 변경되었을 때 호출.
   * `args.old_st_store.preferences`와 `device.preferences`를 비교하여 변경된 항목만 선별적으로 게이트웨이에 반영.
4. **`removed`**:
   * 기기가 삭제될 때 등록된 모든 타이머(`poll_timer`, `master_roll_timer`)를 명시적으로 취소(`cancel_timer`).

---

## 5. 코드 스타일 및 품질 표준 (Clean Code & Linting)

`~/Home/SmartThings` 표준 규칙에 따른 필수 준수 사항:

1. **들여쓰기**: 2 스페이스 (탭 금지).
2. **모듈 임포트**: `require "module"` 형태 (반드시 큰따옴표 사용).
3. **네이밍**: 변수/함수는 `snake_case`, 상수는 `UPPER_SNAKE_CASE`.
4. **모듈 반환**: 파일 최하단에서 테이블을 단일 반환 (`return ModuleName`).
5. **데드 코드 금지 (Zero Dead Code Policy)**:
   * 프로필 및 `capability_handlers`에 등록되지 않은 핸들러(과거 잔재)는 즉시 제거.
   * 사용되지 않는 매핑 테이블이나 유틸리티 함수는 방치하지 않고 정리.
6. **버그 방지**:
   * 제거되거나 변경된 구형 Capability ID(예: `logHistory`, `crashDump`)를 코드에 방치하지 않고 최신 스키마(`history`)와 동기화.

---

## 6. SmartThings CLI 배포 및 운영 워크플로우

### 6.1 단일 단계 원라이너 배포 (Recommended)
드라이버 패키징, 채널 할당, 허브 설치를 단일 명령으로 즉시 실행:
```bash
smartthings edge:drivers:package "/Users/eri/Library/CloudStorage/OneDrive-개인/Home/Gateway/Gateway-edge-driver" \
  --channel="5c5ac2ac-84fb-4783-82df-da58cc675f41" \
  --hub="b65b1792-8510-423f-b12d-00d7ff78b700"
```
> **참고**: macOS 환경에서 SmartThings CLI는 `~/Library/Logs/@smartthings/cli`에 로그를 기록하므로, 샌드박스 우회(`BypassSandbox: true`)가 요구됩니다.

### 6.2 실시간 로그 모니터링 (Logcat)
```bash
smartthings edge:drivers:logcat bcfbdfbd-891b-4cf3-a969-df9b237e1f7b \
  --hub=b65b1792-8510-423f-b12d-00d7ff78b700
```

### 6.3 배포 정보 요약
* **Hub ID**: `b65b1792-8510-423f-b12d-00d7ff78b700`
* **Channel ID**: `5c5ac2ac-84fb-4783-82df-da58cc675f41` (Eri)
* **Driver ID**: `bcfbdfbd-891b-4cf3-a969-df9b237e1f7b`
* **Package Key**: `com.gateway.smartthings.controller`
* **Device Profile**: `gateway-ultra`
