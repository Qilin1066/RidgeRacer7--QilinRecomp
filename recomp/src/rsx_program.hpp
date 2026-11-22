// The RSX's vertex and fragment programs: decoding their microcode.
//
// The RSX is of NVIDIA's NV40 family, and its programs are the NV40's. The
// field layout is from the public description of that family in the
// nouveau driver's documentation (Mesa's nv40_vertprog.h and
// nvfx_shader.h, MIT-licensed: facts about the hardware, not code), checked
// against every one of the game's 242 compiled programs and the Cg
// binaries' own tables (`check_program`). Where the game shows the RSX to
// differ from those headers, it is said where the field is decoded.
//
// A vertex program instruction is four words and can do two things at once:
// a vector operation and a scalar one, each with its own destination. A
// fragment program instruction is four words too, stored with each word's
// 16-bit halves exchanged; one that reads a constant is followed by the
// constant itself, four words more (likewise exchanged).
//
// This is the tools' part (CMake's qilin_recomp): what reads and checks the
// programs. Translating them to SPIR-V is rsx_spirv's.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace qilin::rsx {

// ---------------------------------------------------------------- vertex programs

enum class VpRegister : uint8_t { unknown = 0, temp = 1, input = 2, constant = 3 };

struct VpSource {
    VpRegister type = VpRegister::unknown;
    unsigned temp = 0;                // for a temp
    std::array<uint8_t, 4> swizzle{};  // 0 to 3: x, y, z, w
    bool negate = false, abs = false;
};

// The vector unit's operations, by number (nouveau's names; the ARB and NV vertex program specifications define what they do).
enum VpVectorOp : uint8_t {
    kVecNop = 0x00, kVecMov = 0x01, kVecMul = 0x02, kVecAdd = 0x03, kVecMad = 0x04, kVecDp3 = 0x05, kVecDph = 0x06, kVecDp4 = 0x07,
    kVecDst = 0x08, kVecMin = 0x09, kVecMax = 0x0A, kVecSlt = 0x0B, kVecSge = 0x0C, kVecArl = 0x0D, kVecFrc = 0x0E, kVecFlr = 0x0F,
    kVecSeq = 0x10, kVecSfl = 0x11, kVecSgt = 0x12, kVecSle = 0x13, kVecSne = 0x14, kVecStr = 0x15, kVecSsg = 0x16, kVecArr = 0x17,
    kVecAra = 0x18, kVecTxl = 0x19,
};
enum VpScalarOp : uint8_t {
    kScaNop = 0x00, kScaMov = 0x01, kScaRcp = 0x02, kScaRcc = 0x03, kScaRsq = 0x04, kScaExp = 0x05, kScaLog = 0x06, kScaLit = 0x07,
    kScaBra = 0x09, kScaCal = 0x0B, kScaRet = 0x0C, kScaLg2 = 0x0D, kScaEx2 = 0x0E, kScaSin = 0x0F, kScaCos = 0x10, kScaPusha = 0x13,
    kScaPopa = 0x14,
};
const char* vp_vector_name(unsigned op);  // null for a number with no name
const char* vp_scalar_name(unsigned op);

struct VpInsn {
    std::array<uint32_t, 4> w{};
    unsigned vec_op = 0, sca_op = 0;
    std::array<VpSource, 3> src{};
    unsigned input = 0;     // the one input register an instruction can read
    unsigned constant = 0;  // and the one constant: 10 bits on the RSX (nouveau's NV40 has 8)
    // Where the results go. The vector unit writes the output `dest` (vec_result) or a temp (vec_temp, 0x3F for none);
    // the scalar unit writes `dest` (sca_result) or a temp (sca_temp). Masks are x = 8, y = 4, z = 2, w = 1.
    bool vec_result = false, sca_result = false;
    unsigned vec_temp = 0x3F, sca_temp = 0, dest = 0;
    unsigned vec_mask = 0, sca_mask = 0;
    bool saturate = false, index_input = false, index_const = false, last = false;
    bool cond_test = false, cond_update = false, cond_reg1 = false, addr_reg1 = false;
    unsigned cond = 7;  // 0 false, 1 <, 2 ==, 3 <=, 4 >, 5 !=, 6 >=, 7 true
    std::array<uint8_t, 4> cond_swizzle{0, 1, 2, 3};
    unsigned addr_swizzle = 0;
    unsigned branch_target = 0;  // for BRA and CAL: an instruction's number
    std::array<uint32_t, 4> unknown{};  // set bits outside every field decoded
};
VpInsn decode_vp(std::span<const uint32_t, 4> w);
std::string disasm(const VpInsn& in);

// ---------------------------------------------------------------- fragment programs

enum class FpRegister : uint8_t { temp = 0, input = 1, constant = 2, unknown = 3 };

struct FpSource {
    FpRegister type = FpRegister::temp;
    unsigned index = 0;
    bool half = false;                 // an fp16 register (H) rather than fp32 (R)
    std::array<uint8_t, 4> swizzle{};  // 0 to 3: x, y, z, w
    bool negate = false, abs = false;
};

enum FpOp : uint8_t {
    kFpNop = 0x00, kFpMov = 0x01, kFpMul = 0x02, kFpAdd = 0x03, kFpMad = 0x04, kFpDp3 = 0x05, kFpDp4 = 0x06, kFpDst = 0x07,
    kFpMin = 0x08, kFpMax = 0x09, kFpSlt = 0x0A, kFpSge = 0x0B, kFpSle = 0x0C, kFpSgt = 0x0D, kFpSne = 0x0E, kFpSeq = 0x0F,
    kFpFrc = 0x10, kFpFlr = 0x11, kFpKil = 0x12, kFpPk4b = 0x13, kFpUp4b = 0x14, kFpDdx = 0x15, kFpDdy = 0x16, kFpTex = 0x17,
    kFpTxp = 0x18, kFpTxd = 0x19, kFpRcp = 0x1A, kFpRsq = 0x1B, kFpEx2 = 0x1C, kFpLg2 = 0x1D, kFpLit = 0x1E, kFpLrp = 0x1F,
    kFpStr = 0x20, kFpSfl = 0x21, kFpCos = 0x22, kFpSin = 0x23, kFpPk2h = 0x24, kFpUp2h = 0x25, kFpPow = 0x26, kFpPk4ub = 0x27,
    kFpUp4ub = 0x28, kFpPk2us = 0x29, kFpUp2us = 0x2A, kFpDp2a = 0x2E, kFpTxl = 0x2F, kFpTxb = 0x31, kFpRfl = 0x36, kFpDp2 = 0x38,
    kFpNrm = 0x39, kFpDiv = 0x3A, kFpDivsq = 0x3B, kFpLif = 0x3C, kFpFenct = 0x3D, kFpFencb = 0x3E,
};
// Flow control: an instruction with word 2's bit 31 set, whose opcode field is one of these.
enum FpBranchOp : uint8_t { kFpBrk = 0x0, kFpCal = 0x1, kFpIf = 0x2, kFpLoop = 0x3, kFpRep = 0x4, kFpRet = 0x5 };
const char* fp_name(unsigned op);
const char* fp_branch_name(unsigned op);

struct FpInsn {
    std::array<uint32_t, 4> w{};  // as the RSX reads them: halves exchanged back
    unsigned op = 0;
    bool branch = false;
    bool end = false;
    unsigned out = 0;       // R or H register written
    bool out_half = false, no_dest = false, saturate = false;
    unsigned mask = 0;      // x = 8, y = 4, z = 2, w = 1 (as the vertex program's, for one way of writing)
    unsigned input = 0;     // the input register an instruction reads: 0 position, 1 COL0, 2 COL1, 3 fog, 4 to 13 TEX0 to TEX9, 14 facing
    unsigned tex_unit = 0;
    unsigned precision = 0;  // 0 fp32, 1 fp16, 2 fixed s1.10, 3 fixed s0.8
    unsigned scale = 0;      // 0 none, 1 x2, 2 x4, 3 x8, 5 /2, 6 /4, 7 /8
    bool cond_write = false;
    unsigned cond = 7;
    std::array<uint8_t, 4> cond_swizzle{0, 1, 2, 3};
    std::array<FpSource, 3> src{};
    bool index_input = false, no_perspective = false;
    // Word 3's bits 19 to 29: nouveau's input-address displacement. In the game's programs it is 0x7FC exactly when an
    // instruction reads an input (and none is indexed), 0 otherwise: the check holds it to that.
    unsigned input_displacement = 0;
    // Word 2's bits 19 to 21: in the game's programs 5 on NRM and 0 everywhere else (nouveau: "input precision"). *Not understood.*
    unsigned src1_extra = 0;
    // Flow control's fields: IF's else and end, LOOP and REP's counts, CAL's target: as found, in words of the program.
    uint32_t else_at = 0, end_at = 0, target = 0, loop_count = 0, loop_init = 0, loop_step = 0;
    bool has_constant = false;
    std::array<uint32_t, 4> constant{};  // the inline constant after it, as floats' bits
    std::array<uint32_t, 4> unknown{};
};
// The instructions of a program, from its microcode (as stored: halves exchanged). Stops at the instruction marked
// as the end, or the microcode's end; `words` says how many of the microcode's words it took.
std::vector<FpInsn> decode_fp(std::span<const uint8_t> microcode, size_t* words = nullptr);
std::string disasm(const FpInsn& in);

// ---------------------------------------------------------------- the compiled programs, checked

// A compiled Cg program, as the program carries it (shaders.hpp finds them).
struct CgParameter {
    uint32_t type = 0, resource = 0, variability = 0;
    int32_t resource_index = -1;
    std::string name, semantic;
    uint32_t direction = 0;
    bool referenced = false;
    std::vector<uint32_t> embedded;  // fragment programs: where in the microcode its value is patched, in bytes
};
struct CgProgram {
    uint32_t address = 0;
    bool vertex = false;
    std::vector<uint32_t> header;  // the program's own header: 6 words for a vertex program, 7 for a fragment program
    std::vector<CgParameter> parameters;
    std::vector<uint8_t> microcode;
};
// One program from the bytes of its whole binary.
std::expected<CgProgram, std::string> parse_cg(std::span<const uint8_t> binary, uint32_t address);

// What the check found wrong with a program, if anything: the instructions' fields against the program's own header and
// parameters. Empty when all agrees.
std::vector<std::string> check_program(const CgProgram& p);

}  // namespace qilin::rsx
