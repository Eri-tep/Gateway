# AI Agent Code Modification & Workflow Optimization Guidelines

This document provides mandatory procedural guidelines for AI agents (Antigravity, etc.) and developers when exploring, analyzing, splitting, and refactoring the codebase. Its purpose is to **prevent token waste, guarantee byte-level integrity, and achieve single-pass zero-error builds**.

> [!IMPORTANT]
> **All agents must read and adhere to these 4 standard procedures before inspecting or modifying source code and headers.**

---

## 1. No Slicing Reads & Enforce Top-Down Outline Extraction

### ❌ Anti-Pattern
- Incrementally reading large files (>300 lines) with chained calls like `view_file(StartLine=1, EndLine=200)` $\rightarrow$ `view_file(StartLine=201, EndLine=400)`.
- **Drawbacks**: Exhausts tens of thousands of tokens, bloats context history, and misses structural architectural boundaries.

### ✅ Best Practice
1. **Extract High-Level Outline Once**:
   - Query function/class/struct/namespace definitions and line numbers in one step using regex grep:
   ```bash
   grep -n "^void \|^bool \|^int \|^uint\|^size_t \|^class \|^struct \|^namespace \|// ====" <file_path>
   ```
2. **Pinpoint Targeted Read**:
   - Identify the exact start and end lines of the target function/block from the outline map, then view only that specific segment.
   - Example: `view_file(StartLine=603, EndLine=650)`

---

## 2. One-Shot Symbol Usages Indexing

### ❌ Anti-Pattern
- Opening candidate `.cpp` and `.h` files one by one to manually search for callers or usages.
- **Drawbacks**: Incomplete blast-radius analysis, missing dependent call sites, causing link errors or runtime defects.

### ✅ Best Practice
- Before opening any files, run `ripgrep` (`rg -n`) to index definition sites and all call sites in a single step:
  ```bash
  # Find all usages of a specific global symbol
  rg -n "g_control_registry" src/ include/

  # Find all callers of a specific function
  rg -n "synthesizeFromConvergedCache" src/ include/
  ```
- Confirm the complete list of affected files before modifying headers or changing function signatures.

---

## 3. Mandatory Pre-Mapping for IWYU (Include-What-You-Use) Header Cleanup

### ❌ Anti-Pattern
- Deleting `#include <xxx.h>` from global headers, running a build, inspecting compilation failures, adding includes, and repeating the cycle.
- **Drawbacks**: Causes 3-4 redundant build cycles, wasting time and compute resources.

### ✅ Best Practice
- Before removing or demoting a library/module from a shared header, list every source file that directly uses those symbols:
  ```bash
  # Identify all source files that directly reference ArduinoOTA
  rg -l "ArduinoOTA" src/
  ```
- Pre-emptively add explicit `#include`s to those identified source files, then safely remove the header from global scopes.
- **Outcome**: Clean single-pass build with 0 compilation errors.

---

## 4. Shell Stream Pipelines for Bulk Code Extraction (Cut & Paste)

### ❌ Anti-Pattern
- Passing 500-1,000 lines of raw code through LLM context using large JSON payloads in `replace_file_content` or `write_to_file`.
- **Drawbacks**: Exploding token usage, output truncation risks, indentation corruption, and escaped character regressions.

### ✅ Best Practice
- Using the exact line numbers identified in Step 1, split and extract code blocks directly via OS filesystem pipelines (`sed`, `head`, `tail`):
  ```bash
  # 1. Extract target block to new module (100% byte integrity preserved)
  (
    echo '#include "ControlTemplate.h"'
    echo '#include "WallpadParser.h"'
    echo ''
    sed -n '603,1588p' src/Control/ControlTemplate.cpp
  ) > src/Control/ControlLearning.cpp

  # 2. Cut out extracted block from original source file
  (
    head -n 602 src/Control/ControlTemplate.cpp
    echo ''
    echo '// (onControlTransaction has been extracted to src/Control/ControlLearning.cpp)'
    echo ''
    tail -n +1589 src/Control/ControlTemplate.cpp
  ) > src/Control/ControlTemplate.cpp.tmp && mv src/Control/ControlTemplate.cpp.tmp src/Control/ControlTemplate.cpp
  ```
- **Benefits**:
  - Zero model I/O tokens consumed for the extracted body.
  - Guarantees byte-level fidelity at the OS level.
  - Eliminates syntax errors from copy-paste or character encoding anomalies.

---

## 5. Agent Pre-Flight Checklist

Before editing code, verify each item:

1. [ ] **File Size**: Is the file >300 lines? $\rightarrow$ No full reads. Extract `grep -n` outline first.
2. [ ] **Blast Radius**: Is the target symbol referenced across other files? $\rightarrow$ Index all usages with `rg -n`.
3. [ ] **Header Demotion**: Which files use the symbols from the header being removed? $\rightarrow$ Pre-map with `rg -l` and add explicit `#include`s first.
4. [ ] **Bulk Code Splitting**: Are you extracting hundreds of lines? $\rightarrow$ Use `sed`/`head`/`tail` shell stream pipelines instead of prompt JSON.
5. [ ] **Verification Scope**: Were any files under `Gateway-edge-driver/` modified? $\rightarrow$ If not, `smartthings edge:drivers:package` is STRICTLY FORBIDDEN. Only run `pio run` if firmware was modified.
