#!/usr/bin/env python3
"""
Architecture Invariant Verification Tool (Arch-Linter)
Integrated project: M5Stack AtomS3 Lite RS-485/TCP Gateway Firmware

Verifies the 4+1 Clean Architecture invariants:
1. Strict Downlink: No upward includes (L_M -> L_N where M < N is strictly forbidden).
2. L0 Foundation Soil: Pure global leaf; zero dependency on L1~L4.
3. L3 Shell-Core Model: L3 Private core engines hidden from external layers (L2, L4).
4. Information Hiding: Zero mutable extern variables in public headers (100% sealed).
"""

import os
import re
import sys
from pathlib import Path

# ANSI colors
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
CYAN = "\033[96m"
BOLD = "\033[1m"
RESET = "\033[0m"

PROJECT_ROOT = Path(__file__).resolve().parent.parent

LAYERS = {
    0: ("L0_Foundation", ["include/L0_Foundation", "src/L0_Foundation"]),
    1: ("L1_HAL", ["include/L1_HAL", "src/L1_HAL"]),
    2: ("L2_Transport", ["include/L2_Transport", "src/L2_Transport"]),
    3: ("L3_Protocol", ["include/L3_Protocol", "src/L3_Protocol"]),
    4: ("L4_Services", ["include/L4_Services", "src/L4_Services"]),
}

# Whitelist for system-level low-level platform handles strictly defined in L0
EXTERN_WHITELIST = {
    "include/L0_Foundation/System_Platform.h": [
        "g_coredump_info",
        "g_system_event_group",
        "g_ota_in_progress",
    ]
}


def find_source_files(dirs):
    files = []
    for d in dirs:
        p = PROJECT_ROOT / d
        if not p.exists():
            continue
        for ext in ("*.h", "*.hpp", "*.c", "*.cpp"):
            files.extend(p.rglob(ext))
    return sorted(files)


def check_layer_invariants():
    violations = []
    include_pattern = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')

    for current_level, (current_name, dirs) in LAYERS.items():
        files = find_source_files(dirs)
        for filepath in files:
            rel_path = filepath.relative_to(PROJECT_ROOT)
            try:
                with open(filepath, "r", encoding="utf-8", errors="replace") as f:
                    for line_no, line in enumerate(f, 1):
                        m = include_pattern.match(line)
                        if not m:
                            continue
                        included = m.group(1)

                        # Check 1: Strict Downlink (No upward includes)
                        for target_level, (target_name, _) in LAYERS.items():
                            if target_level > current_level and target_name in included:
                                violations.append({
                                    "rule": f"Strict Downlink Violation (L{current_level} -> L{target_level})",
                                    "file": str(rel_path),
                                    "line": line_no,
                                    "content": line.strip(),
                                    "detail": f"{current_name} must never include higher layer {target_name}",
                                })

                        # Check 2: Shell-Core Information Hiding
                        # L3 Private headers must never be included by external layers (L2, L4)
                        if current_level in (2, 4) and "L3_Protocol/Private" in included:
                            violations.append({
                                "rule": "Shell-Core Violation (L3 Private Leak)",
                                "file": str(rel_path),
                                "line": line_no,
                                "content": line.strip(),
                                "detail": f"External layer {current_name} must not access L3 Private headers",
                            })
            except Exception as e:
                violations.append({
                    "rule": "File Read Error",
                    "file": str(rel_path),
                    "line": 0,
                    "content": str(e),
                    "detail": "Failed to read file",
                })
    return violations


def check_extern_invariants():
    violations = []
    include_dir = PROJECT_ROOT / "include"
    headers = sorted(list(include_dir.rglob("*.h")) + list(include_dir.rglob("*.hpp")))

    extern_pattern = re.compile(r'^\s*extern\s+([^;]+);')

    for header in headers:
        rel_path = str(header.relative_to(PROJECT_ROOT))
        try:
            with open(header, "r", encoding="utf-8", errors="replace") as f:
                for line_no, line in enumerate(f, 1):
                    raw_line = line.strip()
                    if not raw_line.startswith("extern"):
                        continue
                    if 'extern "C"' in raw_line:
                        continue
                    if "extern const" in raw_line or "extern constexpr" in raw_line:
                        continue

                    # Check against whitelist
                    whitelisted = False
                    if rel_path in EXTERN_WHITELIST:
                        for symbol in EXTERN_WHITELIST[rel_path]:
                            if symbol in raw_line:
                                whitelisted = True
                                break
                    if whitelisted:
                        continue

                    violations.append({
                        "rule": "Information Hiding Violation (Mutable Extern Leak)",
                        "file": rel_path,
                        "line": line_no,
                        "content": raw_line,
                        "detail": "Mutable extern variables are forbidden in public headers; seal in .cpp static scope",
                    })
        except Exception as e:
            violations.append({
                "rule": "File Read Error",
                "file": rel_path,
                "line": 0,
                "content": str(e),
                "detail": "Failed to read file",
            })
    return violations


def main():
    print(f"\n{BOLD}{CYAN}=== 4+1 Clean Architecture Invariant Linter ==={RESET}")
    print(f"Project Root: {PROJECT_ROOT}\n")

    layer_violations = check_layer_invariants()
    extern_violations = check_extern_invariants()

    all_violations = layer_violations + extern_violations

    # Results reporting
    print(f"{BOLD}[1] Upward Include Check (Strict Downlink & Shell-Core):{RESET}")
    if not layer_violations:
        print(f"    {GREEN}✔ PASS:{RESET} Zero upward includes detected (L4 -> L3 -> L2 -> L1 strict order verified).")
    else:
        print(f"    {RED}✘ FAIL:{RESET} {len(layer_violations)} violation(s) detected:")
        for v in layer_violations:
            print(f"      - {v['file']}:{v['line']} [{v['rule']}]")
            print(f"        {YELLOW}{v['content']}{RESET} -> {v['detail']}")

    print(f"\n{BOLD}[2] Public Header Extern Leak Check (100% Information Hiding):{RESET}")
    if not extern_violations:
        print(f"    {GREEN}✔ PASS:{RESET} Zero unauthorized mutable extern variables in include/ headers.")
    else:
        print(f"    {RED}✘ FAIL:{RESET} {len(extern_violations)} violation(s) detected:")
        for v in extern_violations:
            print(f"      - {v['file']}:{v['line']} [{v['rule']}]")
            print(f"        {YELLOW}{v['content']}{RESET} -> {v['detail']}")

    print("\n" + "=" * 50)
    if not all_violations:
        print(f"{BOLD}{GREEN}ALL ARCHITECTURAL INVARIANTS SATISFIED (0 Violations){RESET}\n")
        return 0
    else:
        print(f"{BOLD}{RED}TOTAL ARCHITECTURAL VIOLATIONS: {len(all_violations)}{RESET}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
