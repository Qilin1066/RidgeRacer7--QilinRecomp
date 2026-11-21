// PPU (the Cell's PowerPC core) instruction decoder.
//
// The PPU implements the 64-bit PowerPC architecture (version 2.02) with the
// vector extension IBM calls VMX and Freescale AltiVec, and a handful of
// vector loads and stores of its own (lvlx, lvrx, stvlx, stvrx: unaligned
// access by halves). Every instruction is one big-endian word. A PS3 program
// runs in user mode, so the supervisor's instructions (the MSR, the segment
// and translation tables, rfid) are not here: they decode as invalid.
//
// The decoder is table-driven, in the shape the other Qilin recompilers use:
// one `Spec` per base mnemonic, with its opcode, extended opcode and operand
// fields. Every bit a spec doesn't define must be zero, so reserved bits set,
// invalid forms (a load with update whose base is its target, a branch whose
// BO has a reserved bit set, ...) and anything outside the PPU's set decode as
// invalid. A retail compiler never emits them, and the recompiler fails loudly
// on them (docs/roadmap-and-design-targets.md).
//
// The text form (`disasm`) follows GNU objdump's `-M cell,raw` output, so
// tools/check/verify_decoder.py can compare the two word by word.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qilin::ppu {

// Every PPU instruction: identifier and mnemonic. Rc ("."), OE ("o"),
// LK ("l") and AA ("a") are bits of the same instruction, not separate entries.
#define QILIN_PPU_OPS(X)                                                                     \
    /* integer arithmetic */                                                                 \
    X(add, "add") X(addc, "addc") X(adde, "adde") X(addi, "addi") X(addic, "addic")          \
    X(addic_rc, "addic.") X(addis, "addis") X(addme, "addme") X(addze, "addze")              \
    X(divd, "divd") X(divdu, "divdu") X(divw, "divw") X(divwu, "divwu")                      \
    X(mulhd, "mulhd") X(mulhdu, "mulhdu") X(mulhw, "mulhw") X(mulhwu, "mulhwu")              \
    X(mulld, "mulld") X(mulli, "mulli") X(mullw, "mullw") X(neg, "neg") X(subf, "subf")      \
    X(subfc, "subfc") X(subfe, "subfe") X(subfic, "subfic") X(subfme, "subfme")              \
    X(subfze, "subfze")                                                                      \
    /* integer compare */                                                                    \
    X(cmp, "cmp") X(cmpi, "cmpi") X(cmpl, "cmpl") X(cmpli, "cmpli")                          \
    /* integer logical */                                                                    \
    X(and_, "and") X(andc, "andc") X(andi_rc, "andi.") X(andis_rc, "andis.")                 \
    X(cntlzd, "cntlzd") X(cntlzw, "cntlzw") X(eqv, "eqv") X(extsb, "extsb")                  \
    X(extsh, "extsh") X(extsw, "extsw") X(nand, "nand") X(nor, "nor") X(or_, "or")           \
    X(orc, "orc") X(ori, "ori") X(oris, "oris") X(xor_, "xor") X(xori, "xori")               \
    X(xoris, "xoris")                                                                        \
    /* rotate and shift */                                                                   \
    X(rldcl, "rldcl") X(rldcr, "rldcr") X(rldic, "rldic") X(rldicl, "rldicl")                \
    X(rldicr, "rldicr") X(rldimi, "rldimi") X(rlwimi, "rlwimi") X(rlwinm, "rlwinm")          \
    X(rlwnm, "rlwnm") X(sld, "sld") X(slw, "slw") X(srad, "srad") X(sradi, "sradi")          \
    X(sraw, "sraw") X(srawi, "srawi") X(srd, "srd") X(srw, "srw")                            \
    /* floating point */                                                                     \
    X(fabs, "fabs") X(fadd, "fadd") X(fadds, "fadds") X(fcfid, "fcfid")                      \
    X(fcmpo, "fcmpo") X(fcmpu, "fcmpu") X(fctid, "fctid") X(fctidz, "fctidz")                \
    X(fctiw, "fctiw") X(fctiwz, "fctiwz") X(fdiv, "fdiv") X(fdivs, "fdivs")                  \
    X(fmadd, "fmadd") X(fmadds, "fmadds") X(fmr, "fmr") X(fmsub, "fmsub")                    \
    X(fmsubs, "fmsubs") X(fmul, "fmul") X(fmuls, "fmuls") X(fnabs, "fnabs")                  \
    X(fneg, "fneg") X(fnmadd, "fnmadd") X(fnmadds, "fnmadds") X(fnmsub, "fnmsub")            \
    X(fnmsubs, "fnmsubs") X(fres, "fres") X(frsp, "frsp") X(frsqrte, "frsqrte")              \
    X(fsel, "fsel") X(fsqrt, "fsqrt") X(fsqrts, "fsqrts") X(fsub, "fsub")                    \
    X(fsubs, "fsubs")                                                                        \
    X(mcrfs, "mcrfs") X(mffs, "mffs") X(mtfsb0, "mtfsb0") X(mtfsb1, "mtfsb1")                \
    X(mtfsf, "mtfsf") X(mtfsfi, "mtfsfi")                                                    \
    /* integer load and store */                                                             \
    X(lbz, "lbz") X(lbzu, "lbzu") X(lbzux, "lbzux") X(lbzx, "lbzx")                          \
    X(ld, "ld") X(ldu, "ldu") X(ldux, "ldux") X(ldx, "ldx")                                  \
    X(lha, "lha") X(lhau, "lhau") X(lhaux, "lhaux") X(lhax, "lhax")                          \
    X(lhz, "lhz") X(lhzu, "lhzu") X(lhzux, "lhzux") X(lhzx, "lhzx")                          \
    X(lwa, "lwa") X(lwaux, "lwaux") X(lwax, "lwax")                                          \
    X(lwz, "lwz") X(lwzu, "lwzu") X(lwzux, "lwzux") X(lwzx, "lwzx")                          \
    X(stb, "stb") X(stbu, "stbu") X(stbux, "stbux") X(stbx, "stbx")                          \
    X(std, "std") X(stdu, "stdu") X(stdux, "stdux") X(stdx, "stdx")                          \
    X(sth, "sth") X(sthu, "sthu") X(sthux, "sthux") X(sthx, "sthx")                          \
    X(stw, "stw") X(stwu, "stwu") X(stwux, "stwux") X(stwx, "stwx")                          \
    X(ldbrx, "ldbrx") X(lhbrx, "lhbrx") X(lwbrx, "lwbrx") X(stdbrx, "stdbrx")                \
    X(sthbrx, "sthbrx") X(stwbrx, "stwbrx")                                                  \
    X(lmw, "lmw") X(stmw, "stmw") X(lswi, "lswi") X(lswx, "lswx") X(stswi, "stswi")          \
    X(stswx, "stswx") X(ldarx, "ldarx") X(lwarx, "lwarx") X(stdcx_rc, "stdcx.")              \
    X(stwcx_rc, "stwcx.")                                                                    \
    /* floating-point load and store */                                                      \
    X(lfd, "lfd") X(lfdu, "lfdu") X(lfdux, "lfdux") X(lfdx, "lfdx")                          \
    X(lfs, "lfs") X(lfsu, "lfsu") X(lfsux, "lfsux") X(lfsx, "lfsx")                          \
    X(stfd, "stfd") X(stfdu, "stfdu") X(stfdux, "stfdux") X(stfdx, "stfdx")                  \
    X(stfs, "stfs") X(stfsu, "stfsu") X(stfsux, "stfsux") X(stfsx, "stfsx")                  \
    X(stfiwx, "stfiwx")                                                                      \
    /* branch, condition register, system */                                                 \
    X(b, "b") X(bc, "bc") X(bcctr, "bcctr") X(bclr, "bclr")                                  \
    X(crand, "crand") X(crandc, "crandc") X(creqv, "creqv") X(crnand, "crnand")              \
    X(crnor, "crnor") X(cror, "cror") X(crorc, "crorc") X(crxor, "crxor") X(mcrf, "mcrf")    \
    X(sc, "sc") X(td, "td") X(tdi, "tdi") X(tw, "tw") X(twi, "twi")                          \
    X(mcrxr, "mcrxr") X(mfcr, "mfcr") X(mfocrf, "mfocrf") X(mfspr, "mfspr")                  \
    X(mftb, "mftb") X(mtcrf, "mtcrf") X(mtocrf, "mtocrf") X(mtspr, "mtspr")                  \
    /* memory synchronisation and caches */                                                  \
    X(eieio, "eieio") X(isync, "isync") X(sync, "sync")                                      \
    X(dcbf, "dcbf") X(dcbst, "dcbst") X(dcbt, "dcbt") X(dcbtst, "dcbtst")                    \
    X(dcbz, "dcbz") X(icbi, "icbi")                                                          \
    /* VMX: load and store, and the data-stream hints */                                     \
    X(lvebx, "lvebx") X(lvehx, "lvehx") X(lvewx, "lvewx") X(lvsl, "lvsl")                    \
    X(lvsr, "lvsr") X(lvx, "lvx") X(lvxl, "lvxl") X(stvebx, "stvebx")                        \
    X(stvehx, "stvehx") X(stvewx, "stvewx") X(stvx, "stvx") X(stvxl, "stvxl")                \
    X(lvlx, "lvlx") X(lvlxl, "lvlxl") X(lvrx, "lvrx") X(lvrxl, "lvrxl")                      \
    X(stvlx, "stvlx") X(stvlxl, "stvlxl") X(stvrx, "stvrx") X(stvrxl, "stvrxl")              \
    X(dst, "dst") X(dstt, "dstt") X(dstst, "dstst") X(dststt, "dststt") X(dss, "dss")        \
    X(dssall, "dssall") X(mfvscr, "mfvscr") X(mtvscr, "mtvscr")                              \
    /* VMX: integer arithmetic */                                                            \
    X(vaddcuw, "vaddcuw") X(vaddsbs, "vaddsbs") X(vaddshs, "vaddshs")                        \
    X(vaddsws, "vaddsws") X(vaddubm, "vaddubm") X(vaddubs, "vaddubs")                        \
    X(vadduhm, "vadduhm") X(vadduhs, "vadduhs") X(vadduwm, "vadduwm")                        \
    X(vadduws, "vadduws") X(vsubcuw, "vsubcuw") X(vsubsbs, "vsubsbs")                        \
    X(vsubshs, "vsubshs") X(vsubsws, "vsubsws") X(vsububm, "vsububm")                        \
    X(vsububs, "vsububs") X(vsubuhm, "vsubuhm") X(vsubuhs, "vsubuhs")                        \
    X(vsubuwm, "vsubuwm") X(vsubuws, "vsubuws")                                              \
    X(vavgsb, "vavgsb") X(vavgsh, "vavgsh") X(vavgsw, "vavgsw") X(vavgub, "vavgub")          \
    X(vavguh, "vavguh") X(vavguw, "vavguw") X(vmaxsb, "vmaxsb") X(vmaxsh, "vmaxsh")          \
    X(vmaxsw, "vmaxsw") X(vmaxub, "vmaxub") X(vmaxuh, "vmaxuh") X(vmaxuw, "vmaxuw")          \
    X(vminsb, "vminsb") X(vminsh, "vminsh") X(vminsw, "vminsw") X(vminub, "vminub")          \
    X(vminuh, "vminuh") X(vminuw, "vminuw")                                                  \
    X(vmulesb, "vmulesb") X(vmulesh, "vmulesh") X(vmuleub, "vmuleub")                        \
    X(vmuleuh, "vmuleuh") X(vmulosb, "vmulosb") X(vmulosh, "vmulosh")                        \
    X(vmuloub, "vmuloub") X(vmulouh, "vmulouh")                                              \
    X(vmhaddshs, "vmhaddshs") X(vmhraddshs, "vmhraddshs") X(vmladduhm, "vmladduhm")          \
    X(vmsummbm, "vmsummbm") X(vmsumshm, "vmsumshm") X(vmsumshs, "vmsumshs")                  \
    X(vmsumubm, "vmsumubm") X(vmsumuhm, "vmsumuhm") X(vmsumuhs, "vmsumuhs")                  \
    X(vsum2sws, "vsum2sws") X(vsum4sbs, "vsum4sbs") X(vsum4shs, "vsum4shs")                  \
    X(vsum4ubs, "vsum4ubs") X(vsumsws, "vsumsws")                                            \
    /* VMX: logical, rotate and shift */                                                     \
    X(vand, "vand") X(vandc, "vandc") X(vnor, "vnor") X(vor, "vor") X(vxor, "vxor")          \
    X(vrlb, "vrlb") X(vrlh, "vrlh") X(vrlw, "vrlw") X(vsl, "vsl") X(vslb, "vslb")            \
    X(vslh, "vslh") X(vslo, "vslo") X(vslw, "vslw") X(vsr, "vsr") X(vsrab, "vsrab")          \
    X(vsrah, "vsrah") X(vsraw, "vsraw") X(vsrb, "vsrb") X(vsrh, "vsrh") X(vsro, "vsro")      \
    X(vsrw, "vsrw") X(vsldoi, "vsldoi")                                                      \
    /* VMX: compare, select, permute, merge, splat, pack */                                  \
    X(vcmpbfp, "vcmpbfp") X(vcmpeqfp, "vcmpeqfp") X(vcmpequb, "vcmpequb")                    \
    X(vcmpequh, "vcmpequh") X(vcmpequw, "vcmpequw") X(vcmpgefp, "vcmpgefp")                  \
    X(vcmpgtfp, "vcmpgtfp") X(vcmpgtsb, "vcmpgtsb") X(vcmpgtsh, "vcmpgtsh")                  \
    X(vcmpgtsw, "vcmpgtsw") X(vcmpgtub, "vcmpgtub") X(vcmpgtuh, "vcmpgtuh")                  \
    X(vcmpgtuw, "vcmpgtuw") X(vsel, "vsel") X(vperm, "vperm")                                \
    X(vmrghb, "vmrghb") X(vmrghh, "vmrghh") X(vmrghw, "vmrghw") X(vmrglb, "vmrglb")          \
    X(vmrglh, "vmrglh") X(vmrglw, "vmrglw") X(vspltb, "vspltb") X(vsplth, "vsplth")          \
    X(vspltw, "vspltw") X(vspltisb, "vspltisb") X(vspltish, "vspltish")                      \
    X(vspltisw, "vspltisw") X(vpkpx, "vpkpx") X(vpkshss, "vpkshss")                          \
    X(vpkshus, "vpkshus") X(vpkswss, "vpkswss") X(vpkswus, "vpkswus")                        \
    X(vpkuhum, "vpkuhum") X(vpkuhus, "vpkuhus") X(vpkuwum, "vpkuwum")                        \
    X(vpkuwus, "vpkuwus") X(vupkhpx, "vupkhpx") X(vupkhsb, "vupkhsb")                        \
    X(vupkhsh, "vupkhsh") X(vupklpx, "vupklpx") X(vupklsb, "vupklsb")                        \
    X(vupklsh, "vupklsh")                                                                    \
    /* VMX: floating point */                                                                \
    X(vaddfp, "vaddfp") X(vsubfp, "vsubfp") X(vmaddfp, "vmaddfp")                            \
    X(vnmsubfp, "vnmsubfp") X(vmaxfp, "vmaxfp") X(vminfp, "vminfp")                          \
    X(vrefp, "vrefp") X(vrsqrtefp, "vrsqrtefp") X(vexptefp, "vexptefp")                      \
    X(vlogefp, "vlogefp") X(vrfim, "vrfim") X(vrfin, "vrfin") X(vrfip, "vrfip")              \
    X(vrfiz, "vrfiz") X(vcfsx, "vcfsx") X(vcfux, "vcfux") X(vctsxs, "vctsxs")                \
    X(vctuxs, "vctuxs")

enum class Op : uint16_t {
    invalid,
#define QILIN_PPU_ENUM(id, name) id,
    QILIN_PPU_OPS(QILIN_PPU_ENUM)
#undef QILIN_PPU_ENUM
    count_
};

// Operand fields, in the order objdump prints them. `zero` prints "0" and
// defines no bits: it stands for a field later versions of the architecture
// added (lwarx's EH, dcbf's L) that is reserved on the PPU.
enum class Opnd : uint8_t {
    none,
    rD,         // GPR in bits 6-10 (also rS)
    rA,         // GPR in bits 11-15
    rA0,        // bits 11-15, where 0 means the value 0, not r0
    rB,         // GPR in bits 16-20
    fD,         // FPR in bits 6-10 (also frS)
    fA, fB, fC, // FPRs in bits 11-15, 16-20, 21-25
    vD,         // vector register in bits 6-10 (also vS)
    vA, vB, vC, // vector registers in bits 11-15, 16-20, 21-25
    crfD,       // CR field in bits 6-8
    crfS,       // CR field in bits 11-13
    bf,         // FPSCR field in bits 6-8 (mtfsfi), printed as a number
    l,          // bit 10 of a compare: 1 compares 64 bits, 0 the low 32
    simm,       // signed 16-bit immediate
    uimm,       // unsigned 16-bit immediate
    d_rA0,      // 16-bit signed displacement and base: d(rA|0)
    ds_rA0,     // 14-bit signed displacement, times four, and base (ld, std, lwa)
    sh, mb, me, // 32-bit rotate amount and mask bounds
    sh6,        // 64-bit rotate or shift amount: bits 16-20, then bit 30 as the top bit
    mb6,        // 64-bit mask bound: bits 21-25, then bit 26 as the top bit
    nb,         // byte count for lswi/stswi (0 means 32)
    spr, tbr,   // special-purpose / time-base register number (halves swapped)
    to,         // trap condition
    bo, bi,     // branch options and condition bit
    bd,         // conditional branch target (relative unless AA)
    li,         // branch target (relative unless AA)
    bh,         // bits 19-20 of bclr/bcctr: a hint about the target
    crbD, crbA, crbB,  // condition-register bits
    fpscr_bit,  // FPSCR bit for mtfsb0/mtfsb1
    crm,        // CR field mask for mtcrf and the one-field forms
    fm,         // FPSCR field mask for mtfsf
    imm,        // 4-bit immediate for mtfsfi
    ls,         // sync's kind in bits 9-10 (0 sync, 1 lwsync, 2 ptesync)
    th,         // dcbt/dcbtst's hint in bits 6-10
    vsimm,      // signed 5-bit immediate in bits 11-15 (vspltis*)
    vuimm,      // unsigned 5-bit immediate in bits 11-15 (vcfsx and friends)
    vuimm4, vuimm3, vuimm2,  // the element number of vspltb, vsplth, vspltw
    shb,        // vsldoi's byte count in bits 22-25
    strm,       // data-stream number in bits 9-10
    zero,
};

// Instruction properties, for the analysis passes and the lifter.
enum Flag : uint32_t {
    kRc = 1u << 0,          // has a record bit (bit 31): "." sets CR0/CR1
    kOe = 1u << 1,          // has an overflow-enable bit (bit 21): "o"
    kLk = 1u << 2,          // has a link bit (bit 31): "l"
    kAa = 1u << 3,          // has an absolute-address bit (bit 30): "a"
    kBranch = 1u << 4,
    kLoad = 1u << 5,
    kStore = 1u << 6,
    kFloat = 1u << 7,       // reads or writes FPRs/FPSCR
    kVector = 1u << 8,      // VMX
    kCache = 1u << 9,       // cache management and prefetch hints
    kSync = 1u << 10,       // memory or instruction synchronisation
    kVrc = 1u << 11,        // VMX compare's record bit (bit 21): "." sets CR6
    k64 = 1u << 12,         // works on all 64 bits of a GPR where its 32-bit twin does not
    // Operand constraints that are invalid forms when broken:
    kRaNonZero = 1u << 16,      // rA != 0 (stores and FP loads with update)
    kRaNotZeroOrRd = 1u << 17,  // rA != 0 and rA != rD (integer loads with update)
    kLmw = 1u << 18,            // rA outside the registers lmw loads
    kLswi = 1u << 19,           // rA outside the registers lswi loads
    kValidBo = 1u << 20,        // BO is one of the defined encodings
    kBoNoDecrement = 1u << 21,  // bcctr: BO must not decrement CTR
    kLswx = 1u << 22,           // rD differs from rA and rB
    kOneCrField = 1u << 23,     // mfocrf/mtocrf: the mask names exactly one field
    kSyncKind = 1u << 24,       // sync: bits 9-10 are not 3
    kTimeBase = 1u << 25,       // mftb: the register is the time base (268) or its upper half (269)
};

struct Spec {
    Op op = Op::invalid;
    std::string_view name;
    uint32_t match = 0;  // opcode bits
    uint32_t mask = 0;   // which bits `match` covers
    Opnd operands[5] = {};
    uint32_t flags = 0;
};

// The bits an operand field occupies, as a mask. (Comments here number bits
// as IBM's manuals do for a 32-bit word, from 0 at the most significant.)
uint32_t field_mask(Opnd o);

struct Insn {
    uint32_t word = 0;
    Op op = Op::invalid;

    bool valid() const { return op != Op::invalid; }

    // Fields. The caller knows which ones the instruction has.
    unsigned primary() const { return word >> 26; }
    unsigned rd() const { return (word >> 21) & 31; }  // also rS, frD, vD, TO, BO, crbD
    unsigned ra() const { return (word >> 16) & 31; }  // also frA, vA, BI, crbA
    unsigned rb() const { return (word >> 11) & 31; }  // also frB, vB, crbB, SH, NB
    unsigned rc_reg() const { return (word >> 6) & 31; }  // frC, vC
    unsigned crfd() const { return (word >> 23) & 7; }
    unsigned crfs() const { return (word >> 18) & 7; }
    bool l() const { return word & 0x00200000; }
    unsigned mb() const { return (word >> 6) & 31; }
    unsigned me() const { return (word >> 1) & 31; }
    unsigned sh6() const { return ((word >> 11) & 31) | ((word & 2) << 4); }
    unsigned mb6() const { return ((word >> 6) & 31) | (word & 0x20); }  // also the 64-bit ME
    int32_t simm() const { return static_cast<int16_t>(word & 0xFFFF); }
    uint32_t uimm() const { return word & 0xFFFF; }
    int32_t ds() const { return static_cast<int16_t>(word & 0xFFFC); }
    unsigned spr() const { return ((word >> 16) & 31) | (((word >> 11) & 31) << 5); }
    unsigned crm() const { return (word >> 12) & 0xFF; }
    unsigned fm() const { return (word >> 17) & 0xFF; }
    unsigned bh() const { return (word >> 11) & 3; }
    unsigned ls() const { return (word >> 21) & 3; }   // also STRM
    int32_t vsimm() const { return static_cast<int32_t>(word << 11) >> 27; }
    unsigned shb() const { return (word >> 6) & 15; }
    bool rc() const { return word & 1; }   // also LK
    bool oe() const { return word & 0x400; }  // also a VMX compare's Rc
    bool aa() const { return word & 2; }
    bool lk() const { return word & 1; }

    // Target of b / bc at address pc. An LV2 process's addresses are 32 bits wide.
    uint32_t branch_target(uint32_t pc) const;
};

// The spec of every op (index by Op), and all of them in table order.
const Spec& spec(Op op);
std::span<const Spec> specs();

Insn decode(uint32_t word);

// Mnemonic with its o / . / l / a suffixes, e.g. "addo.", "bcl", "bla", "vcmpequb.".
std::string mnemonic(const Insn& insn);
// Text of an instruction at address pc, objdump `-M cell,raw` style:
// "addi r3,r1,8", "bc 12,eq,0x10204", "ld r2,40(r1)", "vor v1,v2,v2"; an
// invalid word is ".long 0x...".
std::string disasm(const Insn& insn, uint32_t pc);

}  // namespace qilin::ppu
