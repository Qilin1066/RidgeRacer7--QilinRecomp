#!/usr/bin/env python3
"""Run Khronos' SPIR-V validator over every translated shader.

    python3 tools/check/validate_shaders.py <folder> --spirv-val <path to spirv-val>

<folder> is where `qilin shaders <game> --out <folder>` wrote shaders.cpp (derived
from the game, so this runs only where the game is). Each module is written to
<folder>/spirv/ and given to `spirv-val --target-env vulkan1.3`, each program's other
forms for draw state (rsx_spirv.hpp's Variant, kSpirv<n>v<form>) too. Exit 0 if every
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
    modules = dict(re.findall(r"const uint32_t kSpirv(\d+(?:v\d)?)\[\] = \{([^}]*)\};", text))
    rows = re.findall(r"\{0x([0-9a-f]{16})ull, (true|false), 0x([0-9a-f]{8})u, kSpirv(\d+),", text)
    plain = [k for k in modules if "v" not in k]
    if not rows or len(rows) != len(plain):
        print(f"{args.folder / 'shaders.cpp'}: no shaders found, or a table that doesn't match them", file=sys.stderr)
        return 1
    out = args.folder / "spirv"
    out.mkdir(exist_ok=True)
    failed = forms = 0
    for _, vertex, address, index in rows:
        # The program's plain form, then its other forms for draw state, each a module of its own.
        for key in [index] + sorted(k for k in modules if k.startswith(index + "v")):
            words = [int(w, 16) for w in re.findall(r"0x([0-9a-f]{8})u", modules[key])]
            form = key[len(index) + 1:]
            forms += form != ""
            path = out / f"{'vs' if vertex == 'true' else 'fs'}_{address}{'_' + form if form else ''}.spv"
            path.write_bytes(struct.pack(f"<{len(words)}I", *words))
            r = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(path)], capture_output=True, text=True)
            if r.returncode != 0 or r.stdout.strip() or r.stderr.strip():
                failed += 1
                print(f"{path.name}:\n{(r.stdout + r.stderr).strip()}")
    print(f"spirv-val: {len(rows)} modules and {forms} other forms of them, {failed} refused")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
