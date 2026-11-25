#!/usr/bin/env python3
"""
test_ast_guard.py - Negative & Positive Verification Suite for E-ASTG 4-Gate Guard
Uses isolated temporary project trees (zero workspace mutation) to prove that all
clean-architecture, DIP, synchronization, C++ hygiene, and DAG cycle gates catch
intentional violations accurately without false positives.
"""

import os
import sys
import tempfile
import unittest

CURR_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, CURR_DIR)

from matchers.layer_guard import check_all_layers
from matchers.call_guard import verify_call_invariants
from matchers.cpp_hygiene_guard import verify_cpp_hygiene
from matchers.srp_audit import audit_srp_metrics


def write_rel(root: str, rel: str, content: str) -> None:
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write(content)


class TestEastg4Gate(unittest.TestCase):
    def test_gate1_layer_skip_and_boundary_and_cycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            # 1) L4 -> L1 layer skip
            write_rel(tmp, "src/L4_Services/BadSvc.cpp", '#include "L1_HAL/Wifi_Driver.h"\n')
            # 2) L0 -> L2 upward include
            write_rel(tmp, "include/L0_Foundation/BadL0.h", '#include "L2_Transport/Bridge_CH.h"\n')
            # 3) L3 Private header reverse-including L3 Public header (non-leaf)
            write_rel(tmp, "include/L3_Protocol/Private/BadPriv.h", '#include "L3_Protocol/Public/Protocol_Device.h"\n')
            # 4) L4 including L3 Private & L2 including L1 Private
            write_rel(tmp, "src/L4_Services/LeakSvc.cpp", '#include "L3_Protocol/Private/Wallpad_Engine.h"\n')
            write_rel(tmp, "src/L2_Transport/LeakL1.cpp", '#include "L1_HAL/Private/Internal.h"\n')
            # 5) Header inside src/ anti-pattern
            write_rel(tmp, "src/L2_Transport/Internal.h", '#pragma once\n')
            # 6) Header include cycle A -> B -> A
            write_rel(tmp, "include/L2_Transport/CycleA.h", '#include "L2_Transport/CycleB.h"\n')
            write_rel(tmp, "include/L2_Transport/CycleB.h", '#include "L2_Transport/CycleA.h"\n')

            _, viols = check_all_layers(tmp)
            rules = " | ".join(v["rule"] for v in viols)
            self.assertIn("Layer-Skip Bypass", rules)
            self.assertIn("Upward Include", rules)
            self.assertIn("Reverse Include", rules)
            self.assertIn("Private Boundary Leak", rules)
            self.assertIn("Forbidden Header in src/", rules)
            self.assertIn("Circular #include", rules)

    def test_gate2_dip_and_mutex_flash_and_l2_uart(self):
        with tempfile.TemporaryDirectory() as tmp:
            # 1) L4 direct WiFi & esp_ota_ops & <WiFi.h>
            write_rel(tmp, "src/L4_Services/BadDip.cpp", (
                '#include <WiFi.h>\n'
                'void foo() { if (WiFi.status() == WL_CONNECTED) {} }\n'
            ))
            # 2) L2 direct uart_write_bytes
            write_rel(tmp, "src/L2_Transport/BadUart.cpp", (
                'void tx() { uart_write_bytes(0, "a", 1); }\n'
            ))
            # 3) Non-NVS MutexLocker holding Flash I/O (P0 bug regression)
            write_rel(tmp, "src/L2_Transport/BadLock.cpp", (
                'void save() {\n'
                '  MutexLocker lock(s_ch5_mutex);\n'
                '  SystemNvs nvs;\n'
                '  nvs.begin("ns", false);\n'
                '}\n'
            ))
            # 4) Allowed NVS serialization mutex (must NOT trigger false positive)
            write_rel(tmp, "src/L0_Foundation/GoodLock.cpp", (
                'void saveOk() {\n'
                '  MutexLocker lock(getSaveMutex());\n'
                '  SystemNvs nvs;\n'
                '}\n'
            ))
            # 5) Allowed BENCHMARK_EXCEPTION in L4
            write_rel(tmp, "src/L4_Services/BenchOk.cpp", (
                '// BENCHMARK_EXCEPTION: cycle probe\n'
                '#include <esp_task_wdt.h>\n'
            ))

            viols = verify_call_invariants(tmp)
            joined = "\n".join(viols)
            self.assertIn("DIP FORBIDDEN HEADER", joined)
            self.assertIn("DIP / LAYER BYPASS VIOLATION", joined)
            self.assertIn("L2 UART HAL BYPASS VIOLATION", joined)
            self.assertIn("MUTEX FLASH I/O VIOLATION", joined)
            self.assertNotIn("GoodLock.cpp", joined)
            self.assertNotIn("BenchOk.cpp", joined)

    def test_gate3_cpp_hygiene_and_flags(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_rel(tmp, "platformio.ini", (
                "[env]\n"
                "build_unflags = -std=gnu++17\n"
                "build_flags = -std=gnu++23 -fno-rtti\n"
            ))
            write_rel(tmp, "include/L0_Foundation/BadHdr.h", (
                "using std::span;\n"
                "extern int g_mutable_leak;\n"
            ))
            write_rel(tmp, "src/L1_HAL/BadCpp.cpp", (
                "#include <mutex>\n"
                "static std::mutex s_m;\n"
                "void f() { String x = \"hi\"; try { throw 1; } catch(...) {} }\n"
            ))

            fatals, warns = verify_cpp_hygiene(tmp)
            f_joined = "\n".join(fatals)
            w_joined = "\n".join(warns)
            self.assertIn("-std=gnu++2b", f_joined)
            self.assertIn("-fno-exceptions", f_joined)
            self.assertIn("-fno-rtti", f_joined)
            self.assertIn("Global header 'using'", f_joined)
            self.assertIn("STL mutex primitive", f_joined)
            self.assertIn("Arduino String", f_joined)
            self.assertIn("'try' block", f_joined)
            self.assertIn("Mutable extern declaration", w_joined)

    def test_gate4_srp_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            # Cohesive single-domain large file should NOT be flagged as multi-domain
            write_rel(tmp, "src/L2_Transport/Cohesive.cpp", "\n".join(["int a = 0;"] * 1050))
            # Multi-domain large file with two 3+ method classes MUST be flagged
            lines = ["class Alpha {}; class Beta {};"] + [f"void Alpha::m{i}() {{}}" for i in range(4)] + [f"void Beta::m{i}() {{}}" for i in range(4)] + ["int pad = 0;"] * 1050
            write_rel(tmp, "src/L3_Protocol/Private/Multi.cpp", "\n".join(lines))
            # Cohesive multi-class file explicitly marked with SRP_COHESIVE goes to info-only
            coh_lines = ["// SRP_COHESIVE: Tightly coupled classes sharing private state"] + lines
            write_rel(tmp, "src/L1_HAL/CohesiveMulti.cpp", "\n".join(coh_lines))
            res = audit_srp_metrics(tmp)
            self.assertEqual(len(res["cohesive_large_files"]), 2)
            self.assertEqual(len(res["multi_domain_files"]), 1)
            self.assertEqual(res["multi_domain_files"][0][2], ["Alpha", "Beta"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
