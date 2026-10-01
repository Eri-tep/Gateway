# GW Gateway & Edge Driver — Agent Instructions

Integrated project: **M5Stack AtomS3 Lite RS-485/TCP Gateway Firmware (C++17)** & **SmartThings Edge LAN Driver (Lua 5.3)**.

---

## 0. Thinking Process Mandatory Anchor (CoT Gate)
> [!IMPORTANT]
> **BEFORE CALLING ANY TOOL**, you MUST internally verify this 4-point checklist:
> 1. **Plan Check**: Am I modifying code without user-approved `implementation_plan.md`? -> If yes, STOP and present plan first.
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
| **Gateway Architecture** | [`docs/ARCHITECTURE_AND_SPECIFICATIONS.md`](docs/ARCHITECTURE_AND_SPECIFICATIONS.md) | CH1~6 task structure, routing, lock hierarchy |
| **Protocol Specification** | [`docs/HYUNDAI_WALLPAD_PROTOCOL_SPECIFICATION.md`](docs/HYUNDAI_WALLPAD_PROTOCOL_SPECIFICATION.md) | Hyundai RS-485 packet frames, slots, doorphone FSM |
| **Naming Conventions** | [`docs/NAMING_CONVENTIONS.md`](docs/NAMING_CONVENTIONS.md) | Symbol naming (`Domain_VerbNoun`), globals (`g_`), snapshots |
| **Modern C++ Standards** | [`docs/MODERN_CPP_GUIDELINES.md`](docs/MODERN_CPP_GUIDELINES.md) | Packet buffer (`AppendBuf`), C++17, zero heap allocation |
| **Code Review & Audit** | [`docs/EMBEDDED_CODE_REVIEW_AND_VERIFICATION_GUIDE.md`](docs/EMBEDDED_CODE_REVIEW_AND_VERIFICATION_GUIDE.md) | 8-Domain audit, RAM/stack/retention optimization |
| **Edge Driver** | [`docs/SMARTTHINGS_EDGE_DRIVER_GUIDE.md`](docs/SMARTTHINGS_EDGE_DRIVER_GUIDE.md) | `Gateway-edge-driver/` Lua code, profiles, cosock |

---

## 2. Core Invariants (STRICTLY FORBIDDEN)
1. **NO AUTO DEPLOY**: Never run `drivers:install`, `channels:assign`, or ESP32 OTA flash without explicit user command.
2. **NO UNAPPROVED EDIT**: Never modify code before user approves `implementation_plan.md`.
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

---

## 3. Mandatory Procedures & Boundaries
1. **Implementation Plan & Approval**: Present plan, request feedback, and wait for approval before any edits.
2. **Build Verification Boundary (Zero Auto-Deploy)**:
   - Firmware: `~/.platformio/penv/bin/pio run` (0 error, 0 warning required).
   - Edge Driver: `smartthings edge:drivers:package Gateway-edge-driver` ONLY (STRICTLY FORBIDDEN if no files under `Gateway-edge-driver/` were modified).
   - Stop and terminate turn immediately after build/package check. DO NOT deploy.
3. **Explicit CLI Target Specification**: When explicit deploy/check command is given, always supply exact Target IDs (Main Hub ID: `b65b1792-8510-423f-b12d-00d7ff78b700`, Channel ID: `5c5ac2ac-84fb-4783-82df-da58cc675f41`) to avoid TTY prompt hangs.
4. **Top-Down Inspection**: For files >300L, extract outlines (`grep -n`) and view targeted segments only. One-shot symbol index via `rg -n "<symbol>" src/ include/`. Stop inspection immediately when target is identified.
5. **Table-Driven & FSM Dispatch**: Use lookup tables/dispatch maps (`HANDLERS[cls]`) for states/classes and cancel existing timers before re-scheduling.
6. **Code Diff & Verification Rigor**: Always verify file line counts (`wc -l`) and symbol existence (`rg -n`) before declaring missing symbols or code status. Never rely on impression or hallucination.

