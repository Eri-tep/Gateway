# GW Home Modern C++ Style & Safety Guidelines

본 문서는 `GW Home` 펌웨어(v2.0.6+)의 **메모리 안전성, 타입 안전성, 실시간 성능 및 모던 C++(C++17) 표준 코딩 규칙**을 정의한 공식 가이드라인입니다.

---

## 1. 문자열 및 프로토콜 식별자 비교 표준 (String & Key Comparison)

- 프로토콜 키, 프로파일 식별자 등 대소문자 구분이 불필요하거나 NVS/CLI 입력과 혼용될 수 있는 식별자는 **`strcasecmp`로 통일**하여 비교합니다.
- 특정 프로토콜 엔진 판별(예: `"auto"`)은 인라인 헬퍼 함수(`isAutoProfile(desc)`)로 캡슐화하여 단일 지점에서 관리합니다.

```cpp
// ❌ BAD: 함수마다 strcmp / strcasecmp 혼용 및 직접 비교
if (strcmp(desc.key, "auto") == 0) { ... }
if (strcasecmp(desc.key, "auto") == 0) { ... }

// ✅ GOOD: 전용 헬퍼 함수로 단일화
inline bool isAutoProfile(const VendorProfileDescriptor &desc) const {
  return strcasecmp(desc.key, "auto") == 0;
}
```

---

## 2. 버퍼 포맷터 DRY 표준 (AppendBuf & Formatter Utilities)

- 가변인자 포맷팅(`appendFormat`, `operator()`)을 제공하는 버퍼 구조체는 **`vsnprintf` 로직을 중복 구현하지 않고 `va_list` 기반의 단일 private 헬퍼(`appendFormatV`)에 위임**합니다.
- 버퍼 오버플로우 방지를 위해 `offset + n < cap` 클램핑을 철저히 유지합니다.

```cpp
// ✅ GOOD: va_list 전용 헬퍼에 가변인자 위임
struct AppendBuf {
  char *buf;
  size_t cap;
  size_t offset = 0;

private:
  void appendFormatV(const char *fmt, va_list a) {
    if (offset >= cap) return;
    int n = vsnprintf(buf + offset, cap - offset, fmt, a);
    if (n > 0) offset = std::min(offset + (size_t)n, cap - 1);
  }

public:
  void appendFormat(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list a; va_start(a, fmt); appendFormatV(fmt, a); va_end(a);
  }
  void operator()(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list a; va_start(a, fmt); appendFormatV(fmt, a); va_end(a);
  }
};
```

---

## 3. 열거형(Enum) 및 타입 별칭 중복 정의 금지 (No Redundant Aliases)

- `enum class` 내 동일한 정수 값을 가리키는 중복 이름(별칭)을 두지 않습니다.
- 코드베이스 전체에서 단 하나의 명확한 표준 식별자만 사용합니다.

```cpp
// ❌ BAD: 동일한 0 슬롯에 대해 AUTO와 ADAPTIVE 혼용 정의
enum class WallpadProfileIndex : uint8_t {
  AUTO = 0,
  ADAPTIVE = 0,
  CUSTOM1 = 1
};

// ✅ GOOD: 단일 명확한 식별자 유지
enum class WallpadProfileIndex : uint8_t {
  ADAPTIVE = 0,
  CUSTOM1 = 1,
  CUSTOM2 = 2,
  CUSTOM3 = 3,
  COUNT = 4
};
```

---

## 4. `span` 폴리필 및 안전한 버퍼 뷰 활용

- C 스타일 고정 크기 배열(`T (&arr)[N]`) 전체를 `span`으로 변환할 때는 템플릿 C-array 생성자를 통해 크기를 자동 추론합니다.
- 단, 동적 길이(`len`)를 사용하는 부분 버퍼는 명시적 포인터+길이 생성자 `span<T>(ptr, len)`를 유지합니다.

```cpp
// ✅ GOOD: C-array 템플릿 생성자 지원
template <typename T> class span {
  ...
  template <size_t N>
  constexpr span(T (&arr)[N]) noexcept : ptr_(arr), len_(N) {}
};
```

---

## 5. C++17 중첩 네임스페이스 축약 문법 (Nested Namespaces)

- C++17 표준에 따라 다단계 중첩 네임스페이스는 들여쓰기 낭비 없이 `namespace A::B { ... }` 축약 문법을 사용합니다.

```cpp
// ❌ BAD: 레거시 C++98/03 중첩 들여쓰기
namespace Config {
  namespace Task {
    constexpr size_t STACK_SIZE = 8192;
  }
}

// ✅ GOOD: C++17 축약 네임스페이스
namespace Config::Task {
constexpr size_t STACK_SIZE = 8192;
}
```

---

## 6. 동기화 객체 무명 임시객체 생성 절대 금지 (RAII Lock Safety)

- `CriticalSectionLocker`, `MutexLocker` 등의 RAII 동기화 객체는 **반드시 스코프 내 지역 변수 이름을 명시하여 생성**해야 합니다.
- 이름을 지정하지 않은 무명 임시객체는 생성된 라인(Line)에서 즉시 소멸(Unlock)되어 심각한 동시성 버그를 유발합니다.

```cpp
// 🚨 CRITICAL ERROR: 한 줄 만에 즉시 락이 해제됨!
CriticalSectionLocker(&_mux); 
shared_variable++; // 동기화 보호 전혀 안 됨!

// ✅ GOOD: 스코프가 끝날 때까지 락 유지
CriticalSectionLocker lock(&_mux);
shared_variable++;
```

---

## 7. C 라이브러리 포인터의 RAII 스마트 포인터화 (Memory Leak Prevention)

- `EmbeddedCli*` 등 C 라이브러리의 동적 할당 리소스는 Raw 포인터로 방치하지 않고, **커스텀 딜리터(Custom Deleter)를 갖춘 `std::unique_ptr`**로 관리합니다.
- 상태가 없는 빈 구조체 딜리터는 EBO(Empty Base Optimization)에 의해 포인터 크기(4바이트) 외 추가 RAM 오버헤드가 0바이트입니다.

```cpp
// ✅ GOOD: Custom Deleter + std::unique_ptr
struct CliDeleter {
  void operator()(EmbeddedCli *p) const noexcept {
    if (p) embeddedCliFree(p);
  }
};

struct TelnetSession {
  std::unique_ptr<EmbeddedCli, CliDeleter> cli;

  void reset() {
    ...
    cli.reset(); // 자동 안전 해제
  }
};
```

---

## 8. 핫패스 메타데이터 접근 헬퍼화 (Hot-path Optimization)

- 통신 루프나 패킷 파싱 등 고빈도로 호출되는 핫패스에서는 전역 저장소 접근 코드를 인라인 헬퍼(`activeProfile()`)로 캡슐화합니다.
- 향후 프로필 변경 감지 에포크(`s_profile_epoch`) 기반 캐싱(8-B)을 도입할 수 있도록 단일 진입점을 보장합니다.

```cpp
class UniversalProtocolEngine : public IWallpadParser {
private:
  inline VendorProfileDescriptor activeProfile() const {
    VendorProfileDescriptor d;
    ProfileRepository::getActiveProfile(d);
    return d;
  }
};
```
