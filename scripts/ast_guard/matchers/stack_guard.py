#!/usr/bin/env python3
"""
stack_guard.py - E-ASTG Static Stack Usage & Cumulative Call-Tree Auditor
Parses GCC -fstack-usage (.su) files to detect single stack frame spikes
and computes cumulative call-tree stack depth against FreeRTOS task budgets.
"""

import os
import glob
import re
from typing import List, Dict, Tuple, Set, Optional

# FreeRTOS task stack size limits
MAX_STACK_FRAME_FATAL = 1024
MAX_STACK_FRAME_WARN = 512

# Task Stack Budgets (Total allocated stack and 70% safe limit)
TASK_STACK_BUDGETS = {
    "Task_Ch1": {"symbol": "Task_Ch1", "alloc": 4160, "limit": 2912, "desc": "CH1 Wallpad Real-Time Loop"},
    "Task_Ch2": {"symbol": "Task_Ch2", "alloc": 4160, "limit": 2912, "desc": "CH2 Thermostat Loop"},
    "Task_Ch3": {"symbol": "Task_Ch3", "alloc": 4160, "limit": 2912, "desc": "CH3 Air Conditioner Loop"},
    "Task_Ch4": {"symbol": "Task_Ch4", "alloc": 3328, "limit": 2329, "desc": "CH4 Doorphone Loop"},
    "Network":  {"symbol": "runTask",  "alloc": 6528, "limit": 4569, "desc": "Network Reactor Thread"},
    "Telnet":   {"symbol": "Task_Telnet", "alloc": 6528, "limit": 4569, "desc": "Telnet CLI Service"},
}

# Standard C++ methods to ignore from call graph to avoid false linkage
IGNORED_SYMS = {
    "load", "store", "lock", "unlock", "empty", "size", "begin", "end",
    "data", "get", "at", "push", "pop", "front", "back", "reset", "set",
    "min", "max", "append", "c_str", "length", "find", "substr",
}


def parse_su_files(build_dir: str) -> Dict[str, Dict]:
    """
    Parses all .su files and returns a map of symbol -> info dict.
    """
    func_map = {}
    su_files = glob.glob(os.path.join(build_dir, "**", "*.su"), recursive=True)

    for su_path in su_files:
        try:
            with open(su_path, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    parts = line.strip().split("\t")
                    if len(parts) >= 3:
                        loc_func = parts[0]
                        try:
                            stack_size = int(parts[1])
                        except ValueError:
                            continue
                        alloc_type = parts[2]
                        
                        m = re.search(r"([a-zA-Z0-9_]+)\s*\(.*?\)", loc_func)
                        sym_name = m.group(1) if m else loc_func.split(":")[-1]

                        func_info = {
                            "location": loc_func,
                            "name": sym_name,
                            "size": stack_size,
                            "type": alloc_type,
                        }
                        if sym_name not in func_map or func_map[sym_name]["size"] < stack_size:
                            func_map[sym_name] = func_info
                        func_map[loc_func] = func_info
        except Exception:
            continue

    return func_map


def build_function_bodies(project_root: str) -> Dict[str, str]:
    """
    Extracts function bodies across src/ for static call graph construction.
    """
    funcs = {}
    src_dir = os.path.join(project_root, "src")
    for root, _, files in os.walk(src_dir):
        for fname in files:
            if not (fname.endswith(".cpp") or fname.endswith(".c")):
                continue
            fpath = os.path.join(root, fname)
            try:
                with open(fpath, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
                for m in re.finditer(r"\b([a-zA-Z0-9_]+)\s*\([^;{]*\)\s*(?:noexcept|const)?\s*\{(.*?)\n\}", content, re.DOTALL):
                    name = m.group(1)
                    body = m.group(2)
                    funcs[name] = body
            except Exception:
                continue
    return funcs


def compute_task_cumulative_depth(root_sym: str, funcs: Dict[str, str], func_map: Dict[str, Dict], max_hops: int = 5) -> Tuple[int, List[str]]:
    """
    Computes worst-case call path stack usage from task root symbol.
    """
    def dfs(curr: str, hops: int, visited: Set[str]) -> Tuple[int, List[str]]:
        if hops >= max_hops or curr in visited:
            return 0, []
        visited.add(curr)
        body = funcs.get(curr, "")
        callees = set(re.findall(r"\b([a-zA-Z0-9_]+)\s*\(", body))
        valid_callees = [c for c in callees if c in func_map and c not in IGNORED_SYMS and c not in visited]

        max_sub = 0
        best_path = []
        for c in valid_callees:
            c_cost, c_path = dfs(c, hops + 1, visited)
            if c_cost > max_sub:
                max_sub = c_cost
                best_path = c_path

        visited.remove(curr)
        curr_cost = func_map[curr]["size"] if curr in func_map else 32
        return curr_cost + max_sub, [f"{curr}({curr_cost}B)"] + best_path

    return dfs(root_sym, 0, set())


def audit_stack_usage(build_dir: str, project_root: Optional[str] = None) -> Tuple[List[Dict], List[Dict], List[Dict], List[Dict]]:
    """
    Audits .su files and cumulative call tree stacks.
    Returns: (fatals, warns, top_5_single, task_audit_results)
    """
    func_map = parse_su_files(build_dir)
    if not func_map:
        return [], [], [], []

    unique_funcs = list({f["location"]: f for f in func_map.values()}.values())
    unique_funcs.sort(key=lambda x: x["size"], reverse=True)

    fatals = [f for f in unique_funcs if f["size"] > MAX_STACK_FRAME_FATAL]
    warns = [f for f in unique_funcs if MAX_STACK_FRAME_WARN < f["size"] <= MAX_STACK_FRAME_FATAL]
    top_5_single = unique_funcs[:5]

    task_results = []
    if project_root:
        funcs = build_function_bodies(project_root)
        for task_key, info in TASK_STACK_BUDGETS.items():
            sym = info["symbol"]
            max_depth, path = compute_task_cumulative_depth(sym, funcs, func_map)
            pct = (max_depth / info["alloc"]) * 100.0
            is_exceeded = max_depth > info["limit"]
            task_results.append({
                "task": task_key,
                "symbol": sym,
                "desc": info["desc"],
                "depth": max_depth,
                "alloc": info["alloc"],
                "limit": info["limit"],
                "pct": pct,
                "exceeded": is_exceeded,
                "path": " -> ".join(path),
            })

    return fatals, warns, top_5_single, task_results


if __name__ == "__main__":
    import sys
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    
    candidate_dirs = [
        "/tmp/pio-gateway-build/m5stack-atoms3-usb",
        os.path.abspath(".pio/build/m5stack-atoms3-usb"),
    ]
    target_dir = next((c for c in candidate_dirs if os.path.exists(c)), None)

    if not target_dir:
        print("[E-ASTG SKIP] Module 5 (Stack Guard): No build directory found.")
        sys.exit(0)

    fatals, warns, top_5, task_res = audit_stack_usage(target_dir, proj_root)

    print(f"\n[E-ASTG] Module 5 (Static Stack Usage & Call-Tree Budget Auditor):")
    if top_5:
        print("  Top 5 Single Frame Stack Consumers:")
        for idx, f in enumerate(top_5, start=1):
            print(f"    {idx}. {f['size']:>4}B | {f['location']}")

    if task_res:
        print("\n  Task Cumulative Stack Depth Budgets (Max Safe Limit = 70%):")
        for tr in task_res:
            tag = "[FAIL]" if tr["exceeded"] else "[PASS]"
            print(f"    {tag} {tr['task']:<10}: {tr['depth']:>4}B / {tr['alloc']}B ({tr['pct']:.1f}%) | {tr['desc']}")
            print(f"           Call Chain: {tr['path']}")

    has_fatal = len(fatals) > 0 or any(t["exceeded"] for t in task_res)
    if has_fatal:
        sys.exit(1)
    else:
        print(f"\n[E-ASTG PASS] Module 5: All function frames & task call trees within safe budget limits.")
        sys.exit(0)
