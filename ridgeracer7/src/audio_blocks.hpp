// A cellAudio port's blocks, as the mixer (audio.cpp) reads them: pure, for ridgeracer7_audio_test.
//
// A block is 256 samples of 2 or 8 channels, big-endian 32-bit floats. Once the mixer has taken a block it fills it
// with a pattern (kUnwritten in every word) rather than zeroes, as the console's clearing would: the game copies a
// whole block over it each time it writes one, so a block still holding the pattern is one the game hasn't written
// yet, which the mixer waits for (audio_pacer.hpp) instead of playing a gap. *A stated choice:* the pattern is a NaN
// (as a float) that no sum of sound gives, and a block taken with it is played as silence.
//
// Runtime code: no std::format.
#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rr7::audio {

constexpr uint32_t kBlockSamples = 256;
constexpr uint32_t kUnwritten = 0x7FA5A5A5u;  // (written and compared as the host's word: its byte order doesn't matter)

constexpr size_t block_bytes(uint32_t channels) { return size_t{kBlockSamples} * channels * 4; }

inline void mark_unwritten(uint8_t* block, size_t bytes) {
    for (size_t i = 0; i + 4 <= bytes; i += 4) std::memcpy(block + i, &kUnwritten, 4);
}

// Whether the game has written the block since it was marked: its first, middle and last words have all changed (a
// copy in progress, front to back or back to front, has changed one end but not the other).
inline bool written(const uint8_t* block, size_t bytes) {
    const auto word = [&](size_t at) {
        uint32_t v;
        std::memcpy(&v, block + at, 4);
        return v;
    };
    return word(0) != kUnwritten && word(bytes / 2) != kUnwritten && word(bytes - 4) != kUnwritten;
}

inline float guest_float(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return std::bit_cast<float>(v);
}

// Adds a block's samples, at `level`, to `mix` (256 left-right pairs). An 8-channel block (front left and right,
// centre, bass, surround left and right, back left and right) is mixed down with the centre and surrounds at -3 dB and
// no bass: *a stated choice (the usual coefficients), to be compared with the console's own down-mixer.*
inline void mix_block(const uint8_t* block, uint32_t channels, float level, float* mix) {
    const auto sample = [&](uint32_t frame, uint32_t channel) { return guest_float(block + (size_t{frame} * channels + channel) * 4); };
    for (uint32_t i = 0; i < kBlockSamples; ++i) {
        float left, right;
        if (channels == 2) {
            left = sample(i, 0), right = sample(i, 1);
        } else {
            constexpr float kMinus3dB = 0.70710678f;
            const float centre = sample(i, 2) * kMinus3dB;
            left = sample(i, 0) + centre + (sample(i, 4) + sample(i, 6)) * kMinus3dB;
            right = sample(i, 1) + centre + (sample(i, 5) + sample(i, 7)) * kMinus3dB;
        }
        mix[2 * i] += left * level;
        mix[2 * i + 1] += right * level;
    }
}

// To 16 bits; returns how many samples were beyond full scale (clipped). `ramp`: -1 fades out across the block, 1 fades
// in (after a pause or the port's start, so that the sound's first step doesn't click), 0 neither.
inline uint32_t to_s16(const float* mix, int16_t* out, size_t frames, int ramp = 0) {
    uint32_t clipped = 0;
    for (size_t i = 0; i < frames; ++i) {
        const float k = ramp == 0 ? 1.0f : ramp > 0 ? float(i + 1) / float(frames) : 1.0f - float(i + 1) / float(frames);
        for (size_t j = 0; j < 2; ++j) {
            const float v = mix[2 * i + j] * k;
            if (!(std::fabs(v) <= 1.0f)) ++clipped;  // (a NaN counts, and becomes 0)
            const float c = v == v ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
            out[2 * i + j] = static_cast<int16_t>(std::lround(c * 32767.0f));
        }
    }
    return clipped;
}

}  // namespace rr7::audio
