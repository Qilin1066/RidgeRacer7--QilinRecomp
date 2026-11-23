// The SPU's state and the helpers lifted SPU code calls.
//
// An SPU has 128 registers of 128 bits and 256 KiB of memory of its own
// (the local store), and reaches the rest of the machine only through
// channels and the stop instruction. A register is four words, word 0
// first (the "preferred slot", which scalar code uses); bytes and
// halfwords are numbered from the most significant end, as the SPU
// numbers them. Everything is plain portable C++ on fixed-size arrays.
//
// Each helper is one instruction's meaning, written from the SPU's
// instruction set as published (IBM's "SPU Instruction Set Architecture").
// The integer part is here; floating point is in spu_fp.hpp.
//
// Runtime code: no std::format here.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#define QILIN_SPU_INLINE __forceinline
#else
#define QILIN_SPU_INLINE inline __attribute__((always_inline))
#endif

namespace qilin::rt {

struct Spu;

// Where lifted SPU code leaves what was known at build time.
struct SpuHooks {
    uint32_t (*read_channel)(Spu&, uint32_t channel, uint32_t pc);               // rdch
    void (*write_channel)(Spu&, uint32_t channel, uint32_t value, uint32_t pc);  // wrch
    uint32_t (*channel_count)(Spu&, uint32_t channel, uint32_t pc);              // rchcnt
    void (*stop)(Spu&, uint32_t code, uint32_t pc);  // stop and signal; if it returns, the program goes on
    void (*halt)(Spu&, uint32_t pc, const char* why);  // a halt instruction that fired, or code that can't go on; must not return
    // A call through a register to an address that is no function of the program's (a system's code in its memory, such as
    // SPURS's kernel): the runtime's to answer as a function would ($3 on), or to halt.
    void (*native_call)(Spu&, uint32_t target, uint32_t pc);
};

struct SpuReg {
    uint32_t w[4];
};

inline constexpr uint32_t kSpuLocalStore = 0x40000;

struct Spu {
    SpuReg r[128] = {};
    uint8_t* ls = nullptr;  // the local store: kSpuLocalStore bytes, in the SPU's byte order
    uint32_t fpscr[4] = {};
    const SpuHooks* hooks = nullptr;
    void* owner = nullptr;  // the runtime's own object for this SPU
};

namespace spu {

// ---- elements ----
// The floating-point status and control register: the bits the instruction set defines, word by word (bits 20-23 the
// double-precision rounding modes, 29-31, 50-55, 61-63, 82-87, 93-95, 116-119 and 125-127 sticky flags). fscrwr keeps
// only those (the others are undefined: zero, *a stated choice*), fscrrd reads them. *The flags are only what was
// written: the runtime doesn't keep them as arithmetic would set them, a stated choice.*
inline constexpr uint32_t kFpscrDefined[4] = {0x00000F07u, 0x00003F07u, 0x00003F07u, 0x00000F07u};
QILIN_SPU_INLINE void fscrwr(Spu& s, const SpuReg& a) {
    for (unsigned i = 0; i < 4; ++i) s.fpscr[i] = a.w[i] & kFpscrDefined[i];
}
QILIN_SPU_INLINE SpuReg fscrrd(const Spu& s) {
    SpuReg v;
    for (unsigned i = 0; i < 4; ++i) v.w[i] = s.fpscr[i] & kFpscrDefined[i];
    return v;
}

QILIN_SPU_INLINE uint32_t byte(const SpuReg& v, unsigned i) { return (v.w[i >> 2] >> (24 - 8 * (i & 3))) & 0xffu; }
QILIN_SPU_INLINE void set_byte(SpuReg& v, unsigned i, uint32_t b) {
    const unsigned shift = 24 - 8 * (i & 3);
    v.w[i >> 2] = (v.w[i >> 2] & ~(0xffu << shift)) | ((b & 0xffu) << shift);
}
// Each word as its two halfwords, the upper first, through `f(halfword)`.
template <typename F>
QILIN_SPU_INLINE SpuReg halves(const SpuReg& a, F f) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = ((f(a.w[i] >> 16) & 0xffffu) << 16) | (f(a.w[i] & 0xffffu) & 0xffffu);
    return r;
}
template <typename F>
QILIN_SPU_INLINE SpuReg halves(const SpuReg& a, const SpuReg& b, F f) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = ((f(a.w[i] >> 16, b.w[i] >> 16) & 0xffffu) << 16) | (f(a.w[i] & 0xffffu, b.w[i] & 0xffffu) & 0xffffu);
    return r;
}
template <typename F>
QILIN_SPU_INLINE SpuReg words(const SpuReg& a, F f) {
    return {{f(a.w[0]), f(a.w[1]), f(a.w[2]), f(a.w[3])}};
}
template <typename F>
QILIN_SPU_INLINE SpuReg words(const SpuReg& a, const SpuReg& b, F f) {
    return {{f(a.w[0], b.w[0]), f(a.w[1], b.w[1]), f(a.w[2], b.w[2]), f(a.w[3], b.w[3])}};
}
template <typename F>
QILIN_SPU_INLINE SpuReg bytes(const SpuReg& a, const SpuReg& b, F f) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4; ++k) {
            const unsigned shift = 24 - 8 * k;
            w |= (f((a.w[i] >> shift) & 0xffu, (b.w[i] >> shift) & 0xffu) & 0xffu) << shift;
        }
        r.w[i] = w;
    }
    return r;
}
QILIN_SPU_INLINE SpuReg splat(uint32_t word) { return {{word, word, word, word}}; }
QILIN_SPU_INLINE uint32_t sext16(uint32_t h) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h))); }

// ---- the local store ----
// A quadword's address: within the local store, at a multiple of 16.
QILIN_SPU_INLINE uint32_t quad_address(uint32_t a) { return a & 0x3fff0u; }
QILIN_SPU_INLINE SpuReg load(const Spu& s, uint32_t address) {
    const uint8_t* const p = s.ls + quad_address(address);
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t w;
        std::memcpy(&w, p + 4 * i, 4);
        r.w[i] = std::endian::native == std::endian::little ? std::byteswap(w) : w;
    }
    return r;
}
QILIN_SPU_INLINE void store(Spu& s, uint32_t address, const SpuReg& v) {
    uint8_t* const p = s.ls + quad_address(address);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t w = std::endian::native == std::endian::little ? std::byteswap(v.w[i]) : v.w[i];
        std::memcpy(p + 4 * i, &w, 4);
    }
}

// ---- constants ----
// fsmbi: each of 16 bits makes a byte of ones or zeros, the top bit the first byte.
QILIN_SPU_INLINE SpuReg fsmbi(uint32_t bits) {
    SpuReg r{};
    for (unsigned i = 0; i < 16; ++i)
        if (bits & (0x8000u >> i)) set_byte(r, i, 0xff);
    return r;
}
QILIN_SPU_INLINE SpuReg fsmb(const SpuReg& a) { return fsmbi(a.w[0] & 0xffffu); }
QILIN_SPU_INLINE SpuReg fsmh(const SpuReg& a) {
    SpuReg r{};
    for (unsigned i = 0; i < 8; ++i)
        if (a.w[0] & (0x80u >> i)) r.w[i >> 1] |= 0xffffu << (16 - 16 * (i & 1));
    return r;
}
QILIN_SPU_INLINE SpuReg fsm(const SpuReg& a) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (a.w[0] & (8u >> i)) ? 0xffffffffu : 0u;
    return r;
}
// gbb, gbh, gb: the lowest bit of each byte, halfword or word, gathered into the preferred word.
QILIN_SPU_INLINE SpuReg gbb(const SpuReg& a) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 16; ++i) bits |= (byte(a, i) & 1u) << (15 - i);
    return {{bits, 0, 0, 0}};
}
QILIN_SPU_INLINE SpuReg gbh(const SpuReg& a) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= ((a.w[i >> 1] >> (16 - 16 * (i & 1))) & 1u) << (7 - i);
    return {{bits, 0, 0, 0}};
}
QILIN_SPU_INLINE SpuReg gb(const SpuReg& a) { return {{((a.w[0] & 1u) << 3) | ((a.w[1] & 1u) << 2) | ((a.w[2] & 1u) << 1) | (a.w[3] & 1u), 0, 0, 0}}; }

// ---- arithmetic ----
QILIN_SPU_INLINE SpuReg a(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p + q; }); }
QILIN_SPU_INLINE SpuReg ah(const SpuReg& x, const SpuReg& y) { return halves(x, y, [](uint32_t p, uint32_t q) { return p + q; }); }
// sf, sfh: "subtract from": b - a.
QILIN_SPU_INLINE SpuReg sf(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return q - p; }); }
QILIN_SPU_INLINE SpuReg sfh(const SpuReg& x, const SpuReg& y) { return halves(x, y, [](uint32_t p, uint32_t q) { return q - p; }); }
// addx, sfx: with the carry in the lowest bit of the target's old value. cg, bg, cgx, bgx: the carry out, or "no borrow".
QILIN_SPU_INLINE SpuReg addx(const SpuReg& x, const SpuReg& y, const SpuReg& t) {
    return {{x.w[0] + y.w[0] + (t.w[0] & 1u), x.w[1] + y.w[1] + (t.w[1] & 1u), x.w[2] + y.w[2] + (t.w[2] & 1u), x.w[3] + y.w[3] + (t.w[3] & 1u)}};
}
QILIN_SPU_INLINE SpuReg sfx(const SpuReg& x, const SpuReg& y, const SpuReg& t) {
    return {{y.w[0] + ~x.w[0] + (t.w[0] & 1u), y.w[1] + ~x.w[1] + (t.w[1] & 1u), y.w[2] + ~x.w[2] + (t.w[2] & 1u), y.w[3] + ~x.w[3] + (t.w[3] & 1u)}};
}
QILIN_SPU_INLINE SpuReg cg(const SpuReg& x, const SpuReg& y) {
    return words(x, y, [](uint32_t p, uint32_t q) { return static_cast<uint32_t>((uint64_t{p} + q) >> 32); });
}
QILIN_SPU_INLINE SpuReg bg(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p <= q ? 1u : 0u; }); }
QILIN_SPU_INLINE SpuReg cgx(const SpuReg& x, const SpuReg& y, const SpuReg& t) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = static_cast<uint32_t>((uint64_t{x.w[i]} + y.w[i] + (t.w[i] & 1u)) >> 32);
    return r;
}
QILIN_SPU_INLINE SpuReg bgx(const SpuReg& x, const SpuReg& y, const SpuReg& t) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = static_cast<uint32_t>((uint64_t{y.w[i]} + static_cast<uint32_t>(~x.w[i]) + (t.w[i] & 1u)) >> 32);
    return r;
}
// The multiplies: of 16-bit halves of each word.
QILIN_SPU_INLINE SpuReg mpy(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return sext16(p) * sext16(q); }); }
QILIN_SPU_INLINE SpuReg mpyu(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return (p & 0xffffu) * (q & 0xffffu); }); }
QILIN_SPU_INLINE SpuReg mpyh(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return ((p >> 16) * (q & 0xffffu)) << 16; }); }
QILIN_SPU_INLINE SpuReg mpys(const SpuReg& x, const SpuReg& y) {
    return words(x, y, [](uint32_t p, uint32_t q) { return static_cast<uint32_t>(static_cast<int32_t>(sext16(p) * sext16(q)) >> 16); });
}
QILIN_SPU_INLINE SpuReg mpyhh(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return sext16(p >> 16) * sext16(q >> 16); }); }
QILIN_SPU_INLINE SpuReg mpyhhu(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return (p >> 16) * (q >> 16); }); }
// clz per word, the ones in each byte, averages and differences of bytes, sums of bytes.
QILIN_SPU_INLINE SpuReg clz(const SpuReg& x) { return words(x, [](uint32_t p) { return static_cast<uint32_t>(std::countl_zero(p)); }); }
QILIN_SPU_INLINE SpuReg cntb(const SpuReg& x) {
    return bytes(x, x, [](uint32_t p, uint32_t) { return static_cast<uint32_t>(std::popcount(p)); });
}
QILIN_SPU_INLINE SpuReg avgb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return (p + q + 1) >> 1; }); }
QILIN_SPU_INLINE SpuReg absdb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return p > q ? p - q : q - p; }); }
// sumb: of each word, the sum of b's four bytes in the upper halfword and of a's in the lower.
QILIN_SPU_INLINE SpuReg sumb(const SpuReg& x, const SpuReg& y) {
    return words(x, y, [](uint32_t p, uint32_t q) {
        const auto sum = [](uint32_t v) { return (v >> 24) + ((v >> 16) & 0xffu) + ((v >> 8) & 0xffu) + (v & 0xffu); };
        return (sum(q) << 16) | sum(p);
    });
}
// Sign extensions: of the lower byte of each halfword, halfword of each word, word of each doubleword.
QILIN_SPU_INLINE SpuReg xsbh(const SpuReg& x) {
    return halves(x, [](uint32_t h) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(h))); });
}
QILIN_SPU_INLINE SpuReg xshw(const SpuReg& x) { return words(x, [](uint32_t p) { return sext16(p); }); }
QILIN_SPU_INLINE SpuReg xswd(const SpuReg& x) {
    return {{static_cast<uint32_t>(static_cast<int32_t>(x.w[1]) >> 31), x.w[1], static_cast<uint32_t>(static_cast<int32_t>(x.w[3]) >> 31), x.w[3]}};
}

// ---- logic ----
QILIN_SPU_INLINE SpuReg and_(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p & q; }); }
QILIN_SPU_INLINE SpuReg andc(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p & ~q; }); }
QILIN_SPU_INLINE SpuReg or_(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p | q; }); }
QILIN_SPU_INLINE SpuReg orc(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p | ~q; }); }
QILIN_SPU_INLINE SpuReg xor_(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p ^ q; }); }
QILIN_SPU_INLINE SpuReg nand(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return ~(p & q); }); }
QILIN_SPU_INLINE SpuReg nor(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return ~(p | q); }); }
QILIN_SPU_INLINE SpuReg eqv(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return ~(p ^ q); }); }
QILIN_SPU_INLINE SpuReg orx(const SpuReg& x) { return {{x.w[0] | x.w[1] | x.w[2] | x.w[3], 0, 0, 0}}; }
// selb: each bit from b where the mask c is set, from a where it is clear.
QILIN_SPU_INLINE SpuReg selb(const SpuReg& x, const SpuReg& y, const SpuReg& c) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (c.w[i] & y.w[i]) | (~c.w[i] & x.w[i]);
    return r;
}
// shufb: each byte of c picks a byte of a (0-15) or b (16-31), or is a constant: 10xxxxxx 0x00, 110xxxxx 0xff, 111xxxxx 0x80.
QILIN_SPU_INLINE SpuReg shufb(const SpuReg& x, const SpuReg& y, const SpuReg& c) {
    SpuReg r{};
    for (unsigned i = 0; i < 16; ++i) {
        const uint32_t pick = byte(c, i);
        uint32_t value;
        if ((pick & 0xc0u) == 0x80u) value = 0x00;
        else if ((pick & 0xe0u) == 0xc0u) value = 0xff;
        else if ((pick & 0xe0u) == 0xe0u) value = 0x80;
        else value = (pick & 0x10u) ? byte(y, pick & 0xfu) : byte(x, pick & 0xfu);
        set_byte(r, i, value);
    }
    return r;
}

// ---- shifts and rotates of elements ----
// The count is each element's own in the register forms (`n` is then that register), or the same constant splatted.
QILIN_SPU_INLINE SpuReg shlh(const SpuReg& x, const SpuReg& n) {
    return halves(x, n, [](uint32_t p, uint32_t c) { return (c & 0x1fu) > 15 ? 0u : p << (c & 0x1fu); });
}
QILIN_SPU_INLINE SpuReg shl(const SpuReg& x, const SpuReg& n) {
    return words(x, n, [](uint32_t p, uint32_t c) { return (c & 0x3fu) > 31 ? 0u : p << (c & 0x3fu); });
}
QILIN_SPU_INLINE SpuReg roth(const SpuReg& x, const SpuReg& n) {
    return halves(x, n, [](uint32_t p, uint32_t c) {
        const uint32_t by = c & 0xfu;
        return by ? (p << by) | ((p & 0xffffu) >> (16 - by)) : p;
    });
}
QILIN_SPU_INLINE SpuReg rot(const SpuReg& x, const SpuReg& n) { return words(x, n, [](uint32_t p, uint32_t c) { return std::rotl(p, static_cast<int>(c & 0x1fu)); }); }
// rothm, rotm: a logical shift right by the negative of the count. rotmah, rotma: an arithmetic one.
QILIN_SPU_INLINE SpuReg rothm(const SpuReg& x, const SpuReg& n) {
    return halves(x, n, [](uint32_t p, uint32_t c) {
        const uint32_t by = (0u - c) & 0x1fu;
        return by > 15 ? 0u : p >> by;
    });
}
QILIN_SPU_INLINE SpuReg rotm(const SpuReg& x, const SpuReg& n) {
    return words(x, n, [](uint32_t p, uint32_t c) {
        const uint32_t by = (0u - c) & 0x3fu;
        return by > 31 ? 0u : p >> by;
    });
}
QILIN_SPU_INLINE SpuReg rotmah(const SpuReg& x, const SpuReg& n) {
    return halves(x, n, [](uint32_t p, uint32_t c) {
        const uint32_t by = (0u - c) & 0x1fu;
        return static_cast<uint32_t>(static_cast<int32_t>(sext16(p)) >> (by > 15 ? 15 : by));
    });
}
QILIN_SPU_INLINE SpuReg rotma(const SpuReg& x, const SpuReg& n) {
    return words(x, n, [](uint32_t p, uint32_t c) {
        const uint32_t by = (0u - c) & 0x3fu;
        return static_cast<uint32_t>(static_cast<int32_t>(p) >> (by > 31 ? 31 : by));
    });
}

// ---- shifts and rotates of the whole register ----
// Left by bits (0 to 7), with what leaves the top lost (shift) or brought round (rotate).
QILIN_SPU_INLINE SpuReg shift_left_bits(const SpuReg& x, unsigned bits, bool rotate) {
    if (bits == 0) return x;
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t next = i < 3 ? x.w[i + 1] : (rotate ? x.w[0] : 0u);
        r.w[i] = (x.w[i] << bits) | (next >> (32 - bits));
    }
    return r;
}
QILIN_SPU_INLINE SpuReg shift_right_bits(const SpuReg& x, unsigned bits) {
    if (bits == 0) return x;
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (x.w[i] >> bits) | (i > 0 ? x.w[i - 1] << (32 - bits) : 0u);
    return r;
}
// Left by bytes: 16 or more leaves nothing of a shift; a rotate's count is 0 to 15.
QILIN_SPU_INLINE SpuReg shift_left_bytes(const SpuReg& x, unsigned count, bool rotate) {
    SpuReg r{};
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned from = i + count;
        if (rotate) set_byte(r, i, byte(x, from & 15u));
        else if (from < 16) set_byte(r, i, byte(x, from));
    }
    return r;
}
QILIN_SPU_INLINE SpuReg shift_right_bytes(const SpuReg& x, unsigned count) {
    SpuReg r{};
    for (unsigned i = count; i < 16; ++i) set_byte(r, i, byte(x, i - count));
    return r;
}
QILIN_SPU_INLINE SpuReg shlqbi(const SpuReg& x, uint32_t n) { return shift_left_bits(x, n & 7u, false); }
QILIN_SPU_INLINE SpuReg shlqby(const SpuReg& x, uint32_t n) { return shift_left_bytes(x, n & 0x1fu, false); }
QILIN_SPU_INLINE SpuReg shlqbybi(const SpuReg& x, uint32_t n) { return shift_left_bytes(x, (n >> 3) & 0x1fu, false); }
QILIN_SPU_INLINE SpuReg rotqbi(const SpuReg& x, uint32_t n) { return shift_left_bits(x, n & 7u, true); }
QILIN_SPU_INLINE SpuReg rotqby(const SpuReg& x, uint32_t n) { return shift_left_bytes(x, n & 0xfu, true); }
QILIN_SPU_INLINE SpuReg rotqbybi(const SpuReg& x, uint32_t n) { return shift_left_bytes(x, (n >> 3) & 0xfu, true); }
QILIN_SPU_INLINE SpuReg rotqmbi(const SpuReg& x, uint32_t n) { return shift_right_bits(x, (0u - n) & 7u); }
QILIN_SPU_INLINE SpuReg rotqmby(const SpuReg& x, uint32_t n) { return shift_right_bytes(x, (0u - n) & 0x1fu); }
QILIN_SPU_INLINE SpuReg rotqmbybi(const SpuReg& x, uint32_t n) { return shift_right_bytes(x, (0u - (n >> 3)) & 0x1fu); }

// ---- compares: all ones where true ----
QILIN_SPU_INLINE SpuReg ceq(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffffffffu : 0u; }); }
QILIN_SPU_INLINE SpuReg cgt(const SpuReg& x, const SpuReg& y) {
    return words(x, y, [](uint32_t p, uint32_t q) { return static_cast<int32_t>(p) > static_cast<int32_t>(q) ? 0xffffffffu : 0u; });
}
QILIN_SPU_INLINE SpuReg clgt(const SpuReg& x, const SpuReg& y) { return words(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffffffffu : 0u; }); }
QILIN_SPU_INLINE SpuReg ceqh(const SpuReg& x, const SpuReg& y) { return halves(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffffu : 0u; }); }
QILIN_SPU_INLINE SpuReg cgth(const SpuReg& x, const SpuReg& y) {
    return halves(x, y, [](uint32_t p, uint32_t q) { return static_cast<int16_t>(p) > static_cast<int16_t>(q) ? 0xffffu : 0u; });
}
QILIN_SPU_INLINE SpuReg clgth(const SpuReg& x, const SpuReg& y) { return halves(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffffu : 0u; }); }
QILIN_SPU_INLINE SpuReg ceqb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffu : 0u; }); }
QILIN_SPU_INLINE SpuReg cgtb(const SpuReg& x, const SpuReg& y) {
    return bytes(x, y, [](uint32_t p, uint32_t q) { return static_cast<int8_t>(p) > static_cast<int8_t>(q) ? 0xffu : 0u; });
}
QILIN_SPU_INLINE SpuReg clgtb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffu : 0u; }); }

// ---- masks for inserting one element with shufb (cbd, chd, cwd, cdd and their indexed forms) ----
// The mask takes every byte from the second operand, but for the element at `address`'s place in a quadword, taken from the first's preferred slot.
QILIN_SPU_INLINE SpuReg insertion_mask(uint32_t address, unsigned size) {
    SpuReg r{{0x10111213u, 0x14151617u, 0x18191a1bu, 0x1c1d1e1fu}};
    const unsigned at = address & 0xfu & ~(size - 1);
    for (unsigned i = 0; i < size; ++i) set_byte(r, at + i, size == 8 ? i : 4 - size + i);
    return r;
}

}  // namespace spu

}  // namespace qilin::rt
