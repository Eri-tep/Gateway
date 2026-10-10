// Supervisor.cpp - P1 (Shadow 모드): 관측 / 범인·피해자 판정 / 기록
#include "Supervisor.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <iterator>

#include "esp_attr.h"
#include "esp_system.h"

namespace Supervisor {

std::atomic<uint32_t> g_lastFeedMs[kMaxTasks];

namespace {

constexpr uint32_t    kStackBytes = 2560;  // 2.5 KB (4KB -> 2.5KB 최적화, 1536B 가용 램 회수)
constexpr UBaseType_t kPriority   = 14;    // CH1_REALTIME(13)보다 높아야 폭주 태스크를 선점할 수 있다
constexpr BaseType_t  kCore       = 0;
constexpr size_t      kRingSize   = 32;
constexpr size_t      kSnapMax    = 24;    // 24개 (최대 15~16개 태스크 대비 마진 확보, 640B 가용 램 회수)
constexpr uint32_t    kRtcMagic   = 0x53555056u;

TaskHandle_t                   s_handle[kMaxTasks];
TaskSpec                       s_spec[kMaxTasks];
uint8_t                        s_suspect[kMaxTasks];
bool                           s_reported[kMaxTasks];
std::atomic<uint32_t>          s_holdUntil[kMaxTasks];  // 0 = 유예 없음
std::atomic<SemaphoreHandle_t> s_waitSem[kMaxTasks];
std::atomic<Mode>              s_mode{Mode::Shadow};

Event    s_ring[kRingSize];
size_t   s_ringHead   = 0;
uint32_t s_wouldAct   = 0;  // Shadow 에서 "Active 였다면 조치했을" 횟수
Event    s_prevBoot{};
bool     s_hasPrevBoot = false;

RTC_NOINIT_ATTR uint32_t s_rtcMagic;  // 리셋 후에도 유지(전원 인가 직후에는 무효)
RTC_NOINIT_ATTR Event    s_rtcLast;

TaskStatus_t s_snapBuf[kSnapMax];
uint32_t     s_baseRt[kMaxTasks];
uint32_t     s_baseTotal = 0;
bool         s_baseValid = false;

StaticTask_t s_tcb;
StackType_t  s_stack[kStackBytes / sizeof(StackType_t)];

struct Snap {
  uint32_t   rt[kMaxTasks];
  eTaskState st[kMaxTasks];
  uint32_t   total;
  bool       ok;
};

struct Judgement {
  Verdict v;
  uint8_t culprit;
  uint8_t state;
  uint8_t cpu;
};

uint8_t idOf(TaskHandle_t h) {
  if (h == nullptr) return kNoTask;
  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == h) return i;
  }
  return kNoTask;
}

// 의심이 시작될 때와 판정할 때 두 번만 호출된다(평상시 0회).
// uxTaskGetSystemState 는 스케줄러를 잠깐 멈추므로 주기 호출 금지.
Snap takeSnap() {
  Snap       s{};
  uint32_t   total = 0;
  const auto n     = uxTaskGetSystemState(s_snapBuf, kSnapMax, &total);
  s.total          = total;
  s.ok             = (n != 0);
  for (UBaseType_t k = 0; k < n; ++k) {
    const uint8_t id = idOf(s_snapBuf[k].xHandle);
    if (id == kNoTask) continue;
    s.rt[id] = s_snapBuf[k].ulRunTimeCounter;
    s.st[id] = s_snapBuf[k].eCurrentState;
  }
  return s;
}

void takeBaseline() {
  const Snap b = takeSnap();
  std::copy(std::begin(b.rt), std::end(b.rt), std::begin(s_baseRt));
  s_baseTotal = b.total;
  s_baseValid = b.ok;
}

// 듀얼코어에서 전체 실행시간 대비 비율이므로 코어 수를 곱해 "한 코어 기준 %"로 환산
uint8_t cpuPct(uint8_t id, const Snap& now) {
  if (!s_baseValid || !now.ok) return 0;
  const uint32_t dt = now.total - s_baseTotal;
  if (dt == 0) return 0;
  const uint64_t dr  = static_cast<uint32_t>(now.rt[id] - s_baseRt[id]);
  const uint64_t pct = dr * 100u * portNUM_PROCESSORS / dt;
  return static_cast<uint8_t>(std::min<uint64_t>(pct, 100u));
}

Judgement classify(uint8_t i, const Snap& snap) {
  const eTaskState st  = snap.ok ? snap.st[i] : eTaskGetState(s_handle[i]);
  const uint8_t    cpu = cpuPct(i, snap);
  Judgement        j{Verdict::SystemWide, kNoTask, static_cast<uint8_t>(st), cpu};

  // R1: 실행 중이거나 코어를 거의 독점 -> 본인이 범인(폭주)
  if (st == eRunning || cpu >= kHogCpuPct) {
    j.v       = Verdict::CulpritHog;
    j.culprit = i;
    return j;
  }
  // R2: 락 대기 중이고 보유자가 있으면 -> 나는 피해자, 보유자가 범인
  if (SemaphoreHandle_t sem = s_waitSem[i].load(std::memory_order_relaxed)) {
    if (TaskHandle_t holder = xSemaphoreGetMutexHolder(sem)) {
      j.v       = Verdict::VictimLock;
      j.culprit = idOf(holder);  // 미등록 태스크면 kNoTask
      return j;
    }
  }
  // R3: Ready 인데 못 돈다 -> 기아. 같은 코어의 점유 태스크가 범인
  if (st == eReady) {
    j.v = Verdict::VictimStarved;
    for (uint8_t k = 0; k < kMaxTasks; ++k) {
      if (k == i || s_handle[k] == nullptr) continue;
      if (s_spec[k].core == s_spec[i].core && cpuPct(k, snap) >= kHogCpuPct) {
        j.culprit = k;
        break;
      }
    }
    return j;  // 못 찾으면 미등록 태스크(시스템/Wi-Fi 등)가 원인
  }
  // R4: Blocked/Suspended 인데 데드라인 초과 -> 자체 정지
  j.v       = Verdict::CulpritSelfStall;
  j.culprit = i;
  return j;
}

void record(const Event& e) {
  s_ring[s_ringHead % kRingSize] = e;
  ++s_ringHead;
  s_rtcLast  = e;
  s_rtcMagic = kRtcMagic;
}

void evaluate() {
  const uint32_t now         = nowMs();
  uint8_t        missing[kMaxTasks];
  uint8_t        nMissing    = 0;
  uint8_t        registered  = 0;
  bool           anySuspect  = false;

  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == nullptr) continue;
    ++registered;

    const uint32_t hold = s_holdUntil[i].load(std::memory_order_relaxed);
    if (hold != 0 && static_cast<int32_t>(hold - now) > 0) {
      s_suspect[i]  = 0;
      s_reported[i] = false;
      continue;
    }
    const uint32_t last = g_lastFeedMs[i].load(std::memory_order_relaxed);
    const uint32_t age  = (now >= last) ? (now - last) : 0;
    if (age <= s_spec[i].deadline_ms) {
      s_suspect[i]  = 0;
      s_reported[i] = false;
      continue;
    }
    anySuspect = true;
    if (s_suspect[i] < 255) ++s_suspect[i];
    if (s_suspect[i] == 1 && !s_baseValid) takeBaseline();
    if (s_suspect[i] >= kConfirmCount && !s_reported[i]) missing[nMissing++] = i;
  }

  if (!anySuspect) {
    s_baseValid = false;
    return;
  }
  if (nMissing == 0) return;

  const Snap snap = takeSnap();
  Judgement  js[kMaxTasks];
  bool       anyRoot = false;
  for (uint8_t m = 0; m < nMissing; ++m) {
    js[m] = classify(missing[m], snap);
    if (js[m].v == Verdict::CulpritHog || (js[m].v == Verdict::VictimLock && js[m].culprit != kNoTask)) {
      anyRoot = true;
    }
  }
  // R5: 과반이 동시에 멈췄는데 뚜렷한 범인이 없으면 시스템 수준 원인으로 본다
  if (static_cast<uint8_t>(nMissing * 2) > registered && !anyRoot) {
    for (uint8_t m = 0; m < nMissing; ++m) js[m].v = Verdict::SystemWide;
  }

  for (uint8_t m = 0; m < nMissing; ++m) {
    const uint8_t id = missing[m];
    Event         e{};
    e.t_ms    = now;
    e.task    = id;
    e.culprit = js[m].culprit;
    e.verdict = js[m].v;
    e.state   = js[m].state;
    e.cpu_pct = js[m].cpu;
    record(e);
    s_reported[id] = true;  // 같은 장애를 100ms 마다 반복 기록하지 않음

    const bool culprit = (js[m].v == Verdict::CulpritHog || js[m].v == Verdict::CulpritSelfStall);
    if (!culprit) continue;
    if (s_mode.load(std::memory_order_relaxed) == Mode::Active) {
      // P2: 협조적 중지 요청 -> 유예 -> (락 미보유 확인 후) 강제 삭제 -> 재생성
    } else {
      ++s_wouldAct;
    }
  }
}

void taskMain(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
    evaluate();
  }
}

}  // namespace

const char* verdictName(Verdict v) {
  static const char* const k[] = {"Healthy", "CulpritHog", "CulpritSelfStall",
                                  "VictimLock", "VictimStarved", "SystemWide"};
  return k[static_cast<uint8_t>(v)];
}

const char* stateName(uint8_t s) {
  static const char* const k[] = {"Running", "Ready", "Blocked", "Suspended", "Deleted", "Invalid"};
  return s < 6 ? k[s] : "?";
}

const char* nameOf(uint8_t id) {
  return (id < kMaxTasks && s_spec[id].name != nullptr) ? s_spec[id].name : "-";
}

#if SUP_FAULT_INJECTION
namespace {
std::atomic<uint8_t>  s_injKind[kMaxTasks];
std::atomic<uint32_t> s_injMs[kMaxTasks];
}  // namespace

void inject(uint8_t id, Inject kind, uint32_t ms) {
  if (id >= kMaxTasks) return;
  s_injMs[id].store(ms);
  s_injKind[id].store(static_cast<uint8_t>(kind));
}

// heartbeat() 맨 앞에서 호출된다. 하트비트 갱신 전에 멈추므로 해당 태스크는 데드라인을 넘긴다.
void injectPoint(uint8_t id) {
  if (id >= kMaxTasks) return;
  const auto kind = static_cast<Inject>(s_injKind[id].exchange(0));
  if (kind == Inject::None) return;
  const uint32_t ms = s_injMs[id].load();
  if (kind == Inject::Block) {
    vTaskDelay(pdMS_TO_TICKS(ms));  // Blocked 상태로 정지 -> CulpritSelfStall 기대
    return;
  }
  const uint32_t end = nowMs() + ms;  // 의도적 CPU 점유 -> CulpritHog 기대
  while (static_cast<int32_t>(end - nowMs()) > 0) {
  }
}
#endif

void registerTask(uint8_t id, TaskHandle_t h, const TaskSpec& spec) {
  if (id >= kMaxTasks) return;
  s_spec[id]   = spec;
  s_handle[id] = h;
  g_lastFeedMs[id].store(nowMs(), std::memory_order_relaxed);
}

void start(Mode m) {
  s_mode.store(m);
  if (esp_reset_reason() == ESP_RST_POWERON) s_rtcMagic = 0;  // 전원 인가 직후 RTC 값은 쓰레기
  if (s_rtcMagic == kRtcMagic) {
    s_prevBoot    = s_rtcLast;  // 직전 부팅의 마지막 판정을 보존
    s_hasPrevBoot = true;
  }
  s_rtcMagic = 0;
  xTaskCreateStaticPinnedToCore(taskMain, "Supervisor", kStackBytes, nullptr, kPriority, s_stack, &s_tcb, kCore);
}

void setMode(Mode m) { s_mode.store(m); }
Mode mode() { return s_mode.load(); }

void holdFor(uint8_t id, uint32_t ms) {
  if (id >= kMaxTasks) return;
  s_holdUntil[id].store(std::max<uint32_t>(1u, nowMs() + ms), std::memory_order_relaxed);
}

void release(uint8_t id) {
  if (id < kMaxTasks) s_holdUntil[id].store(0, std::memory_order_relaxed);
}

void noteWaitBegin(SemaphoreHandle_t s) {
  const uint8_t id = idOf(xTaskGetCurrentTaskHandle());
  if (id != kNoTask) s_waitSem[id].store(s, std::memory_order_relaxed);
}

void noteWaitEnd() {
  const uint8_t id = idOf(xTaskGetCurrentTaskHandle());
  if (id != kNoTask) s_waitSem[id].store(nullptr, std::memory_order_relaxed);
}

void dump(Writer w, void* ctx) {
  char           line[112];
  const uint32_t now = nowMs();

  std::snprintf(line, sizeof line, "mode=%s would_act=%" PRIu32,
                s_mode.load() == Mode::Shadow ? "shadow" : "active", s_wouldAct);
  w(line, ctx);

  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == nullptr) continue;
    const uint32_t curNow = nowMs();
    const uint32_t last   = g_lastFeedMs[i].load(std::memory_order_relaxed);
    const uint32_t age    = (curNow >= last) ? (curNow - last) : 0;
    std::snprintf(line, sizeof line, "[%u] %-10s age=%" PRIu32 "ms/%" PRIu32 "ms suspect=%u state=%s stack_free=%" PRIu32,
                  static_cast<unsigned>(i), nameOf(i), age,
                  s_spec[i].deadline_ms, static_cast<unsigned>(s_suspect[i]),
                  stateName(static_cast<uint8_t>(eTaskGetState(s_handle[i]))),
                  static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_handle[i])));
    w(line, ctx);
  }

  const size_t count = std::min(s_ringHead, kRingSize);
  for (size_t k = 0; k < count; ++k) {  // 최신순
    const Event& e = s_ring[(s_ringHead - 1 - k) % kRingSize];
    std::snprintf(line, sizeof line, "ev t=%" PRIu32 " task=%s verdict=%s culprit=%s state=%s cpu=%u%%", e.t_ms,
                  nameOf(e.task), verdictName(e.verdict), nameOf(e.culprit), stateName(e.state),
                  static_cast<unsigned>(e.cpu_pct));
    w(line, ctx);
  }

  if (s_hasPrevBoot) {
    std::snprintf(line, sizeof line, "prev-boot last: task=%s verdict=%s culprit=%s cpu=%u%%", nameOf(s_prevBoot.task),
                  verdictName(s_prevBoot.verdict), nameOf(s_prevBoot.culprit), static_cast<unsigned>(s_prevBoot.cpu_pct));
    w(line, ctx);
  }
}

void getSnapshot(Snapshot& out) {
  out.mode       = s_mode.load(std::memory_order_relaxed);
  out.would_act  = s_wouldAct;
  out.task_count = 0;
  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == nullptr) continue;
    const uint32_t curNow = nowMs();
    const uint32_t last   = g_lastFeedMs[i].load(std::memory_order_relaxed);
    TaskSnapshot& ts      = out.tasks[out.task_count++];
    ts.id          = i;
    ts.name        = nameOf(i);
    ts.age_ms      = (curNow >= last) ? (curNow - last) : 0;
    ts.deadline_ms = s_spec[i].deadline_ms;
    ts.suspect     = s_suspect[i];
    ts.state       = static_cast<uint8_t>(eTaskGetState(s_handle[i]));
    ts.stack_free  = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_handle[i]));
  }
  const size_t totalEv = std::min(s_ringHead, kRingSize);
  out.event_count = static_cast<uint8_t>(std::min<size_t>(totalEv, 16));
  for (uint8_t k = 0; k < out.event_count; ++k) {
    out.events[k] = s_ring[(s_ringHead - 1 - k) % kRingSize];
  }
  out.has_prev_boot = s_hasPrevBoot;
  out.prev_boot     = s_prevBoot;
}

}  // namespace Supervisor
