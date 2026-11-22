#!/usr/bin/env python3
"""Run Khronos' SPIR-V validator over every translated shader.

    python3 tools/check/validate_shaders.py <folder> --spirv-val <path to spirv-val>

<folder> is where `qilin shaders <game> --out <folder>` wrote shaders.cpp (derived
from the game, so this runs only where the game is). Each module is written to
<folder>/spirv/ and given to `spirv-val --target-env vulkan1.3`. Exit 0 if every
module passes, 1 otherwise.

spirv-val is SPIRV-Tools' (Khronos); it is no part of the build. Build it from
source (KhronosGroup/SPIRV-Tools with SPIRV-Headers in external/spirv-headers:
`cmake --build <build> --target spirv-val`), or take it from a Vulkan SDK.
"""
import argparse
import pathlib
import re
import struct
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("folder", type=pathlib.Path)
    ap.add_argument("--spirv-val", required=True)
    args = ap.parse_args()
    text = (args.folder / "shaders.cpp").read_text()
    modules = dict(re.findall(r"const uint32_t kSpirv(\d+)\[\] = \{([^}]*)\};", text))
    rows = re.findall(r"\{0x([0-9a-f]{16})ull, (true|false), 0x([0-9a-f]{8})u, kSpirv(\d+),", text)
    if not rows or len(rows) != len(modules):
        print(f"{args.folder / 'shaders.cpp'}: no shaders found, or a table that doesn't match them", file=sys.stderr)
        return 1
    out = args.folder / "spirv"
    out.mkdir(exist_ok=True)
    failed = 0
    for _, vertex, address, index in rows:
        words = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})u", modules[index])]
        path = out / f"{'vs' if vertex == 'true' else 'fs'}_{address}.spv"
        path.write_bytes(struct.pack(f"<{len(words)}I", *words))
        r = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
        if r.returncode != 0 or r.stdout.strip() or r.stderr.strip():
            failed += 1
            print(f"{path.name}:\n{(r.stdout + r.stderr).strip()}")
    print(f"spirv-val: {len(rows)} modules, {failed} refused")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
