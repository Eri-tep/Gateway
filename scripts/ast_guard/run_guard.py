#!/usr/bin/env python3
# ============================================================================
# run_guard.py: Standalone CLI Runner for Embedded AST Guard System (E-ASTG)
# Modules:
#   1. Canonical 4+1 Layer Invariant Checker (AGENTS.md Rule 17)
#   2. Function Call AST Matcher (Zero-Heap, Non-Blocking, Critical Sections)
#   3. Static Stack Usage & Task Call-Tree Budget Auditor
#   4. Clang Thread Safety & Lock Order Deep Auditor (--deep)
# ============================================================================

import os
import sys
import argparse
import subprocess
from typing import List

curr_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, curr_dir)

from matchers.layer_guard import check_all_layers
from matchers.call_guard import verify_call_invariants
from matchers.stack_guard import audit_stack_usage, MAX_STACK_FRAME_FATAL, MAX_STACK_FRAME_WARN
from matchers.thread_safety_guard import audit_thread_safety


def get_staged_cpp_files(root_dir: str) -> List[str]:
    try:
        res = subprocess.run(
            ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
            cwd=root_dir, capture_output=True, text=True, check=True
        )
        lines = [line.strip() for line in res.stdout.splitlines() if line.strip()]
        return [f for f in lines if f.endswith((".cpp", ".c", ".h", ".hpp"))]
    except Exception:
        return []


def main():
    parser = argparse.ArgumentParser(description="E-ASTG: Embedded AST Guard CLI")
    parser.add_argument("--root", default=os.path.abspath(os.path.join(curr_dir, "../..")),
                        help="Root directory of the project")
    parser.add_argument("--build-dir", default=None,
                        help="Build directory containing .su files")
    parser.add_argument("--staged", action="store_true",
                        help="Scan only git staged C/C++ files (Pre-Commit Mode)")
    parser.add_argument("--deep", action="store_true",
                        help="Run Clang Thread Safety & Lock Order Deep Scan")
    parser.add_argument("--install-git-hook", action="store_true",
                        help="Install pre-commit hook into .git/hooks/pre-commit")
    args = parser.parse_args()

    root_dir = os.path.abspath(args.root)

    # Git hook installation helper
    if args.install_git_hook:
        hook_path = os.path.join(root_dir, ".git", "hooks", "pre-commit")
        hook_content = (
            "#!/bin/sh\n"
            "# Embedded AST Guard (E-ASTG) Pre-Commit Hook\n"
            "exec python3 scripts/ast_guard/run_guard.py --staged\n"
        )
        os.makedirs(os.path.dirname(hook_path), exist_ok=True)
        with open(hook_path, "w", encoding="utf-8") as f:
            f.write(hook_content)
        os.chmod(hook_path, 0o755)
        print(f"\033[92m[E-ASTG] Installed Git pre-commit hook to: {hook_path}\033[0m")
        sys.exit(0)

    staged_files = None
    if args.staged:
        staged_files = get_staged_cpp_files(root_dir)
        if not staged_files:
            print("\033[90m[E-ASTG SKIP] No staged C/C++ files detected. Clean commit.\033[0m")
            sys.exit(0)
        print(f"\033[96m[E-ASTG Pre-Commit] Auditing {len(staged_files)} staged C/C++ files...\033[0m")

    mode_label = "Deep Analysis Mode" if args.deep else "Standard Mode"
    print(f"\033[96m================================================================================\033[0m")
    print(f"\033[96m  [E-ASTG] Embedded AST Guard Suite ({mode_label}): {root_dir}\033[0m")
    print(f"\033[96m================================================================================\033[0m")

    has_fatal_error = False

    # -------------------------------------------------------------------------
    # Module 1: Layer Invariant Checker
    # -------------------------------------------------------------------------
    scanned_cnt, layer_viols = check_all_layers(root_dir, staged_files)
    if layer_viols:
        has_fatal_error = True
        print(f"\n\033[91m[E-ASTG FATAL] Module 1: {len(layer_viols)} Layer Invariant Violations Detected!\033[0m")
        for v in layer_viols:
            print(f"  \033[93m{v['file']}:{v['line']}\033[0m -> {v['content']}")
            print(f"    \033[91mForbidden: {v['src_layer']} cannot include higher {v['target_layer']}!\033[0m (AGENTS.md Rule 17)")
    else:
        print(f"\033[92m  [OK] Module 1 (Layer Invariant): Verified {scanned_cnt} files (0 Upward Includes)\033[0m")

    # -------------------------------------------------------------------------
    # Module 2 & 3.2: Function Call & Critical Section AST Matcher
    # -------------------------------------------------------------------------
    call_viols = verify_call_invariants(root_dir, staged_files)
    if call_viols:
        has_fatal_error = True
        print(f"\n\033[91m[E-ASTG FATAL] Module 2 & 3.2: {len(call_viols)} Call/Purity Violations Detected!\033[0m")
        for cv in call_viols:
            print(f"  \033[91m{cv}\033[0m\n")
    else:
        print(f"\033[92m  [OK] Module 2 & 3.2 (Call & Critical Section AST): Zero-Heap, Non-Blocking, Pure Atomic\033[0m")

    # -------------------------------------------------------------------------
    # Module 4: Clang Thread Safety Deep Scan (--deep)
    # -------------------------------------------------------------------------
    if args.deep:
        ts_errs = audit_thread_safety(root_dir, staged_files)
        fatal_ts = [e for e in ts_errs if "[WARN]" not in e]
        if fatal_ts:
            has_fatal_error = True
            print(f"\n\033[91m[E-ASTG FATAL] Module 4: Clang Thread Safety Violations Detected!\033[0m")
            for e in fatal_ts:
                print(f"  \033[91m{e}\033[0m\n")
        else:
            print(f"\033[92m  [OK] Module 4 (Clang Thread Safety Analysis): Mutex capabilities & lock orders clean\033[0m")

    # -------------------------------------------------------------------------
    # Module 5: Static Stack Usage & Task Call-Tree Auditor
    # -------------------------------------------------------------------------
    if not args.staged:
        build_dir = args.build_dir
        if not build_dir:
            for c in ["/tmp/pio-gateway-build/m5stack-atoms3-usb", os.path.join(root_dir, ".pio/build/m5stack-atoms3-usb")]:
                if os.path.exists(c):
                    build_dir = c
                    break

        if build_dir and os.path.exists(build_dir):
            fatals, warns, top_5, task_res = audit_stack_usage(build_dir, root_dir)
            if top_5:
                print(f"\033[94m  [INFO] Module 5 (Top 3 Single Frames):\033[0m")
                for idx, f in enumerate(top_5[:3], start=1):
                    loc_short = f['location'].split('/')[-1] if '/' in f['location'] else f['location']
                    print(f"    {idx}. {f['size']:>4}B | {loc_short}")
            if task_res:
                print(f"\033[94m  [INFO] Module 5 (Task Cumulative Call-Tree Depth):\033[0m")
                for tr in task_res:
                    status_col = "\033[91m" if tr["exceeded"] else "\033[92m"
                    tag = "[FAIL]" if tr["exceeded"] else "[PASS]"
                    print(f"    {status_col}{tag}\033[0m {tr['task']:<10}: {tr['depth']:>4}B / {tr['alloc']}B ({tr['pct']:.1f}%) | {tr['desc']}")
            if fatals:
                has_fatal_error = True
                print(f"\n\033[91m[E-ASTG FATAL] Module 5: {len(fatals)} Functions exceed {MAX_STACK_FRAME_FATAL}B stack threshold!\033[0m")
                for fatal in fatals:
                    print(f"    - {fatal['size']}B: {fatal['location']}")
            elif any(t["exceeded"] for t in task_res):
                has_fatal_error = True
                print(f"\n\033[91m[E-ASTG FATAL] Module 5: One or more tasks exceeded 70% safe call-tree stack budget!\033[0m")
            else:
                print(f"\033[92m  [OK] Module 5 (Stack & Call-Tree): All frames <= {MAX_STACK_FRAME_FATAL}B & tasks within safe budget\033[0m")
        else:
            print(f"\033[90m  [SKIP] Module 5 (Stack Auditor): No .su files found (run build first)\033[0m")

    print(f"\033[96m================================================================================\033[0m")
    if has_fatal_error:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
