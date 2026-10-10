// Supervisor.cpp - 태스크 생존성 슈퍼바이저 (P1: Shadow / P2: Active 신속 클린 리부트)
#include "Supervisor.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <Preferences.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace Supervisor {

std::atomic<uint32_t> g_lastFeedMs[kMaxTasks];

namespace {

constexpr uint32_t    kStackBytes       = 2560;  // 2.5 KB (4KB -> 2.5KB 최적화, 1536B 가용 램 회수)
constexpr UBaseType_t kPriority         = 14;    // CH1_REALTIME(13)보다 높아야 폭주 태스크를 선점할 수 있다
constexpr BaseType_t  kCore             = 0;
constexpr size_t      kRingSize         = 32;
constexpr size_t      kSnapMax          = 24;    // 24개 (최대 15~16개 태스크 대비 마진 확보, 640B 가용 램 회수)
constexpr uint32_t    kRtcMagic         = 0x53555056u;  // 'SUPV' (Shadow 아노말리 링버퍼용)
constexpr uint32_t    kRtcMagicActive   = 0x53555052u;  // 'SUPR' (P2 Active 재부팅 레코드용)
constexpr uint32_t    kLoopWindowS      = 600;          // 급속 재부팅 판정 창 (10분)
constexpr uint8_t     kMaxConsecutive   = 3;            // 연속 N회 초과 시 Shadow 강등
constexpr uint32_t    kHookTimeoutMs    = 200;          // 재부팅 직전 훅 허용 시간
constexpr uint64_t    kBackstopUs       = 1'500'000;    // 훅이 멈춰도 1.5초 후 강제 재시작

struct RtcRecord {
  uint32_t magic;
  uint32_t total;        // Supervisor 유발 재부팅 누적
  uint32_t lastUptimeS;  // 직전 재부팅 시점의 업타임(초)
  uint8_t  consecutive;  // 급속 재부팅 연속 횟수
  uint8_t  lastTask;
  uint8_t  lastVerdict;
  uint8_t  demoted;      // 1 = 루프 방지로 Shadow 강제 (전원 사이클 시 NVS 동기화)
  char     why[16];
};
static_assert(sizeof(RtcRecord) <= 32, "RTC 레코드는 작게 유지");

RTC_NOINIT_ATTR RtcRecord s_rtc;

TaskHandle_t                   s_handle[kMaxTasks];
TaskSpec                       s_spec[kMaxTasks];
uint8_t                        s_suspect[kMaxTasks];
bool                           s_reported[kMaxTasks];
std::atomic<uint32_t>          s_holdUntil[kMaxTasks];  // 0 = 유예 없음
std::atomic<SemaphoreHandle_t> s_waitSem[kMaxTasks];
std::atomic<Mode>              s_mode{Mode::Shadow};

std::atomic<uint8_t>  s_exempt{0};
std::atomic<uint32_t> s_exemptStartMs{0};
std::atomic<uint32_t> s_exemptCooldownUntilMs{0};
std::atomic<uint32_t> s_bootGraceUntilMs{0};
std::atomic<bool>     s_rebooting{false};
PreRebootHook         s_hook = nullptr;
uint8_t               s_confirm[kMaxTasks] = {};
uint32_t              s_acted = 0;
[[nodiscard]] [[gnu::always_inline]] inline uint32_t tickAge(uint32_t now, uint32_t last) noexcept {
  const int32_t d = static_cast<int32_t>(now - last);
  return d > 0 ? static_cast<uint32_t>(d) : 0u;
}

[[nodiscard]] uint32_t uptimeSeconds() noexcept {
  return static_cast<uint32_t>(esp_timer_get_time() / 1'000'000ULL);
}

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
  const uint32_t now = nowMs();

  // 1) 부팅 시동 유예 (15초): 초기화 과정 오탐 방지
  const uint32_t grace = s_bootGraceUntilMs.load(std::memory_order_relaxed);
  if (grace != 0 && static_cast<int32_t>(grace - now) > 0) {
    for (uint8_t i = 0; i < kMaxTasks; ++i) {
      s_suspect[i]  = 0;
      s_confirm[i]  = 0;
      s_reported[i] = false;
    }
    s_baseValid = false;
    return;
  }

  // 2) 글로벌 유예 (OTA, Wi-Fi 스캔) 및 사후 쿨다운 (2초)
  bool is_exempt = false;
  if (s_exempt.load(std::memory_order_relaxed) > 0) {
    const uint32_t start_ms = s_exemptStartMs.load(std::memory_order_relaxed);
    const uint32_t elapsed  = tickAge(now, start_ms);
    if (elapsed <= kMaxExemptDurationMs) {
      is_exempt = true;
    }
  }
  if (!is_exempt) {
    const uint32_t cd = s_exemptCooldownUntilMs.load(std::memory_order_relaxed);
    if (cd != 0 && static_cast<int32_t>(cd - now) > 0) {
      is_exempt = true;
    }
  }
  if (is_exempt) {
    for (uint8_t i = 0; i < kMaxTasks; ++i) {
      s_suspect[i]  = 0;
      s_confirm[i]  = 0;
      s_reported[i] = false;
    }
    s_baseValid = false;
    return;
  }

  uint8_t missing[kMaxTasks];
  uint8_t nMissing   = 0;
  uint8_t registered = 0;
  bool    anySuspect = false;

  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == nullptr) continue;
    ++registered;

    const uint32_t hold = s_holdUntil[i].load(std::memory_order_relaxed);
    if (hold != 0 && static_cast<int32_t>(hold - now) > 0) {
      s_suspect[i]  = 0;
      s_reported[i] = false;
      s_confirm[i]  = 0;
      continue;
    }
    const uint32_t last = g_lastFeedMs[i].load(std::memory_order_relaxed);
    const uint32_t age  = tickAge(now, last);
    if (age <= s_spec[i].deadline_ms) {
      s_suspect[i]  = 0;
      s_reported[i] = false;
      s_confirm[i]  = 0;
      continue;
    }
    anySuspect = true;
    if (s_suspect[i] < 255) ++s_suspect[i];
    if (s_suspect[i] == 1 && !s_baseValid) takeBaseline();
    if (s_suspect[i] >= kConfirmCount) missing[nMissing++] = i;
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
    if (id >= kMaxTasks) continue;

    Event e{};
    e.t_ms    = now;
    e.task    = id;
    e.culprit = js[m].culprit;
    e.verdict = js[m].v;
    e.state   = js[m].state;
    e.cpu_pct = js[m].cpu;

    if (!s_reported[id]) {
      record(e);
      s_reported[id] = true;  // RTC 링버퍼는 첫 감지 시 1회만 기록
    }

    const bool culprit = (js[m].v == Verdict::CulpritHog || js[m].v == Verdict::CulpritSelfStall);
    if (!culprit) {
      s_confirm[id] = 0;
      continue;
    }

    if (s_mode.load(std::memory_order_relaxed) == Mode::Active) {
      if (s_confirm[id] < 0xFF) ++s_confirm[id];
      if (s_confirm[id] >= kConfirmCount) {
        s_confirm[id] = 0;
        ++s_acted;
        (void)rebootNow(id, static_cast<uint8_t>(js[m].v),
                        js[m].v == Verdict::CulpritHog ? "HOG" : "STALL",
                        /*countsTowardLoop=*/true);
      }
    } else {
      if (s_confirm[id] == 0) {
        // Shadow 모드: 비정상 첫 감지 시에만 wouldAct 증가
        ++s_wouldAct;
        s_confirm[id] = 1;
      }
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

void setPreRebootHook(PreRebootHook hook) noexcept { s_hook = hook; }

void enterGlobalExempt() noexcept {
  if (s_exempt.fetch_add(1, std::memory_order_relaxed) == 0) {
    s_exemptStartMs.store(nowMs(), std::memory_order_relaxed);
  }
}

void exitGlobalExempt() noexcept {
  if (s_exempt.fetch_sub(1, std::memory_order_relaxed) == 1) {
    s_exemptCooldownUntilMs.store(nowMs() + kPostExemptCooldownMs, std::memory_order_relaxed);
  }
}

void onBoot() noexcept {
  const bool swReset = (esp_reset_reason() == ESP_RST_SW);
  if (!swReset || s_rtc.magic != kRtcMagicActive) {
    std::memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = kRtcMagicActive;
  }

  Mode saved = Mode::Shadow;
  {
    Preferences p;
    if (p.begin("sup", /*readOnly=*/true)) {
      saved = (p.getUChar("mode", 0) == 1) ? Mode::Active : Mode::Shadow;
      p.end();
    }
  }

  if (s_rtc.demoted != 0) {
    saved = Mode::Shadow;
    ESP_LOGW("SUP", "demoted to Shadow (rapid reboot loop guard)");
    // 전원 사이클 후에도 루프를 방지하도록 NVS의 mode 키도 Shadow로 영구 동기화
    Preferences p;
    if (p.begin("sup", false)) {
      p.putUChar("mode", 0);
      p.end();
    }
  }

  s_mode.store(saved, std::memory_order_relaxed);
}

void start(Mode m) {
  s_mode.store(m, std::memory_order_relaxed);
  s_bootGraceUntilMs.store(nowMs() + kBootStartupGraceMs, std::memory_order_relaxed);

  if (esp_reset_reason() == ESP_RST_POWERON) s_rtcMagic = 0;  // 전원 인가 직후 RTC 값은 쓰레기
  if (s_rtcMagic == kRtcMagic) {
    s_prevBoot    = s_rtcLast;  // 직전 부팅의 마지막 판정을 보존
    s_hasPrevBoot = true;
  }
  s_rtcMagic = 0;
  xTaskCreateStaticPinnedToCore(taskMain, "Supervisor", kStackBytes, nullptr, kPriority, s_stack, &s_tcb, kCore);
}

void setMode(Mode m, bool persist) noexcept {
  if (m == Mode::Active) {
    s_rtc.demoted = 0;  // 명시적 Active 요청 = 강등 플래그 해제
  }
  s_mode.store(m, std::memory_order_relaxed);
  if (persist) {
    Preferences p;
    if (p.begin("sup", false)) {
      p.putUChar("mode", m == Mode::Active ? 1 : 0);
      p.end();
    }
  }
}

Mode mode() noexcept { return s_mode.load(std::memory_order_relaxed); }

namespace {
void backstopCb(void*) { esp_restart(); }
}  // namespace

bool rebootNow(uint8_t taskIdx, uint8_t verdict, const char* why, bool countsTowardLoop) noexcept {
  if (s_rebooting.exchange(true, std::memory_order_acq_rel)) return true;  // 이미 재부팅 진행 중

  const uint32_t up = uptimeSeconds();

  // 1) 루프 방지 판정 (락 없음, RTC 메모리만 사용)
  if (countsTowardLoop) {
    const uint8_t next = (up < kLoopWindowS)
                             ? static_cast<uint8_t>(s_rtc.consecutive + 1)
                             : 1;
    if (next > kMaxConsecutive) {
      s_rtc.demoted     = 1;
      s_rtc.lastTask    = taskIdx;
      s_rtc.lastVerdict = verdict;
      std::snprintf(s_rtc.why, sizeof(s_rtc.why), "LOOPGUARD");
      s_mode.store(Mode::Shadow, std::memory_order_relaxed);
      s_rebooting.store(false, std::memory_order_release);
      ESP_LOGE("SUP", "reboot loop guard tripped -> Demoted to Shadow");
      return false;
    }
    s_rtc.consecutive = next;
  }

  // 2) 사유 기록 (재부팅 후 sup status 로 확인)
  ++s_rtc.total;
  s_rtc.lastUptimeS = up;
  s_rtc.lastTask    = taskIdx;
  s_rtc.lastVerdict = verdict;
  std::snprintf(s_rtc.why, sizeof(s_rtc.why), "%s", why ? why : "-");

  // 3) 백스톱 타이머: 1.5초 후 강제 재시작 (사전 훅이 멈춰도 esp_timer 태스크에서 독립 실행)
  esp_timer_handle_t t = nullptr;
  const esp_timer_create_args_t args{
      .callback              = backstopCb,
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "sup_bs",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &t) == ESP_OK) {
    esp_timer_start_once(t, kBackstopUs);
  }

  // 4) 선택적 사전 훅 (NVS pending flush 등 - try-lock 계약)
  if (s_hook) {
    (void)s_hook(kHookTimeoutMs);
  }

  // 5) 락/클라이언트 정리 없이 직접 재시작
  esp_restart();
  return true;
}

namespace {
void manualRebootTimerCb(void*) {
  rebootNow(kNoTask, kNoTask, "manual", /*countsTowardLoop=*/false);
}
}  // namespace

void scheduleManualReboot(uint32_t delay_ms) noexcept {
  esp_timer_handle_t t = nullptr;
  const esp_timer_create_args_t args{
      .callback              = manualRebootTimerCb,
      .arg                   = nullptr,
      .dispatch_method       = ESP_TIMER_TASK,
      .name                  = "sup_man_rb",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &t) == ESP_OK) {
    esp_timer_start_once(t, static_cast<uint64_t>(delay_ms) * 1000ULL);
  } else {
    rebootNow(kNoTask, kNoTask, "manual", false);
  }
}

void formatRebootStatus(char* out, size_t n) noexcept {
  std::snprintf(out, n,
                "Reboots(sup): total=%lu consec=%u last{task=%u v=%u up=%lus why=%s}%s",
                static_cast<unsigned long>(s_rtc.total), s_rtc.consecutive,
                s_rtc.lastTask, s_rtc.lastVerdict,
                static_cast<unsigned long>(s_rtc.lastUptimeS), s_rtc.why,
                s_rtc.demoted ? " [DEMOTED]" : "");
}

bool cliMode(const char* arg, char* out, size_t n) noexcept {
  if (arg && *arg) {
    if (std::strcmp(arg, "active") == 0) {
      setMode(Mode::Active, true);
    } else if (std::strcmp(arg, "shadow") == 0) {
      setMode(Mode::Shadow, true);
    } else {
      std::snprintf(out, n, "usage: sup mode [shadow|active]");
      return false;
    }
  }
  std::snprintf(out, n, "Mode: %s%s",
                mode() == Mode::Active ? "Active" : "Shadow",
                s_rtc.demoted ? " (demoted: use 'sup mode active' to re-arm)" : "");
  return true;
}

bool cliReboot(char* out, size_t n) noexcept {
  std::snprintf(out, n, "Gateway is rebooting manually in 150ms...\r\n");
  scheduleManualReboot(150);
  return true;
}

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
    const uint32_t age    = tickAge(curNow, last);
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
  out.acted      = s_acted;
  out.task_count = 0;
  for (uint8_t i = 0; i < kMaxTasks; ++i) {
    if (s_handle[i] == nullptr) continue;
    const uint32_t curNow = nowMs();
    const uint32_t last   = g_lastFeedMs[i].load(std::memory_order_relaxed);
    TaskSnapshot& ts      = out.tasks[out.task_count++];
    ts.id          = i;
    ts.name        = nameOf(i);
    ts.age_ms      = tickAge(curNow, last);
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
