// SPU (the Cell's Synergistic Processor Unit) instruction decoder.
//
// An SPU is not a PowerPC. It has 128 registers of 128 bits, each used as a
// vector or for the "preferred slot" (the first word) of one, no condition
// register, 256 KiB of memory of its own, and channels for talking to the
// rest of the machine. Every instruction is one big-endian word whose opcode
// is its first 4, 7, 8, 9, 10 or 11 bits, depending on the form.
//
// The decoder has the PPU decoder's shape (ppu.hpp): one `Spec` per
// instruction, tables built from the specs, and every bit a spec doesn't
// define must be zero, so reserved bits set decode as invalid.
//
// The text form (`disasm`) follows GNU objdump's for the SPU (binutils 2.31,
// the last line to have the target), without its trailing "# hex" comments,
// so tools/check/verify_spu_decoder.py can compare the two word by word.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qilin::spu {

// The forms: which operand fields an instruction has, in the order printed.
enum class Form : uint8_t {
    none,       // no operands (lnop, dsync)
    sync,       // none; bit 11 (C) may be set
    nop,        // rt, which is ignored
    stop,       // a 14-bit signal type
    rr,         // rt, ra, rb
    rr2,        // rt, ra
    rt1,        // rt
    ra1,        // ra
    rrr,        // rt (bits 4-10), ra, rb, rc (bits 25-31)
    ri7,        // rt, ra, 7-bit immediate
    ri7_index,  // rt, i7(ra)
    scale173,   // rt, ra, 173 - the 8-bit immediate (cflts, cfltu)
    scale155,   // rt, ra, 155 - the 8-bit immediate (csflt, cuflt)
    ri10,       // rt, ra, 10-bit signed immediate
    ri10_index, // rt, 16*i10(ra)
    ri16,       // rt, 16-bit signed immediate
    rx16,       // rt, 16-bit unsigned immediate
    rt_rel,     // rt, an address relative to this instruction
    rt_abs,     // rt, an absolute address
    rel,        // a relative address
    abs,        // an absolute address
    ri18,       // rt, 18-bit unsigned immediate
    spr_read,   // rt, $spN
    spr_write,  // $spN, rt
    ch_read,    // rt, $chN
    ch_write,   // $chN, rt
    branch_ra,  // ra; bits 12 and 13 (D, E: disable or enable interrupts) may be set
    branch_rt_ra,  // rt, ra; D and E likewise
    hint_ra,    // the hinted branch's address, ra; bit 11 (P) may be set
    hint_abs,   // the hinted branch's address, the absolute target
    hint_rel,   // the hinted branch's address, the relative target
};

// Instruction properties, for the analysis passes and the lifter.
enum Flag : uint32_t {
    kBranch = 1u << 0,
    kCall = 1u << 1,         // a branch that sets a link register
    kConditional = 1u << 2,
    kIndirect = 1u << 3,     // the target is in a register
    kLoad = 1u << 4,
    kStore = 1u << 5,
    kFloat = 1u << 6,
    kChannel = 1u << 7,
    kHint = 1u << 8,         // a branch hint: no effect on what the program computes
    kStop = 1u << 9,
};

// Every SPU instruction: identifier, mnemonic, the opcode's length in bits,
// the opcode, the form, the flags.
#define QILIN_SPU_OPS(X)                                                                   \
    /* memory */                                                                           \
    X(lqd, "lqd", 8, 0x34, ri10_index, kLoad) X(lqx, "lqx", 11, 0x1c4, rr, kLoad)          \
    X(lqa, "lqa", 9, 0x061, rt_abs, kLoad) X(lqr, "lqr", 9, 0x067, rt_rel, kLoad)          \
    X(stqd, "stqd", 8, 0x24, ri10_index, kStore) X(stqx, "stqx", 11, 0x144, rr, kStore)    \
    X(stqa, "stqa", 9, 0x041, rt_abs, kStore) X(stqr, "stqr", 9, 0x047, rt_rel, kStore)    \
    X(cbd, "cbd", 11, 0x1f4, ri7_index, 0) X(cbx, "cbx", 11, 0x1d4, rr, 0)                 \
    X(chd, "chd", 11, 0x1f5, ri7_index, 0) X(chx, "chx", 11, 0x1d5, rr, 0)                 \
    X(cwd, "cwd", 11, 0x1f6, ri7_index, 0) X(cwx, "cwx", 11, 0x1d6, rr, 0)                 \
    X(cdd, "cdd", 11, 0x1f7, ri7_index, 0) X(cdx, "cdx", 11, 0x1d7, rr, 0)                 \
    /* constants */                                                                        \
    X(ilh, "ilh", 9, 0x083, rx16, 0) X(ilhu, "ilhu", 9, 0x082, rx16, 0)                    \
    X(il, "il", 9, 0x081, ri16, 0) X(ila, "ila", 7, 0x21, ri18, 0)                         \
    X(iohl, "iohl", 9, 0x0c1, rx16, 0) X(fsmbi, "fsmbi", 9, 0x065, rx16, 0)                \
    /* integer arithmetic */                                                               \
    X(ah, "ah", 11, 0x0c8, rr, 0) X(ahi, "ahi", 8, 0x1d, ri10, 0)                          \
    X(a, "a", 11, 0x0c0, rr, 0) X(ai, "ai", 8, 0x1c, ri10, 0)                              \
    X(sfh, "sfh", 11, 0x048, rr, 0) X(sfhi, "sfhi", 8, 0x0d, ri10, 0)                      \
    X(sf, "sf", 11, 0x040, rr, 0) X(sfi, "sfi", 8, 0x0c, ri10, 0)                          \
    X(addx, "addx", 11, 0x340, rr, 0) X(cg, "cg", 11, 0x0c2, rr, 0)                        \
    X(cgx, "cgx", 11, 0x342, rr, 0) X(sfx, "sfx", 11, 0x341, rr, 0)                        \
    X(bg, "bg", 11, 0x042, rr, 0) X(bgx, "bgx", 11, 0x343, rr, 0)                          \
    X(mpy, "mpy", 11, 0x3c4, rr, 0) X(mpyu, "mpyu", 11, 0x3cc, rr, 0)                      \
    X(mpyi, "mpyi", 8, 0x74, ri10, 0) X(mpyui, "mpyui", 8, 0x75, ri10, 0)                  \
    X(mpya, "mpya", 4, 0xc, rrr, 0) X(mpyh, "mpyh", 11, 0x3c5, rr, 0)                      \
    X(mpys, "mpys", 11, 0x3c7, rr, 0) X(mpyhh, "mpyhh", 11, 0x3c6, rr, 0)                  \
    X(mpyhha, "mpyhha", 11, 0x346, rr, 0) X(mpyhhu, "mpyhhu", 11, 0x3ce, rr, 0)            \
    X(mpyhhau, "mpyhhau", 11, 0x34e, rr, 0)                                                \
    X(clz, "clz", 11, 0x2a5, rr2, 0) X(cntb, "cntb", 11, 0x2b4, rr2, 0)                    \
    X(fsmb, "fsmb", 11, 0x1b6, rr2, 0) X(fsmh, "fsmh", 11, 0x1b5, rr2, 0)                  \
    X(fsm, "fsm", 11, 0x1b4, rr2, 0) X(gbb, "gbb", 11, 0x1b2, rr2, 0)                      \
    X(gbh, "gbh", 11, 0x1b1, rr2, 0) X(gb, "gb", 11, 0x1b0, rr2, 0)                        \
    X(avgb, "avgb", 11, 0x0d3, rr, 0) X(absdb, "absdb", 11, 0x053, rr, 0)                  \
    X(sumb, "sumb", 11, 0x253, rr, 0) X(xsbh, "xsbh", 11, 0x2b6, rr2, 0)                   \
    X(xshw, "xshw", 11, 0x2ae, rr2, 0) X(xswd, "xswd", 11, 0x2a6, rr2, 0)                  \
    /* logical */                                                                          \
    X(and_, "and", 11, 0x0c1, rr, 0) X(andc, "andc", 11, 0x2c1, rr, 0)                     \
    X(andbi, "andbi", 8, 0x16, ri10, 0) X(andhi, "andhi", 8, 0x15, ri10, 0)                \
    X(andi, "andi", 8, 0x14, ri10, 0) X(or_, "or", 11, 0x041, rr, 0)                       \
    X(orc, "orc", 11, 0x2c9, rr, 0) X(orbi, "orbi", 8, 0x06, ri10, 0)                      \
    X(orhi, "orhi", 8, 0x05, ri10, 0) X(ori, "ori", 8, 0x04, ri10, 0)                      \
    X(orx, "orx", 11, 0x1f0, rr2, 0) X(xor_, "xor", 11, 0x241, rr, 0)                      \
    X(xorbi, "xorbi", 8, 0x46, ri10, 0) X(xorhi, "xorhi", 8, 0x45, ri10, 0)                \
    X(xori, "xori", 8, 0x44, ri10, 0) X(nand, "nand", 11, 0x0c9, rr, 0)                    \
    X(nor, "nor", 11, 0x049, rr, 0) X(eqv, "eqv", 11, 0x249, rr, 0)                        \
    X(selb, "selb", 4, 0x8, rrr, 0) X(shufb, "shufb", 4, 0xb, rrr, 0)                      \
    /* shift and rotate */                                                                 \
    X(shlh, "shlh", 11, 0x05f, rr, 0) X(shlhi, "shlhi", 11, 0x07f, ri7, 0)                 \
    X(shl, "shl", 11, 0x05b, rr, 0) X(shli, "shli", 11, 0x07b, ri7, 0)                     \
    X(shlqbi, "shlqbi", 11, 0x1db, rr, 0) X(shlqbii, "shlqbii", 11, 0x1fb, ri7, 0)         \
    X(shlqby, "shlqby", 11, 0x1df, rr, 0) X(shlqbyi, "shlqbyi", 11, 0x1ff, ri7, 0)         \
    X(shlqbybi, "shlqbybi", 11, 0x1cf, rr, 0)                                              \
    X(roth, "roth", 11, 0x05c, rr, 0) X(rothi, "rothi", 11, 0x07c, ri7, 0)                 \
    X(rot, "rot", 11, 0x058, rr, 0) X(roti, "roti", 11, 0x078, ri7, 0)                     \
    X(rotqby, "rotqby", 11, 0x1dc, rr, 0) X(rotqbyi, "rotqbyi", 11, 0x1fc, ri7, 0)         \
    X(rotqbybi, "rotqbybi", 11, 0x1cc, rr, 0) X(rotqbi, "rotqbi", 11, 0x1d8, rr, 0)        \
    X(rotqbii, "rotqbii", 11, 0x1f8, ri7, 0)                                               \
    X(rothm, "rothm", 11, 0x05d, rr, 0) X(rothmi, "rothmi", 11, 0x07d, ri7, 0)             \
    X(rotm, "rotm", 11, 0x059, rr, 0) X(rotmi, "rotmi", 11, 0x079, ri7, 0)                 \
    X(rotqmby, "rotqmby", 11, 0x1dd, rr, 0) X(rotqmbyi, "rotqmbyi", 11, 0x1fd, ri7, 0)     \
    X(rotqmbybi, "rotqmbybi", 11, 0x1cd, rr, 0) X(rotqmbi, "rotqmbi", 11, 0x1d9, rr, 0)    \
    X(rotqmbii, "rotqmbii", 11, 0x1f9, ri7, 0)                                             \
    X(rotmah, "rotmah", 11, 0x05e, rr, 0) X(rotmahi, "rotmahi", 11, 0x07e, ri7, 0)         \
    X(rotma, "rotma", 11, 0x05a, rr, 0) X(rotmai, "rotmai", 11, 0x07a, ri7, 0)             \
    /* compare, branch, halt */                                                            \
    X(heq, "heq", 11, 0x3d8, rr, kStop) X(heqi, "heqi", 8, 0x7f, ri10, kStop)              \
    X(hgt, "hgt", 11, 0x258, rr, kStop) X(hgti, "hgti", 8, 0x4f, ri10, kStop)              \
    X(hlgt, "hlgt", 11, 0x2d8, rr, kStop) X(hlgti, "hlgti", 8, 0x5f, ri10, kStop)          \
    X(ceqb, "ceqb", 11, 0x3d0, rr, 0) X(ceqbi, "ceqbi", 8, 0x7e, ri10, 0)                  \
    X(ceqh, "ceqh", 11, 0x3c8, rr, 0) X(ceqhi, "ceqhi", 8, 0x7d, ri10, 0)                  \
    X(ceq, "ceq", 11, 0x3c0, rr, 0) X(ceqi, "ceqi", 8, 0x7c, ri10, 0)                      \
    X(cgtb, "cgtb", 11, 0x250, rr, 0) X(cgtbi, "cgtbi", 8, 0x4e, ri10, 0)                  \
    X(cgth, "cgth", 11, 0x248, rr, 0) X(cgthi, "cgthi", 8, 0x4d, ri10, 0)                  \
    X(cgt, "cgt", 11, 0x240, rr, 0) X(cgti, "cgti", 8, 0x4c, ri10, 0)                      \
    X(clgtb, "clgtb", 11, 0x2d0, rr, 0) X(clgtbi, "clgtbi", 8, 0x5e, ri10, 0)              \
    X(clgth, "clgth", 11, 0x2c8, rr, 0) X(clgthi, "clgthi", 8, 0x5d, ri10, 0)              \
    X(clgt, "clgt", 11, 0x2c0, rr, 0) X(clgti, "clgti", 8, 0x5c, ri10, 0)                  \
    X(br, "br", 9, 0x064, rel, kBranch) X(bra, "bra", 9, 0x060, abs, kBranch)              \
    X(brsl, "brsl", 9, 0x066, rt_rel, kBranch | kCall)                                     \
    X(brasl, "brasl", 9, 0x062, rt_abs, kBranch | kCall)                                   \
    X(bi, "bi", 11, 0x1a8, branch_ra, kBranch | kIndirect)                                 \
    X(iret, "iret", 11, 0x1aa, branch_ra, kBranch | kIndirect)                             \
    X(bisled, "bisled", 11, 0x1ab, branch_rt_ra, kBranch | kCall | kIndirect | kConditional) \
    X(bisl, "bisl", 11, 0x1a9, branch_rt_ra, kBranch | kCall | kIndirect)                  \
    X(brnz, "brnz", 9, 0x042, rt_rel, kBranch | kConditional)                              \
    X(brz, "brz", 9, 0x040, rt_rel, kBranch | kConditional)                                \
    X(brhnz, "brhnz", 9, 0x046, rt_rel, kBranch | kConditional)                            \
    X(brhz, "brhz", 9, 0x044, rt_rel, kBranch | kConditional)                              \
    X(biz, "biz", 11, 0x128, branch_rt_ra, kBranch | kIndirect | kConditional)             \
    X(binz, "binz", 11, 0x129, branch_rt_ra, kBranch | kIndirect | kConditional)           \
    X(bihz, "bihz", 11, 0x12a, branch_rt_ra, kBranch | kIndirect | kConditional)           \
    X(bihnz, "bihnz", 11, 0x12b, branch_rt_ra, kBranch | kIndirect | kConditional)         \
    X(hbr, "hbr", 11, 0x1ac, hint_ra, kHint) X(hbra, "hbra", 7, 0x08, hint_abs, kHint)     \
    X(hbrr, "hbrr", 7, 0x09, hint_rel, kHint)                                              \
    /* floating point */                                                                   \
    X(fa, "fa", 11, 0x2c4, rr, kFloat) X(dfa, "dfa", 11, 0x2cc, rr, kFloat)                \
    X(fs, "fs", 11, 0x2c5, rr, kFloat) X(dfs, "dfs", 11, 0x2cd, rr, kFloat)                \
    X(fm, "fm", 11, 0x2c6, rr, kFloat) X(dfm, "dfm", 11, 0x2ce, rr, kFloat)                \
    X(fma, "fma", 4, 0xe, rrr, kFloat) X(dfma, "dfma", 11, 0x35c, rr, kFloat)              \
    X(fnms, "fnms", 4, 0xd, rrr, kFloat) X(dfnms, "dfnms", 11, 0x35e, rr, kFloat)          \
    X(fms, "fms", 4, 0xf, rrr, kFloat) X(dfms, "dfms", 11, 0x35d, rr, kFloat)              \
    X(dfnma, "dfnma", 11, 0x35f, rr, kFloat)                                               \
    X(frest, "frest", 11, 0x1b8, rr2, kFloat) X(frsqest, "frsqest", 11, 0x1b9, rr2, kFloat) \
    X(fi, "fi", 11, 0x3d4, rr, kFloat)                                                     \
    X(csflt, "csflt", 10, 0x1da, scale155, kFloat) X(cflts, "cflts", 10, 0x1d8, scale173, kFloat) \
    X(cuflt, "cuflt", 10, 0x1db, scale155, kFloat) X(cfltu, "cfltu", 10, 0x1d9, scale173, kFloat) \
    X(frds, "frds", 11, 0x3b9, rr2, kFloat) X(fesd, "fesd", 11, 0x3b8, rr2, kFloat)        \
    X(dfceq, "dfceq", 11, 0x3c3, rr, kFloat) X(dfcmeq, "dfcmeq", 11, 0x3cb, rr, kFloat)    \
    X(dfcgt, "dfcgt", 11, 0x2c3, rr, kFloat) X(dfcmgt, "dfcmgt", 11, 0x2cb, rr, kFloat)    \
    X(dftsv, "dftsv", 11, 0x3bf, ri7, kFloat)                                              \
    X(fceq, "fceq", 11, 0x3c2, rr, kFloat) X(fcmeq, "fcmeq", 11, 0x3ca, rr, kFloat)        \
    X(fcgt, "fcgt", 11, 0x2c2, rr, kFloat) X(fcmgt, "fcmgt", 11, 0x2ca, rr, kFloat)        \
    X(fscrwr, "fscrwr", 11, 0x3ba, rr2, kFloat) X(fscrrd, "fscrrd", 11, 0x398, rt1, kFloat) \
    /* control */                                                                          \
    X(stop, "stop", 11, 0x000, stop, kStop) X(stopd, "stopd", 11, 0x140, rr, kStop)        \
    X(lnop, "lnop", 11, 0x001, none, 0) X(nop, "nop", 11, 0x201, nop, 0)                   \
    X(sync, "sync", 11, 0x002, sync, 0) X(dsync, "dsync", 11, 0x003, none, 0)              \
    X(mfspr, "mfspr", 11, 0x00c, spr_read, 0) X(mtspr, "mtspr", 11, 0x10c, spr_write, 0)   \
    X(rdch, "rdch", 11, 0x00d, ch_read, kChannel)                                          \
    X(rchcnt, "rchcnt", 11, 0x00f, ch_read, kChannel)                                      \
    X(wrch, "wrch", 11, 0x10d, ch_write, kChannel)

enum class Op : uint16_t {
    invalid,
#define QILIN_SPU_ENUM(id, name, bits, opcode, form, flags) id,
    QILIN_SPU_OPS(QILIN_SPU_ENUM)
#undef QILIN_SPU_ENUM
    count_
};

struct Spec {
    Op op = Op::invalid;
    std::string_view name;
    uint8_t bits = 0;      // the opcode's length
    uint16_t opcode = 0;   // its value, in that many bits
    Form form = Form::none;
    uint32_t flags = 0;
    uint32_t defined = 0;  // the bits the instruction defines; the rest must be zero
};

struct Insn {
    uint32_t word = 0;
    Op op = Op::invalid;

    bool valid() const { return op != Op::invalid; }

    // Fields. The caller knows which ones the instruction has.
    unsigned rt() const { return word & 0x7f; }          // in the rrr form, the fourth operand (rc)
    unsigned ra() const { return (word >> 7) & 0x7f; }   // also the channel, the special-purpose register
    unsigned rb() const { return (word >> 14) & 0x7f; }
    unsigned rt4() const { return (word >> 21) & 0x7f; } // the rrr form's target
    int32_t i7() const { return static_cast<int32_t>(word << 11) >> 25; }
    unsigned u8() const { return (word >> 14) & 0xff; }
    int32_t i10() const { return static_cast<int32_t>(word << 8) >> 22; }
    int32_t i16() const { return static_cast<int32_t>(word << 9) >> 16; }
    uint32_t u16() const { return (word >> 7) & 0xffff; }
    uint32_t u18() const { return (word >> 7) & 0x3ffff; }
    uint32_t u14() const { return word & 0x3fff; }
    bool d() const { return word & 0x00080000; }  // an indirect branch that disables interrupts
    bool e() const { return word & 0x00040000; }  // ... or enables them

    // The target of a relative or absolute branch or load at address pc,
    // within an SPU's 256 KiB.
    uint32_t relative(uint32_t pc) const { return (pc + (static_cast<uint32_t>(i16()) << 2)) & 0x3ffff; }
    uint32_t absolute() const { return (u16() << 2) & 0x3ffff; }
    // The address of the branch a hint is for.
    uint32_t hinted(uint32_t pc) const;
};

const Spec& spec(Op op);
std::span<const Spec> specs();

// `ignore_reserved`: decode by the opcode alone, as binutils does (for the check against it).
Insn decode(uint32_t word, bool ignore_reserved = false);

// Text of an instruction at address pc, as objdump prints it: "ai $1,$1,-32",
// "lqd $3,16($1)", "brsl $0,0x3040"; an invalid word is ".long 0x...".
std::string disasm(const Insn& insn, uint32_t pc);

}  // namespace qilin::spu
