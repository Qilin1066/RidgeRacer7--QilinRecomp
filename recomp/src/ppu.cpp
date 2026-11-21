#include "ppu.hpp"

#include <array>
#include <format>
#include <stdexcept>

namespace qilin::ppu {

namespace {

constexpr std::string_view kNames[] = {
    "(invalid)",
#define QILIN_PPU_NAME(id, name) name,
    QILIN_PPU_OPS(QILIN_PPU_NAME)
#undef QILIN_PPU_NAME
};
static_assert(std::size(kNames) == static_cast<size_t>(Op::count_));

constexpr uint32_t kPrimary = 0xFC000000u;

struct Match {
    uint32_t match, mask;
};
// Primary opcode only (D, I, B, M forms).
constexpr Match P(unsigned p) { return {p << 26, kPrimary}; }
// 10-bit extended opcode in bits 21-30 (X, XL, XFX, XFL forms).
constexpr Match X(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x3FFu << 1)}; }
// 9-bit extended opcode in bits 22-30; bit 21 is OE (XO form).
constexpr Match XO(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x1FFu << 1)}; }
// 9-bit extended opcode in bits 21-29; bit 30 is the shift amount's top bit (XS form: sradi).
constexpr Match XS(unsigned p, unsigned xo) { return {(p << 26) | (xo << 2), kPrimary | (0x1FFu << 2)}; }
// 5-bit extended opcode in bits 26-30 (A form).
constexpr Match A(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x1Fu << 1)}; }
// 2-bit extended opcode in bits 30-31 (DS form: ld, std, lwa).
constexpr Match DS(unsigned p, unsigned xo) { return {(p << 26) | xo, kPrimary | 3u}; }
// Opcode 30's 64-bit rotates: 3 bits in 27-29 (MD form), or 4 in 27-30 (MDS form).
constexpr Match MD(unsigned xo) { return {(30u << 26) | (xo << 2), kPrimary | (7u << 2)}; }
constexpr Match MDS(unsigned xo) { return {(30u << 26) | (xo << 1), kPrimary | (15u << 1)}; }
// Opcode 4, VMX: 11 bits in 21-31 (VX form), 10 in 22-31 with Rc in bit 21
// (VXR form: the compares), or 6 in 26-31 (VA form).
constexpr Match VX(unsigned xo) { return {(4u << 26) | xo, kPrimary | 0x7FFu}; }
constexpr Match VXR(unsigned xo) { return {(4u << 26) | xo, kPrimary | 0x3FFu}; }
constexpr Match VA(unsigned xo) { return {(4u << 26) | xo, kPrimary | 0x3Fu}; }
// The same with fixed bits added: set, or clear.
constexpr Match with(Match m, uint32_t bits) { return {m.match | bits, m.mask | bits}; }
constexpr Match without(Match m, uint32_t bits) { return {m.match, m.mask | bits}; }

using enum Opnd;

struct Row {
    Op op;
    Match m;
    std::array<Opnd, 5> ops;
    uint32_t flags;
};

constexpr uint32_t kRcOe = kRc | kOe;
constexpr uint32_t kLd = kLoad;
constexpr uint32_t kSt = kStore;
constexpr uint32_t kFL = kFloat | kLoad;
constexpr uint32_t kFS = kFloat | kStore;
constexpr uint32_t kF = kFloat;
constexpr uint32_t kFRc = kFloat | kRc;
constexpr uint32_t kV = kVector;
constexpr uint32_t kVL = kVector | kLoad;
constexpr uint32_t kVS = kVector | kStore;
constexpr uint32_t kVC = kVector | kVrc;
constexpr uint32_t kHint = kVector | kCache;
constexpr uint32_t kFxm = 0x00100000u;  // bit 11: mfocrf/mtocrf rather than mfcr/mtcrf
constexpr uint32_t kAll = 0x02000000u;  // bit 6: dstt, dststt, dssall

// In decode-table order: within an extended primary opcode, the forms with
// the widest extended opcode come first. No two rows share an encoding
// (checked when the tables are built).
constexpr Row kRows[] = {
    // ---- primary opcodes ----
    {Op::tdi, P(2), {to, rA, simm}, k64},
    {Op::twi, P(3), {to, rA, simm}, 0},
    {Op::mulli, P(7), {rD, rA, simm}, 0},
    {Op::subfic, P(8), {rD, rA, simm}, 0},
    {Op::cmpli, P(10), {crfD, l, rA, uimm}, 0},
    {Op::cmpi, P(11), {crfD, l, rA, simm}, 0},
    {Op::addic, P(12), {rD, rA, simm}, 0},
    {Op::addic_rc, P(13), {rD, rA, simm}, 0},
    {Op::addi, P(14), {rD, rA0, simm}, 0},
    {Op::addis, P(15), {rD, rA0, simm}, 0},
    {Op::bc, P(16), {bo, bi, bd}, kBranch | kLk | kAa | kValidBo},
    {Op::sc, with(P(17), 2), {zero}, 0},
    {Op::b, P(18), {li}, kBranch | kLk | kAa},
    {Op::rlwimi, P(20), {rA, rD, sh, mb, me}, kRc},
    {Op::rlwinm, P(21), {rA, rD, sh, mb, me}, kRc},
    {Op::rlwnm, P(23), {rA, rD, rB, mb, me}, kRc},
    {Op::ori, P(24), {rA, rD, uimm}, 0},
    {Op::oris, P(25), {rA, rD, uimm}, 0},
    {Op::xori, P(26), {rA, rD, uimm}, 0},
    {Op::xoris, P(27), {rA, rD, uimm}, 0},
    {Op::andi_rc, P(28), {rA, rD, uimm}, 0},
    {Op::andis_rc, P(29), {rA, rD, uimm}, 0},
    {Op::lwz, P(32), {rD, d_rA0}, kLd},
    {Op::lwzu, P(33), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::lbz, P(34), {rD, d_rA0}, kLd},
    {Op::lbzu, P(35), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::stw, P(36), {rD, d_rA0}, kSt},
    {Op::stwu, P(37), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::stb, P(38), {rD, d_rA0}, kSt},
    {Op::stbu, P(39), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::lhz, P(40), {rD, d_rA0}, kLd},
    {Op::lhzu, P(41), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::lha, P(42), {rD, d_rA0}, kLd},
    {Op::lhau, P(43), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::sth, P(44), {rD, d_rA0}, kSt},
    {Op::sthu, P(45), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::lmw, P(46), {rD, d_rA0}, kLd | kLmw},
    {Op::stmw, P(47), {rD, d_rA0}, kSt},
    {Op::lfs, P(48), {fD, d_rA0}, kFL},
    {Op::lfsu, P(49), {fD, d_rA0}, kFL | kRaNonZero},
    {Op::lfd, P(50), {fD, d_rA0}, kFL},
    {Op::lfdu, P(51), {fD, d_rA0}, kFL | kRaNonZero},
    {Op::stfs, P(52), {fD, d_rA0}, kFS},
    {Op::stfsu, P(53), {fD, d_rA0}, kFS | kRaNonZero},
    {Op::stfd, P(54), {fD, d_rA0}, kFS},
    {Op::stfdu, P(55), {fD, d_rA0}, kFS | kRaNonZero},

    // ---- 58, 62: doubleword loads and stores ----
    {Op::ld, DS(58, 0), {rD, ds_rA0}, kLd | k64},
    {Op::ldu, DS(58, 1), {rD, ds_rA0}, kLd | k64 | kRaNotZeroOrRd},
    {Op::lwa, DS(58, 2), {rD, ds_rA0}, kLd | k64},
    {Op::std, DS(62, 0), {rD, ds_rA0}, kSt | k64},
    {Op::stdu, DS(62, 1), {rD, ds_rA0}, kSt | k64 | kRaNonZero},

    // ---- 30: 64-bit rotates ----
    {Op::rldicl, MD(0), {rA, rD, sh6, mb6}, kRc | k64},
    {Op::rldicr, MD(1), {rA, rD, sh6, mb6}, kRc | k64},
    {Op::rldic, MD(2), {rA, rD, sh6, mb6}, kRc | k64},
    {Op::rldimi, MD(3), {rA, rD, sh6, mb6}, kRc | k64},
    {Op::rldcl, MDS(8), {rA, rD, rB, mb6}, kRc | k64},
    {Op::rldcr, MDS(9), {rA, rD, rB, mb6}, kRc | k64},

    // ---- 4: VMX ----
    {Op::vaddubm, VX(0), {vD, vA, vB}, kV},
    {Op::vmaxub, VX(2), {vD, vA, vB}, kV},
    {Op::vrlb, VX(4), {vD, vA, vB}, kV},
    {Op::vmuloub, VX(8), {vD, vA, vB}, kV},
    {Op::vaddfp, VX(10), {vD, vA, vB}, kV},
    {Op::vmrghb, VX(12), {vD, vA, vB}, kV},
    {Op::vpkuhum, VX(14), {vD, vA, vB}, kV},
    {Op::vadduhm, VX(64), {vD, vA, vB}, kV},
    {Op::vmaxuh, VX(66), {vD, vA, vB}, kV},
    {Op::vrlh, VX(68), {vD, vA, vB}, kV},
    {Op::vmulouh, VX(72), {vD, vA, vB}, kV},
    {Op::vsubfp, VX(74), {vD, vA, vB}, kV},
    {Op::vmrghh, VX(76), {vD, vA, vB}, kV},
    {Op::vpkuwum, VX(78), {vD, vA, vB}, kV},
    {Op::vadduwm, VX(128), {vD, vA, vB}, kV},
    {Op::vmaxuw, VX(130), {vD, vA, vB}, kV},
    {Op::vrlw, VX(132), {vD, vA, vB}, kV},
    {Op::vmrghw, VX(140), {vD, vA, vB}, kV},
    {Op::vpkuhus, VX(142), {vD, vA, vB}, kV},
    {Op::vpkuwus, VX(206), {vD, vA, vB}, kV},
    {Op::vmaxsb, VX(258), {vD, vA, vB}, kV},
    {Op::vslb, VX(260), {vD, vA, vB}, kV},
    {Op::vmulosb, VX(264), {vD, vA, vB}, kV},
    {Op::vrefp, VX(266), {vD, vB}, kV},
    {Op::vmrglb, VX(268), {vD, vA, vB}, kV},
    {Op::vpkshus, VX(270), {vD, vA, vB}, kV},
    {Op::vmaxsh, VX(322), {vD, vA, vB}, kV},
    {Op::vslh, VX(324), {vD, vA, vB}, kV},
    {Op::vmulosh, VX(328), {vD, vA, vB}, kV},
    {Op::vrsqrtefp, VX(330), {vD, vB}, kV},
    {Op::vmrglh, VX(332), {vD, vA, vB}, kV},
    {Op::vpkswus, VX(334), {vD, vA, vB}, kV},
    {Op::vaddcuw, VX(384), {vD, vA, vB}, kV},
    {Op::vmaxsw, VX(386), {vD, vA, vB}, kV},
    {Op::vslw, VX(388), {vD, vA, vB}, kV},
    {Op::vexptefp, VX(394), {vD, vB}, kV},
    {Op::vmrglw, VX(396), {vD, vA, vB}, kV},
    {Op::vpkshss, VX(398), {vD, vA, vB}, kV},
    {Op::vsl, VX(452), {vD, vA, vB}, kV},
    {Op::vlogefp, VX(458), {vD, vB}, kV},
    {Op::vpkswss, VX(462), {vD, vA, vB}, kV},
    {Op::vaddubs, VX(512), {vD, vA, vB}, kV},
    {Op::vminub, VX(514), {vD, vA, vB}, kV},
    {Op::vsrb, VX(516), {vD, vA, vB}, kV},
    {Op::vmuleub, VX(520), {vD, vA, vB}, kV},
    {Op::vrfin, VX(522), {vD, vB}, kV},
    {Op::vspltb, VX(524), {vD, vB, vuimm4}, kV},
    {Op::vupkhsb, VX(526), {vD, vB}, kV},
    {Op::vadduhs, VX(576), {vD, vA, vB}, kV},
    {Op::vminuh, VX(578), {vD, vA, vB}, kV},
    {Op::vsrh, VX(580), {vD, vA, vB}, kV},
    {Op::vmuleuh, VX(584), {vD, vA, vB}, kV},
    {Op::vrfiz, VX(586), {vD, vB}, kV},
    {Op::vsplth, VX(588), {vD, vB, vuimm3}, kV},
    {Op::vupkhsh, VX(590), {vD, vB}, kV},
    {Op::vadduws, VX(640), {vD, vA, vB}, kV},
    {Op::vminuw, VX(642), {vD, vA, vB}, kV},
    {Op::vsrw, VX(644), {vD, vA, vB}, kV},
    {Op::vrfip, VX(650), {vD, vB}, kV},
    {Op::vspltw, VX(652), {vD, vB, vuimm2}, kV},
    {Op::vupklsb, VX(654), {vD, vB}, kV},
    {Op::vsr, VX(708), {vD, vA, vB}, kV},
    {Op::vrfim, VX(714), {vD, vB}, kV},
    {Op::vupklsh, VX(718), {vD, vB}, kV},
    {Op::vaddsbs, VX(768), {vD, vA, vB}, kV},
    {Op::vminsb, VX(770), {vD, vA, vB}, kV},
    {Op::vsrab, VX(772), {vD, vA, vB}, kV},
    {Op::vmulesb, VX(776), {vD, vA, vB}, kV},
    {Op::vcfux, VX(778), {vD, vB, vuimm}, kV},
    {Op::vspltisb, VX(780), {vD, vsimm}, kV},
    {Op::vpkpx, VX(782), {vD, vA, vB}, kV},
    {Op::vaddshs, VX(832), {vD, vA, vB}, kV},
    {Op::vminsh, VX(834), {vD, vA, vB}, kV},
    {Op::vsrah, VX(836), {vD, vA, vB}, kV},
    {Op::vmulesh, VX(840), {vD, vA, vB}, kV},
    {Op::vcfsx, VX(842), {vD, vB, vuimm}, kV},
    {Op::vspltish, VX(844), {vD, vsimm}, kV},
    {Op::vupkhpx, VX(846), {vD, vB}, kV},
    {Op::vaddsws, VX(896), {vD, vA, vB}, kV},
    {Op::vminsw, VX(898), {vD, vA, vB}, kV},
    {Op::vsraw, VX(900), {vD, vA, vB}, kV},
    {Op::vctuxs, VX(906), {vD, vB, vuimm}, kV},
    {Op::vspltisw, VX(908), {vD, vsimm}, kV},
    {Op::vctsxs, VX(970), {vD, vB, vuimm}, kV},
    {Op::vupklpx, VX(974), {vD, vB}, kV},
    {Op::vsububm, VX(1024), {vD, vA, vB}, kV},
    {Op::vavgub, VX(1026), {vD, vA, vB}, kV},
    {Op::vand, VX(1028), {vD, vA, vB}, kV},
    {Op::vmaxfp, VX(1034), {vD, vA, vB}, kV},
    {Op::vslo, VX(1036), {vD, vA, vB}, kV},
    {Op::vsubuhm, VX(1088), {vD, vA, vB}, kV},
    {Op::vavguh, VX(1090), {vD, vA, vB}, kV},
    {Op::vandc, VX(1092), {vD, vA, vB}, kV},
    {Op::vminfp, VX(1098), {vD, vA, vB}, kV},
    {Op::vsro, VX(1100), {vD, vA, vB}, kV},
    {Op::vsubuwm, VX(1152), {vD, vA, vB}, kV},
    {Op::vavguw, VX(1154), {vD, vA, vB}, kV},
    {Op::vor, VX(1156), {vD, vA, vB}, kV},
    {Op::vxor, VX(1220), {vD, vA, vB}, kV},
    {Op::vavgsb, VX(1282), {vD, vA, vB}, kV},
    {Op::vnor, VX(1284), {vD, vA, vB}, kV},
    {Op::vavgsh, VX(1346), {vD, vA, vB}, kV},
    {Op::vsubcuw, VX(1408), {vD, vA, vB}, kV},
    {Op::vavgsw, VX(1410), {vD, vA, vB}, kV},
    {Op::vsububs, VX(1536), {vD, vA, vB}, kV},
    {Op::mfvscr, VX(1540), {vD}, kV},
    {Op::vsum4ubs, VX(1544), {vD, vA, vB}, kV},
    {Op::vsubuhs, VX(1600), {vD, vA, vB}, kV},
    {Op::mtvscr, VX(1604), {vB}, kV},
    {Op::vsum4shs, VX(1608), {vD, vA, vB}, kV},
    {Op::vsubuws, VX(1664), {vD, vA, vB}, kV},
    {Op::vsum2sws, VX(1672), {vD, vA, vB}, kV},
    {Op::vsubsbs, VX(1792), {vD, vA, vB}, kV},
    {Op::vsum4sbs, VX(1800), {vD, vA, vB}, kV},
    {Op::vsubshs, VX(1856), {vD, vA, vB}, kV},
    {Op::vsubsws, VX(1920), {vD, vA, vB}, kV},
    {Op::vsumsws, VX(1928), {vD, vA, vB}, kV},
    // The compares: bit 21 is their record bit.
    {Op::vcmpequb, VXR(6), {vD, vA, vB}, kVC},
    {Op::vcmpequh, VXR(70), {vD, vA, vB}, kVC},
    {Op::vcmpequw, VXR(134), {vD, vA, vB}, kVC},
    {Op::vcmpeqfp, VXR(198), {vD, vA, vB}, kVC},
    {Op::vcmpgefp, VXR(454), {vD, vA, vB}, kVC},
    {Op::vcmpgtub, VXR(518), {vD, vA, vB}, kVC},
    {Op::vcmpgtuh, VXR(582), {vD, vA, vB}, kVC},
    {Op::vcmpgtuw, VXR(646), {vD, vA, vB}, kVC},
    {Op::vcmpgtfp, VXR(710), {vD, vA, vB}, kVC},
    {Op::vcmpgtsb, VXR(774), {vD, vA, vB}, kVC},
    {Op::vcmpgtsh, VXR(838), {vD, vA, vB}, kVC},
    {Op::vcmpgtsw, VXR(902), {vD, vA, vB}, kVC},
    {Op::vcmpbfp, VXR(966), {vD, vA, vB}, kVC},
    // Four operands.
    {Op::vmhaddshs, VA(32), {vD, vA, vB, vC}, kV},
    {Op::vmhraddshs, VA(33), {vD, vA, vB, vC}, kV},
    {Op::vmladduhm, VA(34), {vD, vA, vB, vC}, kV},
    {Op::vmsumubm, VA(36), {vD, vA, vB, vC}, kV},
    {Op::vmsummbm, VA(37), {vD, vA, vB, vC}, kV},
    {Op::vmsumuhm, VA(38), {vD, vA, vB, vC}, kV},
    {Op::vmsumuhs, VA(39), {vD, vA, vB, vC}, kV},
    {Op::vmsumshm, VA(40), {vD, vA, vB, vC}, kV},
    {Op::vmsumshs, VA(41), {vD, vA, vB, vC}, kV},
    {Op::vsel, VA(42), {vD, vA, vB, vC}, kV},
    {Op::vperm, VA(43), {vD, vA, vB, vC}, kV},
    {Op::vsldoi, VA(44), {vD, vA, vB, shb}, kV},
    {Op::vmaddfp, VA(46), {vD, vA, vC, vB}, kV},
    {Op::vnmsubfp, VA(47), {vD, vA, vC, vB}, kV},

    // ---- 19: branch to LR/CTR, CR logical, isync ----
    {Op::mcrf, X(19, 0), {crfD, crfS}, 0},
    {Op::bclr, X(19, 16), {bo, bi, bh}, kBranch | kLk | kValidBo},
    {Op::crnor, X(19, 33), {crbD, crbA, crbB}, 0},
    {Op::crandc, X(19, 129), {crbD, crbA, crbB}, 0},
    {Op::isync, X(19, 150), {}, kSync},
    {Op::crxor, X(19, 193), {crbD, crbA, crbB}, 0},
    {Op::crnand, X(19, 225), {crbD, crbA, crbB}, 0},
    {Op::crand, X(19, 257), {crbD, crbA, crbB}, 0},
    {Op::creqv, X(19, 289), {crbD, crbA, crbB}, 0},
    {Op::crorc, X(19, 417), {crbD, crbA, crbB}, 0},
    {Op::cror, X(19, 449), {crbD, crbA, crbB}, 0},
    {Op::bcctr, X(19, 528), {bo, bi, bh}, kBranch | kLk | kValidBo | kBoNoDecrement},

    // ---- 31: integer register forms, indexed loads and stores, system ----
    {Op::cmp, X(31, 0), {crfD, l, rA, rB}, 0},
    {Op::tw, X(31, 4), {to, rA, rB}, 0},
    {Op::lvsl, X(31, 6), {vD, rA0, rB}, kV},
    {Op::lvebx, X(31, 7), {vD, rA0, rB}, kVL},
    {Op::mfcr, without(X(31, 19), kFxm), {rD}, 0},
    {Op::mfocrf, with(X(31, 19), kFxm), {rD, crm}, kOneCrField},
    {Op::lwarx, X(31, 20), {rD, rA0, rB, zero}, kLd},
    {Op::ldx, X(31, 21), {rD, rA0, rB}, kLd | k64},
    {Op::lwzx, X(31, 23), {rD, rA0, rB}, kLd},
    {Op::slw, X(31, 24), {rA, rD, rB}, kRc},
    {Op::cntlzw, X(31, 26), {rA, rD}, kRc},
    {Op::sld, X(31, 27), {rA, rD, rB}, kRc | k64},
    {Op::and_, X(31, 28), {rA, rD, rB}, kRc},
    {Op::cmpl, X(31, 32), {crfD, l, rA, rB}, 0},
    {Op::lvsr, X(31, 38), {vD, rA0, rB}, kV},
    {Op::lvehx, X(31, 39), {vD, rA0, rB}, kVL},
    {Op::ldux, X(31, 53), {rD, rA, rB}, kLd | k64 | kRaNotZeroOrRd},
    {Op::dcbst, X(31, 54), {rA0, rB}, kCache},
    {Op::lwzux, X(31, 55), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::cntlzd, X(31, 58), {rA, rD}, kRc | k64},
    {Op::andc, X(31, 60), {rA, rD, rB}, kRc},
    {Op::td, X(31, 68), {to, rA, rB}, k64},
    {Op::lvewx, X(31, 71), {vD, rA0, rB}, kVL},
    {Op::ldarx, X(31, 84), {rD, rA0, rB, zero}, kLd | k64},
    {Op::dcbf, X(31, 86), {rA0, rB, zero}, kCache},
    {Op::lbzx, X(31, 87), {rD, rA0, rB}, kLd},
    {Op::lvx, X(31, 103), {vD, rA0, rB}, kVL},
    {Op::lbzux, X(31, 119), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::nor, X(31, 124), {rA, rD, rB}, kRc},
    {Op::stvebx, X(31, 135), {vD, rA0, rB}, kVS},
    {Op::mtcrf, without(X(31, 144), kFxm), {crm, rD}, 0},
    {Op::mtocrf, with(X(31, 144), kFxm), {crm, rD}, kOneCrField},
    {Op::stdx, X(31, 149), {rD, rA0, rB}, kSt | k64},
    {Op::stwcx_rc, with(X(31, 150), 1), {rD, rA0, rB}, kSt},
    {Op::stwx, X(31, 151), {rD, rA0, rB}, kSt},
    {Op::stvehx, X(31, 167), {vD, rA0, rB}, kVS},
    {Op::stdux, X(31, 181), {rD, rA, rB}, kSt | k64 | kRaNonZero},
    {Op::stwux, X(31, 183), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::stvewx, X(31, 199), {vD, rA0, rB}, kVS},
    {Op::stdcx_rc, with(X(31, 214), 1), {rD, rA0, rB}, kSt | k64},
    {Op::stbx, X(31, 215), {rD, rA0, rB}, kSt},
    {Op::stvx, X(31, 231), {vD, rA0, rB}, kVS},
    {Op::dcbtst, X(31, 246), {rA0, rB, th}, kCache},
    {Op::stbux, X(31, 247), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::dcbt, X(31, 278), {rA0, rB, th}, kCache},
    {Op::lhzx, X(31, 279), {rD, rA0, rB}, kLd},
    {Op::eqv, X(31, 284), {rA, rD, rB}, kRc},
    {Op::lhzux, X(31, 311), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::xor_, X(31, 316), {rA, rD, rB}, kRc},
    {Op::mfspr, X(31, 339), {rD, spr}, 0},
    {Op::lwax, X(31, 341), {rD, rA0, rB}, kLd | k64},
    {Op::dst, without(X(31, 342), kAll), {rA, rB, strm}, kHint},
    {Op::dstt, with(X(31, 342), kAll), {rA, rB, strm}, kHint},
    {Op::lhax, X(31, 343), {rD, rA0, rB}, kLd},
    {Op::lvxl, X(31, 359), {vD, rA0, rB}, kVL},
    {Op::mftb, X(31, 371), {rD, tbr}, 0},
    {Op::lwaux, X(31, 373), {rD, rA, rB}, kLd | k64 | kRaNotZeroOrRd},
    {Op::dstst, without(X(31, 374), kAll), {rA, rB, strm}, kHint},
    {Op::dststt, with(X(31, 374), kAll), {rA, rB, strm}, kHint},
    {Op::lhaux, X(31, 375), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::sthx, X(31, 407), {rD, rA0, rB}, kSt},
    {Op::orc, X(31, 412), {rA, rD, rB}, kRc},
    {Op::sradi, XS(31, 413), {rA, rD, sh6}, kRc | k64},
    {Op::sthux, X(31, 439), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::or_, X(31, 444), {rA, rD, rB}, kRc},
    {Op::mtspr, X(31, 467), {spr, rD}, 0},
    {Op::nand, X(31, 476), {rA, rD, rB}, kRc},
    {Op::stvxl, X(31, 487), {vD, rA0, rB}, kVS},
    {Op::mcrxr, X(31, 512), {crfD}, 0},
    {Op::lvlx, X(31, 519), {vD, rA0, rB}, kVL},
    {Op::ldbrx, X(31, 532), {rD, rA0, rB}, kLd | k64},
    {Op::lswx, X(31, 533), {rD, rA0, rB}, kLd | kLswx},
    {Op::lwbrx, X(31, 534), {rD, rA0, rB}, kLd},
    {Op::lfsx, X(31, 535), {fD, rA0, rB}, kFL},
    {Op::srw, X(31, 536), {rA, rD, rB}, kRc},
    {Op::srd, X(31, 539), {rA, rD, rB}, kRc | k64},
    {Op::lvrx, X(31, 551), {vD, rA0, rB}, kVL},
    {Op::lfsux, X(31, 567), {fD, rA, rB}, kFL | kRaNonZero},
    {Op::lswi, X(31, 597), {rD, rA0, nb}, kLd | kLswi},
    {Op::sync, X(31, 598), {ls}, kSync | kSyncKind},
    {Op::lfdx, X(31, 599), {fD, rA0, rB}, kFL},
    {Op::lfdux, X(31, 631), {fD, rA, rB}, kFL | kRaNonZero},
    {Op::stvlx, X(31, 647), {vD, rA0, rB}, kVS},
    {Op::stdbrx, X(31, 660), {rD, rA0, rB}, kSt | k64},
    {Op::stswx, X(31, 661), {rD, rA0, rB}, kSt},
    {Op::stwbrx, X(31, 662), {rD, rA0, rB}, kSt},
    {Op::stfsx, X(31, 663), {fD, rA0, rB}, kFS},
    {Op::stvrx, X(31, 679), {vD, rA0, rB}, kVS},
    {Op::stfsux, X(31, 695), {fD, rA, rB}, kFS | kRaNonZero},
    {Op::stswi, X(31, 725), {rD, rA0, nb}, kSt},
    {Op::stfdx, X(31, 727), {fD, rA0, rB}, kFS},
    {Op::stfdux, X(31, 759), {fD, rA, rB}, kFS | kRaNonZero},
    {Op::lvlxl, X(31, 775), {vD, rA0, rB}, kVL},
    {Op::lhbrx, X(31, 790), {rD, rA0, rB}, kLd},
    {Op::sraw, X(31, 792), {rA, rD, rB}, kRc},
    {Op::srad, X(31, 794), {rA, rD, rB}, kRc | k64},
    {Op::lvrxl, X(31, 807), {vD, rA0, rB}, kVL},
    {Op::dss, without(X(31, 822), kAll), {strm}, kHint},
    {Op::dssall, with(X(31, 822), kAll), {}, kHint},
    {Op::srawi, X(31, 824), {rA, rD, sh}, kRc},
    {Op::eieio, X(31, 854), {}, kSync},
    {Op::stvlxl, X(31, 903), {vD, rA0, rB}, kVS},
    {Op::sthbrx, X(31, 918), {rD, rA0, rB}, kSt},
    {Op::extsh, X(31, 922), {rA, rD}, kRc},
    {Op::stvrxl, X(31, 935), {vD, rA0, rB}, kVS},
    {Op::extsb, X(31, 954), {rA, rD}, kRc},
    {Op::icbi, X(31, 982), {rA0, rB}, kCache},
    {Op::stfiwx, X(31, 983), {fD, rA0, rB}, kFS},
    {Op::extsw, X(31, 986), {rA, rD}, kRc | k64},
    {Op::dcbz, X(31, 1014), {rA0, rB}, kCache | kStore},
    // XO form: bit 21 is OE where the instruction has it, reserved otherwise.
    {Op::subfc, XO(31, 8), {rD, rA, rB}, kRcOe},
    {Op::mulhdu, XO(31, 9), {rD, rA, rB}, kRc | k64},
    {Op::addc, XO(31, 10), {rD, rA, rB}, kRcOe},
    {Op::mulhwu, XO(31, 11), {rD, rA, rB}, kRc},
    {Op::subf, XO(31, 40), {rD, rA, rB}, kRcOe},
    {Op::mulhd, XO(31, 73), {rD, rA, rB}, kRc | k64},
    {Op::mulhw, XO(31, 75), {rD, rA, rB}, kRc},
    {Op::neg, XO(31, 104), {rD, rA}, kRcOe},
    {Op::subfe, XO(31, 136), {rD, rA, rB}, kRcOe},
    {Op::adde, XO(31, 138), {rD, rA, rB}, kRcOe},
    {Op::subfze, XO(31, 200), {rD, rA}, kRcOe},
    {Op::addze, XO(31, 202), {rD, rA}, kRcOe},
    {Op::subfme, XO(31, 232), {rD, rA}, kRcOe},
    {Op::mulld, XO(31, 233), {rD, rA, rB}, kRcOe | k64},
    {Op::addme, XO(31, 234), {rD, rA}, kRcOe},
    {Op::mullw, XO(31, 235), {rD, rA, rB}, kRcOe},
    {Op::add, XO(31, 266), {rD, rA, rB}, kRcOe},
    {Op::divdu, XO(31, 457), {rD, rA, rB}, kRcOe | k64},
    {Op::divwu, XO(31, 459), {rD, rA, rB}, kRcOe},
    {Op::divd, XO(31, 489), {rD, rA, rB}, kRcOe | k64},
    {Op::divw, XO(31, 491), {rD, rA, rB}, kRcOe},

    // ---- 59: single-precision arithmetic ----
    {Op::fdivs, A(59, 18), {fD, fA, fB}, kFRc},
    {Op::fsubs, A(59, 20), {fD, fA, fB}, kFRc},
    {Op::fadds, A(59, 21), {fD, fA, fB}, kFRc},
    {Op::fsqrts, A(59, 22), {fD, fB}, kFRc},
    {Op::fres, A(59, 24), {fD, fB, zero}, kFRc},
    {Op::fmuls, A(59, 25), {fD, fA, fC}, kFRc},
    {Op::fmsubs, A(59, 28), {fD, fA, fC, fB}, kFRc},
    {Op::fmadds, A(59, 29), {fD, fA, fC, fB}, kFRc},
    {Op::fnmsubs, A(59, 30), {fD, fA, fC, fB}, kFRc},
    {Op::fnmadds, A(59, 31), {fD, fA, fC, fB}, kFRc},

    // ---- 63: double-precision arithmetic, moves, compares, conversions, FPSCR ----
    {Op::fcmpu, X(63, 0), {crfD, fA, fB}, kF},
    {Op::frsp, X(63, 12), {fD, fB}, kFRc},
    {Op::fctiw, X(63, 14), {fD, fB}, kFRc},
    {Op::fctiwz, X(63, 15), {fD, fB}, kFRc},
    {Op::fcmpo, X(63, 32), {crfD, fA, fB}, kF},
    {Op::mtfsb1, X(63, 38), {fpscr_bit}, kFRc},
    {Op::fneg, X(63, 40), {fD, fB}, kFRc},
    {Op::mcrfs, X(63, 64), {crfD, crfS}, kF},
    {Op::mtfsb0, X(63, 70), {fpscr_bit}, kFRc},
    {Op::fmr, X(63, 72), {fD, fB}, kFRc},
    {Op::mtfsfi, X(63, 134), {bf, imm}, kFRc},
    {Op::fnabs, X(63, 136), {fD, fB}, kFRc},
    {Op::fabs, X(63, 264), {fD, fB}, kFRc},
    {Op::mffs, X(63, 583), {fD}, kFRc},
    {Op::mtfsf, X(63, 711), {fm, fB}, kFRc},
    {Op::fctid, X(63, 814), {fD, fB}, kFRc | k64},
    {Op::fctidz, X(63, 815), {fD, fB}, kFRc | k64},
    {Op::fcfid, X(63, 846), {fD, fB}, kFRc | k64},
    {Op::fdiv, A(63, 18), {fD, fA, fB}, kFRc},
    {Op::fsub, A(63, 20), {fD, fA, fB}, kFRc},
    {Op::fadd, A(63, 21), {fD, fA, fB}, kFRc},
    {Op::fsqrt, A(63, 22), {fD, fB}, kFRc},
    {Op::fsel, A(63, 23), {fD, fA, fC, fB}, kFRc},
    {Op::fmul, A(63, 25), {fD, fA, fC}, kFRc},
    {Op::frsqrte, A(63, 26), {fD, fB, zero}, kFRc},
    {Op::fmsub, A(63, 28), {fD, fA, fC, fB}, kFRc},
    {Op::fmadd, A(63, 29), {fD, fA, fC, fB}, kFRc},
    {Op::fnmsub, A(63, 30), {fD, fA, fC, fB}, kFRc},
    {Op::fnmadd, A(63, 31), {fD, fA, fC, fB}, kFRc},
};
constexpr size_t kRowCount = std::size(kRows);
static_assert(kRowCount == static_cast<size_t>(Op::count_) - 1, "one row per op");

// Primary opcodes whose low bits choose the instruction.
constexpr bool is_extended(unsigned p) {
    return p == 4 || p == 19 || p == 30 || p == 31 || p == 58 || p == 59 || p == 62 || p == 63;
}
constexpr size_t extended_index(unsigned p) {
    switch (p) {
    case 4: return 0;
    case 19: return 1;
    case 30: return 2;
    case 31: return 3;
    case 58: return 4;
    case 59: return 5;
    case 62: return 6;
    default: return 7;
    }
}
constexpr uint32_t kLow = 0x7FFu;  // the bits an extended opcode can occupy (21-31)

struct Tables {
    std::array<Spec, static_cast<size_t>(Op::count_)> by_op{};  // indexed by Op
    std::array<Spec, kRowCount> in_order{};
    std::array<uint32_t, static_cast<size_t>(Op::count_)> defined{};  // bits each op defines
    std::array<uint16_t, 64> primary{};                 // Op for primary-only opcodes
    std::array<std::array<uint16_t, 2048>, 8> ext{};    // first Op by bits 21-31, extended opcodes
    // Another op in the same slot, told apart by a bit above bit 21 (mfcr and
    // mfocrf, dst and dstt); 0 ends the chain.
    std::array<uint16_t, static_cast<size_t>(Op::count_)> next{};
};

Tables build() {
    Tables t;
    t.by_op[0] = Spec{Op::invalid, kNames[0], 0, 0, {}, 0};
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        const auto idx = static_cast<size_t>(r.op);
        if (t.by_op[idx].op != Op::invalid)
            throw std::logic_error(std::format("decoder: {} has two rows", kNames[idx]));
        Spec s{r.op, kNames[idx], r.m.match, r.m.mask, {}, r.flags};
        uint32_t defined = r.m.mask;
        for (size_t k = 0; k < r.ops.size(); ++k) {
            s.operands[k] = r.ops[k];
            defined |= field_mask(r.ops[k]);
        }
        if (r.flags & (kRc | kLk)) defined |= 1;
        if (r.flags & (kOe | kVrc)) defined |= 0x400;
        if (r.flags & kAa) defined |= 2;
        t.by_op[idx] = s;
        t.in_order[i] = s;
        t.defined[idx] = defined;

        const unsigned p = r.m.match >> 26;
        const auto code = static_cast<uint16_t>(r.op);
        if (!is_extended(p)) {
            if (t.primary[p])
                throw std::logic_error(std::format("decoder: opcode {} has two rows", p));
            t.primary[p] = code;
            continue;
        }
        auto& slots = t.ext[extended_index(p)];
        bool chained = false;
        for (uint32_t low = 0; low <= kLow; ++low) {
            if ((low & r.m.mask & kLow) != (r.m.match & kLow)) continue;
            if (!slots[low]) {
                slots[low] = code;
                continue;
            }
            // Shared slots: every op already there must differ in a bit both fix.
            uint16_t last = slots[low];
            for (uint16_t other = last; other; other = t.next[other]) {
                last = other;
                if (other == code) break;  // linked at an earlier slot
                const Spec& o = t.by_op[other];
                if (((o.match ^ r.m.match) & o.mask & r.m.mask) == 0)
                    throw std::logic_error(std::format("decoder: {} overlaps {}", kNames[idx], kNames[other]));
            }
            if (last != code && !chained) {
                t.next[last] = code;
                chained = true;
            }
        }
    }
    for (size_t i = 1; i < t.by_op.size(); ++i)
        if (t.by_op[i].op == Op::invalid)
            throw std::logic_error(std::format("decoder: {} has no row", kNames[i]));
    return t;
}

const Tables& tables() {
    static const Tables t = build();
    return t;
}

// The BO encodings version 2 of the architecture defines; "z" bits must be
// zero, and the hint "at" may be 00, 10 or 11 but not 01:
//   0000z 0001z 001at 0100z 0101z 011at 1a00t 1a01t 1z1zz
bool valid_bo(unsigned bo) {
    if ((bo & 0x14) == 0) return (bo & 1) == 0;
    if ((bo & 0x14) == 0x04) return (bo & 3) != 1;
    if ((bo & 0x14) == 0x10) return (bo & 9) != 1;
    return bo == 0x14;
}

bool constraints_hold(uint32_t flags, const Insn& in) {
    const unsigned ra = in.ra(), rd = in.rd();
    if ((flags & kRaNonZero) && ra == 0) return false;
    if ((flags & kRaNotZeroOrRd) && (ra == 0 || ra == rd)) return false;
    if ((flags & kLmw) && ra >= rd) return false;
    if (flags & kLswi) {
        const unsigned bytes = in.rb() ? in.rb() : 32;
        const unsigned regs = (bytes + 3) / 4;
        if (((ra - rd) & 31) < regs) return false;  // rA is among rD .. rD+regs-1 (mod 32)
    }
    if ((flags & kLswx) && (rd == ra || rd == in.rb())) return false;
    if ((flags & kValidBo) && !valid_bo(in.rd())) return false;
    if ((flags & kBoNoDecrement) && !(in.rd() & 4)) return false;
    if ((flags & kOneCrField) && (in.crm() == 0 || (in.crm() & (in.crm() - 1)) != 0)) return false;
    if ((flags & kSyncKind) && in.ls() == 3) return false;
    return true;
}

std::string cr_bit(unsigned bit) {
    static constexpr std::string_view kBits[] = {"lt", "gt", "eq", "so"};
    if (bit < 4) return std::string(kBits[bit]);
    return std::format("4*cr{}+{}", bit / 4, kBits[bit % 4]);
}

std::string gpr_or_zero(unsigned r) { return r ? std::format("r{}", r) : std::string("0"); }

}  // namespace

uint32_t field_mask(Opnd o) {
    switch (o) {
    case rD: case fD: case vD: case to: case bo: case crbD: case fpscr_bit: case th: return 0x03E00000u;
    case rA: case rA0: case fA: case vA: case bi: case crbA: case vsimm: case vuimm: return 0x001F0000u;
    case rB: case fB: case vB: case crbB: case sh: case nb: return 0x0000F800u;
    case fC: case vC: case mb: return 0x000007C0u;
    case me: return 0x0000003Eu;
    case sh6: return 0x0000F802u;
    case mb6: return 0x000007E0u;
    case crfD: case bf: return 0x03800000u;
    case crfS: return 0x001C0000u;
    case l: return 0x00200000u;
    case simm: case uimm: return 0x0000FFFFu;
    case d_rA0: return 0x001FFFFFu;
    case ds_rA0: return 0x001FFFFCu;
    case spr: case tbr: return 0x001FF800u;
    case bd: return 0x0000FFFCu;
    case li: return 0x03FFFFFCu;
    case bh: return 0x00001800u;
    case crm: return 0x000FF000u;
    case fm: return 0x01FE0000u;
    case imm: return 0x0000F000u;
    case ls: case strm: return 0x00600000u;
    case vuimm4: return 0x000F0000u;
    case vuimm3: return 0x00070000u;
    case vuimm2: return 0x00030000u;
    case shb: return 0x000003C0u;
    case none: case zero: return 0;
    }
    return 0;
}

uint32_t Insn::branch_target(uint32_t pc) const {
    const int32_t disp = op == Op::b ? static_cast<int32_t>((word & 0x03FFFFFCu) << 6) >> 6
                                     : static_cast<int16_t>(word & 0xFFFCu);
    return aa() ? static_cast<uint32_t>(disp) : pc + static_cast<uint32_t>(disp);
}

const Spec& spec(Op op) { return tables().by_op[static_cast<size_t>(op)]; }

std::span<const Spec> specs() { return tables().in_order; }

Insn decode(uint32_t word) {
    const Tables& t = tables();
    const unsigned p = word >> 26;
    uint16_t code = is_extended(p) ? t.ext[extended_index(p)][word & kLow] : t.primary[p];
    for (; code; code = t.next[code]) {
        const Spec& s = t.by_op[code];
        if ((word & s.mask) != s.match) continue;
        if ((word & ~t.defined[code]) != 0) break;
        const Insn in{word, s.op};
        if (!constraints_hold(s.flags, in)) break;
        return in;
    }
    return {word, Op::invalid};
}

std::string mnemonic(const Insn& in) {
    const Spec& s = spec(in.op);
    std::string m(s.name);
    if ((s.flags & kOe) && in.oe()) m += 'o';
    if ((s.flags & kRc) && in.rc()) m += '.';
    if ((s.flags & kVrc) && in.oe()) m += '.';
    if ((s.flags & kLk) && in.lk()) m += 'l';
    if ((s.flags & kAa) && in.aa()) m += 'a';
    return m;
}

std::string disasm(const Insn& in, uint32_t pc) {
    if (!in.valid()) return std::format(".long 0x{:x}", in.word);
    const Spec& s = spec(in.op);
    std::string text = mnemonic(in);
    bool first = true;
    for (Opnd o : s.operands) {
        if (o == none) break;
        text += first ? ' ' : ',';
        first = false;
        switch (o) {
        case rD: text += std::format("r{}", in.rd()); break;
        case rA: text += std::format("r{}", in.ra()); break;
        case rA0: text += gpr_or_zero(in.ra()); break;
        case rB: text += std::format("r{}", in.rb()); break;
        case fD: text += std::format("f{}", in.rd()); break;
        case fA: text += std::format("f{}", in.ra()); break;
        case fB: text += std::format("f{}", in.rb()); break;
        case fC: text += std::format("f{}", in.rc_reg()); break;
        case vD: text += std::format("v{}", in.rd()); break;
        case vA: text += std::format("v{}", in.ra()); break;
        case vB: text += std::format("v{}", in.rb()); break;
        case vC: text += std::format("v{}", in.rc_reg()); break;
        case crfD: text += std::format("cr{}", in.crfd()); break;
        case crfS: text += std::format("cr{}", in.crfs()); break;
        case bf: text += std::format("{}", in.crfd()); break;
        case l: text += in.l() ? '1' : '0'; break;
        case simm: text += std::format("{}", in.simm()); break;
        case uimm: text += std::format("{}", in.uimm()); break;
        case d_rA0: text += std::format("{}({})", in.simm(), gpr_or_zero(in.ra())); break;
        case ds_rA0: text += std::format("{}({})", in.ds(), gpr_or_zero(in.ra())); break;
        case sh: text += std::format("{}", in.rb()); break;
        case nb: text += std::format("{}", in.rb() ? in.rb() : 32); break;
        case mb: text += std::format("{}", in.mb()); break;
        case me: text += std::format("{}", in.me()); break;
        case sh6: text += std::format("{}", in.sh6()); break;
        case mb6: text += std::format("{}", in.mb6()); break;
        case spr: case tbr: text += std::format("{}", in.spr()); break;
        case to: case bo: case fpscr_bit: case th: text += std::format("{}", in.rd()); break;
        case bi: text += cr_bit(in.ra()); break;
        case bh: text += std::format("{}", in.bh()); break;
        case crbD: text += cr_bit(in.rd()); break;
        case crbA: text += cr_bit(in.ra()); break;
        case crbB: text += cr_bit(in.rb()); break;
        case bd: case li: text += std::format("0x{:x}", in.branch_target(pc)); break;
        case crm: text += std::format("{}", in.crm()); break;
        case fm: text += std::format("{}", in.fm()); break;
        case imm: text += std::format("{}", (in.word >> 12) & 15); break;
        case ls: case strm: text += std::format("{}", in.ls()); break;
        case vsimm: text += std::format("{}", in.vsimm()); break;
        case vuimm: text += std::format("{}", in.ra()); break;
        case vuimm4: text += std::format("{}", in.ra() & 15); break;
        case vuimm3: text += std::format("{}", in.ra() & 7); break;
        case vuimm2: text += std::format("{}", in.ra() & 3); break;
        case shb: text += std::format("{}", in.shb()); break;
        case zero: text += '0'; break;
        case none: break;
        }
    }
    return text;
}

}  // namespace qilin::ppu
