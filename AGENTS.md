# GW Gateway & Edge Driver — Agent Instructions

Integrated project: **M5Stack AtomS3 Lite RS-485/TCP Gateway Firmware (C++23 / GCC 13.2.0)** & **SmartThings Edge LAN Driver (Lua 5.3)**.

---

## 0. Thinking Process Mandatory Anchor (CoT Gate)
> [!IMPORTANT]
> **BEFORE CALLING ANY TOOL**, you MUST internally verify this 4-point checklist:
> 1. **Plan Check**: Am I modifying code without user-approved implementation plan presented as an artifact? -> If yes, STOP and create plan artifact in artifact directory (`write_to_file` with `RequestFeedback=true`) first, and wait for user approval.
> 2. **Deploy Check**: Am I running `install` or `assign` without explicit user command? -> If yes, STOP. Verification is strictly `package` only.
> 3. **Boundary Check**: Is verification limited to `pio run` / `edge:drivers:package`? -> If yes, proceed. (Packaging is STRICTLY FORBIDDEN if no files under `Gateway-edge-driver/` were modified).
> 4. **Target Disambiguation Check**: Is the target file unambiguously identified and verified against repository state? -> If ambiguous, STOP and clarify with the user.

---

## 1. Documentation Router (On-Demand Pinpoint Read ONLY)
> [!CAUTION]
> **TOKEN DIET MANDATE**: NEVER proactively load `docs/` files. Extract line outlines first (`grep -n`) and view targeted slices only.

| Domain | Reference | When to Inspect |
|---|---|---|
| **Workflow & Tools** | [`docs/AGENT_WORKFLOW_GUIDELINES.md`](docs/AGENT_WORKFLOW_GUIDELINES.md) | File inspection >300L, symbol indexing, IWYU cleanup |
| **Architecture & Philosophy** | [`docs/ARCHITECTURE_AND_SPECIFICATIONS.md`](docs/ARCHITECTURE_AND_SPECIFICATIONS.md) | Task topology, CH1~6 ports, lock hierarchy & 7 engineering pillars |
| **Protocol Specification** | [`docs/HYUNDAI_WALLPAD_PROTOCOL_SPECIFICATION.md`](docs/HYUNDAI_WALLPAD_PROTOCOL_SPECIFICATION.md) | Hyundai RS-485 packet frames, slots, doorphone FSM |
| **FCU Protocol Spec** | [`docs/AP_FCU_PROTOCOL_SPECIFICATION.md`](docs/AP_FCU_PROTOCOL_SPECIFICATION.md) | AP FCU Modbus-RTU packet frames, registers & CRC-16 |
| **Stability & Benchmark** | [`docs/EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md`](docs/EMBEDDED_STABILITY_AND_BENCHMARK_SPECIFICATION.md) | ESP32-S3 cycle-accurate hot-path mock harness, WDT 12 pillars (P0~P11) & CPU profiling |
| **Edge Driver** | [`docs/SMARTTHINGS_EDGE_DRIVER_GUIDE.md`](docs/SMARTTHINGS_EDGE_DRIVER_GUIDE.md) | `Gateway-edge-driver/` Lua code, profiles, cosock |

---

## 2. Core Invariants (STRICTLY FORBIDDEN)
1. **NO AUTO DEPLOY**: Never run `drivers:install`, `channels:assign`, or ESP32 OTA flash without explicit user command.
2. **NO UNAPPROVED EDIT**: Never modify code before user approves an implementation plan presented as an artifact in the artifact directory (`RequestFeedback=true`).
3. **NO GIT PUSH/COMMIT**: `git commit` and `git push` are strictly forbidden.
4. **NO HEAP IN HOT PATHS**: No `new`, `malloc`, or dynamic `String` in RX/TX paths (`Task_Ch1`, `Task_Ch2Ch3`).
5. **NO BLOCKING IN LUA**: Zero CPU loops (`while true do`) or OS `sleep` in SmartThings `cosock`.
6. **NO BULK CODE VIA JSON**: When splitting large files (>500L), use OS stream pipelines (`sed`, `head`, `tail`).
7. **NO AUTONOMOUS LOOPS**: Never trigger unprompted review/audit loops. Report build and terminate turn immediately.
8. **NO ANTI-PATTERNS**: No chained `if-else`/`strcasecmp` for device classes; use table dispatch (`HANDLERS[cls]`).
9. **NO UNMODIFIED PACKAGE**: Never package the Edge Driver (`edge:drivers:package`) if no files under `Gateway-edge-driver/` were modified. Packaging without edge driver modifications is STRICTLY FORBIDDEN.
10. **NO REPETITIVE TOOL LOOPS**: Once the root cause or target code is identified, stop inspection immediately. Do NOT run recursive or speculative inspection loops on tangential files.
11. **NO INTERACTIVE CLI HANGS**: Never invoke CLI tools without non-interactive flags (e.g., `-H <hub_id>`, `-C <channel_id>`, `--yes`, non-interactive flags). Never allow commands to wait on interactive prompt (`? Select...`).
12. **NO UNNECESSARY EXPLORATION IN EXECUTION PHASE**: Once `implementation_plan.md` is approved, architectural exploration or unrelated file reading is strictly forbidden. Targeted `view_file` slices are permitted ONLY when strictly necessary to verify exact line numbers/indentation for `replace_file_content` or to inspect compiler/package error locations. Proceed directly to edits and verification.
13. **MAX 4 PINPOINT INSPECTIONS BEFORE PLAN**: Prohibit unnecessary or broad file walks. Pinpoint inspections must focus strictly on directly relevant symbols/files (headers, implementation, callers) to identify root causes and targets, and must NOT exceed 4 calls before formulating and presenting `implementation_plan.md`. Never engage in wandering inspection loops without user check-in.
14. **NO ASSUMED TARGET DISPATCH**: Never assume or guess a target file when user refers to code with ambiguous terms (e.g., "내가 준 코드", "다이어트 코드"). When multiple files were discussed, always confirm the exact file path (`src/Protocol.cpp` vs `src/Console.cpp`) before executing analysis or proposing plans.
15. **NO BLIND FULL OVERWRITE & HALLUCINATED OMISSIONS**: Never propose or execute blind full-file overwrites without line-by-line diff verification. Never claim symbols/functions are missing or state incorrect line counts without explicit physical inspection (`wc -l`, `rg -n <symbol>`).
16. **NO RUNAWAY TOKEN LOOPS IN CONFUSION**: When confusion or unexpected mismatch is detected, never engage in multi-turn speculative tool loops (running repetitive diffs or shell inspections). Stop tool calls immediately, present factual state, and confirm with the user.
17. **NO ARCHITECTURAL VIOLATIONS (Canonical 4+1 Layer Invariant)**:
    - **No Upward Includes**: Never include a higher layer from a lower layer ($L_M \rightarrow L_N$ where $M < N$ is STRICTLY FORBIDDEN).
    - **Strict 4-Tier Vertical Pipeline**: 런타임 수직 호출은 반드시 $L4 \rightarrow L3 \rightarrow L2 \rightarrow L1$ 순차 인접만 허용.
    - **L0 Base Foundation Soil Mandate**: L0 Base(`BufferUtils.h`, `SystemConfig.h`, `SystemPlatform.h`)는 수직 파이프라인의 층이 아닌 '전역 순수 기반 Leaf(Foundation Soil)'로서 모든 계층($L1 \sim L4$)에서 컴파일 타임 직접 참조 허용. 중간 계층의 미들맨(Middle-Man) 패스스루 래퍼 금지.
    - **No Extern State Leaks (100% Information Hiding)**: 통신 슬롯, 소켓, 채널 뮤텍스는 절대 `extern`으로 노출하지 않으며, `.cpp` 내부 `static` 봉인 후 읽기 전용 Snapshot API로만 소비.
---

## 3. Mandatory Procedures & Boundaries
1. **Implementation Plan & Approval**: Create and present implementation plan as an artifact in the artifact directory (`<appDataDir>/brain/<conversation-id>/implementation_plan.md`) with `RequestFeedback=true`, and wait for user approval before making any code modifications.
2. **Build Verification Boundary (Zero Auto-Deploy)**:
   - Firmware: `~/.platformio/penv/bin/pio run` (0 error, 0 warning required).
   - Edge Driver: `smartthings edge:drivers:package Gateway-edge-driver` ONLY (STRICTLY FORBIDDEN if no files under `Gateway-edge-driver/` were modified).
   - Stop and terminate turn immediately after build/package check. DO NOT deploy.
3. **Explicit CLI Target Specification**: When explicit deploy/check command is given, always supply exact Target IDs (Main Hub ID: `b65b1792-8510-423f-b12d-00d7ff78b700`, Channel ID: `5c5ac2ac-84fb-4783-82df-da58cc675f41`) to avoid TTY prompt hangs.
4. **Top-Down Inspection**: For files >300L, extract outlines (`grep -n`) and view targeted segments only. One-shot symbol index via `rg -n "<symbol>" src/ include/`. Stop inspection immediately when target is identified.
5. **Table-Driven & FSM Dispatch**: Use lookup tables/dispatch maps (`HANDLERS[cls]`) for states/classes and cancel existing timers before re-scheduling.
6. **Code Diff & Verification Rigor**: Always verify file line counts (`wc -l`) and symbol existence (`rg -n`) before declaring missing symbols or code status. Never rely on impression or hallucination.
7. **Architecture Invariant Verification**: Any code modification touching headers or module boundaries must verify zero upward includes and adherence to the 4+1 Canonical Layer structure before presenting completion.

