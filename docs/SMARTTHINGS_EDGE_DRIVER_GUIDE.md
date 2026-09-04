# SmartThings Edge Driver 개발자 가이드 및 레퍼런스

> **문서 목적**: SmartThings 공식 개발자 문서(Edge Device Driver Reference v20) 및 공식 가이드를 바탕으로, 본 RS-485 LAN Gateway Edge 드라이버 개발/유지보수에 필요한 모든 핵심 API, 아키텍처 규칙, 프로파일 스키마, 수명 주기 및 CLI 배포 절차를 단일 문서로 종합 정리한 레퍼런스입니다.

---

## 1. 아키텍처 및 런타임 환경

### 1.1 협력형 멀티태스킹 (`cosock`)
* **단일 스레드 비선점형 코루틴**: SmartThings Edge Hub의 Lua 5.3 런타임은 `cosock` 라이브러리를 기반으로 동작합니다.
* **비블로킹 규칙**:
  * ❌ `os.execute("sleep")`, `while true do end` 같은 블로킹/비지루프 절대 금지 (전체 드라이버 동결)
  * ✅ `local socket = require "cosock.socket"` → `socket.sleep(seconds)` 로 코루틴 제어 양보
  * ✅ 백그라운드 태스크는 `local cosock = require "cosock"` → `cosock.spawn(func, name)` 으로 생성

### 1.2 드라이버 싱글톤 및 수명 주기
* 허브에서 단 하나의 드라이버 인스턴스가 실행되며, 모든 부모/자식 기기를 단일 Lua 프로세스에서 관리합니다.
* `Driver:run()` 호출 시 이벤트 루프가 시작되어 등록된 소켓 채널, 타이머, 플랫폼 메시지를 수신합니다.

---

## 2. 드라이버 패키지 구조

```
smartthings-edge/
├── config.yaml                     # [필수] 드라이버 메타데이터 및 허브 권한
├── profiles/                       # [필수] 디바이스 프로파일 정의 (YAML)
│   ├── gateway-bridge.v1.yaml      # 부모 브릿지 Hub 기기
│   ├── light-switch.v1.yaml        # 조명 기기
│   ├── smart-outlet.v1.yaml        # 콘센트 기기 (전력 측정)
│   ├── thermostat.v1.yaml          # 난방 조절기
│   ├── ventilation.v1.yaml         # 전열교환기
│   ├── induction-breaker.v1.yaml   # 인덕션 차단기
│   └── elevator.v1.yaml            # 엘리베이터 호출기
├── src/                            # [필수] Lua 소스 코드
│   ├── init.lua                    # 메인 진입점 (Driver 생성 및 run)
│   ├── discovery.lua               # Discovery 핸들러 (브릿지 기기 검색/생성)
│   ├── lifecycle.lua               # 기기/드라이버 라이프사이클 콜백
│   ├── child_manager.lua           # 24대 자식 기기 매핑 및 동적 생성/관리
│   ├── connection.lua              # TCP 소켓 연결/재연결/수신 코루틴
│   ├── frame_parser.lua            # RS-485 F7~EE 프레이밍 및 누적 버퍼
│   ├── packet_builder.lua          # 제어 명령 패킷 빌더 및 XOR 체크섬
│   ├── energy_manager.lua          # 실시간 W -> 누적 kWh 적산 및 월초 리셋
│   └── sub_drivers/                # SubDriver 모듈 (기기 타입별 분리)
│       ├── light.lua
│       ├── outlet.lua
│       ├── thermostat.lua
│       ├── ventilation.lua
│       ├── induction.lua
│       └── elevator.lua
└── locales/                        # [선택] 다국어 레이블
    └── ko.yaml
```

---

## 3. 핵심 API 명세

### 3.1 `st.driver` (드라이버)
* **생성**:
  ```lua
  local Driver = require "st.driver"
  local driver = Driver("RS485GatewayDriver", {
    discovery = discovery_handler,
    lifecycle_handlers = {
      init = on_init,
      added = on_added,
      doConfigure = on_configure,
      infoChanged = on_info_changed,
      driverSwitched = on_driver_switched,
      removed = on_removed,
    },
    capability_handlers = { ... },
    sub_drivers = { sub_light, sub_outlet, ... },
    driver_lifecycle = on_driver_lifecycle,
  })
  driver:run()
  ```
* **동적 기기 생성 (`driver:try_create_device`)**:
  ```lua
  -- LAN 부모 기기 생성 (Discovery 시)
  driver:try_create_device({
    type = "LAN",
    device_network_id = "RS485_GATEWAY_BRIDGE",
    label = "월패드 게이트웨이",
    profile = "gateway-bridge.v1",
    manufacturer = "CustomHome",
    model = "RS485-Bridge",
  })

  -- 자식 기기 생성 (EDGE_CHILD)
  driver:try_create_device({
    type = "EDGE_CHILD",
    parent_device_id = parent_device.id,
    parent_assigned_child_key = "light_11",
    label = "거실 조명 1",
    profile = "light-switch.v1",
    manufacturer = "CustomHome",
    model = "RS485-Light",
  })
  ```
* **타이머 API**:
  * `driver:call_with_delay(delay_sec, callback, name)` : 1회성 타이머
  * `driver:call_on_schedule(interval_sec, callback, name)` : 주기적 타이머
  * `driver:cancel_timer(timer_handle)` : 타이머 취소

---

### 3.2 `st.device` (기기 인스턴스)
* **필드 저장소 이원화**:
  * `device:set_field(key, val)` : 휘발성 RAM (`transient_store`). 소켓 객체, 콜백 함수, 코루틴 핸들 저장.
  * `device:set_field(key, val, { persist = true })` : 플래시 저장소 (`persistent_store`). **JSON 직렬화 가능한 데이터만 허용** (숫자, 문자열, 불리언, 순수 테이블). 에너지 누적량, 리셋 일자 등에 사용.
* **자식/부모 검색**:
  * `parent_device:get_child_by_parent_assigned_key(key)` : $O(1)$ 자식 기기 검색
  * `parent_device:get_child_list()` : 전체 자식 기기 배열 반환
  * `device:get_parent_device()` : 부모 기기 반환 (⚠️ `init`/`added` 내부에서 동기 호출 시 데드락 위험 있으므로 부모 ID 캐싱 권장)
* **온라인/오프라인 상태 관리**:
  * `device:online()` : 기기를 온라인으로 설정
  * `device:offline()` : 기기를 오프라인으로 설정 (⚠️ 부모 브릿지가 오프라인이 되면 모든 자식이 자동 오프라인 처리됨. 부모가 온라인 복귀 시 자식은 각각 `child:online()`을 호출해야 함)

---

### 3.3 `st.capabilities` (캐패빌리티 및 이벤트)
* **이벤트 발행 (`emit_event`)**:
  ```lua
  local capabilities = require "st.capabilities"

  -- 스위치
  device:emit_event(capabilities.switch.switch.on())
  device:emit_event(capabilities.switch.switch.off())

  -- 전력 및 에너지
  device:emit_event(capabilities.powerMeter.power(125)) -- 단위: W
  device:emit_event(capabilities.energyMeter.energy({ value = 14.52, unit = "kWh" }))

  -- 온도
  device:emit_event(capabilities.temperatureMeasurement.temperature({ value = 24.5, unit = "C" }))
  device:emit_event(capabilities.thermostatHeatingSetpoint.heatingSetpoint({ value = 22.0, unit = "C" }))
  device:emit_event(capabilities.thermostatMode.thermostatMode.heat())
  device:emit_event(capabilities.thermostatMode.thermostatMode.away())

  -- 팬 풍속 (0~3)
  device:emit_event(capabilities.fanSpeed.fanSpeed(2))

  -- 버튼 (엘리베이터 도착 알림 등)
  device:emit_event(capabilities.button.button.pushed({ state_change = true }))
  ```

---

### 3.4 `cosock.socket` (TCP 통신)
* **소켓 연결 및 송수신**:
  ```lua
  local socket = require "cosock.socket"

  local sock = socket.tcp()
  sock:settimeout(5) -- 5초 타임아웃
  local ok, err = sock:connect(ip, port)

  -- 데이터 전송
  sock:send(binary_string)

  -- 데이터 수신 (비블로킹 버퍼 읽기)
  sock:settimeout(0.5)
  local data, err, partial = sock:receive(1024)
  local chunk = data or partial
  ```

---

## 4. SubDriver 구조 패턴

SmartThings Edge는 기기별 로직을 `SubDriver`로 분리하여 모듈화할 수 있습니다:

```lua
-- src/sub_drivers/light.lua 예시
local capabilities = require "st.capabilities"
local log = require "log"

local function can_handle(opts, driver, device, ...)
  return device:get_field("device_type") == "light"
end

local function handle_switch_on(driver, device, command)
  -- 1. 제어 패킷 생성 및 부모 TCP 소켓으로 전송
  -- 2. 낙관적 이벤트 발행
end

local function handle_switch_off(driver, device, command)
  -- OFF 제어
end

return {
  NAME = "LightSubDriver",
  can_handle = can_handle,
  capability_handlers = {
    [capabilities.switch.ID] = {
      [capabilities.switch.commands.on.NAME] = handle_switch_on,
      [capabilities.switch.commands.off.NAME] = handle_switch_off,
    }
  }
}
```

---

## 5. 프로파일 YAML 스키마

### 5.1 Preferences (설정창) 스키마
* **필수 키**: `title`, `name`, `preferenceType`, `definition`
* **지원 타입**: `string`, `integer`, `number`, `boolean`, `enumeration`
* **예시**:
  ```yaml
  preferences:
    - title: "서버 IP 주소"
      name: serverIp
      description: "RS-485 TCP 서버의 IPv4 주소"
      required: true
      preferenceType: string
      definition:
        stringType: text
        minLength: 7
        maxLength: 15
        default: "172.30.1.3"
    - title: "통신 포트"
      name: serverPort
      description: "TCP 포트 번호"
      required: true
      preferenceType: integer
      definition:
        minimum: 1
        maximum: 65535
        default: 8899
  ```

---

## 6. SmartThings CLI 명령어 가이드

SmartThings CLI를 통한 드라이버 패키징 및 배포 표준 절차:

```bash
# 1. 드라이버 패키징
smartthings edge:drivers:package ./smartthings-edge

# 2. 채널 생성 (최초 1회)
smartthings edge:channels:create

# 3. 드라이버를 채널에 할당
smartthings edge:channels:assign <DRIVER_ID> --channel <CHANNEL_ID> --version <VERSION>

# 4. 스마트싱스 허브를 채널에 등록
smartthings edge:channels:enroll <CHANNEL_ID> --hub <HUB_DEVICE_ID>

# 5. 허브에 드라이버 설치
smartthings edge:drivers:install <DRIVER_ID> --channel <CHANNEL_ID> --hub <HUB_DEVICE_ID>

# 6. 실시간 허브 로그 모니터링
smartthings edge:drivers:logcat --hub-address=<HUB_LOCAL_IP>
```
