#!/usr/bin/env python3
# ============================================================================
# layer_guard.py: Gate 1 — Canonical 4+1 Layer, Boundary & Include DAG Checker
# ============================================================================
# Enforces AGENTS.md Rule 17:
#   1. Adjacent-Layer Pipeline: L4 -> {L3, L0}, L3 -> {L2, L0}, L2 -> {L1, L0},
#      L1 -> {L0}, L0 -> {L0} (No upward includes AND no layer skipping).
#   2. L3 Encapsulation Boundary:
#      - L4 and L3_Public headers cannot include L3_Protocol/Private/.
#      - L3_Private headers cannot reverse-include L3_Protocol/Public/ headers
#        except L3_Protocol/Public/Protocol_Types.h (leaf shared POD types).
#   3. Include Graph Acyclicity: DFS cycle detection over include/ and src/.
# ============================================================================

import os
import re
import sys
from typing import Dict, List, Optional, Set, Tuple

INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')

LAYER_MAP = {
    "L0_Foundation": 0,
    "L1_HAL": 1,
    "L2_Transport": 2,
    "L3_Protocol": 3,
    "L4_Services": 4,
}

# Strict downward-adjacent + L0 Foundation Soil rule
ALLOWED_TARGETS: Dict[int, Set[int]] = {
    0: {0},
    1: {1, 0},
    2: {2, 1, 0},
    3: {3, 2, 0},
    4: {4, 3, 0},
}

L3_LEAF_TYPE_HEADERS = {
    "L3_Protocol/Public/Protocol_Types.h",
}


def detect_file_layer(file_path: str) -> int:
    norm = file_path.replace("\\", "/")
    for layer_name, level in LAYER_MAP.items():
        if f"/{layer_name}/" in norm or norm.startswith(f"{layer_name}/"):
            return level
    return -1  # Composition root (e.g. main.cpp)


def detect_included_layer(include_path: str) -> int:
    norm = include_path.replace("\\", "/")
    for layer_name, level in LAYER_MAP.items():
        if norm.startswith(f"{layer_name}/") or f"/{layer_name}/" in norm:
            return level
    return -1


def detect_l3_subzone(path: str) -> Optional[str]:
    norm = path.replace("\\", "/")
    if "L3_Protocol/Private/" in norm:
        return "Private"
    if "L3_Protocol/Public/" in norm:
        return "Public"
    return None


def scan_file_for_layer_violations(file_path: str) -> List[Dict]:
    """
    Scans a single file for:
      - Upward includes (target > src)
      - Layer-skip bypass includes (e.g. L4->L1, L4->L2, L3->L1, L0->L1..L4)
      - L3 Private <-> Public encapsulation boundary violations
    """
    norm_fp = file_path.replace("\\", "/")
    src_layer = detect_file_layer(norm_fp)
    if src_layer < 0:
        return []

    is_header = norm_fp.endswith((".h", ".hpp"))
    src_l3_zone = detect_l3_subzone(norm_fp) if src_layer == 3 else None

    violations: List[Dict] = []
    try:
        with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
            for idx, line in enumerate(f, start=1):
                m = INCLUDE_PATTERN.match(line)
                if not m:
                    continue
                inc_target = m.group(1).replace("\\", "/")
                target_layer = detect_included_layer(inc_target)

                # 1. Layer Adjacency & Direction Check
                if target_layer >= 0 and target_layer not in ALLOWED_TARGETS[src_layer]:
                    kind = "Upward Include" if target_layer > src_layer else "Layer-Skip Bypass"
                    violations.append({
                        "line": idx,
                        "content": line.strip(),
                        "src_layer": f"L{src_layer}",
                        "target_layer": f"L{target_layer}",
                        "rule": f"Forbidden {kind}: L{src_layer} cannot include L{target_layer} (allowed: {sorted(ALLOWED_TARGETS[src_layer])})",
                    })
                    continue

                # 2. L3 Private Encapsulation Boundary Check
                tgt_l3_zone = detect_l3_subzone(inc_target) if target_layer == 3 else None
                if tgt_l3_zone == "Private":
                    # L4 or L3_Public headers cannot depend on L3_Private
                    if src_layer == 4 or (src_layer == 3 and src_l3_zone == "Public" and is_header):
                        violations.append({
                            "line": idx,
                            "content": line.strip(),
                            "src_layer": f"L{src_layer}",
                            "target_layer": "L3_Private",
                            "rule": "Forbidden Private Boundary Leak: L4 and L3 Public headers cannot include L3_Protocol/Private/",
                        })
                elif tgt_l3_zone == "Public":
                    # L3_Private headers cannot reverse-include L3_Public (except pure leaf Protocol_Types.h)
                    if src_layer == 3 and src_l3_zone == "Private" and is_header:
                        if not any(inc_target.endswith(leaf) for leaf in L3_LEAF_TYPE_HEADERS):
                            violations.append({
                                "line": idx,
                                "content": line.strip(),
                                "src_layer": "L3_Private(.h)",
                                "target_layer": "L3_Public(.h)",
                                "rule": "Forbidden Reverse Include: L3 Private header cannot include L3 Public header (use Protocol_Types.h or forward decl)",
                            })
    except Exception as e:
        print(f"[WARN] Error reading {file_path}: {e}", file=sys.stderr)

    return violations


def detect_include_cycles(root_dir: str) -> List[List[str]]:
    """
    Builds directed #include graph across include/ and src/ and returns
    any cycles found (each cycle is a list of relative file paths).
    """
    include_root = os.path.join(root_dir, "include")
    all_files: List[str] = []
    for sub in ("include", "src"):
        d = os.path.join(root_dir, sub)
        if not os.path.exists(d):
            continue
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.endswith((".h", ".hpp", ".cpp", ".c")):
                    all_files.append(os.path.relpath(os.path.join(root, fn), root_dir).replace("\\", "/"))

    file_set = set(all_files)
    graph: Dict[str, List[str]] = {fp: [] for fp in all_files}

    for rel_fp in all_files:
        abs_fp = os.path.join(root_dir, rel_fp)
        cur_dir = os.path.dirname(rel_fp)
        try:
            with open(abs_fp, "r", encoding="utf-8", errors="ignore") as f:
                for line in f:
                    m = INCLUDE_PATTERN.match(line)
                    if not m:
                        continue
                    inc = m.group(1).replace("\\", "/")
                    # Resolve relative to include/ or current directory
                    cand1 = f"include/{inc}"
                    cand2 = os.path.normpath(os.path.join(cur_dir, inc)).replace("\\", "/")
                    # Also check include/<Layer>/inc for -Iinclude/<Layer> flags
                    resolved = None
                    if cand1 in file_set:
                        resolved = cand1
                    elif cand2 in file_set:
                        resolved = cand2
                    else:
                        for layer_name in LAYER_MAP:
                            cand3 = f"include/{layer_name}/{inc}"
                            if cand3 in file_set:
                                resolved = cand3
                                break
                    if resolved and resolved != rel_fp:
                        graph[rel_fp].append(resolved)
        except Exception:
            pass

    state: Dict[str, int] = {}  # 0/absent=unvisited, 1=visiting, 2=done
    stack: List[str] = []
    cycles: List[List[str]] = []
    seen_cycle_keys: Set[Tuple[str, ...]] = set()

    def dfs(node: str) -> None:
        state[node] = 1
        stack.append(node)
        for nxt in graph.get(node, ()):
            st = state.get(nxt, 0)
            if st == 1:
                idx = stack.index(nxt)
                cyc = stack[idx:] + [nxt]
                key = tuple(sorted(set(cyc)))
                if key not in seen_cycle_keys:
                    seen_cycle_keys.add(key)
                    cycles.append(cyc)
            elif st == 0:
                dfs(nxt)
        stack.pop()
        state[node] = 2

    for fp in sorted(all_files):
        if state.get(fp, 0) == 0:
            dfs(fp)

    return cycles


def check_all_layers(root_dir: str, target_files: Optional[List[str]] = None) -> Tuple[int, List[Dict]]:
    violation_reports: List[Dict] = []
    total_files_scanned = 0

    files_to_check: List[str] = []
    if target_files:
        for tf in target_files:
            fp = os.path.abspath(tf) if os.path.isabs(tf) else os.path.abspath(os.path.join(root_dir, tf))
            if os.path.exists(fp) and fp.endswith((".cpp", ".h", ".hpp", ".c")):
                files_to_check.append(fp)
    else:
        for sub in ("src", "include"):
            dir_path = os.path.join(root_dir, sub)
            if not os.path.exists(dir_path):
                continue
            for root, _, files in os.walk(dir_path):
                for file in sorted(files):
                    if file.endswith((".cpp", ".h", ".hpp", ".c")):
                        files_to_check.append(os.path.join(root, file))

    for fp in files_to_check:
        total_files_scanned += 1
        rel_fp = os.path.relpath(fp, root_dir).replace("\\", "/")
        for item in scan_file_for_layer_violations(fp):
            violation_reports.append({
                "file": rel_fp,
                "line": item["line"],
                "content": item["content"],
                "src_layer": item["src_layer"],
                "target_layer": item["target_layer"],
                "rule": item["rule"],
            })

    if not target_files:
        for cyc in detect_include_cycles(root_dir):
            violation_reports.append({
                "file": cyc[0],
                "line": 1,
                "content": " -> ".join(cyc),
                "src_layer": "IncludeDAG",
                "target_layer": "Cycle",
                "rule": "Circular #include dependency detected",
            })

    return total_files_scanned, violation_reports


if __name__ == "__main__":
    proj_root = sys.argv[1] if len(sys.argv) > 1 else "."
    files_cnt, reports = check_all_layers(proj_root)
    if reports:
        print(f"\n\033[91m[CANONICAL 4+1 LAYER / BOUNDARY VIOLATIONS DETECTED] ({len(reports)} violations in {files_cnt} files)\033[0m")
        for r in reports:
            print(f"  \033[93m{r['file']}:{r['line']}\033[0m -> {r['content']}")
            print(f"    \033[91m{r.get('rule', 'Layer violation')}\033[0m")
        sys.exit(1)
    else:
        print(f"\033[92m[LAYER GUARD CLEAN] All {files_cnt} files conform to Strict Adjacent 4+1 Layer, Boundary & Acyclic DAG Invariants.\033[0m")
        sys.exit(0)
