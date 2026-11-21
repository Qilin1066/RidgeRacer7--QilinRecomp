#!/usr/bin/env python3
"""Cross-check qilin's SPU decoder against GNU objdump, word by word.

Two inputs:
  --game <game>    every word of the code of the SPU programs the game carries
                   (needs your own copy and the decrypted program)
  --sweep          a sweep of the encoding space (no game files; runs in CI):
                   random words for every 11-bit opcode, then every single-bit
                   neighbour of each word either decoder accepts.

The reference is binutils 2.31.1 built for `spu-elf`: binutils dropped the
SPU after the 2.3x line, so no distribution packages it now. CI builds its
objdump from the GNU source (.github/workflows/ci.yml); set OBJDUMP_SPU to
yours.

The comparison is strict: mnemonic and every operand must match, and a word
must be valid for both or neither, with one systematic exception. binutils
decodes by the opcode alone and ignores every other bit, where qilin wants
the bits an instruction doesn't define to be zero. So when qilin rejects a
word, the check asks it again to look past reserved bits, and if that reading
is objdump's, the word is counted as "reserved bits set", not as a
disagreement. Anything else that is reviewed and explained is a narrow rule in
known_difference below; the rest fails the check.
"""
import argparse
import collections
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

BASE = 0x10000   # inside an SPU's 256 KiB, with room below for backward branches
CHUNK = 0x8000   # words per objdump run, so every address stays inside it


def known_difference(word, ours, theirs):
    return None


def find_objdump():
    for name in [os.environ.get("OBJDUMP_SPU"), "spu-elf-objdump", "spu-objdump"]:
        if name and shutil.which(name):
            return shutil.which(name)
    sys.exit("verify_spu_decoder: no SPU objdump found (build binutils 2.31.1 with --target=spu-elf "
             "and set OBJDUMP_SPU to its objdump)")


def normalise(text):
    text = text.split("#")[0].strip()   # objdump adds "# hex" after an operand above 16
    parts = text.split(None, 1)
    if not parts:
        return ""
    return parts[0] if len(parts) == 1 else parts[0] + " " + parts[1].replace(" ", "")


def write_words(words):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
        return f.name


def run_objdump(objdump, words, base):
    path = write_words(words)
    try:
        out = subprocess.run([objdump, "-D", "-z", "-b", "binary", "-m", "spu", "-EB", f"--adjust-vma={base:#x}", path],
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    result = {}
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) < 3 or not cols[0].strip().endswith(":"):
            continue
        result[int(cols[0].strip()[:-1], 16)] = normalise("\t".join(cols[2:]))
    return [result[base + 4 * i] for i in range(len(words))]


def run_qilin(qilin, words, base, lenient=False):
    path = write_words(words)
    try:
        out = subprocess.run([qilin, "spu-disasm", path, "--base", hex(base)] + (["--ignore-reserved"] if lenient else []),
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    lines = out.splitlines()
    assert len(lines) == len(words)
    return [normalise(line.split(" ", 2)[2]) for line in lines]


def invalid(text):
    return text.startswith(".long") or text == ""


def check(qilin, objdump, words, report):
    """Compare `words` in chunks. Returns the unexplained (address, word, ours, theirs), and the words either side accepts."""
    unexplained, accepted = [], []
    for start in range(0, len(words), CHUNK):
        chunk = words[start:start + CHUNK]
        ours = run_qilin(qilin, chunk, BASE)
        lenient = run_qilin(qilin, chunk, BASE, lenient=True)
        theirs = run_objdump(objdump, chunk, BASE)
        for i, w in enumerate(chunk):
            if ours[i] == theirs[i] or (invalid(ours[i]) and invalid(theirs[i])):
                report["agree_invalid" if invalid(ours[i]) else "agree_valid"] += 1
            elif invalid(ours[i]) and lenient[i] == theirs[i]:
                report["known:reserved bits set (binutils decodes by the opcode alone)"] += 1
            else:
                why = known_difference(w, ours[i], theirs[i])
                if why:
                    report["known:" + why] += 1
                else:
                    unexplained.append((BASE + 4 * i, w, ours[i], theirs[i]))
        accepted += [w for w, a, b in zip(chunk, ours, theirs) if not invalid(a) or not invalid(b)]
    return unexplained, accepted


# Random bits below the 11-bit opcode, with each seven-bit field left zero half
# the time, so that instructions with reserved fields come up in their valid forms.
FIELDS = (0x0000007F, 0x00003F80, 0x001FC000)


def sweep_words(rng, per_opcode):
    words = []
    for opcode in range(2048):
        for _ in range(per_opcode):
            w = opcode << 21
            for f in FIELDS:
                if rng.random() < 0.5:
                    w |= rng.getrandbits(32) & f
            words.append(w)
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True, help="path to the qilin executable")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--game", help="your copy of the game: check every word of its SPU programs' code")
    src.add_argument("--sweep", action="store_true", help="sweep the encoding space")
    ap.add_argument("--seed", type=int, default=2006)
    ap.add_argument("--per-opcode", type=int, default=48, help="sweep: random words per 11-bit opcode")
    ap.add_argument("--show", type=int, default=3, help="examples to print per class of unexplained disagreement")
    args = ap.parse_args()
    objdump = find_objdump()
    report = collections.Counter()
    unexplained = []

    if args.game:
        out = subprocess.run([args.qilin, "spu-insns", args.game], check=True, capture_output=True, text=True).stdout
        words = [int(line.split(" ", 2)[1], 16) for line in out.splitlines() if not line.startswith("#")]
        unexplained += check(args.qilin, objdump, words, report)[0]
    else:
        rng = random.Random(args.seed)
        words = sweep_words(rng, args.per_opcode)
        first, accepted = check(args.qilin, objdump, words, report)
        neighbours = sorted({w ^ (1 << bit) for w in set(accepted) for bit in range(32)} - set(words))
        unexplained += first + check(args.qilin, objdump, neighbours, report)[0]

    total = sum(report.values()) + len(unexplained)
    print(f"{total} words: {report['agree_valid']} valid and identical, {report['agree_invalid']} invalid for both")
    for key, n in sorted(report.items()):
        if key.startswith("known:"):
            print(f"  known difference ({n}): {key[6:]}")
    if unexplained:
        print(f"{len(unexplained)} UNEXPLAINED disagreements (address word | qilin | objdump):")
        classes = collections.defaultdict(list)
        for t in unexplained:
            classes[((t[2].split() or ["(nothing)"])[0], (t[3].split() or ["(nothing)"])[0])].append(t)
        for (a, b), items in sorted(classes.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(items):7} qilin {a!r:14} objdump {b!r}")
            for addr, w, x, y in items[:args.show]:
                print(f"            {addr:08x} {w:08x} | {x} | {y}")
        return 1
    print("no unexplained disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
