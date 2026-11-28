// qilin_spu_difftest: every case the generator wrote, run from many states
// as lifted code and by the reference, and compared: all 128 registers, the
// quadword stored (and, for a store, the whole local store), what was
// written to a channel, a stop's code, whether a halt fired. The lifted side
// runs in both exact ways of single precision (spu_fp.hpp: `whole`, and
// `exact` with the host's float unit set for it), every other run each. The
// relaxed way is checked apart, against the exact one (check_relaxed).
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "qilin/rt/spu_fp.hpp"
#include "ref.hpp"

using namespace spu_difftest;
using qilin::rt::Spu;
using qilin::rt::SpuReg;

namespace {

// What the lifted side's hooks saw.
struct Seen {
    bool wrote_channel = false;
    uint32_t channel = 0, channel_data = 0;
    bool stopped = false;
    uint32_t stop_code = 0;
    bool halted = false;
};
Seen g_seen;

const qilin::rt::SpuHooks kHooks{
    [](Spu&, uint32_t channel, uint32_t) { return channel_value(channel); },
    [](Spu&, uint32_t channel, uint32_t value, uint32_t) { g_seen.wrote_channel = true, g_seen.channel = channel, g_seen.channel_data = value; },
    [](Spu&, uint32_t channel, uint32_t) { return channel_count(channel); },
    [](Spu&, uint32_t code, uint32_t) { g_seen.stopped = true, g_seen.stop_code = code; },
    [](Spu&, uint32_t, const char*) { g_seen.halted = true; },  // (the test's goes on, to compare; a real one may not)
    [](Spu&, uint32_t, uint32_t) { g_seen.halted = true; },
};

// A word that is often something an instruction treats specially.
// Words that are singles of every kind: zeros, denormals, the smallest and largest, the largest exponent, and ordinary ones near each other.
uint32_t single_like(std::mt19937& rng) {
    static constexpr uint32_t kSpecial[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x807fffffu, 0x00800000u, 0x80800000u, 0x00800001u, 0x7fffffffu, 0xffffffffu,
                                            0x7f800000u, 0xff800000u, 0x7fc00000u, 0x7f7fffffu, 0x3f800000u, 0xbf800000u, 0x3f7fffffu, 0x3f800001u, 0x40000000u,
                                            0x7e800000u, 0x7e7fffffu, 0x01000000u, 0x4f000000u, 0xcf000000u, 0x4effffffu, 0x4f800000u, 0x3f000000u};
    switch (rng() % 8) {
    case 0: case 1: return kSpecial[rng() % std::size(kSpecial)];
    case 2: return (rng() & 0x807fffffu) | ((120 + rng() % 16) << 23);  // near 1: sums that cancel and carry
    case 3: return (rng() & 0x807fffffu) | ((rng() % 4 ? 252 + rng() % 4 : rng() % 4) << 23);  // the ends of the range
    default: return rng();
    }
}

uint32_t interesting(std::mt19937& rng) {
    static constexpr uint32_t kSpecial[] = {0, 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                            0x10000, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffff0u, 0xffffffe0u, 0x80808080u, 0xc0c0c0c0u, 0xe0e0e0e0u, 0x00010203u};
    switch (rng() % 4) {
    case 0: return kSpecial[rng() % std::size(kSpecial)];
    case 1: return rng() & 0x3f;  // a count, or a small number
    default: return rng();
    }
}

// The estimates against the instruction set's own bound. The document gives two sequences (an estimate, an
// interpolation, one step of Newton-Raphson) and says how near the result y2 must be to Y, the largest single
// with x * Y < 1 (or x * Y * Y < 1): for the reciprocal, y2 is Y or the single just under it; for the root,
// within one either way. The tables are this runtime's own, so this is what says they are good enough.
// Returns how many of `count` random x broke the bound.
namespace fp = qilin::rt::spu;

// How m * m * x (m below 2^25, x below 2^24) compares with 2^power: by whole numbers.
int compare_square(uint64_t m, uint64_t x, int power) {
    const uint64_t square = m * m, low = (square & 0xffffffffu) * x, high = (square >> 32) * x + (low >> 32);
    if (power >= 96) return -1;
    if (power >= 32) {
        const uint64_t limit = uint64_t{1} << (power - 32);
        return high < limit ? -1 : high > limit || (low & 0xffffffffu) ? 1 : 0;
    }
    return high || (low & 0xffffffffu) >= (uint64_t{1} << power) ? ((high == 0 && (low & 0xffffffffu) == (uint64_t{1} << power)) ? 0 : 1) : -1;
}

// The lifted side's function, in one of the ways of single precision, its float unit set for it while it runs (and put
// back for the reference, whose double precision wants the default).
void run_lifted(void (*fn)(Spu&), Spu& spu, fp::FloatMode mode) {
#if QILIN_SPU_HOST_SINGLE
    const fp::FloatControl saved = fp::float_control();
    fp::enter_single_mode(mode);
    fn(spu);
    fp::set_float_control(saved);
#else
    fp::enter_single_mode(mode);
    fn(spu);
#endif
}

// The relaxed way (spu_fp.hpp) against the exact one, as the top of spu_fp.hpp says it is: on ordinary numbers (normal
// ones whose results stay normal, and zeros and denormals, which are zero to both) the same bit for bit for an add,
// subtract, multiply and the compares, and for a fused form the same or, where the host has no fused multiply-add of its
// own, within two units of the last place of the larger of the product and the addend; and different, as it says, past
// the host's range. Returns how many broke that; `fused_differ` is how many fused results were not the same.
size_t check_relaxed(std::mt19937& rng, size_t count, size_t& fused_differ) {
    fused_differ = 0;
#if QILIN_SPU_HOST_SINGLE
    size_t broken = 0;
    const auto ordinary = [&]() -> uint32_t {
        const uint32_t sign = rng() & 0x80000000u, fraction = rng() & 0x007fffffu;
        if (rng() % 16 == 0) return sign | (rng() % 2 ? fraction : 0u);                          // a zero or a denormal
        return sign | ((70u + static_cast<uint32_t>(rng() % 115)) << 23) | fraction;  // exponents 70 to 184: products stay in range
    };
    // Each operation in a way: the float unit set for it, and put back after.
    struct Results {
        SpuReg add, subtract, multiply, fused[3], compares[4];
    };
    const auto run = [](fp::FloatMode mode, const SpuReg& a, const SpuReg& b, const SpuReg& c) {
        const fp::FloatControl saved = fp::float_control();
        fp::enter_single_mode(mode);
        const Results r{fp::fa(a, b), fp::fs(a, b), fp::fm(a, b), {fp::fma(a, b, c), fp::fnms(a, b, c), fp::fms(a, b, c)},
                        {fp::fceq(a, b), fp::fcgt(a, b), fp::fcmeq(a, b), fp::fcmgt(a, b)}};
        fp::set_float_control(saved);
        return r;
    };
    // (A single as a double, a zero exponent zero.)
    const auto value = [](uint32_t w) { return static_cast<double>(std::bit_cast<float>((w & 0x7f800000u) ? w : (w & 0x80000000u))); };
    for (size_t n = 0; n < count; ++n) {
        SpuReg a, b, c;
        for (unsigned i = 0; i < 4; ++i) a.w[i] = ordinary(), b.w[i] = ordinary(), c.w[i] = ordinary();
        // Now and then the same magnitudes, for the compares' equalities and the sums that cancel.
        if (rng() % 8 == 0) b.w[rng() % 4] = a.w[rng() % 4] ^ (rng() & 0x80000000u);
        if (rng() % 8 == 0) c.w[rng() % 4] = fp::multiply32(a.w[rng() % 4], b.w[rng() % 4]) ^ 0x80000000u;
        const Results exact = run(fp::FloatMode::whole, a, b, c), relaxed = run(fp::FloatMode::relaxed, a, b, c);
        for (unsigned i = 0; i < 4; ++i) {
            const auto same = [&](const char* what, const SpuReg& e, const SpuReg& r) {
                if (e.w[i] != r.w[i] && broken++ < 10) std::printf("FAIL relaxed %s of %08x %08x: exact %08x, relaxed %08x\n", what, a.w[i], b.w[i], e.w[i], r.w[i]);
            };
            same("fa", exact.add, relaxed.add);
            same("fs", exact.subtract, relaxed.subtract);
            same("fm", exact.multiply, relaxed.multiply);
            for (unsigned k = 0; k < 4; ++k) same("compare", exact.compares[k], relaxed.compares[k]);
            for (unsigned k = 0; k < 3; ++k) {
                if (exact.fused[k].w[i] == relaxed.fused[k].w[i]) continue;
                ++fused_differ;
                const double product = value(a.w[i]) * value(b.w[i]), addend = value(c.w[i]);  // (exact in double precision)
                int e = 0;
                std::frexp(std::fmax(std::fabs(product), std::fabs(addend)), &e);
                const double unit = std::ldexp(1.0, e - 24);  // the last place of a single as large
                if (std::fabs(value(exact.fused[k].w[i]) - value(relaxed.fused[k].w[i])) > 2 * unit && broken++ < 10)
                    std::printf("FAIL relaxed fused form %u of %08x %08x %08x: exact %08x, relaxed %08x\n", k, a.w[i], b.w[i], c.w[i], exact.fused[k].w[i],
                                relaxed.fused[k].w[i]);
            }
        }
    }
    // Where it differs, as the top of spu_fp.hpp says: past the host's range.
    struct Known {
        const char* what;
        uint32_t a, b, exact, relaxed;
        bool multiply;
    };
    static constexpr Known kKnown[] = {
        {"2^127 * 4, which the SPU saturates and the host stops at the largest float", 0x7f000000u, 0x40800000u, 0x7fffffffu, 0x7f7fffffu, true},
        {"2^128 * 0.5, a number to the SPU and infinity to the host", 0x7f800000u, 0x3f000000u, 0x7f000000u, 0x7f800000u, true},
        {"the largest float doubled, the SPU's next binade", 0x7f7fffffu, 0x7f7fffffu, 0x7fffffffu, 0x7f7fffffu, false},
    };
    for (const Known& k : kKnown) {
        const Results exact = run(fp::FloatMode::whole, fp::splat(k.a), fp::splat(k.b), fp::splat(0)),
                      relaxed = run(fp::FloatMode::relaxed, fp::splat(k.a), fp::splat(k.b), fp::splat(0));
        const uint32_t e = (k.multiply ? exact.multiply : exact.add).w[0], r = (k.multiply ? relaxed.multiply : relaxed.add).w[0];
        if ((e != k.exact || r != k.relaxed) && broken++ < 10) std::printf("FAIL relaxed, %s: exact %08x, relaxed %08x\n", k.what, e, r);
    }
    {  // A NaN to the host is a number to the SPU: 1.5 * 2^128 is more than 1 there, and nothing is to the host.
        const Results exact = run(fp::FloatMode::whole, fp::splat(0x7fc00000u), fp::splat(0x3f800000u), fp::splat(0)),
                      relaxed = run(fp::FloatMode::relaxed, fp::splat(0x7fc00000u), fp::splat(0x3f800000u), fp::splat(0));
        if ((exact.compares[1].w[0] != 0xffffffffu || relaxed.compares[1].w[0] != 0) && broken++ < 10) std::printf("FAIL relaxed fcgt of a NaN to the host\n");
    }
    return broken;
#else
    (void)rng, (void)count;
    return 0;
#endif
}

size_t check_estimates(std::mt19937& rng, size_t count, size_t& worst_reciprocal, size_t& worst_root) {
    constexpr uint32_t kOne = 0x3f800000u, kHalf = 0x3f000000u;
    size_t broken = 0;
    worst_reciprocal = worst_root = 0;
    for (size_t n = 0; n < count; ++n) {
        // x well inside the range, so that nothing in the sequence leaves it.
        const uint32_t x = (rng() & 0x007fffffu) | ((64 + rng() % 128) << 23);
        const uint64_t mx = (x & 0x7fffff) | 0x800000;
        const int ex = static_cast<int>(x >> 23) - 150;
        {
            // FREST y0,x; FI y1,x,y0; FNMS t1,x,y1,ONE; FMA y2,t1,y1,y1
            const uint32_t y0 = fp::reciprocal_estimate32(x), y1 = fp::interpolate32(x, y0), t1 = fp::fused32(x, y1, kOne, true, false), y2 = fp::fused32(t1, y1, y1, false, false);
            // Y: the largest single with x * Y < 1. From y2, step until it is.
            uint32_t Y = y2;
            const auto below_one = [&](uint32_t y) {  // x * y < 1: mantissas' product against 2^-(exponents)
                const uint64_t product = mx * ((y & 0x7fffff) | 0x800000);
                const int power = -(ex + static_cast<int>(y >> 23) - 150);
                return power >= 64 || (power >= 0 && product < (uint64_t{1} << power));
            };
            int steps = 0;
            while (!below_one(Y) && steps < 64) --Y, ++steps;
            while (below_one(Y + 1) && steps < 64) ++Y, ++steps;
            const size_t off = Y >= y2 ? Y - y2 : 2 + (y2 - Y);  // 0 or 1 below is within the bound; anything above is not
            if (off > 1) ++broken;
            if (off > worst_reciprocal) worst_reciprocal = off;
        }
        {
            // FRSQEST y0,x; FI y1,x,y0; FM t1,x,y1; FM t2,y1,HALF; FNMS t1,t1,y1,ONE; FMA y2,t1,t2,y1
            const uint32_t y0 = fp::reciprocal_root_estimate32(x), y1 = fp::interpolate32(x, y0);
            uint32_t t1 = fp::multiply32(x, y1);
            const uint32_t t2 = fp::multiply32(y1, kHalf);
            t1 = fp::fused32(t1, y1, kOne, true, false);
            const uint32_t y2 = fp::fused32(t1, t2, y1, false, false);
            uint32_t Y = y2;
            const auto below_one = [&](uint32_t y) {  // x * y * y < 1
                return compare_square((y & 0x7fffff) | 0x800000, mx, -(ex + 2 * (static_cast<int>(y >> 23) - 150))) < 0;
            };
            int steps = 0;
            while (!below_one(Y) && steps < 64) --Y, ++steps;
            while (below_one(Y + 1) && steps < 64) ++Y, ++steps;
            const size_t off = Y >= y2 ? Y - y2 : y2 - Y;
            if (off > 1) ++broken;
            if (off > worst_root) worst_root = off;
        }
    }
    return broken;
}

}  // namespace

int main(int argc, char** argv) {
    const int states = argc > 1 ? std::atoi(argv[1]) : 64;
    std::mt19937 rng(20061111);
    // Two local stores with the same random contents: the lifted code's in the host's word order (spu.hpp), the
    // reference's in the SPU's byte order.
    std::vector<uint8_t> ls_lifted(qilin::rt::kSpuLocalStore), ls_reference(qilin::rt::kSpuLocalStore);
    for (size_t i = 0; i < ls_reference.size(); ++i) ls_reference[i] = static_cast<uint8_t>(rng());
    const auto copy_reference = [&] { qilin::rt::spu::ls_write(ls_lifted.data(), 0, ls_reference.data(), ls_reference.size()); };
    const auto same_stores = [&] {
        for (uint32_t i = 0; i < qilin::rt::kSpuLocalStore; i += 4)
            if (qilin::rt::spu::ls_word(ls_lifted.data(), i) != ((uint32_t{ls_reference[i]} << 24) | (uint32_t{ls_reference[i + 1]} << 16) | (uint32_t{ls_reference[i + 2]} << 8) | ls_reference[i + 3]))
                return false;
        return true;
    };
    copy_reference();

    size_t disagreements = 0, runs = 0, stores = 0, halts = 0;
    // Both sides from the same random state.
    const auto seed = [&](Spu& spu, State& ref, bool is_float) {
        spu.ls = ls_lifted.data();
        spu.hooks = &kHooks;
        ref.ls = ls_reference.data();
        for (unsigned r = 0; r < 128; ++r)
            for (unsigned w = 0; w < 4; ++w) {
                const uint32_t v = is_float ? single_like(rng) : interesting(rng);
                spu.r[r].w[w] = v;
                for (unsigned b = 0; b < 4; ++b) ref.r[r][4 * w + b] = static_cast<uint8_t>(v >> (24 - 8 * b));
            }
        for (unsigned w = 0; w < 4; ++w) {  // the status register: any bits, mostly round to nearest
            const uint32_t v = static_cast<uint32_t>(rng()) & (rng() % 4 == 0 ? ~0u : ~0xF00u);
            spu.fpscr[w] = v;
            for (unsigned b = 0; b < 4; ++b) ref.fpscr[4 * w + b] = static_cast<uint8_t>(v >> (24 - 8 * b));
        }
        g_seen = Seen{};
    };
    // Everything the two sides can differ in, each difference to `fail`.
    const auto compare = [&](const Spu& spu, const State& ref, bool stored, const auto& fail) {
        for (unsigned r = 0; r < 128; ++r)
            for (unsigned w = 0; w < 4; ++w) {
                const uint32_t reference = (uint32_t{ref.r[r][4 * w]} << 24) | (uint32_t{ref.r[r][4 * w + 1]} << 16) | (uint32_t{ref.r[r][4 * w + 2]} << 8) | ref.r[r][4 * w + 3];
                if (spu.r[r].w[w] != reference) {
                    char what[32];
                    std::snprintf(what, sizeof what, "$%u word %u", r, w);
                    fail(what, spu.r[r].w[w], reference);
                }
            }
        if (g_seen.wrote_channel != ref.wrote_channel || g_seen.channel != ref.channel || g_seen.channel_data != ref.channel_data)
            fail("a channel's write", (static_cast<unsigned long long>(g_seen.channel) << 32) | g_seen.channel_data, (static_cast<unsigned long long>(ref.channel) << 32) | ref.channel_data);
        for (unsigned w = 0; w < 4; ++w) {
            const uint32_t reference = (uint32_t{ref.fpscr[4 * w]} << 24) | (uint32_t{ref.fpscr[4 * w + 1]} << 16) | (uint32_t{ref.fpscr[4 * w + 2]} << 8) | ref.fpscr[4 * w + 3];
            if (spu.fpscr[w] != reference) fail("the FPSCR", spu.fpscr[w], reference);
        }
        if (g_seen.stopped != ref.stopped || g_seen.stop_code != ref.stop_code) fail("a stop", g_seen.stop_code, ref.stop_code);
        if (g_seen.halted != ref.halted) fail("a halt", g_seen.halted, ref.halted);
        halts += ref.halted;
        if (stored) {
            ++stores;
            if (!same_stores()) {
                fail("the local store", 0, ref.stored_at);
                copy_reference();
            }
        }
    };
    for (size_t k = 0; k < kCaseCount; ++k) {
        const Case& c = kCases[k];
        const qilin::spu::Insn in = qilin::spu::decode(c.word);
        const bool is_float = (qilin::spu::spec(in.op).flags & qilin::spu::kFloat) != 0;
        for (int n = 0; n < (is_float ? 8 * states : states); ++n) {
            Spu spu;
            State ref{};
            seed(spu, ref, is_float);
            run_lifted(c.fn, spu, n & 1 ? fp::FloatMode::exact : fp::FloatMode::whole);
            if (!step(ref, in, c.pc)) {
                std::printf("the reference doesn't know %s (%08x)\n", qilin::spu::disasm(in, c.pc).c_str(), c.word);
                return 1;
            }
            ++runs;
            compare(spu, ref, ref.stored || (qilin::spu::spec(in.op).flags & qilin::spu::kStore), [&](const char* what, unsigned long long lifted, unsigned long long reference) {
                if (disagreements++ < 20)
                    std::printf("FAIL %08x %s at %05x: %s: lifted %#llx, reference %#llx\n", c.word, qilin::spu::disasm(in, c.pc).c_str(), c.pc, what, lifted, reference);
            });
        }
    }
    // The programs: whole lifted functions (a loop, branches over code, a call, conditional returns), against the
    // reference stepping through their words.
    size_t program_runs = 0;
    for (size_t k = 0; k < kProgramCount; ++k) {
        const Program& p = kPrograms[k];
        for (int n = 0; n < states; ++n) {
            Spu spu;
            State ref{};
            seed(spu, ref, false);
            run_lifted(p.fn, spu, n & 1 ? fp::FloatMode::exact : fp::FloatMode::whole);
            if (!run_program(ref, p.words, p.count)) {
                std::printf("the reference can't run program %zu to its end\n", k);
                return 1;
            }
            ++program_runs;
            compare(spu, ref, true, [&](const char* what, unsigned long long lifted, unsigned long long reference) {
                if (disagreements++ < 20) std::printf("FAIL program %zu (run %d): %s: lifted %#llx, reference %#llx\n", k, n, what, lifted, reference);
            });
        }
    }
    size_t fused_differ = 0;
    const size_t relaxed_broken = check_relaxed(rng, 1000000, fused_differ);
    if (relaxed_broken) {
        std::printf("spu difftest: the relaxed way breaks its bounds %zu times\n", relaxed_broken);
        return 1;
    }
#if QILIN_SPU_HOST_SINGLE
    std::printf("spu difftest: the relaxed way keeps its bounds over 1000000 vectors of ordinary numbers (%zu fused results not the same, all within two units), "
                "and differs where it says\n",
                fused_differ);
#else
    std::printf("spu difftest: no host float unit for single precision here: every way is the whole numbers'\n");
#endif
    size_t worst_reciprocal = 0, worst_root = 0;
    const size_t broken = check_estimates(rng, 2000000, worst_reciprocal, worst_root);
    if (broken) {
        std::printf("spu difftest: the estimates break the instruction set's bound for %zu of 2000000 numbers (worst: %zu and %zu units)\n", broken, worst_reciprocal, worst_root);
        return 1;
    }
    std::printf("spu difftest: the estimates keep the instruction set's bound over 2000000 numbers (furthest: %zu and %zu units)\n", worst_reciprocal, worst_root);
    if (disagreements) {
        std::printf("spu difftest: %zu disagreements\n", disagreements);
        return 1;
    }
    std::printf("spu difftest: %zu cases, %zu runs, %zu programs, %zu runs of them, all agree (%zu stores, %zu halts fired)\n", kCaseCount, runs, kProgramCount, program_runs,
                stores, halts);
    return 0;
}
