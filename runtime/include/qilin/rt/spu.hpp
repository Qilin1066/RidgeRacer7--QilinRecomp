// The SPU's state and the helpers lifted SPU code calls.
//
// An SPU has 128 registers of 128 bits and 256 KiB of memory of its own
// (the local store), and reaches the rest of the machine only through
// channels and the stop instruction. A register is four words, word 0
// first (the "preferred slot", which scalar code uses); bytes and
// halfwords are numbered from the most significant end, as the SPU
// numbers them.
//
// Each helper is one instruction's meaning, written from the SPU's
// instruction set as published (IBM's "SPU Instruction Set Architecture").
// The integer part is here; floating point is in spu_fp.hpp.
//
// **The host's vector unit.** A register is also the host's 128-bit vector
// (`SpuReg::v`, word 0 in its lowest lane), so lifted code, which keeps the
// registers a function uses in locals (spu_lift.hpp), holds them in the
// host's vector registers. On x86-64 the common helpers are SSE2, and the
// byte shuffles (`shufb`, the whole-register byte shifts) SSSE3 where the
// build may use it (`QILIN_SPU_SSSE3`: MSVC always, as every x86-64 CPU
// the game could run on has it, checked when the first SPU thread starts;
// GCC and Clang when built for it). Elsewhere, and for the rarer
// instructions, the helpers are plain C++ on the four words. The two views
// of a register are one union: type punning through a union is what GCC,
// Clang and MSVC all define.
//
// **The local store is kept in the host's word order**: each aligned word
// of it is a host `uint32_t` (on a little-endian host, the bytes of each
// word reversed from the SPU's order), so a quadword's load or store is a
// plain 16-byte copy into a register's words. SPU byte `a` is host byte
// `ls_index(a)`. Whatever moves bytes between the local store and anything
// in the SPU's byte order (memory transfers, the program's loading, the
// system's reads and writes of it) goes through `ls_read` and `ls_write`.
//
// Runtime code: no std::format here.
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

// (QILIN_SPU_PORTABLE: the plain C++ everywhere, as on a host with neither, for testing it.)
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(QILIN_SPU_PORTABLE)
#include <emmintrin.h>
#define QILIN_SPU_SSE2 1
#if !defined(QILIN_SPU_SSSE3)
#if defined(__SSSE3__) || (defined(_MSC_VER) && !defined(__clang__))
#define QILIN_SPU_SSSE3 1
#else
#define QILIN_SPU_SSSE3 0
#endif
#endif
#if QILIN_SPU_SSSE3
#include <tmmintrin.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define QILIN_SPU_SSE2 0
#define QILIN_SPU_SSSE3 0
#endif

// The helpers are forced inline: lifted code is one call of them per instruction. The bulky ones, which loop over the
// sixteen bytes, are left to the compiler with MSVC: forced into the lifted programs' longest functions they took MSVC
// 10 GB of memory and six and a half minutes for one file (spu_p24), against 0.6 GB and well under a minute when not.
#if !defined(QILIN_SPU_INLINE)
#if defined(_MSC_VER)
#define QILIN_SPU_INLINE __forceinline
#else
#define QILIN_SPU_INLINE inline __attribute__((always_inline))
#endif
#endif
#if !defined(QILIN_SPU_BULKY)
#if defined(_MSC_VER)
#define QILIN_SPU_BULKY inline
#else
#define QILIN_SPU_BULKY QILIN_SPU_INLINE
#endif
#endif
// What lifted code reaches rarely, kept out of it.
#if !defined(QILIN_SPU_COLD)
#if defined(_MSC_VER)
#define QILIN_SPU_COLD __declspec(noinline)
#else
#define QILIN_SPU_COLD __attribute__((noinline, cold))
#endif
#endif

namespace qilin::rt {

struct Spu;

// Where lifted SPU code leaves what was known at build time. Lifted code keeps the registers a function uses in locals:
// they are in `Spu::r` for `native_call` (and for every function the program calls), but not for the others, which
// must not look at the registers.
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

union SpuReg {
    uint32_t w[4];
#if QILIN_SPU_SSE2
    __m128i v;
#endif
};

inline constexpr uint32_t kSpuLocalStore = 0x40000;

struct Spu {
    SpuReg r[128] = {};
    uint8_t* ls = nullptr;  // the local store: kSpuLocalStore bytes, in the host's word order (above)
    uint32_t fpscr[4] = {};
    const SpuHooks* hooks = nullptr;
    void* owner = nullptr;  // the runtime's own object for this SPU
};

namespace spu {

// ---- the local store's order ----
// Where SPU byte `a` of the local store is.
inline constexpr uint32_t kLsSwizzle = std::endian::native == std::endian::little ? 3u : 0u;
constexpr uint32_t ls_index(uint32_t a) { return a ^ kLsSwizzle; }
// `n` bytes in the SPU's order to and from the local store at SPU address `at` (within it).
inline void ls_write(uint8_t* ls, uint32_t at, const uint8_t* from, size_t n) {
    if constexpr (kLsSwizzle == 0) {
        std::memcpy(ls + at, from, n);
    } else {
        for (; n && (at & 3); --n) ls[ls_index(at++)] = *from++;
        for (; n >= 4; n -= 4, at += 4, from += 4) {
            uint32_t w;
            std::memcpy(&w, from, 4);
            w = std::byteswap(w);
            std::memcpy(ls + at, &w, 4);
        }
        for (; n; --n) ls[ls_index(at++)] = *from++;
    }
}
inline void ls_read(const uint8_t* ls, uint32_t at, uint8_t* to, size_t n) {
    if constexpr (kLsSwizzle == 0) {
        std::memcpy(to, ls + at, n);
    } else {
        for (; n && (at & 3); --n) *to++ = ls[ls_index(at++)];
        for (; n >= 4; n -= 4, at += 4, to += 4) {
            uint32_t w;
            std::memcpy(&w, ls + at, 4);
            w = std::byteswap(w);
            std::memcpy(to, &w, 4);
        }
        for (; n; --n) *to++ = ls[ls_index(at++)];
    }
}
// An aligned word of the local store (as the SPU reads it).
inline uint32_t ls_word(const uint8_t* ls, uint32_t at) {
    uint32_t w;
    std::memcpy(&w, ls + at, 4);
    return w;
}
inline void set_ls_word(uint8_t* ls, uint32_t at, uint32_t w) { std::memcpy(ls + at, &w, 4); }

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

#if QILIN_SPU_SSE2
QILIN_SPU_INLINE SpuReg vec(__m128i v) {
    SpuReg r;
    r.v = v;
    return r;
}
#endif
// A register of four words, word 0 first.
QILIN_SPU_INLINE SpuReg quad(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
#if QILIN_SPU_SSE2
    return vec(_mm_setr_epi32(static_cast<int>(w0), static_cast<int>(w1), static_cast<int>(w2), static_cast<int>(w3)));
#else
    return {{w0, w1, w2, w3}};
#endif
}
// A word in the preferred slot, the rest zeroes.
QILIN_SPU_INLINE SpuReg scalar(uint32_t w0) {
#if QILIN_SPU_SSE2
    return vec(_mm_cvtsi32_si128(static_cast<int>(w0)));
#else
    return {{w0, 0, 0, 0}};
#endif
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
QILIN_SPU_BULKY SpuReg bytes(const SpuReg& a, const SpuReg& b, F f) {
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
QILIN_SPU_INLINE SpuReg splat(uint32_t word) {
#if QILIN_SPU_SSE2
    return vec(_mm_set1_epi32(static_cast<int>(word)));
#else
    return {{word, word, word, word}};
#endif
}
QILIN_SPU_INLINE uint32_t sext16(uint32_t h) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(h))); }

// ---- the local store ----
// A quadword's address: within the local store, at a multiple of 16. Its four words are a register's, in order.
QILIN_SPU_INLINE uint32_t quad_address(uint32_t a) { return a & 0x3fff0u; }
QILIN_SPU_INLINE SpuReg load(const uint8_t* ls, uint32_t address) {
#if QILIN_SPU_SSE2
    return vec(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ls + quad_address(address))));
#else
    SpuReg r;
    std::memcpy(r.w, ls + quad_address(address), 16);
    return r;
#endif
}
QILIN_SPU_INLINE void store(uint8_t* ls, uint32_t address, const SpuReg& v) {
#if QILIN_SPU_SSE2
    _mm_storeu_si128(reinterpret_cast<__m128i*>(ls + quad_address(address)), v.v);
#else
    std::memcpy(ls + quad_address(address), v.w, 16);
#endif
}
QILIN_SPU_INLINE SpuReg load(const Spu& s, uint32_t address) { return load(s.ls, address); }
QILIN_SPU_INLINE void store(Spu& s, uint32_t address, const SpuReg& v) { store(s.ls, address, v); }

// ---- constants ----
// fsmbi: each of 16 bits makes a byte of ones or zeros, the top bit the first byte.
// (Each four bits make a word: of a table of the sixteen.)
inline constexpr std::array<uint32_t, 16> kByteMasks = [] {
    std::array<uint32_t, 16> masks{};
    for (unsigned n = 0; n < 16; ++n)
        for (unsigned k = 0; k < 4; ++k)
            if (n & (8u >> k)) masks[n] |= 0xffu << (24 - 8 * k);
    return masks;
}();
QILIN_SPU_INLINE SpuReg fsmbi(uint32_t bits) {
    return quad(kByteMasks[(bits >> 12) & 0xfu], kByteMasks[(bits >> 8) & 0xfu], kByteMasks[(bits >> 4) & 0xfu], kByteMasks[bits & 0xfu]);
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
QILIN_SPU_BULKY SpuReg gbb(const SpuReg& a) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 16; ++i) bits |= (byte(a, i) & 1u) << (15 - i);
    return {{bits, 0, 0, 0}};
}
QILIN_SPU_BULKY SpuReg gbh(const SpuReg& a) {
    uint32_t bits = 0;
    for (unsigned i = 0; i < 8; ++i) bits |= ((a.w[i >> 1] >> (16 - 16 * (i & 1))) & 1u) << (7 - i);
    return {{bits, 0, 0, 0}};
}
QILIN_SPU_INLINE SpuReg gb(const SpuReg& a) { return {{((a.w[0] & 1u) << 3) | ((a.w[1] & 1u) << 2) | ((a.w[2] & 1u) << 1) | (a.w[3] & 1u), 0, 0, 0}}; }

// ---- arithmetic ----
#if QILIN_SPU_SSE2
#define QILIN_SPU_VECTOR(scalar_form, vector_form) return vector_form
#else
#define QILIN_SPU_VECTOR(scalar_form, vector_form) return scalar_form
#endif
QILIN_SPU_INLINE SpuReg a(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p + q; }), vec(_mm_add_epi32(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg ah(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(halves(x, y, [](uint32_t p, uint32_t q) { return p + q; }), vec(_mm_add_epi16(x.v, y.v)));
}
// sf, sfh: "subtract from": b - a.
QILIN_SPU_INLINE SpuReg sf(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return q - p; }), vec(_mm_sub_epi32(y.v, x.v)));
}
QILIN_SPU_INLINE SpuReg sfh(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(halves(x, y, [](uint32_t p, uint32_t q) { return q - p; }), vec(_mm_sub_epi16(y.v, x.v)));
}
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
QILIN_SPU_BULKY SpuReg cntb(const SpuReg& x) {
    return bytes(x, x, [](uint32_t p, uint32_t) { return static_cast<uint32_t>(std::popcount(p)); });
}
QILIN_SPU_INLINE SpuReg avgb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return (p + q + 1) >> 1; }); }
QILIN_SPU_INLINE SpuReg absdb(const SpuReg& x, const SpuReg& y) { return bytes(x, y, [](uint32_t p, uint32_t q) { return p > q ? p - q : q - p; }); }
// sumb: of each word, the sum of b's four bytes in the upper halfword and of a's in the lower.
QILIN_SPU_BULKY SpuReg sumb(const SpuReg& x, const SpuReg& y) {
    return words(x, y, [](uint32_t p, uint32_t q) {
        const auto sum = [](uint32_t v) { return (v >> 24) + ((v >> 16) & 0xffu) + ((v >> 8) & 0xffu) + (v & 0xffu); };
        return (sum(q) << 16) | sum(p);
    });
}
// Sign extensions: of the lower byte of each halfword, halfword of each word, word of each doubleword.
QILIN_SPU_INLINE SpuReg xsbh(const SpuReg& x) {
    QILIN_SPU_VECTOR(halves(x, [](uint32_t h) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(h))); }),
                     vec(_mm_srai_epi16(_mm_slli_epi16(x.v, 8), 8)));
}
QILIN_SPU_INLINE SpuReg xshw(const SpuReg& x) { QILIN_SPU_VECTOR(words(x, [](uint32_t p) { return sext16(p); }), vec(_mm_srai_epi32(_mm_slli_epi32(x.v, 16), 16))); }
QILIN_SPU_INLINE SpuReg xswd(const SpuReg& x) {
    return {{static_cast<uint32_t>(static_cast<int32_t>(x.w[1]) >> 31), x.w[1], static_cast<uint32_t>(static_cast<int32_t>(x.w[3]) >> 31), x.w[3]}};
}

// ---- logic ----
#if QILIN_SPU_SSE2
QILIN_SPU_INLINE __m128i ones() { return _mm_set1_epi32(-1); }
#endif
QILIN_SPU_INLINE SpuReg and_(const SpuReg& x, const SpuReg& y) { QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p & q; }), vec(_mm_and_si128(x.v, y.v))); }
QILIN_SPU_INLINE SpuReg andc(const SpuReg& x, const SpuReg& y) { QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p & ~q; }), vec(_mm_andnot_si128(y.v, x.v))); }
QILIN_SPU_INLINE SpuReg or_(const SpuReg& x, const SpuReg& y) { QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p | q; }), vec(_mm_or_si128(x.v, y.v))); }
QILIN_SPU_INLINE SpuReg orc(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p | ~q; }), vec(_mm_or_si128(x.v, _mm_xor_si128(y.v, ones()))));
}
QILIN_SPU_INLINE SpuReg xor_(const SpuReg& x, const SpuReg& y) { QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p ^ q; }), vec(_mm_xor_si128(x.v, y.v))); }
QILIN_SPU_INLINE SpuReg nand(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return ~(p & q); }), vec(_mm_xor_si128(_mm_and_si128(x.v, y.v), ones())));
}
QILIN_SPU_INLINE SpuReg nor(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return ~(p | q); }), vec(_mm_xor_si128(_mm_or_si128(x.v, y.v), ones())));
}
QILIN_SPU_INLINE SpuReg eqv(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return ~(p ^ q); }), vec(_mm_xor_si128(_mm_xor_si128(x.v, y.v), ones())));
}
QILIN_SPU_INLINE SpuReg orx(const SpuReg& x) { return {{x.w[0] | x.w[1] | x.w[2] | x.w[3], 0, 0, 0}}; }
// selb: each bit from b where the mask c is set, from a where it is clear.
QILIN_SPU_INLINE SpuReg selb(const SpuReg& x, const SpuReg& y, const SpuReg& c) {
#if QILIN_SPU_SSE2
    return vec(_mm_or_si128(_mm_and_si128(c.v, y.v), _mm_andnot_si128(c.v, x.v)));
#else
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (c.w[i] & y.w[i]) | (~c.w[i] & x.w[i]);
    return r;
#endif
}
// shufb: each byte of c picks a byte of a (0-15) or b (16-31), or is a constant: 10xxxxxx 0x00, 110xxxxx 0xff, 111xxxxx 0x80.
// (Built a word at a time, each byte read straight from its word; with SSSE3, by the host's byte shuffle: SPU byte j is
// host byte j ^ 3 of the vector, so a control byte's pick is the host's index once its low two bits are flipped.)
QILIN_SPU_BULKY SpuReg shufb_words(const SpuReg& x, const SpuReg& y, const SpuReg& c) {
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4; ++k) {
            const uint32_t pick = (c.w[i] >> (24 - 8 * k)) & 0xffu;
            uint32_t value;
            if (pick >= 0x80u) value = pick < 0xc0u ? 0x00u : pick < 0xe0u ? 0xffu : 0x80u;
            else value = (((pick & 0x10u) ? y : x).w[(pick >> 2) & 3u] >> (24 - 8 * (pick & 3u))) & 0xffu;
            w |= value << (24 - 8 * k);
        }
        r.w[i] = w;
    }
    return r;
}
#if QILIN_SPU_SSSE3
QILIN_SPU_INLINE SpuReg shufb(const SpuReg& x, const SpuReg& y, const SpuReg& c) {
    const __m128i index = _mm_xor_si128(_mm_and_si128(c.v, _mm_set1_epi8(0x0f)), _mm_set1_epi8(0x03));
    const __m128i from_b = _mm_cmpeq_epi8(_mm_and_si128(c.v, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
    const __m128i picked = _mm_or_si128(_mm_and_si128(from_b, _mm_shuffle_epi8(y.v, index)), _mm_andnot_si128(from_b, _mm_shuffle_epi8(x.v, index)));
    // The constants: 0x80 and up (negative as signed bytes); 0xc0 and up are ones, and 0xe0 and up 0x80.
    const __m128i special = _mm_cmplt_epi8(c.v, _mm_setzero_si128());
    const __m128i from_c0 = _mm_and_si128(special, _mm_cmpgt_epi8(c.v, _mm_set1_epi8(-65)));
    const __m128i from_e0 = _mm_and_si128(special, _mm_cmpgt_epi8(c.v, _mm_set1_epi8(-33)));
    const __m128i constant = _mm_xor_si128(from_c0, _mm_and_si128(from_e0, _mm_set1_epi8(0x7f)));
    return vec(_mm_or_si128(_mm_andnot_si128(special, picked), constant));
}
#else
QILIN_SPU_INLINE SpuReg shufb(const SpuReg& x, const SpuReg& y, const SpuReg& c) { return shufb_words(x, y, c); }
#endif

// ---- shifts and rotates of elements ----
// The count is each element's own in the register forms (`n` is then that register).
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
// The immediate forms: one count `c` (the instruction's field) for every element, so a shift of the whole vector. The
// host's shifts give zero (or the sign) for a count past the element's width, as the SPU's do.
#if QILIN_SPU_SSE2
QILIN_SPU_INLINE __m128i shift_count(uint32_t c) { return _mm_cvtsi32_si128(static_cast<int>(c)); }
#endif
QILIN_SPU_INLINE SpuReg shlhi(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(shlh(x, splat((c & 0xffffu) * 0x10001u)), vec(_mm_sll_epi16(x.v, shift_count(c & 0x1fu)))); }
QILIN_SPU_INLINE SpuReg shli(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(shl(x, splat(c)), vec(_mm_sll_epi32(x.v, shift_count(c & 0x3fu)))); }
QILIN_SPU_INLINE SpuReg rothi(const SpuReg& x, uint32_t c) {
    QILIN_SPU_VECTOR(roth(x, splat((c & 0xffffu) * 0x10001u)), vec(_mm_or_si128(_mm_sll_epi16(x.v, shift_count(c & 0xfu)), _mm_srl_epi16(x.v, shift_count(16 - (c & 0xfu))))));
}
QILIN_SPU_INLINE SpuReg roti(const SpuReg& x, uint32_t c) {
    QILIN_SPU_VECTOR(rot(x, splat(c)), vec(_mm_or_si128(_mm_sll_epi32(x.v, shift_count(c & 0x1fu)), _mm_srl_epi32(x.v, shift_count(32 - (c & 0x1fu))))));
}
QILIN_SPU_INLINE SpuReg rothmi(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(rothm(x, splat((c & 0xffffu) * 0x10001u)), vec(_mm_srl_epi16(x.v, shift_count((0u - c) & 0x1fu)))); }
QILIN_SPU_INLINE SpuReg rotmi(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(rotm(x, splat(c)), vec(_mm_srl_epi32(x.v, shift_count((0u - c) & 0x3fu)))); }
QILIN_SPU_INLINE SpuReg rotmahi(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(rotmah(x, splat((c & 0xffffu) * 0x10001u)), vec(_mm_sra_epi16(x.v, shift_count((0u - c) & 0x1fu)))); }
QILIN_SPU_INLINE SpuReg rotmai(const SpuReg& x, uint32_t c) { QILIN_SPU_VECTOR(rotma(x, splat(c)), vec(_mm_sra_epi32(x.v, shift_count((0u - c) & 0x3fu)))); }

// ---- shifts and rotates of the whole register ----
// Left by bits (0 to 7), with what leaves the top lost (shift) or brought round (rotate).
QILIN_SPU_BULKY SpuReg shift_left_bits(const SpuReg& x, unsigned bits, bool rotate) {
    if (bits == 0) return x;
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t next = i < 3 ? x.w[i + 1] : (rotate ? x.w[0] : 0u);
        r.w[i] = (x.w[i] << bits) | (next >> (32 - bits));
    }
    return r;
}
QILIN_SPU_BULKY SpuReg shift_right_bits(const SpuReg& x, unsigned bits) {
    if (bits == 0) return x;
    SpuReg r;
    for (unsigned i = 0; i < 4; ++i) r.w[i] = (x.w[i] >> bits) | (i > 0 ? x.w[i - 1] << (32 - bits) : 0u);
    return r;
}
// The register as a 128-bit number, its first byte the most significant, and its shifts by 0 to 127 bits.
struct SpuQuad {
    uint64_t hi, lo;
};
QILIN_SPU_INLINE SpuQuad as_quad(const SpuReg& x) { return {(uint64_t{x.w[0]} << 32) | x.w[1], (uint64_t{x.w[2]} << 32) | x.w[3]}; }
QILIN_SPU_INLINE SpuReg from_quad(SpuQuad q) {
    return {{static_cast<uint32_t>(q.hi >> 32), static_cast<uint32_t>(q.hi), static_cast<uint32_t>(q.lo >> 32), static_cast<uint32_t>(q.lo)}};
}
QILIN_SPU_INLINE SpuQuad quad_left(SpuQuad q, unsigned bits) {
    if (bits == 0) return q;
    if (bits >= 64) return {q.lo << (bits - 64), 0};
    return {(q.hi << bits) | (q.lo >> (64 - bits)), q.lo << bits};
}
QILIN_SPU_INLINE SpuQuad quad_right(SpuQuad q, unsigned bits) {
    if (bits == 0) return q;
    if (bits >= 64) return {0, q.hi >> (bits - 64)};
    return {q.hi >> bits, (q.lo >> bits) | (q.hi << (64 - bits))};
}
#if QILIN_SPU_SSSE3
// The host's shuffles for the whole-register byte shifts, by count: [0] rotates left, [1] shifts left, [2] shifts right
// (SPU byte j is host byte j ^ 3; 0x80 makes a zero).
struct alignas(16) SpuShuffle {
    uint8_t index[16];
};
inline constexpr auto kByteShifts = [] {
    std::array<std::array<SpuShuffle, 32>, 3> t{};
    for (int n = 0; n < 32; ++n)
        for (int h = 0; h < 16; ++h) {
            const int j = h ^ 3, rotated = (j + n) & 15, left = j + n, right = j - n;
            t[0][static_cast<size_t>(n)].index[h] = static_cast<uint8_t>(rotated ^ 3);
            t[1][static_cast<size_t>(n)].index[h] = static_cast<uint8_t>(left > 15 ? 0x80 : left ^ 3);
            t[2][static_cast<size_t>(n)].index[h] = static_cast<uint8_t>(right < 0 ? 0x80 : right ^ 3);
        }
    return t;
}();
QILIN_SPU_INLINE SpuReg shuffle_bytes(const SpuReg& x, unsigned table, unsigned count) {
    return vec(_mm_shuffle_epi8(x.v, _mm_load_si128(reinterpret_cast<const __m128i*>(kByteShifts[table][count].index))));
}
// Left by bytes: 16 or more leaves nothing of a shift; a rotate's count is 0 to 15. Right by bytes: a shift, 0 to 31.
QILIN_SPU_INLINE SpuReg shift_left_bytes(const SpuReg& x, unsigned count, bool rotate) { return shuffle_bytes(x, rotate ? 0 : 1, count & 31u); }
QILIN_SPU_INLINE SpuReg shift_right_bytes(const SpuReg& x, unsigned count) { return shuffle_bytes(x, 2, count & 31u); }
#else
// Left by bytes: 16 or more leaves nothing of a shift; a rotate's count is 0 to 15. Right by bytes: a shift, 0 to 31.
QILIN_SPU_BULKY SpuReg shift_left_bytes(const SpuReg& x, unsigned count, bool rotate) {
    if (rotate) {
        const unsigned bits = 8 * (count & 15u);
        if (bits == 0) return x;
        const SpuQuad left = quad_left(as_quad(x), bits), right = quad_right(as_quad(x), 128 - bits);
        return from_quad({left.hi | right.hi, left.lo | right.lo});
    }
    return count >= 16 ? SpuReg{} : from_quad(quad_left(as_quad(x), 8 * count));
}
QILIN_SPU_BULKY SpuReg shift_right_bytes(const SpuReg& x, unsigned count) { return count >= 16 ? SpuReg{} : from_quad(quad_right(as_quad(x), 8 * count)); }
#endif
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
#if QILIN_SPU_SSE2
// (Unsigned compares as signed ones, with the top bits flipped.)
QILIN_SPU_INLINE __m128i flip(__m128i v, int top) { return _mm_xor_si128(v, _mm_set1_epi32(top)); }
#endif
QILIN_SPU_INLINE SpuReg ceq(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffffffffu : 0u; }), vec(_mm_cmpeq_epi32(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg cgt(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return static_cast<int32_t>(p) > static_cast<int32_t>(q) ? 0xffffffffu : 0u; }),
                     vec(_mm_cmpgt_epi32(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg clgt(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(words(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffffffffu : 0u; }),
                     vec(_mm_cmpgt_epi32(flip(x.v, static_cast<int>(0x80000000u)), flip(y.v, static_cast<int>(0x80000000u)))));
}
QILIN_SPU_INLINE SpuReg ceqh(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(halves(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffffu : 0u; }), vec(_mm_cmpeq_epi16(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg cgth(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(halves(x, y, [](uint32_t p, uint32_t q) { return static_cast<int16_t>(p) > static_cast<int16_t>(q) ? 0xffffu : 0u; }),
                     vec(_mm_cmpgt_epi16(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg clgth(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(halves(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffffu : 0u; }), vec(_mm_cmpgt_epi16(flip(x.v, static_cast<int>(0x80008000u)), flip(y.v, static_cast<int>(0x80008000u)))));
}
QILIN_SPU_INLINE SpuReg ceqb(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(bytes(x, y, [](uint32_t p, uint32_t q) { return p == q ? 0xffu : 0u; }), vec(_mm_cmpeq_epi8(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg cgtb(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(bytes(x, y, [](uint32_t p, uint32_t q) { return static_cast<int8_t>(p) > static_cast<int8_t>(q) ? 0xffu : 0u; }), vec(_mm_cmpgt_epi8(x.v, y.v)));
}
QILIN_SPU_INLINE SpuReg clgtb(const SpuReg& x, const SpuReg& y) {
    QILIN_SPU_VECTOR(bytes(x, y, [](uint32_t p, uint32_t q) { return p > q ? 0xffu : 0u; }), vec(_mm_cmpgt_epi8(flip(x.v, static_cast<int>(0x80808080u)), flip(y.v, static_cast<int>(0x80808080u)))));
}

// ---- masks for inserting one element with shufb (cbd, chd, cwd, cdd and their indexed forms) ----
// The mask takes every byte from the second operand, but for the element at `address`'s place in a quadword, taken from
// the first's preferred slot. (Of a table: by the element's size and the address's last four bits.)
inline constexpr auto kInsertionMasks = [] {
    std::array<std::array<std::array<uint32_t, 4>, 16>, 4> masks{};
    for (unsigned k = 0; k < 4; ++k)
        for (unsigned address = 0; address < 16; ++address) {
            std::array<uint32_t, 4>& r = masks[k][address];
            r = {0x10111213u, 0x14151617u, 0x18191a1bu, 0x1c1d1e1fu};
            const unsigned size = 1u << k, at = address & ~(size - 1);
            for (unsigned i = 0; i < size; ++i) {
                const unsigned b = at + i, shift = 24 - 8 * (b & 3), value = size == 8 ? i : 4 - size + i;
                r[b >> 2] = (r[b >> 2] & ~(0xffu << shift)) | (value << shift);
            }
        }
    return masks;
}();
QILIN_SPU_INLINE SpuReg insertion_mask(uint32_t address, unsigned size) {
    const std::array<uint32_t, 4>& m = kInsertionMasks[size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3][address & 0xfu];
#if QILIN_SPU_SSE2
    return vec(_mm_loadu_si128(reinterpret_cast<const __m128i*>(m.data())));
#else
    return {{m[0], m[1], m[2], m[3]}};
#endif
}

#undef QILIN_SPU_VECTOR

// Whether this host can run what the helpers were built to use (SSSE3, where QILIN_SPU_SSSE3 is set without the
// compiler having been told the host has it): the runtime asks before the first SPU program runs.
inline bool host_has_ssse3() {
#if QILIN_SPU_SSE2 && defined(_MSC_VER)
    int info[4];
    __cpuid(info, 1);
    return (info[2] & (1 << 9)) != 0;
#elif QILIN_SPU_SSE2
    unsigned a = 0, b = 0, c = 0, d = 0;
    return __get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 9)) != 0;
#else
    return true;
#endif
}

}  // namespace spu

}  // namespace qilin::rt
