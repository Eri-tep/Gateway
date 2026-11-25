#!/usr/bin/env python3
"""
srp_audit.py - Gate 4B: Cohesion-Centered SRP & Long Function Auditor
Evaluates Single Responsibility Principle without penalizing large cohesive single-domain files:
  - Multi-Domain File: Large .cpp (>= 1000L) containing >= 2 distinct major domain classes
    (>= 3 methods defined per ClassName::) -> Fails under --strict.
  - Long Function (>= 150 lines): Candidate for internal helper extraction -> Fails under --strict.
  - Cohesive Large File (>= 1000 lines, single domain): Info-only metric.
"""

import os
import re
import sys
from collections import Counter
from typing import Dict, List, Optional, Tuple
from matchers.call_guard import CodeStrip, extract_function_blocks

COHESIVE_INFO_LINES = 1000
WARN_FUNC_LINES = 150
MAJOR_CLASS_MIN_METHODS = 3

CLASS_METHOD_RE = re.compile(r"(?m)^\s*(?:[a-zA-Z0-9_:*&<>]+\s+)+([A-Z][a-zA-Z0-9_]+)::[a-zA-Z0-9_~]+\s*\(")
COHESIVE_ACK_RE = re.compile(r"//\s*SRP_COHESIVE\s*:\s*\S+")


def audit_srp_metrics(project_root: str, target_files: Optional[List[str]] = None) -> Dict:
    multi_domain_files: List[Tuple[str, int, List[str]]] = []
    cohesive_large_files: List[Tuple[str, int]] = []
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

        if rel_path.endswith((".cpp", ".c")):
            stripped = CodeStrip.strip(content)
            for func in extract_function_blocks(stripped):
                flen = func["end_line"] - func["start_line"] + 1
                if flen >= WARN_FUNC_LINES:
                    long_funcs.append((rel_path, func["name"], func["start_line"], flen))

            if total_lines >= COHESIVE_INFO_LINES:
                has_cohesive_ack = bool(COHESIVE_ACK_RE.search(content))
                stripped_text = "\n".join(ln for _, ln in stripped)
                cls_counts = Counter(CLASS_METHOD_RE.findall(stripped_text))
                major_classes = sorted([c for c, cnt in cls_counts.items() if cnt >= MAJOR_CLASS_MIN_METHODS])
                if len(major_classes) >= 2 and not has_cohesive_ack:
                    multi_domain_files.append((rel_path, total_lines, major_classes))
                else:
                    cohesive_large_files.append((rel_path, total_lines))

    multi_domain_files.sort(key=lambda x: -x[1])
    cohesive_large_files.sort(key=lambda x: -x[1])
    long_funcs.sort(key=lambda x: -x[3])

    return {
        "multi_domain_files": multi_domain_files,
        "cohesive_large_files": cohesive_large_files,
        "long_funcs": long_funcs,
    }


if __name__ == "__main__":
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    strict = "--strict" in sys.argv
    res = audit_srp_metrics(proj_root)
    print(f"[SRP AUDIT] Multi-Domain (>=1000L): {len(res['multi_domain_files'])}, "
          f"Long Funcs (>={WARN_FUNC_LINES}L): {len(res['long_funcs'])}, "
          f"Cohesive Large Files (>=1000L): {len(res['cohesive_large_files'])}")
    for fp, n, classes in res["multi_domain_files"]:
        print(f"  [SRP MULTI-DOMAIN] {fp}: {n}L (classes: {', '.join(classes)})")
    for fp, fn, line_no, n in res["long_funcs"]:
        print(f"  [SRP LONG FUNC]    {fp}:{line_no} {fn}() -> {n}L")
    for fp, n in res["cohesive_large_files"]:
        print(f"  [INFO COHESIVE]    {fp}: {n}L")
    if strict and (res["multi_domain_files"] or res["long_funcs"]):
        sys.exit(1)
    sys.exit(0)
