Import("env")
import os
import shutil

def copy_artifacts(source, target, env):
    firmware_bin = str(target[0])
    bin_dir = "/Users/eri/Library/CloudStorage/OneDrive-개인/Home/Gateway/bin"
    os.makedirs(bin_dir, exist_ok=True)
    dest_bin = os.path.join(bin_dir, "firmware.bin")
    shutil.copy2(firmware_bin, dest_bin)
    print(f"📦 [BUILD POST-ACTION] Copied {firmware_bin} -> {dest_bin}")

    # Auto sync version.txt from Common.h
    common_h = "/Users/eri/Library/CloudStorage/OneDrive-개인/Home/Gateway/include/Common.h"
    version_txt = os.path.join(bin_dir, "version.txt")
    if os.path.exists(common_h):
        with open(common_h, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if "FIRMWARE_VERSION" in line and "=" in line:
                    v = line.split("=")[1].strip().strip('";')
                    with open(version_txt, "w", encoding="utf-8") as vf:
                        vf.write(v + "\n")
                    print(f"📦 [BUILD POST-ACTION] Synced {version_txt} -> {v}")
                    break

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_artifacts)
