#!/usr/bin/env python3
# ============================================================================
# layer_guard.py: Canonical 4+1 Layer Invariant Checker (AST Pre-Build Guard)
# AGENTS.md Rule 17: No Upward Includes (L_M -> L_N where M < N is FORBIDDEN)
# L0 Base Foundation Soil (System_Buffer, System_Config, System_Platform, ThreadSafety)
# is Leaf Soil, accessible from any layer (L1 ~ L4) directly.
# ============================================================================

import os
import re
import sys
from typing import List, Tuple, Dict, Optional

# Match #include "..." or #include <...>
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')

# Layer path keywords
LAYER_MAP = {
    "L0_Foundation": 0,
    "L1_HAL": 1,
    "L2_Transport": 2,
    "L3_Protocol": 3,
    "L4_Services": 4,
}

def detect_file_layer(file_path: str) -> int:
    for layer_name, level in LAYER_MAP.items():
        if f"/{layer_name}/" in file_path or file_path.startswith(f"{layer_name}/"):
            return level
    return -1  # Not a tiered layer file (e.g. main.cpp)

def detect_included_layer(include_path: str) -> int:
    for layer_name, level in LAYER_MAP.items():
        if include_path.startswith(f"{layer_name}/") or f"/{layer_name}/" in include_path:
            return level
    return -1

def scan_file_for_layer_violations(file_path: str) -> List[Tuple[int, str, int, int]]:
    """
    Returns list of tuples: (line_no, line_content, src_layer, target_layer)
    where target_layer > src_layer and target_layer != 0 (Upward include violation).
    """
    src_layer = detect_file_layer(file_path)
    if src_layer <= 0:
        # L0 is foundation leaf, main.cpp or root files are outside L1..L4
        return []

    violations = []
    try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            for idx, line in enumerate(f, start=1):
                m = INCLUDE_PATTERN.match(line)
                if not m:
                    continue
                inc_target = m.group(1)
                target_layer = detect_included_layer(inc_target)
                if target_layer > src_layer:
                    violations.append((idx, line.strip(), src_layer, target_layer))
    except Exception as e:
        print(f"[WARN] Error reading {file_path}: {e}", file=sys.stderr)

    return violations

def check_all_layers(root_dir: str, target_files: Optional[List[str]] = None) -> Tuple[int, List[Dict]]:
    violation_reports = []
    total_files_scanned = 0

    if target_files:
        for tf in target_files:
            fp = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(root_dir, tf))
            if os.path.exists(fp) and fp.endswith((".cpp", ".h", ".hpp", ".c")):
                total_files_scanned += 1
                rel_fp = os.path.relpath(fp, root_dir)
                v = scan_file_for_layer_violations(fp)
                for line_no, content, s_l, t_l in v:
                    violation_reports.append({
                        "file": rel_fp,
                        "line": line_no,
                        "content": content,
                        "src_layer": f"L{s_l}",
                        "target_layer": f"L{t_l}",
                    })
        return total_files_scanned, violation_reports

    scan_dirs = ["src", "include"]
    for sub in scan_dirs:
        dir_path = os.path.join(root_dir, sub)
        if not os.path.exists(dir_path):
            continue
        for root, _, files in os.walk(dir_path):
            for file in files:
                if file.endswith((".cpp", ".h", ".hpp", ".c")):
                    total_files_scanned += 1
                    fp = os.path.join(root, file)
                    rel_fp = os.path.relpath(fp, root_dir)
                    v = scan_file_for_layer_violations(fp)
                    for line_no, content, s_l, t_l in v:
                        violation_reports.append({
                            "file": rel_fp,
                            "line": line_no,
                            "content": content,
                            "src_layer": f"L{s_l}",
                            "target_layer": f"L{t_l}",
                        })

    return total_files_scanned, violation_reports

if __name__ == "__main__":
    proj_root = sys.argv[1] if len(sys.argv) > 1 else "."
    files_cnt, reports = check_all_layers(proj_root)
    if reports:
        print(f"\n\033[91m[CANONICAL 4+1 LAYER VIOLATIONS DETECTED] ({len(reports)} violations in {files_cnt} files)\033[0m")
        for r in reports:
            print(f"  \033[93m{r['file']}:{r['line']}\033[0m -> {r['content']}")
            print(f"    \033[91mForbidden Upward Include: {r['src_layer']} cannot include higher {r['target_layer']}\033[0m (AGENTS.md Rule 17)")
        sys.exit(1)
    else:
        print(f"\033[92m[LAYER GUARD CLEAN] All {files_cnt} files conform to Canonical 4+1 Layer Invariant (No Upward Includes).\033[0m")
        sys.exit(0)
