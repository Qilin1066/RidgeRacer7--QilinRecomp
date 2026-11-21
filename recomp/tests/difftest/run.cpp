// qilin_difftest: every generated case, run lifted and on the reference
// interpreter from the same random states; registers, CR, XER, LR, CTR,
// traps, indirect jumps, system calls and memory must all agree.
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "cases.hpp"
#include "ppu.hpp"
#include "qilin/rt/memory.hpp"
#include "ref.hpp"

using namespace qilin;
using difftest::Case;
using difftest::Kind;

namespace {

constexpr uint32_t kWin = 0x30100000u, kWinSize = 0x2000u;  // the memory tests touch
constexpr int kTrials = 48;

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
const rt::CpuHooks kHooks{on_call, on_system_call, on_trap};

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

void compare(const Case& c, const rt::Cpu& cpu, const ref::State& s, const std::vector<uint8_t>& lifted_memory, const uint8_t* reference_memory) {
    for (unsigned i = 0; i < 32; ++i)
        if (cpu.r[i] != s.gpr[i]) fail(c, std::format("r{}: lifted {:#x}, reference {:#x}", i, cpu.r[i], s.gpr[i]));
    if (cpu.cr != s.cr) fail(c, std::format("CR: lifted {:#x}, reference {:#x}", cpu.cr, s.cr));
    if (cpu.xer() != s.xer) fail(c, std::format("XER: lifted {:#x}, reference {:#x}", cpu.xer(), s.xer));
    if (cpu.lr != s.lr) fail(c, std::format("LR: lifted {:#x}, reference {:#x}", cpu.lr, s.lr));
    if (cpu.ctr != s.ctr) fail(c, std::format("CTR: lifted {:#x}, reference {:#x}", cpu.ctr, s.ctr));
    if (std::memcmp(lifted_memory.data(), reference_memory, kWinSize) != 0) fail(c, "memory differs");
}

}  // namespace

int main() {
    rt::AddressSpace space;
    if (!space.base() || !space.commit(kWin, kWinSize)) {
        std::println(stderr, "difftest: can't reserve the guest's address space");
        return 1;
    }
    uint8_t* const window = space.base() + kWin;
    std::vector<uint8_t> before(kWinSize), lifted_memory(kWinSize);
    size_t traps = 0, taken = 0, runs = 0;

    for (size_t k = 0; k < difftest::kCaseCount; ++k) {
        const Case& c = difftest::kCases[k];
        const ppu::Insn in = ppu::decode(c.word);
        const ppu::Spec& spec = ppu::spec(in.op);
        for (int trial = 0; trial < kTrials; ++trial, ++runs) {
            rt::Cpu cpu;
            cpu.mem = space.base();
            cpu.hooks = &kHooks;
            for (uint64_t& r : cpu.r) r = interesting();
            cpu.cr = static_cast<uint32_t>(rng());
            cpu.lr = interesting();
            cpu.ctr = trial % 4 == 0 ? 1 : trial % 4 == 1 ? 0 : interesting();  // CTR - 1 == 0, and its wrap, come up often
            cpu.xer_so = rng() & 1;
            cpu.xer_ov = rng() & 1;
            cpu.xer_ca = rng() & 1;
            cpu.xer_bc = rng() & 0x7f;
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
            if (c.kind == Kind::Switch) cpu.ctr = trial % 3 == 0 ? c.pc + 16 : trial % 3 == 1 ? c.pc + 24 : interesting();
            if (c.kind == Kind::Bc || c.kind == Kind::Bclr || c.kind == Kind::Bcctr || c.kind == Kind::Trap) cpu.r[3] = 7;

            ref::State s;
            for (unsigned i = 0; i < 32; ++i) s.gpr[i] = cpu.r[i];
            s.cr = cpu.cr;
            s.xer = cpu.xer();
            s.lr = cpu.lr;
            s.ctr = cpu.ctr;
            s.mem = space.base();

            // ---- lifted ----
            std::memcpy(window, before.data(), kWinSize);
            g_did_call = g_did_system_call = false;
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
            case Kind::Plain:
                if (!ref::step(s, in)) {
                    fail(c, "the reference doesn't know this instruction");
                    trial = kTrials;
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
            else if (expect_system_call && g_system_call_pc != c.pc) fail(c, "system call's address");
            compare(c, cpu, s, lifted_memory, window);
        }
    }
    if (failures) {
        std::println(stderr, "difftest: {} disagreements", failures);
        return 1;
    }
    std::println("difftest: {} cases, {} runs, all agree ({} branches taken, {} traps fired)", difftest::kCaseCount, runs, taken, traps);
    return 0;
}
