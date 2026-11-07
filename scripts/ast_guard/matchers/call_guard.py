#!/usr/bin/env python3
"""
call_guard.py - E-ASTG Function Call & AST Invariant Matcher
Checks function bodies for:
1. Zero-Heap violations in Hot Paths (Task_Ch1, Task_Ch2Ch3, etc.)
2. Blocking calls in Reactor tasks (Task_Network, Remote_Tick, etc.)
3. Layer bypass calls (L4 calling L1 HAL directly)
4. Critical Section Atomic Purity (portENTER_CRITICAL / CriticalSectionLocker)
"""

import os
import re
from typing import List, Dict, Tuple, Optional, Set

# Hot path functions where heap allocations (new, malloc, String, etc.) are strictly forbidden
HOT_PATH_FUNCTIONS = {
    "Task_Ch1": "CH1 Wallpad Real-Time Loop",
    "Task_Ch2Ch3": "CH2/CH3 Thermostat & Air Conditioner Real-Time Loop",
    "RS485_PollBus": "RS-485 Bus Polling Engine",
    "RS485_HandlePacket": "Packet Ingestion Engine",
    "Wallpad_ProcessRxPacket": "Hyundai Wallpad RX Ingestion",
    "Fcu_ProcessRxPacket": "FCU Modbus RX Ingestion",
}

# Reactor functions where blocking delays or synchronous long calls are forbidden
REACTOR_FUNCTIONS = {
    "Task_Network": "Network Reactor Thread",
    "Remote_Tick": "Management Service Tick Loop",
    "Remote_ProcessEvents": "Management Service Event Loop",
    "Tcp_PollAndReceive": "TCP Session Multiplexer",
}

# L1 HAL symbols that L4 (Services) or L3 (Protocol) must never call directly (Layer Bypass)
HAL_L1_SYMBOLS = {
    "uart_write_bytes",
    "uart_read_bytes",
    "uart_driver_install",
    "uart_set_pin",
    "gpio_set_level",
    "gpio_set_direction",
    "esp_rom_delay_us",
    "g_doorphone_serial",
}

# Forbidden allocation patterns in hot paths
HOT_PATH_FORBIDDEN_CALLS = [
    (r"\bmalloc\s*\(", "malloc() dynamic heap allocation"),
    (r"\bcalloc\s*\(", "calloc() dynamic heap allocation"),
    (r"\brealloc\s*\(", "realloc() dynamic heap allocation"),
    (r"\bfree\s*\(", "free() dynamic heap deallocation"),
    (r"\bstrdup\s*\(", "strdup() dynamic heap allocation"),
    (r"\bnew\s+(?!\()", "C++ 'new' dynamic allocation"),
    (r"\bdelete\s+", "C++ 'delete' heap deallocation"),
    (r"\bString\s+[a-zA-Z0-9_]+\s*=", "Dynamic Arduino String instantiation"),
    (r"\bstd::string\b", "Dynamic std::string usage"),
]

# Forbidden blocking patterns in reactor functions
REACTOR_FORBIDDEN_CALLS = [
    (r"\bvTaskDelay\s*\(", "FreeRTOS vTaskDelay() blocking delay in reactor thread"),
    (r"\bdelay\s*\(", "Arduino delay() blocking delay in reactor thread"),
    (r"\bdelayMicroseconds\s*\(", "delayMicroseconds() spinlock in reactor thread"),
    (r"\busleep\s*\(", "POSIX usleep() blocking in reactor thread"),
    (r"\bsleep\s*\(", "POSIX sleep() blocking in reactor thread"),
    (r"\bWiFi\.scanNetworks\s*\(\s*false\s*,\s*true\b", "Synchronous WiFi.scanNetworks(false, true) reactor freeze"),
]

# Forbidden operations inside Critical Sections (portENTER_CRITICAL / CriticalSectionLocker)
CRITICAL_SECTION_FORBIDDEN_CALLS = [
    (r"\bxSemaphoreTake\s*\(", "xSemaphoreTake() lock acquisition in critical section (Deadlock/Crash)"),
    (r"\bxSemaphoreTakeRecursive\s*\(", "xSemaphoreTakeRecursive() lock acquisition in critical section"),
    (r"\bMutexLocker\b", "MutexLocker nested acquisition in critical section"),
    (r"\bvTaskDelay\s*\(", "vTaskDelay() blocking call in critical section (Guru Meditation)"),
    (r"\bdelay\s*\(", "delay() blocking call in critical section"),
    (r"\btaskYIELD\s*\(", "taskYIELD() scheduler yield in critical section"),
    (r"\bmalloc\s*\(", "malloc() dynamic heap allocation in critical section"),
    (r"\bcalloc\s*\(", "calloc() dynamic heap allocation in critical section"),
    (r"\bnew\s+(?!\()", "C++ 'new' allocation in critical section"),
    (r"\bSerial\s*\.\s*(print|write)\b", "Hardware Serial I/O in critical section"),
    (r"\buart_write_bytes\s*\(", "uart_write_bytes() I/O blocking in critical section"),
]


class CodeStrip:
    """Strips comments and string literals from C++ code while preserving newlines and line numbers."""
    @staticmethod
    def strip(content: str) -> List[Tuple[int, str]]:
        lines = content.splitlines()
        stripped_lines = []
        in_block_comment = False

        for line_no, line in enumerate(lines, start=1):
            buf = []
            i = 0
            n = len(line)
            while i < n:
                if in_block_comment:
                    if line[i:i+2] == "*/":
                        in_block_comment = False
                        i += 2
                    else:
                        buf.append(" ")
                        i += 1
                else:
                    if line[i:i+2] == "//":
                        # Line comment rest of line
                        buf.append(" " * (n - i))
                        break
                    elif line[i:i+2] == "/*":
                        in_block_comment = True
                        buf.append("  ")
                        i += 2
                    elif line[i] == '"':
                        # String literal: blank out contents
                        buf.append('"')
                        i += 1
                        while i < n:
                            if line[i] == '\\' and i + 1 < n:
                                buf.append("  ")
                                i += 2
                            elif line[i] == '"':
                                buf.append('"')
                                i += 1
                                break
                            else:
                                buf.append(" ")
                                i += 1
                    elif line[i] == "'":
                        # Char literal
                        buf.append("'")
                        i += 1
                        while i < n:
                            if line[i] == '\\' and i + 1 < n:
                                buf.append("  ")
                                i += 2
                            elif line[i] == "'":
                                buf.append("'")
                                i += 1
                                break
                            else:
                                buf.append(" ")
                                i += 1
                    else:
                        buf.append(line[i])
                        i += 1
            stripped_lines.append((line_no, "".join(buf)))
        return stripped_lines


def extract_function_blocks(stripped_lines: List[Tuple[int, str]]) -> List[Dict]:
    """
    Extracts functions with their name, start line, end line, lines, and called functions.
    """
    functions = []
    brace_depth = 0
    current_func_name = None
    current_func_start_line = None
    current_func_lines = []
    candidate_header = []

    for line_no, line in stripped_lines:
        line_clean = line.strip()
        
        # When outside any function
        if brace_depth == 0:
            if line_clean:
                candidate_header.append((line_no, line_clean))
            if "{" in line:
                full_header = " ".join(txt for _, txt in candidate_header)
                match = re.search(r"\b([a-zA-Z0-9_]+)\s*\([^;{]*\)\s*(?:noexcept|const|override)?\s*(?:GUARDED_BY\([^)]+\))?\s*\{", full_header)
                if match:
                    current_func_name = match.group(1)
                    current_func_start_line = candidate_header[0][0]
                    current_func_lines = []
                candidate_header = []
        
        open_count = line.count("{")
        close_count = line.count("}")
        
        if current_func_name:
            current_func_lines.append((line_no, line))
        
        brace_depth += open_count - close_count
        
        if brace_depth <= 0 and current_func_name:
            # Extract callee function identifiers
            callees = set()
            for _, fl in current_func_lines:
                for cm in re.finditer(r"\b([a-zA-Z0-9_]+)\s*\(", fl):
                    callees.add(cm.group(1))

            functions.append({
                "name": current_func_name,
                "start_line": current_func_start_line,
                "end_line": line_no,
                "lines": current_func_lines,
                "callees": callees,
            })
            current_func_name = None
            current_func_start_line = None
            current_func_lines = []
            candidate_header = []
            brace_depth = 0

    return functions


def verify_critical_sections(rel_path: str, stripped_lines: List[Tuple[int, str]]) -> List[str]:
    """
    Scans for CriticalSectionLocker or portENTER_CRITICAL scopes and ensures atomic purity.
    """
    violations = []
    in_explicit_critical = False
    explicit_start_line = 0
    locker_brace_depths: List[int] = []
    current_brace_depth = 0

    for line_no, line in stripped_lines:
        # Track brace depth
        open_braces = line.count("{")
        close_braces = line.count("}")

        # Check for CriticalSectionLocker entry
        if "CriticalSectionLocker" in line:
            locker_brace_depths.append(current_brace_depth + (1 if "{" in line else 0))

        # Check for explicit portENTER_CRITICAL
        if "portENTER_CRITICAL" in line or "taskENTER_CRITICAL" in line:
            in_explicit_critical = True
            explicit_start_line = line_no

        is_in_critical = in_explicit_critical or (len(locker_brace_depths) > 0)

        # If inside a critical section, inspect operations
        if is_in_critical:
            for pattern, reason in CRITICAL_SECTION_FORBIDDEN_CALLS:
                if re.search(pattern, line):
                    # Exclude the locker declaration itself
                    if "CriticalSectionLocker" in line and "MutexLocker" not in line:
                        continue
                    violations.append(
                        f"[CRITICAL SECTION PURITY VIOLATION] {rel_path}:{line_no}\n"
                        f"    Found: {reason}\n"
                        f"    Code : {line.strip()}"
                    )

        # Check for explicit portEXIT_CRITICAL
        if "portEXIT_CRITICAL" in line or "taskEXIT_CRITICAL" in line:
            in_explicit_critical = False

        # Update brace depth
        current_brace_depth += open_braces - close_braces

        # Pop finished locker scopes
        while locker_brace_depths and current_brace_depth < locker_brace_depths[-1]:
            locker_brace_depths.pop()

    return violations


def verify_call_invariants(project_root: str, target_files: Optional[List[str]] = None) -> List[str]:
    """
    Scans project C++ sources and verifies AST call invariants.
    Returns list of violations.
    """
    violations = []
    src_dir = os.path.join(project_root, "src")

    files_to_scan = []
    if target_files:
        for tf in target_files:
            full_p = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(project_root, tf))
            if os.path.exists(full_p) and (full_p.endswith(".cpp") or full_p.endswith(".c")):
                files_to_scan.append(full_p)
    else:
        for root, _, files in os.walk(src_dir):
            for fname in files:
                if fname.endswith(".cpp") or fname.endswith(".c"):
                    files_to_scan.append(os.path.join(root, fname))

    for fpath in files_to_scan:
        rel_path = os.path.relpath(fpath, project_root)
        
        with open(fpath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()

        stripped = CodeStrip.strip(content)
        functions = extract_function_blocks(stripped)

        # 1. Critical Section Atomic Purity Scan
        crit_viols = verify_critical_sections(rel_path, stripped)
        violations.extend(crit_viols)

        # Determine Layer from rel_path
        current_layer = None
        for layer in ["L0_Foundation", "L1_HAL", "L2_Transport", "L3_Protocol", "L4_Services"]:
            if layer in rel_path:
                current_layer = layer
                break

        for func in functions:
            func_name = func["name"]
            
            # 2. Hot Path Zero-Heap Verification
            if func_name in HOT_PATH_FUNCTIONS:
                role_desc = HOT_PATH_FUNCTIONS[func_name]
                for lno, line in func["lines"]:
                    for pattern, reason in HOT_PATH_FORBIDDEN_CALLS:
                        if re.search(pattern, line):
                            violations.append(
                                f"[HOT-PATH HEAP VIOLATION] {rel_path}:{lno} in '{func_name}' ({role_desc})\n"
                                f"    Found: {reason}\n"
                                f"    Code : {line.strip()}"
                            )

            # 3. Reactor Non-Blocking Verification
            if func_name in REACTOR_FUNCTIONS:
                role_desc = REACTOR_FUNCTIONS[func_name]
                for lno, line in func["lines"]:
                    for pattern, reason in REACTOR_FORBIDDEN_CALLS:
                        if re.search(pattern, line):
                            violations.append(
                                f"[REACTOR BLOCKING VIOLATION] {rel_path}:{lno} in '{func_name}' ({role_desc})\n"
                                f"    Found: {reason}\n"
                                f"    Code : {line.strip()}"
                            )

        # 4. Layer Bypass Verification (L4/L3 calling L1 HAL symbols directly)
        if current_layer in ("L4_Services", "L3_Protocol"):
            for lno, line in stripped:
                for hal_sym in HAL_L1_SYMBOLS:
                    if re.search(rf"\b{hal_sym}\s*\(", line):
                        violations.append(
                            f"[LAYER BYPASS VIOLATION] {rel_path}:{lno} ({current_layer})\n"
                            f"    Direct invocation of L1 HAL symbol '{hal_sym}()' is forbidden in {current_layer}!\n"
                            f"    Rule : Use L2 Transport or L3 Facade abstraction."
                        )

    return violations


if __name__ == "__main__":
    import sys
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    viols = verify_call_invariants(proj_root)
    if viols:
        print(f"\n[E-ASTG FATAL] {len(viols)} Function Call Invariant Violations Detected!\n")
        for v in viols:
            print(f"  {v}\n")
        sys.exit(1)
    else:
        print(f"[E-ASTG PASS] Module 2 & 3.2 (Call & Critical Section Analyzer): Verified clean.")
        sys.exit(0)
