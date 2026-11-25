#!/usr/bin/env python3
"""
cpp_hygiene_guard.py - Gate 3: ESP-IDF C++ Guide & Project C++23 Hygiene Guard
Verifies conformance to Espressif ESP-IDF C++ rules and AGENTS.md invariants:
  1. platformio.ini flags: -std=gnu++23 paired with unflag -std=gnu++2b,
     -fno-exceptions active, -fno-rtti not placed in C/C++ shared build_flags.
  2. No STL mutex primitives (std::mutex, std::shared_mutex, std::unique_lock,
     std::lock_guard) — use FreeRTOS MutexLocker.
  3. No C++ exceptions (try/catch/throw) or RTTI (dynamic_cast/typeid).
  4. No global header scope 'using namespace ...;' or 'using std::...;'.
  5. No Arduino String instantiations / .toString() / direct Preferences usage.
  6. Non-const extern variable declarations in headers (warning / strict).
"""

import os
import re
import sys
from typing import List, Optional, Tuple
from matchers.call_guard import CodeStrip

EXCEPTION_MARKERS = ("STRING_EXCEPTION:", "HYGIENE_EXCEPTION:")
EXTERN_EXCEPTION_MARKER = "EXTERN_OK:"

STL_MUTEX_PATTERNS = [
    (r"^\s*#\s*include\s*<(?:shared_)?mutex>", "Forbidden <mutex>/<shared_mutex> (Use FreeRTOS MutexLocker)"),
    (r"\bstd::(?:shared_mutex|mutex|recursive_mutex|timed_mutex|unique_lock|lock_guard|scoped_lock)\b",
     "Forbidden STL mutex primitive (Use FreeRTOS MutexLocker / StaticSemaphore_t)"),
]

NO_EXCEPTIONS_RTTI_PATTERNS = [
    (r"\btry\s*\{", "Forbidden C++ exception 'try' block (-fno-exceptions)"),
    (r"\bcatch\s*\(", "Forbidden C++ exception 'catch' handler (-fno-exceptions)"),
    (r"\bthrow\s*[^;]*;", "Forbidden C++ 'throw' statement (-fno-exceptions)"),
    (r"\bdynamic_cast\s*<", "Forbidden RTTI 'dynamic_cast' (-fno-rtti)"),
    (r"\btypeid\s*\(", "Forbidden RTTI 'typeid' (-fno-rtti)"),
]

STRING_PATTERNS = [
    (r"\bString\s+[a-zA-Z0-9_]+\s*[=;{(]", "Arduino String heap allocation (Use fixed char[] / AppendBuf)"),
    (r"\.toString\s*\(\s*\)", "Arduino .toString() heap String allocation (Use snprintf on IP octets)"),
    (r"^\s*#\s*include\s*[<\"](?:Preferences)\.h[>\"]", "Direct <Preferences.h> include (Use SystemNvs L0 contract)"),
    (r"\bPreferences\b", "Direct Arduino Preferences usage (Use SystemNvs L0 contract)"),
]


def _has_marker(raw_lines: List[str], line_no: int, markers: Tuple[str, ...]) -> bool:
    idx = line_no - 1
    if 0 <= idx < len(raw_lines) and any(m in raw_lines[idx] for m in markers):
        return True
    if idx - 1 >= 0 and any(m in raw_lines[idx - 1] for m in markers):
        return True
    return False


def check_platformio_flags(project_root: str) -> List[str]:
    viols: List[str] = []
    ini_path = os.path.join(project_root, "platformio.ini")
    if not os.path.exists(ini_path):
        return viols

    with open(ini_path, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()

    # Check flags only when -std=gnu++23 is active
    if "-std=gnu++23" in text:
        unflags_match = re.search(r"build_unflags\s*=(.*?)(?=\n[a-zA-Z0-9_]+\s*=|\n\[|\Z)", text, re.S)
        unflags_block = unflags_match.group(1) if unflags_match else ""
        if "-std=gnu++2b" not in unflags_block:
            viols.append(
                "[CPP FLAGS VIOLATION] platformio.ini: '-std=gnu++23' requires '-std=gnu++2b' in build_unflags "
                "to prevent duplicate C++ standard flags."
            )

    flags_match = re.search(r"(?m)^build_flags\s*=(.*?)(?=\n[a-zA-Z0-9_]+\s*=|\n\[|\Z)", text, re.S)
    if flags_match:
        flags_block = flags_match.group(1)
        if "-fno-exceptions" not in flags_block:
            viols.append("[CPP FLAGS VIOLATION] platformio.ini: Missing '-fno-exceptions' in build_flags.")
        if "-fno-rtti" in flags_block:
            viols.append(
                "[CPP FLAGS VIOLATION] platformio.ini: '-fno-rtti' in shared build_flags causes C compiler warnings; "
                "enforce in CXXFLAGS via pio_guard_hook.py."
            )

    return viols


def verify_cpp_hygiene(project_root: str, target_files: Optional[List[str]] = None) -> Tuple[List[str], List[str]]:
    """
    Returns (fatal_violations, warnings).
    """
    fatals: List[str] = []
    warnings: List[str] = []

    if not target_files:
        fatals.extend(check_platformio_flags(project_root))

    files_to_scan: List[str] = []
    if target_files:
        for tf in target_files:
            fp = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(project_root, tf))
            if os.path.exists(fp) and fp.endswith((".h", ".hpp", ".cpp", ".c")):
                files_to_scan.append(fp)
    else:
        for sub in ("include", "src"):
            d = os.path.join(project_root, sub)
            if not os.path.exists(d):
                continue
            for root, _, files in os.walk(d):
                for fn in sorted(files):
                    if fn.endswith((".h", ".hpp", ".cpp", ".c")):
                        files_to_scan.append(os.path.join(root, fn))

    for fpath in files_to_scan:
        rel_path = os.path.relpath(fpath, project_root).replace("\\", "/")
        is_header = rel_path.endswith((".h", ".hpp"))

        with open(fpath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()

        raw_lines = content.splitlines()
        stripped = CodeStrip.strip(content)

        # Header checks: global using pollution & mutable externs
        brace_depth = 0
        for lno, line in stripped:
            s = line.strip()
            if not _has_marker(raw_lines, lno, EXCEPTION_MARKERS):
                # 1. STL mutexes
                for pat, msg in STL_MUTEX_PATTERNS:
                    if re.search(pat, raw_lines[lno - 1] if pat.startswith("^") else line):
                        fatals.append(f"[CPP HYGIENE] {rel_path}:{lno} -> {msg}\n    Code: {raw_lines[lno - 1].strip()}")

                # 2. Exceptions & RTTI
                for pat, msg in NO_EXCEPTIONS_RTTI_PATTERNS:
                    if re.search(pat, line):
                        fatals.append(f"[CPP HYGIENE] {rel_path}:{lno} -> {msg}\n    Code: {s}")

                # 3. Arduino String / Preferences
                for pat, msg in STRING_PATTERNS:
                    target_str = raw_lines[lno - 1] if pat.startswith("^") else line
                    if re.search(pat, target_str):
                        fatals.append(f"[CPP HYGIENE] {rel_path}:{lno} -> {msg}\n    Code: {raw_lines[lno - 1].strip()}")

                # 4. Header top-level 'using' pollution (allow 'using Alias = ...;' type aliases)
                if is_header and brace_depth == 0:
                    if re.match(r"^\s*using\s+(?:namespace\s+\w+|std::[a-zA-Z0-9_]+)\s*;", s):
                        fatals.append(
                            f"[CPP HYGIENE] {rel_path}:{lno} -> Global header 'using' declaration pollutes includers\n"
                            f"    Code: {s}"
                        )

            # 5. Non-const extern state leaks in headers (Rule 17)
            if is_header and brace_depth == 0:
                if re.match(r"^\s*extern\b", s) and 'extern "C"' not in s and "(" not in s:
                    if not re.search(r"\bconst\b|\bconstexpr\b", s):
                        if not _has_marker(raw_lines, lno, (EXTERN_EXCEPTION_MARKER,)):
                            warnings.append(
                                f"[EXTERN STATE WARN] {rel_path}:{lno} -> Mutable extern declaration in header (Rule 17)\n"
                                f"    Code: {s}"
                            )

            brace_depth += line.count("{") - line.count("}")

    return fatals, warnings


if __name__ == "__main__":
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    fatals, warns = verify_cpp_hygiene(proj_root)
    if warns:
        print(f"\n[E-ASTG WARN] {len(warns)} C++ Hygiene / Extern Warnings:")
        for w in warns:
            print(f"  {w}")
    if fatals:
        print(f"\n[E-ASTG FATAL] {len(fatals)} C++ Hygiene Violations Detected!\n")
        for v in fatals:
            print(f"  {v}\n")
        sys.exit(1)
    print("[E-ASTG PASS] Module 3 (ESP-IDF C++ Hygiene & Flags): Verified clean.")
    sys.exit(0)
