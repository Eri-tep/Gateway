// Supervisor.h - 태스크 생존성 슈퍼바이저
// P1: Shadow 모드 = 관측 / 범인·피해자 판정 / 기록만 수행하고 어떤 조치도 하지 않는다.
// 기존 TWDT(30초, panic)는 P1에서 전혀 건드리지 않는다.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef SUP_FAULT_INJECTION
#define SUP_FAULT_INJECTION 0  // 개발용 env 에서만 -DSUP_FAULT_INJECTION=1
#endif

// ── Task Identifier (Global L0 Foundation) ──
enum class SystemTaskId : uint8_t {
  CH1 = 0,
  CH2,
  CH3,
  CH4,
  NETWORK,
  TELNET,
  COUNT
};

namespace Supervisor {

inline constexpr uint8_t  kMaxTasks     = 8;
inline constexpr uint8_t  kNoTask       = 0xFF;
inline constexpr uint32_t kPeriodMs     = 100;  // 감시 주기
inline constexpr uint8_t  kConfirmCount = 3;    // 연속 N회 데드라인 초과 시에만 판정(오탐 방지)
inline constexpr uint8_t  kHogCpuPct    = 90;   // 한 코어 기준 점유율 임계값(%)

enum class Mode : uint8_t { Shadow, Active };

enum class Verdict : uint8_t {
  Healthy,
  CulpritHog,        // 범인: 실행 중이거나 CPU 점유(폭주/장시간 연산)
  CulpritSelfStall,  // 범인: 선언한 시간 안에 못 돌아옴(블로킹 I/O, 큐 대기 정지 등)
  VictimLock,        // 피해자: 락 대기 중. culprit = 락 보유자
  VictimStarved,     // 피해자: Ready 인데 CPU 를 못 받음. culprit = 같은 코어의 점유 태스크
  SystemWide,        // 다수 동시 누락 + 뚜렷한 범인 없음(힙 고갈, Wi-Fi/lwIP 정지 등)
};

struct TaskSpec {
  const char* name;
  uint32_t    deadline_ms;  // alive 데드라인. 초기값은 TaskWdtMonitor 의 최대 갭 실측 x3 이상으로
  BaseType_t  core;
};

struct Event {
  uint32_t t_ms;
  uint8_t  task;     // 하트비트를 놓친 태스크
  uint8_t  culprit;  // 범인(자기 자신이면 task 와 동일, 불명이면 kNoTask)
  Verdict  verdict;
  uint8_t  state;    // eTaskState
  uint8_t  cpu_pct;  // 판정 창 동안 한 코어 기준 점유율
};

extern std::atomic<uint32_t> g_lastFeedMs[kMaxTasks];

#if SUP_FAULT_INJECTION
enum class Inject : uint8_t { None, Spin, Block };
void injectPoint(uint8_t id);
void inject(uint8_t id, Inject kind, uint32_t ms);  // ms: 권장 5000 이하(기존 TWDT 30초 보호)
#endif

inline uint32_t nowMs() { return xTaskGetTickCount() * portTICK_PERIOD_MS; }

// System_FeedWdt() / TaskWdtMonitor::feed() 안에서 호출. 비용: relaxed store 1회.
inline void heartbeat(uint8_t id) {
#if SUP_FAULT_INJECTION
  injectPoint(id);
#endif
  if (id < kMaxTasks) g_lastFeedMs[id].store(nowMs(), std::memory_order_relaxed);
}

// System_RegisterTaskHandle() 안에서 호출. start() 보다 먼저 모두 등록할 것.
void registerTask(uint8_t id, TaskHandle_t h, const TaskSpec& spec);

void start(Mode mode);
void setMode(Mode m);
Mode mode();

// 정당한 장시간 작업(OTA, bench, wifi scan)은 지금처럼 TWDT 를 직접 먹이지 말고
// 이 유예로 선언한다. ms 동안 해당 태스크는 판정에서 제외된다.
void holdFor(uint8_t id, uint32_t ms);
void release(uint8_t id);

class DeadlineHold {
 public:
  DeadlineHold(uint8_t id, uint32_t ms) : id_(id) { holdFor(id_, ms); }
  DeadlineHold(SystemTaskId id, uint32_t ms) : id_(static_cast<uint8_t>(id)) { holdFor(id_, ms); }
  ~DeadlineHold() {
    release(id_);
    heartbeat(id_);
  }
  DeadlineHold(const DeadlineHold&)            = delete;
  DeadlineHold& operator=(const DeadlineHold&) = delete;

 private:
  uint8_t id_;
};

// MutexLocker 훅. 빠른 경로(xSemaphoreTake(s, 0) 성공)에서는 호출하지 말고,
// 실제로 블로킹될 때만 begin -> take(timeout) -> end 로 감싼다.
void noteWaitBegin(SemaphoreHandle_t s);
void noteWaitEnd();

// CLI 용: 줄 단위 출력 콜백(CLI 구조와 무관하게 쓸 수 있도록 콜백으로 둠)
using Writer = void (*)(const char* line, void* ctx);
void dump(Writer w, void* ctx);

// 구조화된 스냅샷 인터페이스 (L4 UI 렌더링용)
struct TaskSnapshot {
  uint8_t     id;
  const char* name;
  uint32_t    age_ms;
  uint32_t    deadline_ms;
  uint8_t     suspect;
  uint8_t     state;
  uint32_t    stack_free;
};

struct Snapshot {
  Mode         mode;
  uint32_t     would_act;
  uint8_t      task_count;
  TaskSnapshot tasks[kMaxTasks];
  uint8_t      event_count;
  Event        events[16];
  bool         has_prev_boot;
  Event        prev_boot;
};

void getSnapshot(Snapshot& out);

const char* verdictName(Verdict v);
const char* stateName(uint8_t s);
const char* nameOf(uint8_t id);

}  // namespace Supervisor
