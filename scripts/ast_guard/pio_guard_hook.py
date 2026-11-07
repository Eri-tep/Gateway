# ============================================================================
# pio_guard_hook.py: PlatformIO Pre/Post Build Guard Hook
# Embedded AST Guard System (E-ASTG) - Phase 3 SCons Integration
# ============================================================================

import os
import sys

Import("env")

project_dir = env.get("PROJECT_DIR", os.getcwd())
sys.path.insert(0, os.path.join(project_dir, "scripts", "ast_guard"))

from matchers.layer_guard import check_all_layers
from matchers.call_guard import verify_call_invariants
from matchers.stack_guard import audit_stack_usage, MAX_STACK_FRAME_FATAL, MAX_STACK_FRAME_WARN

def run_pre_build_guards(target, source, env):
    print("\n\033[96m================================================================================\033[0m")
    print("\033[96m  [E-ASTG] Embedded AST Guard: Pre-Build Architectural Verification             \033[0m")
    print("\033[96m================================================================================\033[0m")

    has_error = False

    # 1. Layer Invariant Check
    files_cnt, layer_violations = check_all_layers(project_dir)
    if layer_violations:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] {len(layer_violations)} Architectural Layer Violations Detected!\033[0m")
        for v in layer_violations:
            print(f"  \033[93m{v['file']}:{v['line']}\033[0m -> {v['content']}")
            print(f"    \033[91mForbidden: {v['src_layer']} is attempting to include higher {v['target_layer']}!\033[0m")
            print("    \033[90mRule: Canonical 4+1 Layer Invariant (AGENTS.md Rule 17).\033[0m")
    else:
        print(f"\033[92m  [OK] Canonical 4+1 Layer Invariant: Verified {files_cnt} files (0 Upward Includes)\033[0m")

    # 2. Function Call & Critical Section AST Check
    call_violations = verify_call_invariants(project_dir)
    if call_violations:
        has_error = True
        print(f"\n\033[91m[E-ASTG FATAL] {len(call_violations)} Function Call/Purity Violations Detected!\033[0m")
        for cv in call_violations:
            print(f"  \033[91m{cv}\033[0m\n")
    else:
        print(f"\033[92m  [OK] Call & Critical Section AST: Zero-Heap, Non-Blocking, Pure Atomic\033[0m")

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
        print(f"\n\033[91m[E-ASTG FATAL] One or more tasks exceeded 70% safe call-tree stack budget!\033[0m")

    if has_error:
        print("\033[91m================================================================================\033[0m\n")
        env.Exit(1)
    else:
        print(f"\n\033[92m  [OK] Static Stack Usage: All frames <= {MAX_STACK_FRAME_FATAL}B & tasks within safe budget\033[0m")
        print("\033[96m================================================================================\033[0m\n")


# Attach hooks
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", run_pre_build_guards)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", run_post_build_guards)
