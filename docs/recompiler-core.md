# The recompiler's core

The parts of `qilin` that turn the game's code into C++: decoders, control-flow passes, lifters. So far (v0.2.0): the PPU decoder.

## The PPU decoder (`recomp/src/ppu.*`, `qilin::ppu`)

The PPU is the Cell's PowerPC core. It implements the 64-bit PowerPC architecture (version 2.02), the vector extension (VMX, or AltiVec), and eight vector loads and stores of its own for unaligned data (`lvlx`, `lvrx`, `stvlx`, `stvrx` and their `l` forms). Every instruction is one big-endian word.

The decoder has the shape of the other Qilin recompilers' (PunchOutWii's and WindWakerHD's decode the 32-bit 750): **one row per base mnemonic** with its opcode bits and operand fields, and tables built from the rows when the program starts.

- **383 instructions.** `Rc` ("."), `OE` ("o"), `LK` ("l") and `AA` ("a") are bits of an instruction, not separate entries.
- **Every bit a row doesn't define must be zero.** Reserved bits set decode as invalid, and so do invalid forms: a load with update whose base is its target or is absent, `lmw` and `lswi` loading over their own base, a branch whose `BO` has a reserved bit set, `bcctr` decrementing the counter, `mfocrf` and `mtocrf` naming other than one field, `mftb` of anything but the time base, a fourth kind of `sync`.
- **The supervisor's instructions are not in the table** (the MSR, the segment and translation tables, `rfid`, `dcbi`): a PS3 program runs in user mode. Nor are instructions the PPU doesn't have (`lq`, `stq`, `eciwx`, `ecowx`, the 970's `dcbzl`). Any of them in the game's code is an invalid word, and the recompiler stops on it.
- Two rows may share an extended opcode where a higher bit tells them apart (`mfcr` and `mfocrf`, `mtcrf` and `mtocrf`, `dst` and `dstt`, `dss` and `dssall`): the table chains them, and building it fails if two rows could match the same word.
- Each row carries the properties the passes go by: branch, load, store, floating point, VMX, cache hint, synchronisation, and `k64` for the instructions that work on all 64 bits of a register where a 32-bit twin does not.

What differs from the Nintendo recomps' decoder: 64-bit forms (`ld`, `std`, `lwa` and their 14-bit displacements; the `rld*` rotates with six-bit shifts and masks; `sradi`; `mulld`, `divd`; `fcfid`, `fctid`), compares with an `L` bit, `BO` under version 2's rules (hint bits `a` and `t`, where the 32-bit architecture had `y`), `bclr` and `bcctr` with a `BH` field, `sync` with a kind (`lwsync` is `sync 1`), `dcbt` with a `TH` field, no paired singles, and VMX: opcode 4 with 11-bit, 10-bit (the compares, whose record bit is bit 21) and 6-bit extended opcodes.

The text form follows GNU objdump's `-M cell,raw`: no simplified mnemonics (`blr` is `bclr 20,lt,0`, `mr` is `or`, `li` is `addi rD,0,n`), so two decoders can be compared as text.

Addresses are 32 bits wide, as an LV2 process's are: a branch target wraps at 4 GB.

### Checked against binutils

`tools/check/verify_decoder.py` compares the decoder with GNU objdump word by word: the mnemonic and every operand must match, and a word must be valid for both or for neither.

- `--sweep` (in CI, on every push): 24 random words for each of opcode 4's 2048 extended opcodes and each of the 1024 of opcodes 19, 31, 59 and 63, 4096 for every other primary opcode, and then **every single-bit neighbour of every word either decoder accepted**, which tests the edge of each instruction's reserved bits. With binutils 2.42: **4,931,891 words; 4,266,210 valid and identical; 539,096 invalid for both; no unexplained disagreement.**
- `--game <game>`: every word of the game's code sections. *Not run yet: it needs the decrypted program.*

The rest of the sweep's words are disagreements that have been reviewed, each matched by a rule as narrow as its cause (`known_difference` in the script), so anything new still fails:

| Words | Why |
|---|---|
| 62,500 | `lq`, `stq`: the POWER processors' quadword load and store, in binutils' `cell` set but not in the PPU |
| 58,920 | `cmpi`, `cmpli` with bit 9 set: reserved, and binutils ignores it |
| 2,154 | `sc` with reserved bits set |
| 844 | the data-stream hints (`dst`, `dstst`, `dss`, `dssall`) with reserved bits set: binutils checks none of them |
| 645 | the supervisor's instructions |
| 564 | `eciwx`, `ecowx`: an optional facility the PPU lacks |
| 371 | `attn`: the POWER processors' call to their service processor |
| 246, 167, 21, 10 | fields later versions of the architecture added, reserved here: `mtfsf`'s `L` and `W`, `lwarx` and `ldarx`'s `EH`, `fres` and `frsqrte`'s bit 15, `dcbf`'s `L` |
| 60, 36 | invalid forms binutils accepts: `bcctr` that decrements the counter, `lswi` loading over its base |
| 27 | `mfcr`: objdump's raw mode prints the absent field as `-1` |
| 20 | `dcbz` with bit 10 set (the 970's `dcbzl`) |

The sweep's first run found one fault in the decoder (`mftb` accepted any register number); the other classes were binutils'.

**Not settled by the sweep**, because binutils can't say what the PPU itself does:

- `dcbzl`, `mcrxr`, `ldbrx` and `stdbrx`: whether the PPU has them is from public descriptions. The first is left out and the other three are in; the profile of the game's code will say whether any of it matters.
- `vspltb`, `vsplth` and `vspltw` take an element number of 4, 3 and 2 bits, as binutils has them; the bits above are reserved here.
- `dcbt` and `dcbtst` accept any `TH`: a prefetch hint changes nothing the program can see.

### The commands

```sh
build/qilin insns   <game>                  # every word of the code: address, word, instruction
build/qilin profile <game>                  # how often each instruction is used; what doesn't decode
build/qilin disasm  <file> [--base 0x10000] # a file of raw big-endian words, as insns prints them
```

`insns` and `profile` read the decrypted program's executable sections, and have only met synthetic programs so far. `profile` is the next thing to run on the real one: it says which VMX and 64-bit forms the game uses, which is the lifter's work list.

## Next

1. The profile of the real code, and `verify_decoder.py --game`.
2. The control-flow pass: seeds from the function descriptors and the unwinding tables, the import stubs as known calls, the `sc` sites with their numbers, coverage to 0 unknown words.
3. The SPU decoder, once the SPU programs are found and counted. Its reference is open: binutils dropped the SPU some versions ago (`roadmap-and-design-targets.md`).
