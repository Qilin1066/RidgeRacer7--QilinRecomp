// qilin_spu_difftest: every case the generator wrote, run from many states
// as lifted code and by the reference, and compared: all 128 registers, the
// quadword stored (and, for a store, the whole local store), what was
// written to a channel, a stop's code, whether a halt fired.
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
    // Two local stores with the same random contents.
    std::vector<uint8_t> ls_lifted(qilin::rt::kSpuLocalStore), ls_reference(qilin::rt::kSpuLocalStore);
    for (size_t i = 0; i < ls_lifted.size(); i += 4) {
        const uint32_t v = rng();
        std::memcpy(&ls_lifted[i], &v, 4);
    }
    ls_reference = ls_lifted;

    size_t disagreements = 0, runs = 0, stores = 0, halts = 0;
    for (size_t k = 0; k < kCaseCount; ++k) {
        const Case& c = kCases[k];
        const qilin::spu::Insn in = qilin::spu::decode(c.word);
        const bool is_float = (qilin::spu::spec(in.op).flags & qilin::spu::kFloat) != 0;
        for (int n = 0; n < (is_float ? 4 * states : states); ++n) {
            Spu spu;
            spu.ls = ls_lifted.data();
            spu.hooks = &kHooks;
            State ref{};
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
            c.fn(spu);
            if (!step(ref, in, c.pc)) {
                std::printf("the reference doesn't know %s (%08x)\n", qilin::spu::disasm(in, c.pc).c_str(), c.word);
                return 1;
            }
            ++runs;
            const auto fail = [&](const char* what, unsigned long long lifted, unsigned long long reference) {
                if (disagreements++ < 20)
                    std::printf("FAIL %08x %s at %05x: %s: lifted %#llx, reference %#llx\n", c.word, qilin::spu::disasm(in, c.pc).c_str(), c.pc, what, lifted, reference);
            };
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
            if (ref.stored || (qilin::spu::spec(in.op).flags & qilin::spu::kStore)) {
                ++stores;
                if (std::memcmp(ls_lifted.data(), ls_reference.data(), ls_lifted.size()) != 0) {
                    fail("the local store", 0, ref.stored_at);
                    ls_lifted = ls_reference;
                }
            }
        }
    }
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
    std::printf("spu difftest: %zu cases, %zu runs, all agree (%zu stores, %zu halts fired)\n", kCaseCount, runs, stores, halts);
    return 0;
}
