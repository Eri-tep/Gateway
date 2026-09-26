import re
import os

files = ["src/init.lua", "src/gateway_client.lua", "src/command_handlers.lua", "src/telemetry_handler.lua"]

patterns = {
    "blocking": [r'while\s+true\s+do', r'socket\.sleep', r'os\.sleep'],
    "table_dispatch": [r'elseif.*==', r'if.*class\s*=='],
    "memory_leak": [r'timer', r'spawn', r'tcp\('],
    "error_handling": [r'[^p]call\(', r'[^x]pcall\('],
    "dead_code": [r'^\s*--[^\[]'],
    "lua_opt": [r'table\.concat', r'pairs\(', r'ipairs\('],
    "cosock_pattern": [r'settimeout'],
}

for fname in files:
    if not os.path.exists(fname): continue
    with open(fname, 'r') as f:
        lines = f.readlines()
        for i, line in enumerate(lines):
            for cat, pats in patterns.items():
                for p in pats:
                    if re.search(p, line):
                        print(f"{fname}:{i+1} [{cat}] {line.strip()}")

