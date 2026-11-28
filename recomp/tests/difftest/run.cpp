// qilin_difftest: every generated case, run lifted and on the reference
// interpreter from the same random states; registers, CR, XER, LR, CTR,
// traps, indirect jumps, system calls and memory must all agree.
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "cases.hpp"
#include "ppu.hpp"
#include "qilin/rt/dispatch.hpp"
#include "qilin/rt/memory.hpp"
#include "ref.hpp"

using namespace qilin;
using difftest::Case;
using difftest::Kind;

namespace {

constexpr uint32_t kWin = 0x30100000u, kWinSize = 0x2000u;  // the memory tests touch
constexpr int kTrials = 48, kFloatTrials = 1200, kVectorTrials = 400;

struct Trapped {};
bool g_did_call = false, g_did_system_call = false;
uint32_t g_called = 0, g_system_call_pc = 0;

void on_call(rt::Cpu&, uint32_t target) {
    g_did_call = true;
    g_called = target;
}
void on_system_call(rt::Cpu&, uint32_t pc) {
    g_did_system_call = true;
    g_system_call_pc = pc;
}
void on_trap(rt::Cpu&, uint32_t, const char*) { throw Trapped{}; }
bool g_did_fpscr_mode = false;
void on_fpscr_mode(rt::Cpu&, uint32_t) { g_did_fpscr_mode = true; }
constexpr uint64_t kTimeBase = 0x0123456789abcdefull;
uint64_t on_time_base(rt::Cpu&) { return kTimeBase; }
const rt::CpuHooks kHooks{on_call, on_system_call, on_trap, on_time_base, on_fpscr_mode};

std::mt19937_64 rng(0x52523721);

// Register values that find edges: zeros, ones, sign boundaries of each width, and noise.
uint64_t interesting() {
    static constexpr uint64_t kValues[] = {0, 1, 2, ~0ull, ~0ull - 1, 0x7fffffffull, 0x80000000ull, 0xffffffffull, 0x100000000ull,
                                           0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffff80000000ull, 0x00000000ffff8000ull, 31, 32, 63, 64, 65};
    const unsigned pick = rng() % 4;
    if (pick == 0) return kValues[rng() % std::size(kValues)];
    if (pick == 1) return rng() & 0xff;
    return rng();
}

// Register contents for floating point, as bits: the special values, singles,
// doubles halfway between two singles and a hair to either side (where
// rounding twice goes wrong), moderate doubles, small integers, and noise.
uint64_t interesting_double() {
    static constexpr uint64_t kSpecial[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x7ff0000000000000ull, 0xfff0000000000000ull,  // zeros, infinities
        0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff4000000000001ull, 0xfff0000000000001ull,  // quiet and signalling NaNs
        0x7ffc00000deadbeeull, 0x7ff00000dead0000ull,                                                // ... with payloads
        0x3ff0000000000000ull, 0xbff0000000000000ull, 0x3fe0000000000000ull, 0xbfe0000000000000ull,  // 1, -1, 0.5, -0.5
        0x3ff8000000000000ull, 0x4004000000000000ull, 0x4000000000000000ull,                          // 1.5, 2.5, 2
        0x7fefffffffffffffull, 0x0010000000000000ull, 0x0000000000000001ull, 0x000fffffffffffffull,  // the double's limits
        0x47efffffe0000000ull, 0x47efffffe0000001ull, 0x47effffff0000000ull, 0x47f0000000000000ull,  // the single's largest, and past it
        0x3810000000000000ull, 0x380fffffffffffffull, 0x36a0000000000000ull, 0x3690000000000000ull,  // its smallest normal and denormal, and half that
        0x3690000000000001ull, 0x368fffffffffffffull,
        0x41dfffffffc00000ull, 0x41e0000000000000ull, 0xc1e0000000000000ull, 0xc1e0000000200000ull,  // 2^31 - 1, 2^31, -2^31, just past
        0x43e0000000000000ull, 0xc3e0000000000000ull, 0x43dfffffffffffffull, 0xc3e0000000000001ull,  // 2^63 and its neighbours
    };
    switch (rng() % 8) {
    case 0: case 1: return kSpecial[rng() % std::size(kSpecial)];
    case 2: case 3: return std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(rng()))));
    case 4: {  // halfway between two singles, or one double either side
        const uint32_t single = (static_cast<uint32_t>(rng()) & 0x807fffffu) | ((100 + static_cast<uint32_t>(rng() % 60)) << 23);
        return std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(single))) + (uint64_t{1} << 28) + (rng() % 3) - 1;
    }
    case 5: return (rng() & 0x800fffffffffffffull) | ((1023 - 32 + rng() % 64) << 52);
    case 6: return std::bit_cast<uint64_t>((static_cast<double>(rng() % 4001) - 2000.0) / 2.0);
    default: return rng();
    }
}

// A vector's element: the single-precision special values, moderate numbers, small integers, and noise.
uint32_t interesting_word() {
    static constexpr uint32_t kSpecial[] = {
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xffa00000u, 0x7fc12345u,  // zeros, infinities, NaNs
        0x3f800000u, 0xbf800000u, 0x3f000000u, 0x40000000u, 0x7f7fffffu, 0xff7fffffu,                                         // 1, -1, 0.5, 2, the largest
        0x00800000u, 0x80800000u, 0x007fffffu, 0x00000001u, 0x80000001u, 0x00800001u,                                         // the smallest normal, denormals
        0x4f000000u, 0xcf000000u, 0x4effffffu, 0xcf000001u, 0x4f800000u,                                                      // 2^31 and its neighbours, 2^32
        0x00000001u, 0xffffffffu, 0x7fffffffu, 0x0000001fu,
    };
    switch (rng() % 6) {
    case 0: case 1: return kSpecial[rng() % std::size(kSpecial)];
    case 2: return (static_cast<uint32_t>(rng()) & 0x807fffffu) | ((127 - 12 + static_cast<uint32_t>(rng() % 24)) << 23);  // near 1 in magnitude
    case 3: return std::bit_cast<uint32_t>(static_cast<float>(static_cast<int>(rng() % 2001) - 1000) * 0.25f);
    default: return static_cast<uint32_t>(rng());
    }
}

bool has(const ppu::Spec& s, ppu::Opnd wanted) {
    for (const ppu::Opnd o : s.operands)
        if (o == wanted) return true;
    return false;
}

struct Run {
    rt::Cpu cpu;
    ref::State state;
};

int failures = 0;

void fail(const Case& c, const std::string& what) {
    if (++failures <= 20) std::println(stderr, "FAIL {:08x} {} at {:08x}: {}", c.word, ppu::disasm(ppu::decode(c.word), c.pc), c.pc, what);
}

void compare(const Case& c, const rt::Cpu& cpu, const ref::State& s, const std::vector<uint8_t>& lifted_memory, const uint8_t* reference_memory,
             const std::string& operands) {
    for (unsigned i = 0; i < 32; ++i)
        if (cpu.r[i] != s.gpr[i]) fail(c, std::format("r{}: lifted {:#x}, reference {:#x}", i, cpu.r[i], s.gpr[i]));
    for (unsigned i = 0; i < 32; ++i)
        if (std::bit_cast<uint64_t>(cpu.f[i]) != s.fpr[i])
            fail(c, std::format("f{}: lifted {:#018x}, reference {:#018x}{}", i, std::bit_cast<uint64_t>(cpu.f[i]), s.fpr[i], operands));
    for (unsigned i = 0; i < 32; ++i)
        for (unsigned k = 0; k < 4; ++k) {
            const uint32_t reference = uint32_t{s.vr[i][4 * k]} << 24 | uint32_t{s.vr[i][4 * k + 1]} << 16 | uint32_t{s.vr[i][4 * k + 2]} << 8 | s.vr[i][4 * k + 3];
            if (cpu.v[i].w[k] != reference) fail(c, std::format("v{} word {}: lifted {:#010x}, reference {:#010x}{}", i, k, cpu.v[i].w[k], reference, operands));
        }
    if (cpu.cr != s.cr) fail(c, std::format("CR: lifted {:#x}, reference {:#x}", cpu.cr, s.cr));
    if (cpu.xer() != s.xer) fail(c, std::format("XER: lifted {:#x}, reference {:#x}", cpu.xer(), s.xer));
    if (cpu.lr != s.lr) fail(c, std::format("LR: lifted {:#x}, reference {:#x}", cpu.lr, s.lr));
    if (cpu.ctr != s.ctr) fail(c, std::format("CTR: lifted {:#x}, reference {:#x}", cpu.ctr, s.ctr));
    if (cpu.fpscr != s.fpscr) fail(c, std::format("FPSCR: lifted {:#x}, reference {:#x}{}", cpu.fpscr, s.fpscr, operands));
    if (std::memcmp(lifted_memory.data(), reference_memory, kWinSize) != 0) fail(c, "memory differs");
}

// ---- the unwinding program (gen.cpp, "C++ exceptions") ----
// Run lifted and by the reference interpreter, instruction by instruction: for the reference, __builtin_eh_return's
// epilogue is an add and a return to LR, so it lands at the pad with nothing known about exceptions. Each depth and
// mode from random states: everything must agree, and r7 must say the right frames ran (gen.cpp).
constexpr uint32_t kReturnSentinel = 0x00001230u;

// Runs a program from `pc` until it returns to the sentinel; false if it leaves the program or runs too long.
bool interpret(ref::State& s, uint32_t pc, const difftest::ProgramWord* words, size_t count) {
    for (int steps = 0; steps < 10000; ++steps) {
        if (pc == kReturnSentinel) return true;
        const difftest::ProgramWord* w = nullptr;
        for (size_t i = 0; i < count; ++i)
            if (words[i].at == pc) w = &words[i];
        if (!w) return false;
        const ppu::Insn in = ppu::decode(w->word);
        switch (in.op) {
        case ppu::Op::b:
            if (in.lk()) s.lr = pc + 4;
            pc = in.branch_target(pc);
            break;
        case ppu::Op::bc: {
            const bool taken = ref::branch_taken(s, in.rd(), in.ra());
            if (in.lk()) s.lr = pc + 4;
            pc = taken ? in.branch_target(pc) : pc + 4;
            break;
        }
        case ppu::Op::bclr:  // (only the unconditional return is in the program)
            if (in.lk() || (in.rd() & 0x14) != 0x14) return false;
            pc = static_cast<uint32_t>(s.lr) & ~3u;
            break;
        default:
            if (!ref::step(s, in)) return false;
            pc += 4;
        }
    }
    return false;
}

int eh_failures = 0;

void eh_fail(unsigned depth, unsigned mode, const std::string& what) {
    if (++eh_failures <= 20) std::println(stderr, "FAIL the unwinding program, depth {} mode {}: {}", depth, mode, what);
}

// Returns the number of runs.
size_t run_unwinding(rt::AddressSpace& space) {
    uint8_t* const window = space.base() + kWin;
    const uint32_t stack = kWin + 0x1800;
    std::vector<uint8_t> before(kWinSize), lifted_memory(kWinSize);
    size_t runs = 0;
    for (unsigned depth = 0; depth < 3; ++depth)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (int trial = 0; trial < 8; ++trial, ++runs) {
                rt::Cpu cpu;
                cpu.mem = space.base();
                cpu.hooks = &kHooks;
                for (uint64_t& r : cpu.r) r = interesting();
                cpu.cr = static_cast<uint32_t>(rng());
                cpu.ctr = interesting();
                cpu.r[1] = stack;
                cpu.r[3] = depth;
                cpu.r[4] = mode == 3 ? 1 : mode;
                // Mode 3: an exception for a frame that isn't on the stack (r5 names a frame whose back chain is no
                // activation's): no function takes it, and it must come out of the outermost.
                cpu.r[5] = mode == 3 ? stack - 0x400 : 0;
                cpu.r[6] = difftest::kEhPad;
                cpu.r[7] = 0;
                cpu.lr = kReturnSentinel;
                for (uint8_t& b : before) b = static_cast<uint8_t>(rng());
                ref::State s;
                for (unsigned i = 0; i < 32; ++i) s.gpr[i] = cpu.r[i];
                s.cr = cpu.cr;
                s.xer = cpu.xer();
                s.lr = cpu.lr;
                s.ctr = cpu.ctr;
                s.mem = space.base();

                std::memcpy(window, before.data(), kWinSize);
                bool escaped = false;
                try {
                    difftest::eh_entry(cpu);
                } catch (const rt::GuestUnwind& e) {
                    escaped = true;
                    if (e.pad != difftest::kEhPad || e.sp != stack - 0x400) eh_fail(depth, mode, std::format("escaped for {:#x} at {:#x}", e.pad, e.sp));
                } catch (const Trapped&) {
                    eh_fail(depth, mode, "trapped");
                    continue;
                }
                if (escaped != (mode == 3)) {
                    eh_fail(depth, mode, escaped ? "no function took the exception" : "an exception for no frame was taken");
                    continue;
                }
                if (escaped) continue;
                std::memcpy(lifted_memory.data(), window, kWinSize);

                std::memcpy(window, before.data(), kWinSize);
                if (!interpret(s, difftest::kEhEntry, difftest::kEhProgram, difftest::kEhProgramSize)) {
                    eh_fail(depth, mode, "the reference left the program");
                    continue;
                }
                const uint64_t r7 = mode == 0 ? 16 + depth + 1 : mode == 1 ? 0x100 : 0x100 + depth;
                if (cpu.r[7] != r7) eh_fail(depth, mode, std::format("r7 {:#x}, not {:#x}", cpu.r[7], r7));
                if (cpu.r[1] != stack) eh_fail(depth, mode, std::format("r1 {:#x} on return, not {:#x}", cpu.r[1], stack));
                for (unsigned i = 0; i < 32; ++i)
                    if (cpu.r[i] != s.gpr[i]) eh_fail(depth, mode, std::format("r{}: lifted {:#x}, reference {:#x}", i, cpu.r[i], s.gpr[i]));
                if (cpu.cr != s.cr) eh_fail(depth, mode, std::format("CR: lifted {:#x}, reference {:#x}", cpu.cr, s.cr));
                if (cpu.lr != s.lr) eh_fail(depth, mode, std::format("LR: lifted {:#x}, reference {:#x}", cpu.lr, s.lr));
                if (cpu.ctr != s.ctr) eh_fail(depth, mode, std::format("CTR: lifted {:#x}, reference {:#x}", cpu.ctr, s.ctr));
                if (std::memcmp(lifted_memory.data(), window, kWinSize) != 0) eh_fail(depth, mode, "memory differs");
            }
    return runs;
}

// ---- rounding once to single: the fused forms' fast path (fp::rounds_once_to_single) ----
// Products of singles near half a unit of the addend's last place, so that the double sum lands on or a hair off a
// point halfway between two singles, where rounding the double again would round twice; and plain noise. The
// scalar fmadds/fmsubs against the reference's integer soft-float, and VMX's vmaddfp and vnmsubfp (and its fused
// element alone) against the general path, which the VMX cases hold to the reference.
size_t run_rounding() {
    size_t runs = 0;
    const auto single = [](uint32_t w) { return static_cast<double>(std::bit_cast<float>(w)); };
    for (int trial = 0; trial < 400000; ++trial, ++runs) {
        uint32_t a, c, b;
        if (trial % 4 == 3) {
            a = static_cast<uint32_t>(rng());
            c = static_cast<uint32_t>(rng());
            b = static_cast<uint32_t>(rng());
        } else {
            // a = 2^ea (1 + u), c = 2^ec (1 - u): the product is just under (or over) a power of two by u^2.
            const uint32_t u = 1u + static_cast<uint32_t>(rng() % 64), shift = 8 + static_cast<uint32_t>(rng() % 15);
            const int eb = 60 + static_cast<int>(rng() % 136);
            const int ep = eb - 24 - static_cast<int>(rng() % 3);  // the product about half the addend's last place
            const uint32_t fa = (0x00800000u + (u << (23 - shift))) & 0x007fffffu;
            const uint32_t fc = (0x00800000u - (u << (23 - shift))) & 0x007fffffu;
            const int ea = ep / 2 + 1, ec = ep - ea + (rng() % 2 ? 0 : 1);
            a = (static_cast<uint32_t>(ea + 0) << 23) | fa;
            c = (static_cast<uint32_t>(ec - 1) << 23) | fc;
            b = (static_cast<uint32_t>(eb) << 23) | (static_cast<uint32_t>(rng()) & 0x007fffffu);
            if (rng() % 2) a ^= 0x80000000u;
            if (rng() % 2) b ^= 0x80000000u;
        }
        for (const bool subtract : {false, true}) {
            const double x = single(a), y = single(c), z = single(b);
            const double lifted = subtract ? rt::fp::fmsubs(x, y, z) : rt::fp::fmadds(x, y, z);
            const uint64_t reference = ref::soft::fused(std::bit_cast<uint64_t>(x), std::bit_cast<uint64_t>(y), std::bit_cast<uint64_t>(z), subtract, true);
            if (std::bit_cast<uint64_t>(lifted) != reference && ++failures <= 20)
                std::println(stderr, "FAIL {} of singles {:08x} {:08x} {:08x}: lifted {:#018x}, reference {:#018x}", subtract ? "fmsubs" : "fmadds", a, c, b,
                             std::bit_cast<uint64_t>(lifted), reference);
            for (const bool negate : {false, true}) {
                const uint32_t general = rt::vmx::fused32_slow(a, c, b, subtract, negate);
                // The element alone, and in a vector (whose fast path takes all four at once), beside three plain ones.
                const rt::Vec va{{a, 0x3f800000u, c, 0x40000000u}}, vc{{c, 0x3f000000u, a, 0x3f800000u}}, vb{{b, 0x3f800000u, b, 0x00000000u}};
                const uint32_t fast = (subtract != negate) ? rt::vmx::fused32(a, c, b, subtract, negate)
                                    : subtract ? rt::vmx::vnmsubfp(va, vc, vb).w[0] : rt::vmx::vmaddfp(va, vc, vb).w[0];
                if (fast != general && ++failures <= 20)
                    std::println(stderr, "FAIL VMX fused element of {:08x} {:08x} {:08x} ({}{}): fast {:08x}, general {:08x}", a, c, b, subtract ? "subtract" : "add",
                                 negate ? ", negated" : "", fast, general);
            }
        }
    }
    return runs;
}

// ---- relaxed floating point (fp.hpp, vmx.hpp: the game's mode) against the exact mode ----
// On ordinary finite values (singles and doubles whose results stay normal): the operations the relaxed mode does
// as the guest does must agree to the bit; the single fused forms, the single operations on doubles that are not
// singles, and vrsqrtefp are bounded (one unit in the single's last place; two for vrsqrtefp's two roundings); and
// how many differ at all is counted, for the record. The host's vector code against its portable twin, and the
// flush of denormals that fp::flush_denormals sets, on a thread of its own.
struct RelaxedCounts {
    size_t runs = 0, fused_differ = 0, doubles_differ = 0, rsqrt_differ = 0, worst_rsqrt = 0, halfway = 0, halfway_differ = 0;
};
// How many singles lie between x and y (of one sign), as the ordered whole numbers their bits are.
uint32_t ulps(uint32_t x, uint32_t y) {
    if ((x ^ y) >> 31) return (x << 1) == 0 && (y << 1) == 0 ? 0u : 0xffffffffu;
    return x > y ? x - y : y - x;
}
uint32_t single_bits(double v) { return std::bit_cast<uint32_t>(static_cast<float>(v)); }
// A normal single of exponent 64 to 190 (from about 2^-63 to 2^63): sums, products and quotients of two stay normal.
uint32_t ordinary_single() { return (static_cast<uint32_t>(rng()) & 0x807fffffu) | ((64 + static_cast<uint32_t>(rng() % 127)) << 23); }

RelaxedCounts run_relaxed() {
    RelaxedCounts n;
    namespace ex = rt::fp::exact;
    namespace rx = rt::fp::relaxed;
    const auto bad = [&](const char* what, uint64_t x, uint64_t y, uint64_t z, uint64_t relaxed, uint64_t exact) {
        if (++failures <= 20)
            std::println(stderr, "FAIL relaxed {} of {:#x} {:#x} {:#x}: relaxed {:#x}, exact {:#x}", what, x, y, z, relaxed, exact);
    };
    const auto value = [](uint32_t w) { return static_cast<double>(std::bit_cast<float>(w)); };
    for (int trial = 0; trial < 200000; ++trial) {
        const uint32_t wa = ordinary_single(), wb = ordinary_single(), wc = ordinary_single();
        const double a = value(wa), b = value(wb), c = value(wc);
        // Singles: one rounding either way, so the same.
        const std::pair<const char*, std::pair<double, double>> same[] = {
            {"fadds", {rx::fadds(a, b), ex::fadds(a, b)}}, {"fsubs", {rx::fsubs(a, b), ex::fsubs(a, b)}},
            {"fmuls", {rx::fmuls(a, c), ex::fmuls(a, c)}}, {"fdivs", {rx::fdivs(a, b), ex::fdivs(a, b)}},
            {"fsqrts", {rx::fsqrts(std::abs(b)), ex::fsqrts(std::abs(b))}}, {"frsp", {rx::frsp(a * c), ex::frsp(a * c)}},
            {"fadd", {rx::fadd(a * c, b), ex::fadd(a * c, b)}}, {"fsub", {rx::fsub(a, b * c), ex::fsub(a, b * c)}},
            {"fmul", {rx::fmul(a * b, c), ex::fmul(a * b, c)}}, {"fdiv", {rx::fdiv(a * b, c), ex::fdiv(a * b, c)}},
            {"fsqrt", {rx::fsqrt(std::abs(a * b)), ex::fsqrt(std::abs(a * b))}}, {"fmadd", {rx::fmadd(a * b, c, b), ex::fmadd(a * b, c, b)}},
            {"fnmsub", {rx::fnmsub(a, c * b, b), ex::fnmsub(a, c * b, b)}},
            {"lfs", {rx::load_single(wa), ex::load_single(wa)}},
            {"stfs", {static_cast<double>(rx::store_single(a)), static_cast<double>(ex::store_single(a))}},
        };
        for (const auto& [what, r] : same)
            if (std::bit_cast<uint64_t>(r.first) != std::bit_cast<uint64_t>(r.second)) bad(what, wa, wb, wc, std::bit_cast<uint64_t>(r.first), std::bit_cast<uint64_t>(r.second));
        // The fused singles: within one unit.
        const std::pair<const char*, std::pair<double, double>> fused[] = {
            {"fmadds", {rx::fmadds(a, c, b), ex::fmadds(a, c, b)}}, {"fmsubs", {rx::fmsubs(a, c, b), ex::fmsubs(a, c, b)}},
            {"fnmadds", {rx::fnmadds(a, c, b), ex::fnmadds(a, c, b)}}, {"fnmsubs", {rx::fnmsubs(a, c, b), ex::fnmsubs(a, c, b)}},
        };
        for (const auto& [what, r] : fused) {
            const uint32_t d = ulps(single_bits(r.first), single_bits(r.second));
            if (d > 1) bad(what, wa, wb, wc, std::bit_cast<uint64_t>(r.first), std::bit_cast<uint64_t>(r.second));
            n.fused_differ += d != 0;
        }
        // Single operations on doubles that are not singles: rounded twice, within one unit.
        const double da = a * (1.0 + 0x1p-40), db = b * (1.0 - 0x1p-37);
        const std::pair<const char*, std::pair<double, double>> doubles[] = {
            {"fadds of doubles", {rx::fadds(da, db), ex::fadds(da, db)}}, {"fmuls of doubles", {rx::fmuls(da, db), ex::fmuls(da, db)}},
            {"fdivs of doubles", {rx::fdivs(da, db), ex::fdivs(da, db)}}, {"fmadds of doubles", {rx::fmadds(da, db, c), ex::fmadds(da, db, c)}},
        };
        for (const auto& [what, r] : doubles) {
            const uint32_t d = ulps(single_bits(r.first), single_bits(r.second));
            if (d > 1) bad(what, std::bit_cast<uint64_t>(da), std::bit_cast<uint64_t>(db), wc, std::bit_cast<uint64_t>(r.first), std::bit_cast<uint64_t>(r.second));
            n.doubles_differ += d != 0;
        }
        // VMX: four elements at once, the host's vector code and its portable twin.
        const rt::Vec va{{wa, wb, wc, ordinary_single()}}, vb{{wb, wc, ordinary_single(), wa}}, vc{{wc, ordinary_single(), wa, wb}};
        const auto vsame = [&](const char* what, const rt::Vec& relaxed, const rt::Vec& exact) {
            for (unsigned i = 0; i < 4; ++i)
                if (relaxed.w[i] != exact.w[i]) bad(what, va.w[i], vb.w[i], vc.w[i], relaxed.w[i], exact.w[i]);
        };
        const auto vnear = [&](const char* what, const rt::Vec& relaxed, const rt::Vec& exact, uint32_t bound) {
            uint32_t worst = 0;
            for (unsigned i = 0; i < 4; ++i) {
                const uint32_t d = ulps(relaxed.w[i], exact.w[i]);
                if (d > bound) bad(what, va.w[i], vb.w[i], vc.w[i], relaxed.w[i], exact.w[i]);
                worst = std::max(worst, d);
            }
            return worst;
        };
        namespace vx = rt::vmx::exact;
        namespace vr = rt::vmx::relaxed;
        namespace vp = rt::vmx::portable;
        vsame("vaddfp", vr::vaddfp(va, vb), vx::vaddfp(va, vb));
        vsame("vsubfp", vr::vsubfp(va, vb), vx::vsubfp(va, vb));
        vsame("vmaxfp", vr::vmaxfp(va, vb), vx::vmaxfp(va, vb));
        vsame("vminfp", vr::vminfp(va, vb), vx::vminfp(va, vb));
        vsame("vcmpeqfp", vr::vcmpeqfp(va, va), vx::vcmpeqfp(va, va));
        vsame("vcmpeqfp", vr::vcmpeqfp(va, vb), vx::vcmpeqfp(va, vb));
        vsame("vrefp", vr::vrefp(va), vx::vrefp(va));
        const rt::Vec positive{{va.w[0] & 0x7fffffffu, va.w[1] & 0x7fffffffu, va.w[2] & 0x7fffffffu, va.w[3] & 0x7fffffffu}};
        const uint32_t root = vnear("vrsqrtefp", vr::vrsqrtefp(positive), vx::vrsqrtefp(positive), 2);
        n.rsqrt_differ += root != 0;
        n.worst_rsqrt = std::max<size_t>(n.worst_rsqrt, root);
        n.fused_differ += vnear("vmaddfp", vr::vmaddfp(va, vc, vb), vx::vmaddfp(va, vc, vb), 1) != 0;
        n.fused_differ += vnear("vnmsubfp", vr::vnmsubfp(va, vc, vb), vx::vnmsubfp(va, vc, vb), 1) != 0;
        // vctsxs on any bits at all (NaNs, infinities, denormals, out of range): the same.
        const rt::Vec any{{static_cast<uint32_t>(rng()), static_cast<uint32_t>(rng()), static_cast<uint32_t>(rng()), trial % 2 ? 0x7fc00000u : 0xcf000001u}};
        const unsigned scale = static_cast<unsigned>(rng() % 32);
        vsame("vctsxs", vr::vctsxs(any, scale), vx::vctsxs(any, scale));
        // The twin: the same results, but for the fused forms where the host has a fused single multiply-add.
        vsame("vaddfp (portable)", vp::vaddfp(va, vb), vr::vaddfp(va, vb));
        vsame("vsubfp (portable)", vp::vsubfp(va, vb), vr::vsubfp(va, vb));
        vsame("vmaxfp (portable)", vp::vmaxfp(va, any), vr::vmaxfp(va, any));
        vsame("vminfp (portable)", vp::vminfp(any, va), vr::vminfp(any, va));
        vsame("vcmpeqfp (portable)", vp::vcmpeqfp(va, any), vr::vcmpeqfp(va, any));
        vsame("vrefp (portable)", vp::vrefp(va), vr::vrefp(va));
        vsame("vrsqrtefp (portable)", vp::vrsqrtefp(positive), vr::vrsqrtefp(positive));
        vsame("vctsxs (portable)", vp::vctsxs(any, scale), vr::vctsxs(any, scale));
#if QILIN_VMX_NEON || defined(__FMA__) || defined(__AVX2__)
        vnear("vmaddfp (portable)", vp::vmaddfp(va, vc, vb), vr::vmaddfp(va, vc, vb), 1);
        vnear("vnmsubfp (portable)", vp::vnmsubfp(va, vc, vb), vr::vnmsubfp(va, vc, vb), 1);
#else
        vsame("vmaddfp (portable)", vp::vmaddfp(va, vc, vb), vr::vmaddfp(va, vc, vb));
        vsame("vnmsubfp (portable)", vp::vnmsubfp(va, vc, vb), vr::vnmsubfp(va, vc, vb));
#endif
        ++n.runs;
    }
    // Where the fused singles do differ: a sum whose double lands on a point halfway between two singles. b odd, and
    // a product of singles 2^(e - 24) (1 - 2^-2k) of b's sign (a = 2^x (1 + 2^-k), c = 2^y (1 - 2^-k)): the true sum is a
    // hair below the halfway point, so the guest rounds to b; the double rounds up onto the halfway point, and then to
    // the even single, one unit away.
    for (int trial = 0; trial < 20000; ++trial, ++n.halfway) {
        const uint32_t eb = 80 + static_cast<uint32_t>(rng() % 100), k = 15 + static_cast<uint32_t>(rng() % 9), x = 100 + static_cast<uint32_t>(rng() % 20);
        const uint32_t sign = static_cast<uint32_t>(rng() % 2) << 31;
        const uint32_t wb = sign | (eb << 23) | ((static_cast<uint32_t>(rng()) & 0x007ffffeu) | 1u);
        const uint32_t wa = sign | (x << 23) | (0x00800000u >> k), wc = ((eb - 24 + 127 - x - 1) << 23) | (0x00800000u - (0x01000000u >> k));  // (2 - 2^(1-k)) / 2
        const double a = value(wa), b = value(wb), c = value(wc);
        for (const bool subtract : {false, true}) {
            const double z = subtract ? -b : b;
            const double relaxed = subtract ? rx::fmsubs(a, c, z) : rx::fmadds(a, c, z), exact = subtract ? ex::fmsubs(a, c, z) : ex::fmadds(a, c, z);
            const uint32_t d = ulps(single_bits(relaxed), single_bits(exact));
            if (d > 1 || exact != b) bad(subtract ? "fmsubs on a halfway point" : "fmadds on a halfway point", wa, wc, wb, std::bit_cast<uint64_t>(relaxed), std::bit_cast<uint64_t>(exact));
            n.halfway_differ += d != 0;
        }
        const rt::Vec va{{wa, wa, wa, wa}}, vc{{wc, wc, wc, wc}}, vb{{wb, wb, wb, wb}};
        const uint32_t d = ulps(rt::vmx::relaxed::vmaddfp(va, vc, vb).w[0], rt::vmx::exact::vmaddfp(va, vc, vb).w[0]);
        if (d > 1) bad("vmaddfp on a halfway point", wa, wc, wb, rt::vmx::relaxed::vmaddfp(va, vc, vb).w[0], rt::vmx::exact::vmaddfp(va, vc, vb).w[0]);
    }
    // Denormals with the thread's float unit flushing them (as fp::enter_thread_mode sets it for play): VMX's results
    // are what its non-Java mode gives; a denormal operand counts as zero.
    std::thread([&] {
        rt::fp::flush_denormals();
        // (Each input through a volatile, read after the flush is set: constants would be folded at compile time, in IEEE
        // mode, which no flush set at run time reaches. Clang and GCC did so.)
        const auto unseen = [](rt::Vec v) {
            rt::Vec r;
            for (unsigned i = 0; i < 4; ++i) {
                volatile uint32_t w = v.w[i];
                r.w[i] = w;
            }
            return r;
        };
        const rt::Vec tiny = unseen({{0x00400000u, 0x80000001u, 0x00800000u, 0x3f800000u}}), half = unseen({{0x3f000000u, 0x3f000000u, 0x3f000000u, 0x00000001u}});
        const rt::Vec zero = unseen({{0, 0, 0, 0}}), small = unseen({{0x00c00000u, 0x80c00000u, 0x00800001u, 0x3f800000u}}),
                      normal = unseen({{0x00800000u, 0x80800000u, 0x00800000u, 0x3f800000u}});
        const std::pair<const char*, std::pair<rt::Vec, rt::Vec>> flushed[] = {
            {"vmaddfp of denormals", {rt::vmx::relaxed::vmaddfp(tiny, half, zero), rt::vmx::exact::vmaddfp(tiny, half, zero)}},
            {"vaddfp of denormals", {rt::vmx::relaxed::vaddfp(tiny, half), rt::vmx::exact::vaddfp(tiny, half)}},
            {"vsubfp to a denormal", {rt::vmx::relaxed::vsubfp(small, normal), rt::vmx::exact::vsubfp(small, normal)}},
            {"vcmpeqfp of denormals", {rt::vmx::relaxed::vcmpeqfp(tiny, zero), rt::vmx::exact::vcmpeqfp(tiny, zero)}},
        };
        for (const auto& [what, r] : flushed)
            for (unsigned i = 0; i < 4; ++i)
                if (r.first.w[i] != r.second.w[i]) bad(what, tiny.w[i], half.w[i], 0, r.first.w[i], r.second.w[i]);
    }).join();
    return n;
}

// ---- the function index (dispatch.hpp): calls through a register find the function by address ----
void f_a(rt::Cpu&) {}
void f_b(rt::Cpu&) {}
void f_c(rt::Cpu&) {}
bool check_index() {
    static const rt::LiftedFunction table[] = {{0x00010000u, &f_a}, {0x00010010u, &f_b}, {0x00010100u, &f_c}};
    const rt::FunctionIndex index(table);
    bool ok = index.find(0x00010000u) == &f_a && index.find(0x00010010u) == &f_b && index.find(0x00010100u) == &f_c;
    for (const uint32_t none : {0x00010004u, 0x0000FFFCu, 0x00010102u, 0x00010104u, 0xFFFFFFFCu, 0u}) ok &= index.find(none) == nullptr;
    if (!ok) std::println(stderr, "FAIL the function index finds a function wrongly");
    return ok;
}

}  // namespace

int main() {
    if (!check_index()) return 1;
    rt::AddressSpace space;
    if (!space.base() || !space.commit(kWin, kWinSize)) {
        std::println(stderr, "difftest: can't reserve the guest's address space");
        return 1;
    }
    uint8_t* const window = space.base() + kWin;
    std::vector<uint8_t> before(kWinSize), lifted_memory(kWinSize);
    size_t traps = 0, taken = 0, runs = 0, programs = 0;

    for (size_t k = 0; k < difftest::kCaseCount; ++k) {
        const Case& c = difftest::kCases[k];
        const ppu::Insn in = ppu::decode(c.word);
        const ppu::Spec& spec = ppu::spec(in.op);
        // Floating-point arithmetic gets many more states: its hard cases (ties, cancellation, the edges of a format) are rare.
        const bool memory = spec.flags & (ppu::kLoad | ppu::kStore);
        const int trials = c.kind == Kind::Program ? kTrials : memory ? kTrials : (spec.flags & ppu::kFloat) ? kFloatTrials : (spec.flags & ppu::kVector) ? kVectorTrials : kTrials;
        for (int trial = 0; trial < trials; ++trial, ++runs) {
            rt::Cpu cpu;
            cpu.mem = space.base();
            cpu.hooks = &kHooks;
            for (uint64_t& r : cpu.r) r = interesting();
            for (double& f : cpu.f) f = std::bit_cast<double>(interesting_double());
            for (rt::Vec& v : cpu.v)
                for (uint32_t& w : v.w) w = interesting_word();
            if ((spec.flags & ppu::kVector) && rng() % 4 == 0)  // elements that cancel, or are equal
                for (unsigned e = 0; e < 4; ++e) cpu.v[in.rb()].w[e] = cpu.v[in.ra()].w[e] ^ (rng() % 2 ? 0x80000000u : 0u);
            if (spec.flags & ppu::kFloat) {  // operands that nearly cancel, and a multiplier of one, come up often
                if (rng() % 4 == 0) cpu.f[in.rb()] = std::bit_cast<double>((std::bit_cast<uint64_t>(cpu.f[in.ra()]) ^ 0x8000000000000000ull) + rng() % 3 - 1);
                if (rng() % 8 == 0) cpu.f[in.rc_reg()] = 1.0;
            }
            cpu.cr = static_cast<uint32_t>(rng());
            cpu.lr = interesting();
            cpu.ctr = trial % 4 == 0 ? 1 : trial % 4 == 1 ? 0 : interesting();  // CTR - 1 == 0, and its wrap, come up often
            cpu.xer_so = rng() & 1;
            cpu.xer_ov = rng() & 1;
            cpu.xer_ca = rng() & 1;
            cpu.xer_bc = rng() & 0x7f;
            // The FPSCR: mostly a mode this runtime provides (low byte zero), sometimes not.
            cpu.fpscr = static_cast<uint32_t>(rng()) & (rng() % 4 == 0 ? ~0u : ~0xFFu);
            for (uint8_t& b : before) b = static_cast<uint8_t>(rng());

            // A load or store: steer its address into the window, keeping noise above bit 31 (which the address drops).
            if (c.kind == Kind::Plain && (spec.flags & (ppu::kLoad | ppu::kStore))) {
                const uint64_t target = kWin + 0x800 + (rng() % 0x1000 & ~15ull), noise = (rng() % 3 == 0 ? rng() : 0) << 32;
                if (has(spec, ppu::Opnd::d_rA0) || has(spec, ppu::Opnd::ds_rA0)) {
                    const int32_t disp = has(spec, ppu::Opnd::ds_rA0) ? in.ds() : in.simm();
                    cpu.r[in.ra()] = ((target - static_cast<uint64_t>(int64_t{disp})) & 0xffffffffu) | noise;
                } else if (in.ra() == 0 || in.ra() == in.rb()) {
                    // (rA|0) + rB with no rA, or the same register twice (then it holds half the address).
                    cpu.r[in.rb()] = in.ra() == 0 ? (target | noise) : (target / 2) | (rng() & 1 ? 0x80000000ull : 0);
                } else {
                    const uint64_t index = rng() % 0x400 & ~1ull;
                    cpu.r[in.rb()] = index | noise;
                    cpu.r[in.ra()] = (target - index) & 0xffffffffu;
                }
            }
            if (c.kind == Kind::Program) {
                cpu.r[1] = kWin + 0x1000;  // the window, which its loads and stores address off r1
                cpu.lr = kReturnSentinel;
                cpu.ctr = trial % 4 == 0 ? 2 : cpu.ctr;  // (CTR's branches decrement it: one that reaches zero)
            }
            if (c.kind == Kind::Switch) cpu.ctr = trial % 3 == 0 ? c.pc + 16 : trial % 3 == 1 ? c.pc + 24 : interesting();
            if (c.kind == Kind::Bc || c.kind == Kind::Bclr || c.kind == Kind::Bcctr || c.kind == Kind::Trap) cpu.r[3] = 7;

            // What a floating-point instruction started from, for a failure's message.
            const std::string operands = (spec.flags & ppu::kVector) ?
                std::format(" (from vA {:08x} {:08x} {:08x} {:08x}, vB {:08x} {:08x} {:08x} {:08x}, vC {:08x} {:08x} {:08x} {:08x})", cpu.v[in.ra()].w[0], cpu.v[in.ra()].w[1],
                            cpu.v[in.ra()].w[2], cpu.v[in.ra()].w[3], cpu.v[in.rb()].w[0], cpu.v[in.rb()].w[1], cpu.v[in.rb()].w[2], cpu.v[in.rb()].w[3],
                            cpu.v[in.rc_reg()].w[0], cpu.v[in.rc_reg()].w[1], cpu.v[in.rc_reg()].w[2], cpu.v[in.rc_reg()].w[3]) :
                !(spec.flags & ppu::kFloat) ? std::string() :
                std::format(" (from frA {:#018x}, frB {:#018x}, frC {:#018x})", std::bit_cast<uint64_t>(cpu.f[in.ra()]), std::bit_cast<uint64_t>(cpu.f[in.rb()]),
                            std::bit_cast<uint64_t>(cpu.f[in.rc_reg()]));
            ref::State s;
            for (unsigned i = 0; i < 32; ++i) s.gpr[i] = cpu.r[i];
            for (unsigned i = 0; i < 32; ++i) s.fpr[i] = std::bit_cast<uint64_t>(cpu.f[i]);
            for (unsigned i = 0; i < 32; ++i)
                for (unsigned e = 0; e < 16; ++e) s.vr[i][e] = static_cast<uint8_t>(cpu.v[i].w[e / 4] >> (24 - 8 * (e % 4)));
            s.cr = cpu.cr;
            s.xer = cpu.xer();
            s.lr = cpu.lr;
            s.ctr = cpu.ctr;
            s.fpscr = cpu.fpscr;
            s.mem = space.base();

            // ---- lifted ----
            std::memcpy(window, before.data(), kWinSize);
            g_did_call = g_did_system_call = g_did_fpscr_mode = false;
            bool trapped = false;
            try {
                c.fn(cpu);
            } catch (const Trapped&) {
                trapped = true;
            }
            std::memcpy(lifted_memory.data(), window, kWinSize);

            // ---- reference ----
            std::memcpy(window, before.data(), kWinSize);
            bool expect_trap = false, expect_call = false, expect_system_call = false;
            uint32_t expect_target = 0;
            switch (c.kind) {
            case Kind::Program:
                if (!interpret(s, c.entry, c.program, c.program_size)) {
                    fail(c, "the reference left the program");
                    trial = trials;
                    continue;
                }
                ++programs;
                break;
            case Kind::Plain:
                if (!ref::step(s, in)) {
                    fail(c, "the reference doesn't know this instruction");
                    trial = trials;
                    continue;
                }
                break;
            case Kind::Bc:
                s.gpr[3] = ref::branch_taken(s, in.rd(), in.ra()) ? 2 : 1;
                taken += s.gpr[3] == 2;
                break;
            case Kind::Bclr:
                if (!ref::branch_taken(s, in.rd(), in.ra())) s.gpr[3] = 1;
                break;
            case Kind::Bcctr:
                if (ref::branch_taken(s, in.rd(), in.ra())) {
                    expect_call = true;
                    expect_target = static_cast<uint32_t>(s.ctr) & ~3u;
                } else {
                    s.gpr[3] = 1;
                }
                break;
            case Kind::Trap:
                expect_trap = ref::trap_fires(s, in);
                if (!expect_trap) s.gpr[3] = 1;
                traps += expect_trap;
                break;
            case Kind::Call:  // bl to a function that adds 16 and returns; then 1 more
                s.lr = c.pc + 4;
                s.gpr[3] += 17;
                break;
            case Kind::Tail: s.gpr[3] += 16; break;
            case Kind::System: expect_system_call = true; break;
            case Kind::TimeBase: s.gpr[in.rd()] = in.spr() == 268 ? kTimeBase : kTimeBase >> 32; break;
            case Kind::Switch:
                if (static_cast<uint32_t>(s.ctr) == c.pc + 16) s.gpr[3] = 1;
                else if (static_cast<uint32_t>(s.ctr) == c.pc + 24) s.gpr[3] = 2;
                else expect_trap = true;
                break;
            }
            if (trapped != expect_trap) fail(c, std::format("trap: lifted {}, reference {}", trapped, expect_trap));
            if (g_did_call != expect_call) fail(c, std::format("jump through a register: lifted {}, reference {}", g_did_call, expect_call));
            else if (expect_call && g_called != expect_target) fail(c, std::format("jump target: lifted {:#x}, reference {:#x}", g_called, expect_target));
            if (g_did_system_call != expect_system_call) fail(c, "system call hook");
            const bool expect_fpscr_mode = in.op == ppu::Op::mtfsf && (s.fpscr & 0xFF) != 0;
            if (g_did_fpscr_mode != expect_fpscr_mode) fail(c, std::format("FPSCR mode hook: lifted {}, reference {}", g_did_fpscr_mode, expect_fpscr_mode));
            else if (expect_system_call && g_system_call_pc != c.pc) fail(c, "system call's address");
            compare(c, cpu, s, lifted_memory, window, operands);
        }
    }
    const size_t unwinding = run_unwinding(space);
    const size_t rounding = run_rounding();
    const RelaxedCounts relaxed = run_relaxed();
    if (failures || eh_failures) {
        std::println(stderr, "difftest: {} disagreements, {} in the unwinding program", failures, eh_failures);
        return 1;
    }
    std::println("difftest: {} cases, {} runs, all agree ({} branches taken, {} traps fired, {} runs of programs); the unwinding program: {} runs, all agree; "
                 "rounding once to single: {} operands, all agree",
                 difftest::kCaseCount, runs, taken, traps, programs, unwinding, rounding);
    std::println("relaxed floating point: {} operand sets, within bounds; differing from exact: the fused singles {} times (of {}), single "
                 "operations on doubles {} (of {}), vrsqrtefp {} vectors (of {}, by at most {} units); fused singles built to round on a halfway point {} "
                 "(of {})",
                 relaxed.runs, relaxed.fused_differ, 6 * relaxed.runs, relaxed.doubles_differ, 4 * relaxed.runs, relaxed.rsqrt_differ, relaxed.runs, relaxed.worst_rsqrt,
                 relaxed.halfway_differ, 2 * relaxed.halfway);
    return 0;
}
