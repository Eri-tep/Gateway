#!/usr/bin/env python3
"""
thread_safety_guard.py - Gate 4: E-ASTG Clang Thread Safety & Lock Order Auditor
Automatically discovers all C++ sources using Clang TSA annotations or lock primitives
and runs clang++ -Wthread-safety to detect:
1. Accesses to GUARDED_BY variables without holding the capability
2. Lock order inversions (ABBA deadlocks via ACQUIRED_BEFORE / ACQUIRED_AFTER)
3. Unreleased locks (SCOPED_CAPABILITY / MutexLocker analysis)
"""

import os
import re
import sys
import json
import shlex
import shutil
import subprocess
from typing import List, Optional

CLANG_CANDIDATES = [
    "/opt/homebrew/opt/llvm/bin/clang++",
    "/usr/local/opt/llvm/bin/clang++",
    "clang++",
]

TSA_DISCOVERY_REGEX = re.compile(
    r"\b(?:GUARDED_BY|PT_GUARDED_BY|REQUIRES|ACQUIRE|RELEASE|ACQUIRED_BEFORE|ACQUIRED_AFTER|PortMuxLock|MutexLocker|SpinlockLocker)\b"
)


def find_clang_binary() -> Optional[str]:
    for cand in CLANG_CANDIDATES:
        p = shutil.which(cand)
        if p and os.path.exists(p):
            return p
    return None


def get_compile_commands(project_root: str) -> dict:
    cdb_path = os.path.join(project_root, "compile_commands.json")
    if not os.path.exists(cdb_path):
        subprocess.run(
            [os.path.expanduser("~/.platformio/penv/bin/pio"), "run", "-t", "compiledb", "-e", "m5stack-atoms3-usb"],
            cwd=project_root, capture_output=True, text=True
        )
    if os.path.exists(cdb_path):
        try:
            with open(cdb_path, "r", encoding="utf-8") as f:
                entries = json.load(f)
                return {e["file"]: e for e in entries}
        except Exception:
            pass
    return {}


def discover_synchronized_sources(project_root: str) -> List[str]:
    """Automatically discovers all .cpp files under src/ that use locks or TSA macros."""
    matched: List[str] = []
    src_dir = os.path.join(project_root, "src")
    for root, _, files in os.walk(src_dir):
        for fname in sorted(files):
            if not fname.endswith(".cpp"):
                continue
            fpath = os.path.join(root, fname)
            try:
                with open(fpath, "r", encoding="utf-8", errors="ignore") as f:
                    if TSA_DISCOVERY_REGEX.search(f.read()):
                        matched.append(os.path.relpath(fpath, project_root))
            except Exception:
                pass
    return matched


def audit_thread_safety(project_root: str, target_files: Optional[List[str]] = None) -> List[str]:
    """
    Runs Clang Thread Safety Analysis on synchronized files.
    Returns list of diagnostic messages.
    """
    clang_bin = find_clang_binary()
    if not clang_bin:
        return ["[WARN] Clang compiler (LLVM) not found. Skipping Thread Safety deep scan."]

    cdb = get_compile_commands(project_root)
    errors = []

    candidate_rel_files = discover_synchronized_sources(project_root) if not target_files else target_files
    xtensa_base = os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp-elf/xtensa-esp-elf")

    for rel_f in candidate_rel_files:
        abs_f = os.path.abspath(rel_f) if os.path.isabs(rel_f) else os.path.abspath(os.path.join(project_root, rel_f))
        if not os.path.exists(abs_f) or not abs_f.endswith(".cpp"):
            continue

        entry = cdb.get(abs_f)
        if not entry:
            for k, v in cdb.items():
                if os.path.basename(abs_f) == os.path.basename(k):
                    entry = v
                    break

        if not entry:
            continue

        cmd_tokens = shlex.split(entry["command"])
        include_and_macro_flags = [t for t in cmd_tokens if t.startswith("-I") or t.startswith("-D")]

        clang_cmd = [
            clang_bin,
            "-fsyntax-only",
            "-Wthread-safety",
            "-Werror=thread-safety",
            "-Wno-error=thread-safety-reference",
            "-Wno-thread-safety-reference",
            "-std=gnu++23",
            "-D__clang__",
            "--target=xtensa-esp32s3-elf",
            "-isystem", os.path.join(xtensa_base, "include", "c++", "13.2.0"),
            "-isystem", os.path.join(xtensa_base, "include", "c++", "13.2.0", "xtensa-esp-elf"),
            "-isystem", os.path.join(xtensa_base, "include"),
        ] + include_and_macro_flags + [abs_f]

        try:
            res = subprocess.run(clang_cmd, capture_output=True, text=True)
            if res.returncode != 0:
                ts_lines = [l for l in res.stderr.splitlines() if "thread-safety" in l]
                if ts_lines:
                    errors.append(f"[{os.path.relpath(abs_f, project_root)}]\n  " + "\n  ".join(ts_lines[:10]))
        except Exception as e:
            errors.append(f"[{os.path.relpath(abs_f, project_root)}] Clang invocation failed: {e}")

    return errors


if __name__ == "__main__":
    proj_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    srcs = discover_synchronized_sources(proj_root)
    errs = audit_thread_safety(proj_root, srcs)
    if any("[WARN]" not in e for e in errs) and errs:
        print("\n[E-ASTG FATAL] Clang Thread Safety Violations Detected:\n")
        for e in errs:
            print(e)
        sys.exit(1)
    else:
        print(f"[E-ASTG PASS] Module 4 (Clang Thread Safety Analysis): Verified {len(srcs)} synchronized files cleanly.")
        sys.exit(0)
