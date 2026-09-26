Import("env")
import os
import shutil
import re

def after_build(source, target, env):
    bin_path = str(target[0])
    project_dir = env.get("PROJECT_DIR")
    bin_dir = os.path.join(project_dir, "bin")
    os.makedirs(bin_dir, exist_ok=True)
    
    # Copy firmware.bin
    dest_bin = os.path.join(bin_dir, "firmware.bin")
    shutil.copyfile(bin_path, dest_bin)
    print(f"[POST-BUILD] Successfully updated: {dest_bin}")
    
    # Extract FIRMWARE_VERSION from include/core/Config.h (fallback: include/Common.h)
    version = "unknown"
    search_files = [
        os.path.join(project_dir, "include", "core", "Config.h"),
        os.path.join(project_dir, "include", "Common.h")
    ]
    for h_file in search_files:
        if os.path.exists(h_file):
            with open(h_file, "r", encoding="utf-8") as f:
                content = f.read()
                m = re.search(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"', content)
                if m:
                    version = m.group(1)
                    break
                
    # Save bin/version.txt
    dest_ver = os.path.join(bin_dir, "version.txt")
    with open(dest_ver, "w", encoding="utf-8") as f:
        f.write(version + "\n")
    print(f"[POST-BUILD] Successfully updated: {dest_ver} (Version: {version})")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)
