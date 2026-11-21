# The recompiler's core

The parts of `qilin` that turn the game's code into C++: decoders, control-flow passes, lifters. So far (v0.6.0): the PPU decoder and control-flow pass, and the SPU decoder, all run on the game's code.

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
- `--game <game>`: every word of the game's code sections. *Not run yet: the decrypted program is on the owner's PC, which has no PowerPC objdump, and CI has no game.*

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

- `dcbzl`, `mcrxr`, `ldbrx` and `stdbrx`: whether the PPU has them is from public descriptions. The first is left out and the other three are in. **None of it matters to this game**: its code uses none of the four, nor `dcbz` (v0.3.0).
- `vspltb`, `vsplth` and `vspltw` take an element number of 4, 3 and 2 bits, as binutils has them; the bits above are reserved here.
- `dcbt` and `dcbtst` accept any `TH`: a prefetch hint changes nothing the program can see.

### The commands

```sh
build/qilin cfg     <game>                  # the control-flow pass's report
build/qilin insns   <game>                  # every word of the code: address, word, instruction
build/qilin profile <game>                  # how often each instruction is used; what doesn't decode
build/qilin disasm  <file> [--base 0x10000] # a file of raw big-endian words, as insns prints them
```

`insns` and `profile` read the decrypted program's executable sections. On the real program (v0.3.0, `elf-analysis.md`): 1,065,028 words, 99.47% of which decode; the rest are jump tables and constants lying in the code. The game uses **151 of the 383 instructions**, which is the lifter's work list: a narrow, floating-point part of VMX, no atomic sequences, no cache instructions but `dcbt`.

## The control-flow pass (`recomp/src/cfg.*`, `qilin cfg`)

Finds every function, follows every branch, and accounts for every word of the code. **On the game: 1,065,028 words, none unknown.**

| | |
|---|---|
| Functions | 14,294: the entry point, 248 import stubs, 14,045 from descriptors. One round: no function is found only by a call or a tail call |
| Code | 1,059,387 words reached |
| Jump tables | 5,634 words in 296 switches |
| Padding | 3 words |
| Dead | 4 words: an epilogue after a trap that always fires |
| Unknown | **0** |
| Words reached that aren't instructions, branches out of the code | 0 |

What the program gives, and the pass takes:

- **Descriptors.** In this ABI every function has one (14,046 of them, in one section, each a code address and the one TOC, `0x4de880`), so they are the list of functions. The section is found as the one holding the entry point's descriptor: RPCS3's file has no section names.
- **Unwinding tables** (`.eh_frame`, found by its content): 170 CIEs and 7,530 FDEs, each FDE the exact first and last address of a function. A function with one is explored to that end; one without (the C library's, mostly) to the next function's start.
- **Exception tables** (`.gcc_except_table`): 932 functions have one, with **2,845 landing pads** between them: the places the C++ unwinder enters a function when an exception passes through it, to run destructors or a `catch`. Nothing branches to a landing pad, so without these tables 51,000 words of code look unreachable. They are entry points of their functions.
- **Import stubs**: the 248, each ending in the `bctr` that enters the system. A call to one is a call to a named system function.

What it works out:

- **Switches.** GCC puts a switch's table in the code, straight after the `bctr`, as 32-bit offsets from the table's own address. The table's address is loaded from a table of constants: the TOC itself, or (most of the game's code, built with a small TOC per source file) the file's own table, whose address the function takes from the TOC into a register in its prologue. The pass reads both from the file. All 296 tables are found, and all lie straight after their `bctr`. **The size of 264 is proven** by the bound the code checks the index against (`cmplwi` and `bgt` to the default on the way in, or `cmplwi` and `ble` as the only way into the switch's block). The other 32, whose check isn't on a path the pass follows yet, are sized by where their targets begin (a table ends where the nearest thing it jumps to starts). The two methods agree on every table that has both, and a wrong size would leave unknown words or reach words that aren't instructions: there are none of either.
- **System calls.** The number is the value of `r11` at the `sc`. The compiler usually sets it just above, sometimes before a branch or a join, so the pass follows the constant in `r11` along every path through the function. **All 502 sites are numbered: 78 different system calls** (`elf-analysis.md`).
- **Tail calls**: a branch out of a function. Here every target was already a function.
- **Open-ended functions**: 491 reach their end without a return or a jump, because they end in a call that doesn't return (a throw, `abort`, `exit`). All but 18 have an unwinding entry that says where they end, and those 18 each end in a call and the `nop` after it.
- 4,943 indirect calls (`bctrl`), and no indirect jump that isn't a switch or an import stub.

**What this means for the runtime.** The game uses C++ exceptions for real: its unwinder and personality routine are part of the lifted code, and they resume a function at a landing pad with registers they have restored. So a lifted function has more than one way in (its start and its landing pads), and the lifted unwinder's jump to a landing pad is an indirect jump the dispatcher must know. That is a design item for the lifter (`roadmap-and-design-targets.md`).

Not done yet: a JSON output for the lifter; proving the last 32 switch sizes; data pointers into code other than descriptors (none are needed for coverage, but vtables point at descriptors, not code, which is worth confirming).

## The SPU decoder (`recomp/src/spu.*`, `qilin::spu`)

An SPU is not a PowerPC: 128 registers of 128 bits, no condition register, 256 KiB of memory of its own, channels to the rest of the machine. Every instruction is one big-endian word whose opcode is its first 4, 7, 8, 9, 10 or 11 bits.

- **199 instructions**, one row each (mnemonic, the opcode and its length, the form, properties), in the PPU decoder's shape. Building the table fails if two opcodes overlap.
- **Every bit an instruction doesn't define must be zero**, as in the PPU decoder: `clz` with its unused register field set, or `lnop` with anything set, is an invalid word.
- The text follows objdump's: registers are `$3`, channels `$ch29`, a load is `lqd $3,16($1)`, a branch names the address it reaches, and bits 11 to 13 of the indirect branches, `sync` and `hbr` show in the mnemonic (`bid`, `bie`, `syncc`, `hbrp`). `stop` prints without its signal type, as objdump's does (the decoder has it).

### Checked against binutils 2.31.1

The roadmap's open question was the reference: binutils dropped the SPU after its 2.3x line, so no distribution packages an SPU objdump. **CI builds one**: binutils 2.31.1's source from the GNU server, pinned by SHA-256, configured for `spu-elf`, and kept in a cache. `tools/check/verify_spu_decoder.py` compares the decoder with it word by word.

- `--sweep` (in CI, on every push): 48 random words for each of the 2,048 values of the first 11 bits, then every single-bit neighbour of every word either decoder accepted. **1,664,400 words; 1,476,288 valid and identical; 151,494 invalid for both; none unexplained.**
- binutils decodes by the opcode alone and ignores every other bit. So for a word qilin rejects, the check asks qilin again to look past reserved bits (`spu-disasm --ignore-reserved`), and if that reading is objdump's, counts it apart: 36,618 words, "reserved bits set". There is no other rule: nothing else differs.
- The decoder was written from the published instruction set without the reference at hand (binutils' source is not read, only run). The sweep's first run found no fault in any opcode or field, only the two matters of text above.
- `--game <game>`: every word of the game's SPU code. *Not run: no SPU objdump where the game is.*

### On the game's SPU programs

`qilin spu-profile`: the 42 programs' **209,660 words; 209,656 decode**. The other four are the one jump table in all of the SPU code (four addresses after a `bi`, in the sound core).

- **147 of the 199 instructions are used.** The most frequent: `lqd`, `il`, `stqd`, `shufb`, `ai`, `ori`, `rotqby`, `lqx`: loads, constants and shuffles, as SPU code is.
- **Floating point is single precision**: `fma` (3,690), `fm`, `fs`, `fcgt`, `fnms`, `fa`, the estimates (`frest`, `frsqest` with `fi`). Double precision appears in two programs only (`dfm`, `dfs`, `dfa`, 219 words), and the double fused forms and compares not at all.
- **5,547 channel writes and 845 reads**: the programs talk to the system through channels (memory transfers, signals, the mailboxes), which is what the runtime has to provide in place of an SPU's hardware.
- 4,714 branch hints (`hbra`, `hbr`, `hbrr`): no effect on what a program computes; the lifter drops them.
- 87 `stop` and 8 `stopd`; `heqi` (331) and `hlgti` (41) are the compiler's traps.
- Not used: `bra`, `brasl`, `iret`, `bisled`, interrupts in general, `sync`, the special-purpose registers, the byte-wise compares and averages, most halfword rotates.

## Next

1. `verify_decoder.py --game`, where there is both the game and a PowerPC objdump.
2. ✅ The control-flow pass (v0.4.0, above). Next for it: its output as JSON, and the lifter that reads it.
3. ✅ The SPU decoder (v0.6.0, above). Next for the SPU: its control-flow pass (branches are relative or through a register; functions start where `brsl` goes), then its lifter.
