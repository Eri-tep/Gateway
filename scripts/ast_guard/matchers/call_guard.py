#!/usr/bin/env python3
"""
call_guard.py - Gate 2: E-ASTG Function Call, DIP & Mutex/Flash Purity Matcher
Checks C/C++ sources for:
1. Zero-Heap violations in Hot Paths (Task_Ch1, Task_Ch2Ch3, etc.)
2. Blocking calls in Reactor tasks (Task_Network, Remote_Tick, etc.)
3. DIP / Layer Bypass calls:
   - L3 & L4: No WiFi.*, <WiFi.h>, <esp_ota_ops.h>, <esp_task_wdt.h>,
     <esp_core_dump.h>, <esp_wifi.h>, <HTTPClient.h>, <ArduinoOTA.h>,
     <Preferences.h>, uart_*, gpio_* (unless // BENCHMARK_EXCEPTION:).
   - L2 (except Uart_Driver): No raw uart_read_bytes/uart_write_bytes/etc.
4. Critical Section Atomic Purity (portENTER_CRITICAL / CriticalSectionLocker)
5. Mutex + NVS/Flash I/O Cross-Violation (non-NVS MutexLocker holding Flash I/O)
"""

import os
import re
from typing import List, Dict, Tuple, Optional, Set

HOT_PATH_FUNCTIONS = {
    "Task_Ch1": "CH1 Wallpad Real-Time Loop",
    "Task_Ch2Ch3": "CH2/CH3 Thermostat & Air Conditioner Real-Time Loop",
    "RS485_PollBus": "RS-485 Bus Polling Engine",
    "RS485_HandlePacket": "Packet Ingestion Engine",
    "Wallpad_ProcessRxPacket": "Hyundai Wallpad RX Ingestion",
    "Fcu_ProcessRxPacket": "FCU Modbus RX Ingestion",
}

REACTOR_FUNCTIONS = {
    "Task_Network": "Network Reactor Thread",
    "Remote_Tick": "Management Service Tick Loop",
    "Remote_ProcessEvents": "Management Service Event Loop",
    "Tcp_PollAndReceive": "TCP Session Multiplexer",
}

# L3 & L4 forbidden direct HAL/Platform headers (DIP enforcement)
FORBIDDEN_HEADERS_L3_L4 = [
    (r'^\s*#\s*include\s*[<"]WiFi\.h[>"]', "<WiFi.h> (Use System_Wifi* L0 contract)"),
    (r'^\s*#\s*include\s*[<"]esp_wifi\.h[>"]', "<esp_wifi.h> (Use System_Wifi* L0 contract)"),
    (r'^\s*#\s*include\s*[<"]esp_ota_ops\.h[>"]', "<esp_ota_ops.h> (Use System_GetOtaPartitions/System_Ota* L0 contract)"),
    (r'^\s*#\s*include\s*[<"]esp_task_wdt\.h[>"]', "<esp_task_wdt.h> (Use System_TaskWdt* L0 contract)"),
    (r'^\s*#\s*include\s*[<"]esp_core_dump\.h[>"]', "<esp_core_dump.h> (Use System_GetCoreDump*/System_EraseCoreDump L0 contract)"),
    (r'^\s*#\s*include\s*[<"]HTTPClient\.h[>"]', "<HTTPClient.h> (Use L1/L0 OTA/HTTP driver)"),
    (r'^\s*#\s*include\s*[<"]ArduinoOTA\.h[>"]', "<ArduinoOTA.h> (Use L1/L0 OTA driver)"),
    (r'^\s*#\s*include\s*[<"]Preferences\.h[>"]', "<Preferences.h> (Use SystemNvs/Config L0 contract)"),
]

# L3 & L4 forbidden direct HAL/Platform API patterns (DIP enforcement)
FORBIDDEN_SYMBOLS_L3_L4 = [
    (r"\bWiFi\s*\.", "WiFi.* direct Arduino call (use System_Wifi* L0 contract)"),
    (r"\besp_ota_[a-zA-Z0-9_]*\s*\(", "esp_ota_* direct IDF call (use System_Ota* L0 contract)"),
    (r"\besp_task_wdt_[a-zA-Z0-9_]*\s*\(", "esp_task_wdt_* direct IDF call (use System_TaskWdt* L0 contract)"),
    (r"\besp_core_dump_[a-zA-Z0-9_]*\s*\(", "esp_core_dump_* direct IDF call (use System_*CoreDump* L0 contract)"),
    (r"\besp_wifi_[a-zA-Z0-9_]*\s*\(", "esp_wifi_* direct IDF call (use System_Wifi* L0 contract)"),
    (r"\bPreferences\b", "Arduino Preferences direct instantiation (use SystemNvs/Config L0 contract)"),
    (r"\buart_[a-zA-Z0-9_]+\s*\(", "uart_* direct IDF UART call"),
    (r"\bgpio_[a-zA-Z0-9_]+\s*\(", "gpio_* direct IDF GPIO call"),
    (r"\besp_rom_delay_us\s*\(", "esp_rom_delay_us() direct ROM call"),
    (r"\bg_doorphone_serial\b", "g_doorphone_serial direct HW serial access"),
]

# L2 forbidden direct IDF UART calls (must go through L1 Uart_Driver)
FORBIDDEN_SYMBOLS_L2 = [
    (r"\buart_read_bytes\s*\(", "uart_read_bytes() direct call in L2 (use Uart_ReadHw)"),
    (r"\buart_write_bytes\s*\(", "uart_write_bytes() direct call in L2 (use Uart_WriteHw)"),
    (r"\buart_get_buffered_data_len\s*\(", "uart_get_buffered_data_len() direct call in L2 (use Uart_AvailableHw)"),
    (r"\buart_flush_input\s*\(", "uart_flush_input() direct call in L2 (use Uart_FlushHw)"),
    (r"\buart_wait_tx_done\s*\(", "uart_wait_tx_done() direct call in L2 (use Uart_WaitTxDoneHw)"),
    (r"\buart_driver_install\s*\(", "uart_driver_install() direct call in L2 (use Uart_InitHw)"),
    (r"\buart_set_pin\s*\(", "uart_set_pin() direct call in L2 (use Uart_InitHw)"),
    (r"\buart_param_config\s*\(", "uart_param_config() direct call in L2 (use Uart_ReconfigHw)"),
]

HOT_PATH_FORBIDDEN_CALLS = [
    (r"\bmalloc\s*\(", "malloc() dynamic heap allocation"),
    (r"\bcalloc\s*\(", "calloc() dynamic heap allocation"),
    (r"\brealloc\s*\(", "realloc() dynamic heap allocation"),
    (r"\bfree\s*\(", "free() dynamic heap deallocation"),
    (r"\bstrdup\s*\(", "strdup() dynamic heap allocation"),
    (r"\bnew\s+(?!\()", "C++ 'new' dynamic allocation"),
    (r"\bdelete\s+", "C++ 'delete' heap deallocation"),
    (r"\bString\s+[a-zA-Z0-9_]+\s*[=;{(]", "Dynamic Arduino String instantiation"),
    (r"\bstd::string\b", "Dynamic std::string usage"),
]

REACTOR_FORBIDDEN_CALLS = [
    (r"\bvTaskDelay\s*\(", "FreeRTOS vTaskDelay() blocking delay in reactor thread"),
    (r"\bdelay\s*\(", "Arduino delay() blocking delay in reactor thread"),
    (r"\bdelayMicroseconds\s*\(", "delayMicroseconds() spinlock in reactor thread"),
    (r"\busleep\s*\(", "POSIX usleep() blocking in reactor thread"),
    (r"\bsleep\s*\(", "POSIX sleep() blocking in reactor thread"),
    (r"\bWiFi\.scanNetworks\s*\(\s*false\s*,\s*true\b", "Synchronous WiFi.scanNetworks(false, true) reactor freeze"),
]

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

# NVS / Flash I/O operations that must NEVER run while holding non-NVS MutexLocker
FLASH_IO_FORBIDDEN_IN_MUTEX = [
    (r"\bSystemNvs\b", "SystemNvs NVS handle creation inside non-NVS MutexLocker"),
    (r"\bPreferences\b", "Preferences NVS handle creation inside non-NVS MutexLocker"),
    (r"\bnvs(?:GetEnv|PutEnv|GetEnvNs|PutEnvNs|ClearNs|ReadPrimitive)\b", "NVS envelope/helper Flash I/O inside non-NVS MutexLocker"),
    (r"\bSystem_NvsPutUInt\s*\(", "System_NvsPutUInt() Flash write inside non-NVS MutexLocker"),
    (r"\bnvs_(?:open|close|set_|get_|commit|erase_)", "ESP-IDF nvs_* Flash I/O inside non-NVS MutexLocker"),
    (r"\bConfig_(?:Save|SaveStaged|SaveWallpadProfile|SaveTelnetHash)\s*\(", "Config NVS save inside non-NVS MutexLocker"),
    (r"\bTimingConfig_Save\s*\(", "TimingConfig_Save() inside non-NVS MutexLocker"),
]

# Dedicated NVS-serialization mutexes allowed to guard Flash writes against concurrent NVS writers
NVS_SERIALIZATION_MUTEXES = {
    "s_save_mutex",
    "getSaveMutex",
    "_nvs_mutex",
    "commit_mutex",
    "_commit_mutex",
}

EXCEPTION_MARKERS = ("BENCHMARK_EXCEPTION:", "HAL_EXCEPTION:")


class CodeStrip:
    """Strips comments and string literals from C++ code while preserving line numbers."""
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
                        buf.append(" " * (n - i))
                        break
                    elif line[i:i+2] == "/*":
                        in_block_comment = True
                        buf.append("  ")
                        i += 2
                    elif line[i] == '"':
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
    functions = []
    brace_depth = 0
    current_func_name = None
    current_func_start_line = None
    current_func_lines = []
    candidate_header = []

    for line_no, line in stripped_lines:
        line_clean = line.strip()

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


def _has_exception_marker(raw_lines: List[str], line_no: int) -> bool:
    idx = line_no - 1
    if 0 <= idx < len(raw_lines):
        if any(m in raw_lines[idx] for m in EXCEPTION_MARKERS):
            return True
    if idx - 1 >= 0:
        if any(m in raw_lines[idx - 1] for m in EXCEPTION_MARKERS):
            return True
    return False


def verify_critical_sections(rel_path: str, stripped_lines: List[Tuple[int, str]]) -> List[str]:
    violations = []
    in_explicit_critical = False
    locker_brace_depths: List[int] = []
    current_brace_depth = 0

    for line_no, line in stripped_lines:
        open_braces = line.count("{")
        close_braces = line.count("}")

        if "CriticalSectionLocker" in line:
            locker_brace_depths.append(current_brace_depth + (1 if "{" in line else 0))

        if "portENTER_CRITICAL" in line or "taskENTER_CRITICAL" in line:
            in_explicit_critical = True

        is_in_critical = in_explicit_critical or (len(locker_brace_depths) > 0)

        if is_in_critical:
            for pattern, reason in CRITICAL_SECTION_FORBIDDEN_CALLS:
                if re.search(pattern, line):
                    if "CriticalSectionLocker" in line and "MutexLocker" not in line:
                        continue
                    violations.append(
                        f"[CRITICAL SECTION PURITY VIOLATION] {rel_path}:{line_no}\n"
                        f"    Found: {reason}\n"
                        f"    Code : {line.strip()}"
                    )

        if "portEXIT_CRITICAL" in line or "taskEXIT_CRITICAL" in line:
            in_explicit_critical = False

        current_brace_depth += open_braces - close_braces

        while locker_brace_depths and current_brace_depth < locker_brace_depths[-1]:
            locker_brace_depths.pop()

    return violations


def verify_mutex_flash_safety(rel_path: str, stripped_lines: List[Tuple[int, str]], raw_lines: List[str]) -> List[str]:
    """
    Detects blocking NVS/Flash I/O performed while holding any non-NVS MutexLocker.
    Prevents P0 lock-hold latency spikes (tens of ms flash stalls blocking channels/queues).
    """
    violations: List[str] = []
    # Stack of (brace_depth, mutex_name)
    active_mutex_scopes: List[Tuple[int, str]] = []
    current_brace_depth = 0

    for line_no, line in stripped_lines:
        open_braces = line.count("{")
        close_braces = line.count("}")

        # Skip function declarations or class definitions matching MutexLocker
        m_lock = re.search(r"\bMutexLocker\s+[a-zA-Z0-9_]+\s*\(\s*([a-zA-Z0-9_>\.-]+(?:\(\))?)", line)
        if m_lock and "class " not in line:
            mux_expr = m_lock.group(1)
            base_name = mux_expr.replace("()", "").split("->")[-1].split(".")[-1]
            if base_name not in NVS_SERIALIZATION_MUTEXES:
                active_mutex_scopes.append((current_brace_depth, base_name))

        if active_mutex_scopes and not _has_exception_marker(raw_lines, line_no):
            for pattern, reason in FLASH_IO_FORBIDDEN_IN_MUTEX:
                if re.search(pattern, line):
                    held_names = ", ".join(name for _, name in active_mutex_scopes)
                    violations.append(
                        f"[MUTEX FLASH I/O VIOLATION] {rel_path}:{line_no} (holding '{held_names}')\n"
                        f"    Found: {reason}\n"
                        f"    Code : {line.strip()}\n"
                        f"    Fix  : Copy data under lock, release MutexLocker, then execute Flash I/O."
                    )
                    break

        current_brace_depth += open_braces - close_braces
        while active_mutex_scopes and current_brace_depth < active_mutex_scopes[-1][0]:
            active_mutex_scopes.pop()

    return violations


def verify_call_invariants(project_root: str, target_files: Optional[List[str]] = None) -> List[str]:
    violations: List[str] = []

    files_to_scan: List[str] = []
    if target_files:
        for tf in target_files:
            full_p = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(project_root, tf))
            if os.path.exists(full_p) and full_p.endswith((".cpp", ".c", ".h", ".hpp")):
                files_to_scan.append(full_p)
    else:
        for sub in ("src", "include"):
            scan_root = os.path.join(project_root, sub)
            if not os.path.exists(scan_root):
                continue
            for root, _, files in os.walk(scan_root):
                for fname in sorted(files):
                    if fname.endswith((".cpp", ".c", ".h", ".hpp")):
                        files_to_scan.append(os.path.join(root, fname))

    for fpath in files_to_scan:
        rel_path = os.path.relpath(fpath, project_root).replace("\\", "/")

        with open(fpath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()

        raw_lines = content.splitlines()
        stripped = CodeStrip.strip(content)
        is_source = rel_path.endswith((".cpp", ".c"))

        current_layer = None
        for layer in ("L0_Foundation", "L1_HAL", "L2_Transport", "L3_Protocol", "L4_Services"):
            if f"/{layer}/" in rel_path or rel_path.startswith(f"{layer}/") or f"src/{layer}" in rel_path or f"include/{layer}" in rel_path:
                current_layer = layer
                break

        # DIP Header check in raw lines (for L3/L4)
        if current_layer in ("L3_Protocol", "L4_Services"):
            for lno, rline in enumerate(raw_lines, start=1):
                if _has_exception_marker(raw_lines, lno):
                    continue
                for h_pat, h_reason in FORBIDDEN_HEADERS_L3_L4:
                    if re.match(h_pat, rline):
                        violations.append(
                            f"[DIP FORBIDDEN HEADER] {rel_path}:{lno} ({current_layer})\n"
                            f"    Found: {h_reason}\n"
                            f"    Code : {rline.strip()}"
                        )

        if not is_source:
            continue

        # 1. Critical Section Atomic Purity & Mutex+Flash Safety
        violations.extend(verify_critical_sections(rel_path, stripped))
        violations.extend(verify_mutex_flash_safety(rel_path, stripped, raw_lines))

        functions = extract_function_blocks(stripped)
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

        # 4. DIP / Layer Bypass Verification (L3/L4 direct HAL/Platform API)
        if current_layer in ("L4_Services", "L3_Protocol"):
            for lno, line in stripped:
                if _has_exception_marker(raw_lines, lno):
                    continue
                for sym_pat, sym_reason in FORBIDDEN_SYMBOLS_L3_L4:
                    if re.search(sym_pat, line):
                        violations.append(
                            f"[DIP / LAYER BYPASS VIOLATION] {rel_path}:{lno} ({current_layer})\n"
                            f"    Found: {sym_reason}\n"
                            f"    Code : {line.strip()}"
                        )

        # 5. L2 Transport direct IDF UART HAL bypass check
        if current_layer == "L2_Transport":
            for lno, line in stripped:
                if _has_exception_marker(raw_lines, lno):
                    continue
                for sym_pat, sym_reason in FORBIDDEN_SYMBOLS_L2:
                    if re.search(sym_pat, line):
                        violations.append(
                            f"[L2 UART HAL BYPASS VIOLATION] {rel_path}:{lno}\n"
                            f"    Found: {sym_reason}\n"
                            f"    Code : {line.strip()}"
                        )

    return violations


if __name__ == "__main__":
    import sys
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    viols = verify_call_invariants(proj_root)
    if viols:
        print(f"\n[E-ASTG FATAL] {len(viols)} Function Call / DIP Violations Detected!\n")
        for v in viols:
            print(f"  {v}\n")
        sys.exit(1)
    else:
        print("[E-ASTG PASS] Module 2 (Call, DIP, Critical Section & Mutex+Flash Safety): Verified clean.")
        sys.exit(0)
