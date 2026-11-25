# ============================================================================
# pio_guard_hook.py: PlatformIO Pre/Post Build Guard Hook
# Embedded AST Guard System (E-ASTG) - 4-Gate SCons Integration
# ============================================================================

import os
import sys

Import("env")

project_dir = env.get("PROJECT_DIR", os.getcwd())
sys.path.insert(0, os.path.join(project_dir, "scripts", "ast_guard"))

from matchers.layer_guard import check_all_layers
from matchers.call_guard import verify_call_invariants
from matchers.cpp_hygiene_guard import verify_cpp_hygiene
from matchers.stack_guard import audit_stack_usage, MAX_STACK_FRAME_FATAL


def run_pre_build_guards(target, source, env):
    print("\n\033[96m================================================================================\033[0m")
    print("\033[96m  [E-ASTG] Embedded AST Guard: Pre-Build Architectural & DIP Verification      \033[0m")
    print("\033[96m================================================================================\033[0m")

    has_error = False

    # 1. Gate 1: Layer, Private/Public Boundary & Include DAG
    files_cnt, layer_violations = check_all_layers(project_dir)
    if layer_violations:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] Gate 1: {len(layer_violations)} Architectural Layer/Boundary Violations Detected!\033[0m")
        for v in layer_violations:
            print(f"  \033[93m{v['file']}:{v['line']}\033[0m -> {v['content']}")
            print(f"    \033[91m{v.get('rule', 'Layer violation')}\033[0m")
    else:
        print(f"\033[92m  [OK] Gate 1 (Layer, Boundary & DAG): Verified {files_cnt} files (Adjacent + Acyclic)\033[0m")

    # 2. Gate 2: Call AST, DIP HAL Isolation & Mutex+Flash Safety
    call_violations = verify_call_invariants(project_dir)
    if call_violations:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] Gate 2: {len(call_violations)} DIP / Call / Mutex+Flash Violations Detected!\033[0m")
        for cv in call_violations:
            print(f"  \033[91m{cv}\033[0m\n")
    else:
        print("\033[92m  [OK] Gate 2 (DIP, Call Purity & Mutex+Flash): Zero-Heap, Non-Blocking, Safe Locks\033[0m")

    # 3. Gate 3: ESP-IDF C++ Guide Hygiene & Flags
    hygiene_fatals, hygiene_warns = verify_cpp_hygiene(project_dir)
    if hygiene_fatals:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] Gate 3: {len(hygiene_fatals)} C++ Hygiene Violations Detected!\033[0m")
        for hf in hygiene_fatals:
            print(f"  \033[91m{hf}\033[0m\n")
    else:
        warn_suffix = f" ({len(hygiene_warns)} extern warn)" if hygiene_warns else ""
        print(f"\033[92m  [OK] Gate 3 (ESP-IDF C++ Hygiene & Flags): Clean C++23 / FreeRTOS Mutex / Zero-String{warn_suffix}\033[0m")

    print("\033[96m================================================================================\033[0m\n")
    if has_error:
        env.Exit(1)


def run_post_build_guards(target, source, env):
    build_dir = env.subst("$BUILD_DIR")
    fatals, warns, top_5, task_res = audit_stack_usage(build_dir, project_dir)
    if not top_5 and not fatals and not warns and not task_res:
        return

    print("\n\033[96m================================================================================\033[0m")
    print("\033[96m  [E-ASTG] Embedded AST Guard: Post-Build Stack Usage & Call-Tree Audit         \033[0m")
    print("\033[96m================================================================================\033[0m")
    if top_5:
        print("  Top Stack Consuming Functions:")
        for idx, f in enumerate(top_5[:3], start=1):
            loc_short = f['location'].split('/')[-1] if '/' in f['location'] else f['location']
            print(f"    {idx}. {f['size']:>4}B | {loc_short}")

    if task_res:
        print("\n  Task Cumulative Stack Depth Budgets (Max Safe Limit = 70%):")
        for tr in task_res:
            tag = "[FAIL]" if tr["exceeded"] else "[PASS]"
            print(f"    {tag} {tr['task']:<10}: {tr['depth']:>4}B / {tr['alloc']}B ({tr['pct']:.1f}%) | {tr['desc']}")

    has_error = False
    if fatals:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] {len(fatals)} Functions exceed {MAX_STACK_FRAME_FATAL}B stack threshold!\033[0m")
        for fatal in fatals:
            print(f"    - {fatal['size']}B: {fatal['location']}")

    if any(t["exceeded"] for t in task_res):
        has_error = True
        print("\n\033[91m[E-ASTG FATAL] One or more tasks exceeded 70% safe call-tree stack budget!\033[0m")

    if has_error:
        print("\033[91m================================================================================\033[0m\n")
        env.Exit(1)
    else:
        print(f"\n\033[92m  [OK] Static Stack Usage: All frames <= {MAX_STACK_FRAME_FATAL}B & tasks within safe budget\033[0m")
        print("\033[96m================================================================================\033[0m\n")


# Enforce -fno-rtti explicitly for C++ targets only (avoids cc1 warning on C files)
if "-fno-rtti" not in env.get("CXXFLAGS", []):
    env.Append(CXXFLAGS=["-fno-rtti"])

# Attach hooks
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", run_pre_build_guards)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", run_post_build_guards)
