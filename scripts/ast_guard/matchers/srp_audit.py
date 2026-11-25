#!/usr/bin/env python3
"""
srp_audit.py - Gate 4: Single Responsibility Principle (SRP) & God-File Reporter
Measures physical file line counts and per-function line counts across src/ and include/:
  - >= 1000 lines : [SRP GOD FILE] (Fails under --strict)
  - >=  600 lines : [SRP WARN]
  - Function >= 150 lines : Function split candidate
"""

import os
import sys
from typing import Dict, List, Optional, Tuple
from matchers.call_guard import CodeStrip, extract_function_blocks

GOD_FILE_LINES = 1000
WARN_FILE_LINES = 600
WARN_FUNC_LINES = 150


def audit_srp_metrics(project_root: str, target_files: Optional[List[str]] = None) -> Dict:
    god_files: List[Tuple[str, int]] = []
    warn_files: List[Tuple[str, int]] = []
    long_funcs: List[Tuple[str, str, int, int]] = []

    files_to_scan: List[str] = []
    if target_files:
        for tf in target_files:
            fp = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(project_root, tf))
            if os.path.exists(fp) and fp.endswith((".cpp", ".c", ".h", ".hpp")):
                files_to_scan.append(fp)
    else:
        for sub in ("src", "include"):
            d = os.path.join(project_root, sub)
            if not os.path.exists(d):
                continue
            for root, _, files in os.walk(d):
                for fn in sorted(files):
                    if fn.endswith((".cpp", ".c", ".h", ".hpp")):
                        files_to_scan.append(os.path.join(root, fn))

    for fpath in files_to_scan:
        rel_path = os.path.relpath(fpath, project_root).replace("\\", "/")
        with open(fpath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()
        total_lines = len(content.splitlines())

        if total_lines >= GOD_FILE_LINES:
            god_files.append((rel_path, total_lines))
        elif total_lines >= WARN_FILE_LINES:
            warn_files.append((rel_path, total_lines))

        if rel_path.endswith((".cpp", ".c")):
            stripped = CodeStrip.strip(content)
            for func in extract_function_blocks(stripped):
                flen = func["end_line"] - func["start_line"] + 1
                if flen >= WARN_FUNC_LINES:
                    long_funcs.append((rel_path, func["name"], func["start_line"], flen))

    god_files.sort(key=lambda x: -x[1])
    warn_files.sort(key=lambda x: -x[1])
    long_funcs.sort(key=lambda x: -x[3])

    return {
        "god_files": god_files,
        "warn_files": warn_files,
        "long_funcs": long_funcs,
    }


if __name__ == "__main__":
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    strict = "--strict" in sys.argv
    res = audit_srp_metrics(proj_root)
    print(f"[SRP AUDIT] God Files (>={GOD_FILE_LINES}L): {len(res['god_files'])}, "
          f"Warn Files (>={WARN_FILE_LINES}L): {len(res['warn_files'])}, "
          f"Long Funcs (>={WARN_FUNC_LINES}L): {len(res['long_funcs'])}")
    for fp, n in res["god_files"]:
        print(f"  [SRP GOD FILE] {fp}: {n}L")
    for fp, n in res["warn_files"]:
        print(f"  [SRP WARN]     {fp}: {n}L")
    for fp, fn, line_no, n in res["long_funcs"][:10]:
        print(f"  [LONG FUNC]    {fp}:{line_no} {fn}() -> {n}L")
    if strict and res["god_files"]:
        sys.exit(1)
    sys.exit(0)
