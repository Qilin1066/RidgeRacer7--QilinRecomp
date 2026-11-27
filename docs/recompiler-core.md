# The recompiler's core

The parts of `qilin` that turn the game's code into C++: decoders, control-flow passes, lifters. So far (v0.10.0): the decoders and control-flow passes for both processors, and the PPU lifter for 96% of the game's code, all run on the game's code.

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

- **Switches.** GCC puts a switch's table in the code, straight after the `bctr`, as 32-bit offsets from the table's own address. The table's address is loaded from a table of constants: the TOC itself, or (most of the game's code, built with a small TOC per source file) the file's own table, whose address the function takes from the TOC into a register in its prologue. The pass reads both from the file. All 296 tables are found, and all lie straight after their `bctr`. **The size of 293 is proven** (264 until after v0.13.0) by the bound the code checks the index against: `cmplwi` and `bgt` to the default on the way in, or the switch's block entered only by `cmplwi` and `ble`, or by `bgt` to the default followed by `b` into it. The compare may be anywhere above its branch on the straight line (the compiler hoists it past the register saves), and the search stops at anything else that writes the same CR field (another compare, `fcmpu`, a CR logical operation, `mcrf`, `mtcrf`, a record form into that field). A block that starts straight after another switch's table counts as entered only by its branches. Unit tests hold each rule, including that a write to the same field loses the proof. The other 3 (`0x4162ec`, `0x419624`, `0x41b8f4`: the out-of-range index goes through a call that presumably doesn't return, which the pass can't prove) are sized by where their targets begin (a table ends where the nearest thing it jumps to starts). The two methods agree on every table that has both, and a wrong size would leave unknown words or reach words that aren't instructions: there are none of either.
- **System calls.** The number is the value of `r11` at the `sc`. The compiler usually sets it just above, sometimes before a branch or a join, so the pass follows the constant in `r11` along every path through the function. **All 502 sites are numbered: 78 different system calls** (`elf-analysis.md`).
- **Tail calls**: a branch out of a function. Here every target was already a function.
- **Open-ended functions**: 491 reach their end without a return or a jump, because they end in a call that doesn't return (a throw, `abort`, `exit`). All but 18 have an unwinding entry that says where they end, and those 18 each end in a call and the `nop` after it.
- 4,943 indirect calls (`bctrl`), and no indirect jump that isn't a switch or an import stub.

**What this means for the runtime.** The game uses C++ exceptions for real: its unwinder and personality routine are part of the lifted code, and they resume a function at a landing pad with registers they have restored. So a lifted function has more than one way in (its start and its landing pads), and the lifted unwinder's jump to a landing pad is an indirect jump the dispatcher must know. That is a design item for the lifter (`roadmap-and-design-targets.md`).

Not done yet: a JSON output for the lifter; proving the last 3 switch sizes (which needs knowing which calls don't return); data pointers into code other than descriptors (none are needed for coverage, but vtables point at descriptors, not code, which is worth confirming).

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

`qilin spu-profile`: the 42 programs' **209,492 words; 209,488 decode**. The other four are the one jump table in all of the SPU code (four addresses after a `bi`, in the sound core).

- **147 of the 199 instructions are used.** The most frequent: `lqd`, `il`, `stqd`, `shufb`, `ai`, `ori`, `rotqby`, `lqx`: loads, constants and shuffles, as SPU code is.
- **Floating point is single precision**: `fma` (3,690), `fm`, `fs`, `fcgt`, `fnms`, `fa`, the estimates (`frest`, `frsqest` with `fi`). Double precision appears in two programs only (`dfm`, `dfs`, `dfa`, 219 words), and the double fused forms and compares not at all.
- **5,547 channel writes and 845 reads**: the programs talk to the system through channels (memory transfers, signals, the mailboxes), which is what the runtime has to provide in place of an SPU's hardware.
- 4,714 branch hints (`hbra`, `hbr`, `hbrr`): no effect on what a program computes; the lifter drops them.
- 87 `stop` and 8 `stopd`; `heqi` (331) and `hlgti` (41) are the compiler's traps.
- Not used: `bra`, `brasl`, `iret`, `bisled`, interrupts in general, `sync`, the special-purpose registers, the byte-wise compares and averages, most halfword rotates.

## The SPU control-flow pass (`recomp/src/spu_cfg.*`, `qilin spu-cfg`)

The same job as the PPU pass, on each of the 42 SPU programs. **Every word accounted for: 209,492 words, none unknown.**

| | |
|---|---|
| Functions | 2,332 |
| found by the entry point | 42 |
| by direct calls (`brsl`) | 1,256 |
| by tail calls (a branch out of a function) | 197 |
| by an address loaded with `ila`, or held in data | 23 and 84 |
| by nothing: unreferenced code at a clean boundary | 730 |
| Code | 208,634 words |
| Jump-table entries | 4 |
| Padding (`nop`, `lnop`, zero) | 854 |
| Unknown | **0** |
| Words reached that aren't instructions | 0 |

An SPU program gives less than the PPU program: no descriptors, no unwinding tables, just a statically linked `.text`. So:

- **Calls and returns** are `brsl` and `bi $0`. Function pointers are addresses loaded by `ila` or held in data; both are taken as starts on weak evidence, and one that turns out not to be a function (it reaches a word that isn't an instruction) is dropped. One was: the sound core's jump table, whose address the code loads.
- **Nearly a third of the functions are called by nothing** (730 of 2,332). The linker kept every function of every object it took, so each program carries library functions it never uses (inline vector-maths functions emitted out of line, most likely). They are real code at clean boundaries (after a return or a jump, and the alignment padding), found one at a time, lowest first, so that a function's own later blocks aren't mistaken for functions. *Whether to lift them is the lifter's choice: nothing reaches them.*
- **One switch in all of the SPU code**, in the sound core: `bi` through a register, its four absolute addresses in the code at the next 16-byte boundary.
- `stop` with signal type 0 ends the start-up code (after `exit`, which doesn't return); the other 51 stops are services the program resumes after.
- 281 functions reach the next function's start without a return or a jump: they end in a call that doesn't return, or run into the function that follows (a tail call by falling through).
- `.SpuGUID`, 16 bytes marked executable before each program's code, is an identifier, not code. (v0.6.0 counted it: its figure of 209,660 words was 42 × 4 too many.)

**The channels** the programs use, which is the whole of what the runtime must provide in an SPU's place besides memory:

| Channel | Uses | What (by the published channel map) |
|---|---|---|
| write 16–21 | 780 each | a memory transfer: local address, effective address (high and low), size, tag, and the command that starts it |
| write 22, 23; read 24 | 370 each | wait for transfers: the tag mask, the kind of wait, the status |
| read 27 | 470 | atomic-command status (`getllar`, `putllc`: the lock-free primitives SPURS is built from) |
| write 28, read 29 | 85, 7 | the outbound and inbound mailboxes |
| write 30 | 42 | the outbound interrupt mailbox (once in each program: how an SPU calls the system) |

## The PPU lifter (`recomp/src/lift.*`, `runtime/include/qilin/rt/cpu.hpp`)

One guest function becomes one C++ function, `void fn_<address>(Cpu& c)`, in the shape the other Qilin recompilers use. `qilin lift <game>` lifts every function the control-flow pass found and says how many the lifter covers.

**Slice 1 (v0.8.0): 11,873 of the game's 14,294 functions lift, 641,755 of 1,059,387 instructions.** What stops the rest is the first instruction outside the slice: floating point (2,053 functions), VMX (363), the time base (5).

How lifted code is shaped:

- **Registers live in host locals.** A function reads the registers it uses from the `Cpu` on entry, works on locals (`uint64_t r3`, a four-bit `cr7`, `lr`, `ctr`, XER's bits one each), and writes them back only where another function can see them: before a call, a return, a trap, a system call; and reads them again after a call.
- **64-bit mode.** Registers are 64 bits, record forms compare the 64-bit result with zero, carries are out of bit 0 of 64. Only the "word" instructions look at the low 32 (compares with `L` = 0, `mullw`, `divw`, `slw`, `srawi`, the `rlw` rotates, which repeat the rotated word in both halves before masking, as the architecture defines them).
- **Addresses are 32 bits.** Every effective address is truncated before use; an update form writes the whole 64-bit sum back to its base register.
- **Memory** is the guest's bytes at `mem + address`, in the guest's order: a load is a `memcpy` and a byte swap.
- **Direct calls and tail calls are C++ calls** (`fn_00403F14(c)`; a branch out of the function is a call and a return). A return is `return`. `bl` sets `lr` first: the callee may save it.
- **Indirect calls and jumps** (`bctrl`, a `bctr` that isn't a switch, `blrl`) go through `hooks->call` with the code address.
- **A switch** is a C++ `switch` on CTR's value, a case for each target address in the table the control-flow pass read; anything else traps.
- **System calls** (`sc`) go through `hooks->system_call`; traps through `hooks->trap`, which doesn't return.
- `sync`, `dcbt` and the other cache and ordering instructions need nothing on a host with real threads' memory semantics: *to be revisited with the threading model.*
- A function that reaches the next function's start without a return (it ends in a call that doesn't return) calls that function and returns.

**Where the architecture leaves a result undefined**, the runtime's helper states its choice (`cpu.hpp`): a divide by zero or of the most negative number by −1 gives 0; the upper half of a word divide is 0 and of `mulhw` the sign extension; `mfocrf` gives the named field and zeros elsewhere. *These are choices, not measurements of the PPU. No code that depends on one is expected; if any is found, RPCS3 running the game is the reference.*

**Not in the slice, and not skipped silently:** a function with any instruction outside the slice is not lifted, and `qilin lift` counts it under the reason. After floating point (below) comes VMX; then `dcbz` and atomic sequences (which this game doesn't use). **Landing pads** are emitted as labels in their functions but nothing can enter at one yet (the open design item in `roadmap-and-design-targets.md`).

With `--out <folder>` the lifted code is written out, 256 functions to a file (derived from the game: keep it out of the repository). All 11,873 functions compile with Visual Studio 2022 (checked on the owner's PC: 47 files, under three minutes).

### Slice 2 (v0.9.0): floating point, bit-exact

**13,732 of 14,294 functions now lift, 914,967 of 1,059,387 instructions.** What is left: VMX (560 functions) and the two functions that write and read the FPSCR.

The floating-point helpers are `runtime/include/qilin/rt/fp.hpp`. A register holds a double; the host's arithmetic is used where it is exactly the guest's, and corrected where it isn't:

- **NaNs.** PowerPC's result for a NaN operand is the first NaN operand in the order frA, frB, frC, made quiet, and for an invalid operation the positive default NaN. Hosts differ from that and from each other (x86's default NaN is negative), so every operation tests its result and works a NaN out itself.
- **Single-precision arithmetic rounds once.** `fadds` and its kin round the exact result of their double operands to single. With operands that are themselves singles (nearly always), the host's float arithmetic is that. With others, rounding the double result again would round twice: the slow path finds on which side of the double result the exact one lies (by error-free transformations: two-sum, the fused remainder of a product, quotient or root) and rounds through "round to odd", which makes the second rounding harmless.
- **The fused forms** (`fmadds`, `fmsubs` and their negatives) with frA and frC singles take the product exactly in a double and round one sum once. The host's single-precision `fma` is not used. Double-precision `fmadd` is `std::fma`.
- **Loads and stores of singles** convert formats as the architecture's own conversions do: no rounding, no quieting (a signalling NaN goes through `lfs` and `stfs` unchanged), a register that doesn't hold a single stored by bit selection.
- **Moves** (`fmr`, `fneg`, `fabs`, `fnabs`) are bit operations.
- Conversions to integer saturate, and give the most negative integer for a NaN. `fsel` and the compares are as defined; the compares set a CR field and nothing in the FPSCR.
- **The time base** (`mftb`) is a hook: the runtime will say what time it is.

**Assumed, and said where:** the rounding mode is round to nearest (the mode a program starts in), and the FPSCR's exception and result flags are not kept. So the instructions that read or write the FPSCR, and floating-point record forms, are not in the slice: a function that could notice is not lifted. The two that do (one `mtfsf`, one `mffs`, in the C library's environment functions by the look of them) are the two left. The estimates (`fres`, `frsqrte`), which the game doesn't use, are out too: their results are the hardware's tables'. The upper half of `fctiwz`'s result, undefined by the architecture, is `0xFFF80000`; `frsp` of a NaN keeps what of its payload a single holds; an arithmetic instruction passes a NaN's whole payload through. *Choices, to be confirmed against RPCS3 if anything is found to depend on them.*

**Its check is integers only.** The reference's floating point (`recomp/tests/difftest/ref_fp.cpp`) uses no host floating-point operation at all: a number is a sign, an integer significand and a power of two; sums, products, quotients and square roots are formed exactly in 128 bits (with a "sticky" note of anything below) and rounded once by one routine. So it shares neither a mistake nor a C library with `fp.hpp`.

- Floating-point arithmetic cases run from 1,200 states each (the hard cases are rare), drawn from the special values, singles, doubles exactly halfway between two singles and one unit either side, operands that nearly cancel, and noise.
- **4,765 cases, 1,085,808 runs, all agree.**
- It found one fault on its first run, in the slow path: a product that underflows in double lost its error term, and a tie was rounded the wrong way. Fixed, with the same hazard in the division path.

### Slice 3 (v0.10.0): VMX

**14,196 of 14,294 functions now lift, 1,016,630 of 1,059,387 instructions (96%).** What is left: 96 functions that use the two VMX estimates, and the two FPSCR functions.

The game uses a narrow part of VMX, and the slice is that part: `lvx`, `stvx`, `lvsl`, `lvsr`; the logical operations, `vsel`, `vperm`, `vsldoi`, the word merges and splats; `vadduwm`, `vcmpequw`; and single-precision `vaddfp`, `vsubfp`, `vmaddfp`, `vnmsubfp`, `vmaxfp`, `vminfp`, `vcmpeqfp`, `vcfsx`, `vctsxs`. Any other VMX instruction is outside the slice, by name.

The helpers are `runtime/include/qilin/rt/vmx.hpp`. A vector register is four words, element 0 first, each a host integer; bytes are numbered as the guest numbers them. All of it is portable C++ on fixed-size arrays (no host intrinsics, by the portability rule).

- Floating point is element by element and bit-exact by the same means as the scalar code: a NaN operand gives the first NaN operand made quiet, an invalid operation the default NaN; `vmaddfp` and `vnmsubfp` are fused, with the product taken exactly in a double and one sum rounded once.
- A recording compare sets CR6: "all true" and "none true".
- `lvx` and `stvx` drop the low four bits of the address.

**Assumed: non-Java mode.** VMX has a mode bit (VSCR's NJ) under which a denormal operand is taken as zero and a denormal result becomes zero. The game never touches the VSCR, so it runs in whatever mode its threads start in, and public descriptions say an LV2 thread starts with NJ set. `vmx.hpp` implements that, behind one switch (`kNonJava`). ***To be confirmed against RPCS3.*** The order of operands in the choice of a NaN for the three-operand forms (vA, vB, vC) is taken from the scalar rule: *likewise to be confirmed.* The VSCR's saturation bit is not kept, and `mfvscr` and `mtvscr` are outside the slice.

**`vrefp` and `vrsqrtefp` (v0.13.0; 170 uses in 96 functions) are estimates, and a stated choice.** The architecture asks only that they be within one part in 4,096 of 1/x and 1/sqrt(x); what a PPU gives exactly comes from tables of its own, for which this project has no source it may use. They were left unlifted until the game reached one (its first, at `0xdcaa0`, comes before SPURS starts). The choice made then: **the correctly rounded value**. It is within the architecture's bound, it is one definite thing a reference can check, and it says plainly what it is, where a table made up to look like the hardware's would not. It can differ from a console's result in the low bits. 1/x is a double's quotient, which rounds to the single correctly (a double has more than twice a single's digits and two); 1/sqrt(x) is the double's value settled exactly against the two midpoints beside it, by whole-number arithmetic (`compare_square`). The reference finds both by whole numbers alone: a division, and a search for the largest n with n·n·m ≤ 2^74. ***Still open:*** *the PPU's own tables, which would replace this if they can be had.*

The reference (`ref_vmx.cpp`) works on sixteen bytes in the guest's order, with arithmetic by the integer soft-float. Vector cases run from 400 states each. **5,403 cases, 1,327,632 runs in all, all agree**; a fault put into `vmaxfp` on purpose gave 426 disagreements. With the estimates (v0.13.0): **5,450 cases, 1,346,784 runs, all agree**; a wrong last bit put into each estimate on purpose gave 16,446 and 29,284 disagreements. **14,292 of 14,294 functions now lift, 1,059,366 of 1,059,387 instructions**: what is left is the two FPSCR functions.

### The differential test (`recomp/tests/difftest/`, `qilin_difftest`)

The lifter is checked against a **reference interpreter written separately** (`ref.cpp`): from the architecture's definitions, on the raw word's fields, with CR and XER as packed words, carries taken bit by bit, masks built bit by bit, products by long multiplication, memory a byte at a time. It shares only the decoder's naming of the opcode with the lifter. It runs in tests only: nothing interprets at run time.

- `qilin_difftest_gen` (at build time) lifts 24 random valid encodings of each instruction in the slice as a one-instruction function, and small programs for what a single instruction can't show: every conditional branch form the decoder accepts over a spread of condition bits, `bclr`, `bcctr`, traps, a call, a tail call, a system call, a switch.
- `qilin_difftest` (in `ctest`, so in CI on all six configurations) runs each from 48 random states, weighted towards the edges (zeros, ones, the sign boundaries of each width, a counter about to reach zero), with loads and stores steered into a window of memory and noise above bit 31 of the address. Registers, CR, XER, LR, CTR, the memory, and whether and where it trapped, jumped or called the system must all agree.
- Slice 1 alone: 3,602 cases, 172,896 runs, all agree, 135 instructions covered one at a time. With the later slices: **5,403 cases, 1,327,632 runs, 210 instructions.**
- That the test can fail was checked by breaking a helper on purpose (the carry of `sraw`): 179 disagreements, named.

The reference shares the lifter's author and the choices above for undefined results, so it checks the lifter's wiring and arithmetic against the definitions, not the definitions against the hardware.

## Next

1. `verify_decoder.py --game`, where there is both the game and a PowerPC objdump.
2. ✅ The control-flow pass (v0.4.0, above). Next for it: its output as JSON, and the lifter that reads it.
3. ✅ The SPU decoder (v0.6.0) and control-flow pass (v0.7.0).
4. 🔨 The PPU lifter: integers and control flow (v0.8.0), floating point and the time base (v0.9.0), VMX (v0.10.0). The VMX estimates (v0.13.0: the correctly rounded values, a stated choice). The FPSCR's `mffs` and `mtfsf` (after v0.13.0): **all 14,294 functions lift**. Left: landing pads as entries.

## The SPU lifter (`recomp/src/spu_lift.*`, `runtime/include/qilin/rt/spu.hpp`)

Started after v0.13.0, when the game reached the first thing that needs an SPU program running (its sound core, started as an SPU thread).

**The shape** is the PPU lifter's, and simpler: an SPU has no condition register and no table of contents. A lifted function is `void fn_XXXXX(Spu& s)`; each instruction is one call of a helper in `spu.hpp`, written from the SPU's published instruction set. Each of the 42 programs is lifted into a file and a namespace of its own (`spu_p00` to `spu_p41`: they all sit at the same addresses), with a `call(s, target, pc)` that is a switch over the program's function starts, for branches through a register. `qilin lift <game> --out` writes them beside the PPU's files, with a table (`kLiftedSpuPrograms`: where each program's ELF lies in the PPU program's memory, which is how the game names it to the system; its entry point; its `call`).

- A register is four words, the preferred slot first; the local store is 256 KiB in the SPU's byte order, and a quadword's address is masked into it at a multiple of 16, as the SPU does.
- **Registers are read and written in the `Spu` itself**, not held in locals as the PPU's are: 128 registers of 16 bytes would cost more to copy in and out around every call than locals would save. *To be measured.*
- Calls are `brsl` (the callee is called; `$0` gets the return address, as the program expects to find it), returns `bi $0`. `biz`, `binz` and the halfword forms through `$0` are conditional returns. A branch to another function's start is a tail call. A function that reaches its end without a return runs into the next one (281 do). The one switch is a `switch` over its table's targets.
- Channels and `stop` go to hooks (`SpuHooks`), which the runtime provides; a halt instruction that fires, or a branch to an address that is no function, is the hook `halt`, which doesn't return.
- Outside the slice, by name: floating point (the next slice), interrupts (`iret`, `bisled`, and indirect branches that enable or disable them: the game has none), the special-purpose registers.

**Slice 1: everything but floating point. 1,719 of the 2,332 functions lift.** Every one of the other 613 is stopped by a floating-point instruction (`fm` in 225, `fcgt` in 143, `fs` in 101, ...): nothing else is in the way.

**The differential test** (`recomp/tests/spu_difftest`, `qilin_spu_difftest` in `ctest`) is the PPU test's twin: the generator lifts random encodings of every instruction in the slice that is not a branch (148 instructions, 3,552 cases), the runner runs each from 64 states (227,328 runs) as lifted code and by a separately written reference, and compares all 128 registers, the whole local store after a store, what was written to a channel, a stop's code and whether a halt fired. The reference (`ref.cpp`) is in another shape on purpose: a register is sixteen bytes, elements are read and written by width and number, and the whole-register shifts work on the register as one 128-bit number, where the helpers work on words and bytes. **All agree**; with `sumb`'s halves exchanged and `rotqmby`'s count cut short on purpose it gave 7,908 disagreements. *Branches, calls and the switch need a program around them, and are not in this test: they are checked by the game's own programs running.*

### Slice 2: floating point (`runtime/include/qilin/rt/spu_fp.hpp`)

**2,330 of the 2,332 functions now lift.** The two left read and write the floating-point status and control register (`fscrrd`, `fscrwr`: one program's library functions), which is not kept, as the PPU's is not.

**After v0.13.0: all 2,332 lift, and all 14,294 of the PPU's.** The status registers are kept as written, not as arithmetic would set their flags (*a stated choice*: the runtime rounds to nearest and raises nothing). On the PPU, `mffs` reads the FPSCR into a register's low word (the high word, undefined, is 0) and `mtfsf` writes the fields its mask picks, with FEX and VX as their summaries; the game uses them in the C library's `fegetenv` (`0x3fc55c`) and `fesetenv` (`0x3fc184`). If `mtfsf` leaves a rounding mode other than to nearest, non-IEEE mode or an exception enabled (the low byte not 0), lifted code calls `CpuHooks::fpscr_mode`, which stops the game by name. On the SPU, `fscrwr` keeps the bits the instruction set defines (20-23, the two double-precision slices' rounding modes, and the sticky flags) and `fscrrd` reads them; a rounding mode other than to nearest halts. The other FPSCR instructions (`mtfsfi`, `mtfsb0`/`1`, `mcrfs`) and the record forms stay out: the game doesn't use them. Both differential tests start every case from a random status register and compare it after (5,498 cases, 1,404,384 runs on the PPU; 4,128 cases, 374,784 runs on the SPU; all agree), and each fails when a bit of the summaries or the defined mask is broken on purpose.

The source is IBM's public *Synergistic Processor Unit Instruction Set Architecture* (version 1.2, January 2007), section 9, read from the Internet Archive's copy.

**Single precision is not IEEE's**, and is done in whole numbers, exactly:

- a word with a zero exponent is zero whatever its sign and fraction (no denormals); every other word is an ordinary number, the largest exponent included (no infinities, no NaNs);
- results are truncated toward zero, the only mode there is; a result too large becomes the largest number of its sign (`0x7FFFFFFF`, `0xFFFFFFFF`), one too small becomes zero, and a zero result is always +0;
- `fma`, `fms` and `fnms` multiply exactly and round once.

So a sum is aligned in 64 bits with what falls off remembered, a product is exact in 48 bits, and the result is cut once (`pack`). The compares take every zero-exponent word as the one zero. The conversions' scale is 155 or 173 less the instruction's field and is defined from 0 to 127: an instruction outside that is outside the slice.

**`fi` is defined exactly** by the document (a base `1.BBBBBBBBBBBBB`, a step `0.000SSSSSSSSSS`, and the operand's low 19 bits as the fraction between them) and is exact here. **The estimates' base and step are an implementation's own**: the document gives their form and how good the result must be after its Newton-Raphson step, not their values, and a console's are not known here. *The stated choice:* 32 straight pieces to the binade for 1/x, and 64 for 1/sqrt(x) (32 for each parity of the exponent), each the line through its piece's ends lowered by half its error at the middle, rounded to the 13 and 10 bits the form has. A result can differ from a console's in the low bits. ***Open:*** *a console's tables, which would replace this.*

**Double precision** (three instructions and the two conversions, in three programs) is IEEE's on the host's doubles, with two choices the document leaves to the implementation, stated: a denormal operand is zero with its sign, and a NaN result is always the default one. Rounding is to nearest. The fused double forms and the double compares, which the game doesn't use, are outside the slice.

**Checked.** The differential test now has **4,080 cases and 362,496 runs, all agree**. The reference's floating point (`ref_fp.cpp`) uses another method on purpose: every number is put whole into one 704-bit unsigned number at its true place, sums and differences are exact, and only then is the answer cut to 24 bits; it builds the estimates' tables by whole numbers of 2^-40. Float cases draw their operands from singles of every kind (zeros, denormals, the ends of the range, the largest exponent, neighbours that cancel). With the sum's borrow left out on purpose: 24,068 disagreements. **The estimates are also checked against the document's own bound**: over 2,000,000 numbers, its two sequences (estimate, interpolate, one Newton-Raphson step) give the largest single below the true value or the one beside it, as it requires (the furthest is 1 unit for each); with the tables' lowering turned the wrong way on purpose, 1,839,906 of them break the bound.

## The RSX's programs (`recomp/src/rsx_program.*`)

The game's first frame draws, so its vertex and fragment programs have to be translated before anything is shown; the first piece is reading them. `qilin shaders <game>` decodes and checks all 242; `--disasm` prints each with its parameters.

**Sources.** The field layout is the NV40's, from the public description in the nouveau driver's documentation (Mesa's `nv40_vertprog.h` and `nvfx_shader.h`, MIT-licensed: read for the facts, no code taken), as `prior-art.md` names. The Cg binary's layout (the program header, the parameter table with types, resources, register indices, names, semantics and, for fragment programs, where each constant is patched into the microcode) is NVIDIA's, published with the Cg toolkit; it was confirmed field by field against the game's bytes before use.

**Vertex programs**: four words an instruction, a vector and a scalar operation in each, with separate destinations. **Fragment programs**: four words an instruction, each word stored with its 16-bit halves exchanged; one that reads a constant is followed by the constant, four more words.

**Where the RSX differs from nouveau's NV40 headers, as the game shows:**
- a vertex instruction's constant index is **10 bits** (word 1, bits 12-21), not 8: the game reads constant 467, and the Cg tables name it so;
- in fragment instructions, word 3's bits 19-29 (nouveau's input-address displacement) are `0x7FC` exactly when the instruction reads an input, and 0 otherwise; word 2's bits 19-21 are 5 on `NRM` and 0 everywhere else (*not understood*; held to that by the check);
- the RSX's own operations: `NRM`, `DIV`, `DIVSQ`, `FENCT`, `FENCB` (fragment), beside the NV40's.

**The check** (`check_program`) is this decoder's independent reference, as objdump is the CPU decoders': every program against its own Cg tables. **All 242 agree, over 7,702 instructions:** every set bit belongs to a decoded field; every operation is known; instruction counts match the headers (a fragment program's counts its constant slots too); each vertex program's input mask matches the inputs it reads; every constant a vertex program reads is a named uniform (by register index, or by a semantic `C<n>`, an array's elements at their offsets) or a literal the compiler made (`internal-constant-N`, variability `0x1007`, its value its default); and every place a fragment parameter's value is patched holds an inline constant. Narrowing the constant field to nouveau's 8 bits on purpose made 110 of the 242 disagree. Unit tests build instructions from the fields, not from the game's bytes.

**What the game uses** (programs): vertex `MOV` 120, `MAD` 102, `MUL` 91, `ADD` 84, `DP3` 72, `RSQ` 71, `MAX` 69, `RCP` 68, `MIN` 67, `SGT` 22, and in three programs each `DP4`, `ARL`, `FLR`, `SLT`, `BRA` with indexing by the address register; condition codes in 30. Fragment `TEX` 106, `MOV` 99, `MUL` 97, `MAD` 81, `ADD` 68, `FENCB` 42, `DP3` 38, `EX2` and `LG2` 31, `FENCT` 31, `DIVSQ` 30, `DIV` 18, `NRM` 16, `MAX` 14; fp16 registers in 75, fixed point in 10, a result scale in 41, condition codes in 27; no flow control.

**The uploaded program is the built one.** The vertex program the game loads for its first draw is the microcode of the Cg program at `0x10201c68`, byte for byte: the programs are found at build time and looked up at run time, as planned.

### Their translation to SPIR-V (`recomp/src/rsx_spirv.*`, `recomp/src/spirv.*`)

**All 242 translate.** `qilin shaders <game> --out <folder>` writes them, as SPIR-V, to `shaders.cpp` (derived from the game: outside the repository), each with its key: a hash of its microcode with a fragment program's inline constants left out, since the game patches those. The renderer finds a program the game uploads by that key; one it doesn't find stops the game.

**The SPIR-V is written by the project's own writer** (`spirv.*`), so building the game needs no shader compiler. Its numbers are from Khronos' published grammar files (SPIRV-Headers, at the tag the Vulkan headers are pinned to), not from memory. SPIR-V 1.3; registers are function variables, loaded and stored around each instruction (the driver's compiler keeps them in registers).

**What a translated program expects of the renderer** is stated at the top of `rsx_spirv.hpp`: vertex constants (468) and a fragment program's inline constants in uniform blocks; the RSX's viewport folded into the position by push constants; outputs and inputs at locations by register number; a texture unit's dimension (2D or cube) from the program's Cg parameters.

**A program's other forms for draw state.** Some of the RSX's state changes what a shader declares, which the renderer can't change in a module: a decoration or a built-in. For those, `qilin shaders` translates a program again in another form (`rsx_spirv.hpp`'s `Variant`), each its own module beside the plain one in `shaders.cpp` (`kSpirv<n>v<form>`) and in the shader table's `variant_spirv` (`shaders.hpp`), made only where a draw could use it. A fragment program that reads a colour has a **flat form** (form 1: `COL0` and `COL1` decorated `Flat`), for `SET_SHADE_MODE` FLAT: 28 of the game's 121. Such a program also has **two-sided forms** (2, and 3 with flat colours: the back colours `BFC0` and `BFC1` read at locations 3 and 4 on back faces, chosen by `FrontFacing`), for `SET_TWO_SIDE_LIGHT_EN`, but only where some vertex program writes a back colour: none of the game's does, so it has none. `ridgeracer7_shader_forms_test` holds what each form declares, on synthetic programs; `validate_shaders.py` and `ridgeracer7_shader_check` take every form as they take the plain modules (all 28 pass both).

**The R and H registers are one file.** The NV40 family's fp16 `H` registers are the halves of its fp32 `R` registers (`H2n` and `H2n+1` of `Rn`), and the game's programs show it: every fragment program's register count is at least `max(Rn+1, (Hn+2)/2)` of what it uses, and exactly that in 77 of the 121 (the rest have the minimum, 2). The translation keeps the two views as separate values, which is exact as long as no value crosses between them, and **refuses any program in which one does**: it tracks which view last wrote each quarter of each register and checks every component an instruction actually uses. No program is refused. (A register moved to itself in place, which the compiler emits, is the same bits either way.)

**Found by that check**: `DIV` and `DIVSQ` divide by their second source's *first* component, as a scalar. The game computes a length into one component and divides a vector by it (`DP3R R1.x, R2, R2; DIVSQR R3.xyz, R2, R1`); read component by component, those programs crossed between the views, which is how it showed.

**Stated choices**, to be compared with the console's picture: an output component the vertex program doesn't write is `(0, 0, 0, 1)`'s; with both vertex units at work, the vector unit's result sets the condition codes; a branch's condition is its first component's (refused if its swizzle isn't one component repeated); the fences (`FENCT`, `FENCB`) have no value; fixed point (s1.10) is a clamp to [-2, 2 - 1/1024] and a rounding to 1/1024; an fp16 result is rounded with `OpQuantizeToF16`; the fragment input flag in word 3's bit 31 is no-perspective interpolation (refused if a program reads an input both ways).

**Checked**: unit tests translate synthetic programs and check refusals by name; and **`ridgeracer7_shader_check`** (built with the lifted code) gives all 242 to the local Vulkan driver: a module for each, and a graphics pipeline for each with a partner that writes what it reads. On the owner's PC (NVIDIA GeForce GTX 1080 Ti) **all 242 modules and 242 pipelines are taken.** A driver is lenient, so they are also given to **Khronos' validator**, `spirv-val` (SPIRV-Tools v2026.4, built from source outside the repository; `tools/check/validate_shaders.py`), for the Vulkan 1.3 environment: **all 242 pass**, and a module with a fault put in on purpose (a fragment shader's origin turned to lower-left) is refused. *Still owed: a test of the translation's meaning against a reference, as the lifters have.*
