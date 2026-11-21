#!/usr/bin/env python3
"""Cross-check qilin's PPU decoder against GNU objdump, word by word.

Two inputs:
  --game <game>    every word of the game's code sections (needs your own copy
                   and the decrypted program: docs/elf-analysis.md)
  --sweep          a structured sweep of the encoding space (no game files; runs in CI):
                   random words for every primary and extended opcode, then every
                   single-bit neighbour of each word either decoder accepts, so the
                   edges of each instruction's reserved bits are tested too.

objdump runs as `-m powerpc:common64 -M cell,raw`: the Cell's PPU (64-bit
PowerPC, VMX and the Cell's own vector loads and stores), no simplified
mnemonics. Use binutils-powerpc-linux-gnu (Debian/Ubuntu) or any PowerPC
binutils; set OBJDUMP to choose one.

The comparison is strict: mnemonic and every operand must match, and a word
must be valid for both or neither. Disagreements that are reviewed and
explained are counted by class (known_difference below) instead of failing;
anything else fails the check. Exit status 0 only if nothing unexplained remains.
"""
import argparse
import collections
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile

BASE = 0x00010000

# The supervisor's instructions: binutils' cell set has them, and a PS3
# program, which runs in user mode, can't use them. qilin leaves them out, so
# the recompiler fails loudly on one.
PRIVILEGED = {
    "mfmsr", "mtmsr", "mtmsrd", "rfid", "hrfid", "rfi", "rfsvc",
    "slbie", "slbia", "slbmte", "slbmfev", "slbmfee",
    "tlbie", "tlbiel", "tlbia", "tlbsync",
    "mtsr", "mtsrin", "mfsr", "mfsrin", "mtsrd", "mtsrdin",
}


# Reviewed differences between qilin and binutils 2.42's `cell` set. Each rule
# is as narrow as the case it explains, so anything else still fails. In most,
# binutils accepts an encoding that the architecture reserves or calls an
# invalid form, which no retail compiler emits: qilin rejects it, so the
# recompiler fails loudly on it.
def known_difference(word, ours, theirs):
    p = word >> 26
    xo10 = (word >> 1) & 0x3FF
    rd, ra = (word >> 21) & 31, (word >> 16) & 31
    ours_valid = not ours.startswith(".long")
    theirs_valid = not theirs.startswith(".long")
    name = theirs.split()[0].rstrip(".")
    if ours_valid and theirs_valid:
        if p == 31 and xo10 == 19 and theirs == ours + ",-1":
            return "mfcr: objdump's raw mode prints the absent FXM field as -1"
        return None
    if ours_valid or not theirs_valid:
        return None
    if name in PRIVILEGED:
        return "supervisor-only instructions (MSR, segment and translation tables, rfid): not in a user program"
    if p == 19 and xo10 == 528 and not rd & 4:
        return "bcctr with a BO that decrements CTR (an invalid form)"
    if p == 17 and word & 2 and name == "sc":
        return "sc with reserved bits set (binutils ignores most of them; prints later ISAs' LEV field)"
    if p == 31 and xo10 == 597:
        regs = ((((word >> 11) & 31) or 32) + 3) // 4
        if (ra - rd) % 32 < regs:
            return "lswi with rA among the registers it loads (an invalid form)"
    return None


def find_objdump():
    for name in [os.environ.get("OBJDUMP"), "powerpc-linux-gnu-objdump", "powerpc64-linux-gnu-objdump",
                 "powerpc64le-linux-gnu-objdump", "powerpc-eabi-objdump"]:
        if name and shutil.which(name):
            return shutil.which(name)
    sys.exit("verify_decoder: no PowerPC objdump found (apt install binutils-powerpc-linux-gnu, "
             "or set OBJDUMP to one)")


def normalise(text):
    text = text.strip()
    parts = text.split(None, 1)
    if not parts:
        return ""
    return parts[0] if len(parts) == 1 else parts[0] + " " + parts[1].replace(" ", "")


def run_objdump(objdump, words, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
        path = f.name
    try:
        out = subprocess.run([objdump, "-D", "-z", "-b", "binary", "-m", "powerpc:common64", "-EB",
                              "-M", "cell,raw", f"--adjust-vma={base:#x}", path],
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    result = {}
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) < 3 or not cols[0].strip().endswith(":"):
            continue
        result[int(cols[0].strip()[:-1], 16)] = normalise(cols[2])
    return [result[base + 4 * i] for i in range(len(words))]


def run_qilin(qilin, words, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
        path = f.name
    try:
        out = subprocess.run([qilin, "disasm", path, "--base", hex(base)], check=True, capture_output=True,
                             text=True).stdout
    finally:
        os.unlink(path)
    lines = out.splitlines()
    assert len(lines) == len(words)
    return [normalise(line.split(" ", 2)[2]) for line in lines]


def compare(words, ours, theirs, base, report):
    unexplained = []
    for i, w in enumerate(words):
        if ours[i] == theirs[i]:
            report["agree_valid" if not ours[i].startswith(".long") else "agree_invalid"] += 1
            continue
        why = known_difference(w, ours[i], theirs[i])
        if why:
            report["known:" + why] += 1
        else:
            unexplained.append((base + 4 * i, w, ours[i], theirs[i]))
    return unexplained


# Random bits within `allowed`, with each 5-bit register field (and the
# record bit) left zero half the time, so that the valid forms of
# instructions with reserved fields come up often.
FIELDS = (0x03E00000, 0x001F0000, 0x0000F800, 0x000007C0, 0x0000003E, 0x00000001)


def fields(rng, allowed):
    w = 0
    for f in FIELDS:
        if rng.random() < 0.5:
            w |= rng.getrandbits(32) & f
    return w & allowed


def sweep_words(rng, per_xo, per_primary):
    words = []
    for p in range(64):
        if p == 4:  # VMX: the extended opcode is bits 21-31
            for xo in range(2048):
                for _ in range(per_xo):
                    words.append((p << 26) | xo | fields(rng, 0x03FFF800))
        elif p in (19, 31, 59, 63):
            for xo in range(1024):
                for _ in range(per_xo):
                    words.append((p << 26) | (xo << 1) | fields(rng, 0x03FFF801))
        else:  # with 30, 58 and 62, whose few extended opcode bits random words cover
            for _ in range(per_primary):
                words.append((p << 26) | fields(rng, 0x03FFFFFF))
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True, help="path to the qilin executable")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--game", help="your copy of the game: check every word of its code sections")
    src.add_argument("--sweep", action="store_true", help="sweep the encoding space")
    ap.add_argument("--seed", type=int, default=2006)
    ap.add_argument("--per-xo", type=int, default=24, help="sweep: random words per extended opcode")
    ap.add_argument("--per-primary", type=int, default=4096, help="sweep: random words per primary opcode")
    ap.add_argument("--show", type=int, default=3, help="examples to print per class of unexplained disagreement")
    args = ap.parse_args()
    objdump = find_objdump()
    report = collections.Counter()
    unexplained = []

    if args.game:
        out = subprocess.run([args.qilin, "insns", args.game], check=True, capture_output=True, text=True).stdout
        by_section = collections.defaultdict(list)  # contiguous runs of words
        prev = None
        for line in out.splitlines():
            addr_s, word_s, text = line.split(" ", 2)
            addr = int(addr_s, 16)
            if prev is None or addr != prev + 4:
                start = addr
            by_section[start].append((int(word_s, 16), normalise(text)))
            prev = addr
        for start, items in by_section.items():
            words = [w for w, _ in items]
            theirs = run_objdump(objdump, words, start)
            unexplained += compare(words, [t for _, t in items], theirs, start, report)
    else:
        rng = random.Random(args.seed)
        words = sweep_words(rng, args.per_xo, args.per_primary)
        ours = run_qilin(args.qilin, words, BASE)
        theirs = run_objdump(objdump, words, BASE)
        unexplained += compare(words, ours, theirs, BASE, report)
        # Every single-bit neighbour of every word either side accepts.
        seeds = sorted({w for w, a, b in zip(words, ours, theirs)
                        if not a.startswith(".long") or not b.startswith(".long")})
        neighbours = sorted({w ^ (1 << bit) for w in seeds for bit in range(32)} - set(words))
        ours = run_qilin(args.qilin, neighbours, BASE)
        theirs = run_objdump(objdump, neighbours, BASE)
        unexplained += compare(neighbours, ours, theirs, BASE, report)
        words += neighbours

    total = sum(report.values()) + len(unexplained)
    print(f"{total} words: {report['agree_valid']} valid and identical, "
          f"{report['agree_invalid']} invalid for both")
    for key, n in sorted(report.items()):
        if key.startswith("known:"):
            print(f"  known difference ({n}): {key[6:]}")
    if unexplained:
        print(f"{len(unexplained)} UNEXPLAINED disagreements (address word | qilin | objdump):")
        classes = collections.defaultdict(list)
        for t in unexplained:
            classes[(t[2].split()[0], t[3].split()[0])].append(t)
        for (a, b), items in sorted(classes.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(items):7} qilin {a!r:14} objdump {b!r}")
            for addr, w, x, y in items[:args.show]:
                print(f"            {addr:08x} {w:08x} | {x} | {y}")
        return 1
    print("no unexplained disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
