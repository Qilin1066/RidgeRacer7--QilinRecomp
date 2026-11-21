// qilin_spu_difftest: every case the generator wrote, run from many states
// as lifted code and by the reference, and compared: all 128 registers, the
// quadword stored (and, for a store, the whole local store), what was
// written to a channel, a stop's code, whether a halt fired.
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

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
};

// A word that is often something an instruction treats specially.
uint32_t interesting(std::mt19937& rng) {
    static constexpr uint32_t kSpecial[] = {0, 1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff,
                                            0x10000, 0x7fffffff, 0x80000000u, 0xffffffffu, 0xfffffff0u, 0xffffffe0u, 0x80808080u, 0xc0c0c0c0u, 0xe0e0e0e0u, 0x00010203u};
    switch (rng() % 4) {
    case 0: return kSpecial[rng() % std::size(kSpecial)];
    case 1: return rng() & 0x3f;  // a count, or a small number
    default: return rng();
    }
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
        for (int n = 0; n < states; ++n) {
            Spu spu;
            spu.ls = ls_lifted.data();
            spu.hooks = &kHooks;
            State ref{};
            ref.ls = ls_reference.data();
            for (unsigned r = 0; r < 128; ++r)
                for (unsigned w = 0; w < 4; ++w) {
                    const uint32_t v = interesting(rng);
                    spu.r[r].w[w] = v;
                    for (unsigned b = 0; b < 4; ++b) ref.r[r][4 * w + b] = static_cast<uint8_t>(v >> (24 - 8 * b));
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
    if (disagreements) {
        std::printf("spu difftest: %zu disagreements\n", disagreements);
        return 1;
    }
    std::printf("spu difftest: %zu cases, %zu runs, all agree (%zu stores, %zu halts fired)\n", kCaseCount, runs, stores, halts);
    return 0;
}
