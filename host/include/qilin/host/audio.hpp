// Sound out: interleaved 16-bit stereo through an SDL audio stream on the
// default playback device. push() may be called from any thread.
//
// The guest makes sound by the host's clock and the sound card plays it by its
// own crystal; the two differ by up to a few hundred parts per million, so a
// plain queue slowly fills (latency creeps up) or drains (gaps). push() keeps
// the queue near kTargetMs by playing up to 0.5% faster or slower (a pitch
// change far below hearing), steered by the queue's smoothed length. Only a
// stall that leaves more than kDropMs queued drops sound.
//
// A producer that paces itself by the card (audio_pacer.hpp) uses put()
// instead: nothing is steered or dropped, and the card's requests are
// watched for finding the queue short (underruns).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct SDL_AudioStream;

namespace qilin::host {

class AudioOut {
public:
    static constexpr double kTargetMs = 40.0;  // queued sound aimed for: the latency it adds
    static constexpr double kDropMs = 200.0;   // more than this queued (a stall): drop it

    AudioOut() : AudioOut(32000) {}
    explicit AudioOut(unsigned rate);  // fails loudly if there's no playback device
    ~AudioOut();
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    // Queue `frames` left/right pairs at `rate` Hz.
    void push(const int16_t* left_right, size_t frames, unsigned rate);

    // Since the last call (any thread): how much was queued on average and at
    // most, the playback speed, and how often the queue ran dry or was dropped.
    struct Stats {
        double average_ms = 0, max_ms = 0, ratio = 1;
        uint32_t pushes = 0, empty = 0, dropped = 0;
    };
    Stats take_stats();

    // ---- Paced by the producer (audio_pacer.hpp) ----
    // Queue `frames` left/right pairs at the rate opened with; silence first, if fewer than `prime_frames` would be
    // queued with them, to make that many (the producer asks for it at its start, after a pause, after a stall).
    void put(const int16_t* left_right, size_t frames, size_t prime_frames = 0);
    size_t queued_frames() const;
    // How much the card takes at a time, in this stream's frames: its last request, or its buffer's size until then.
    size_t pull_frames() const;
    // Before a pause: a ramp from the last sample put down to silence (so the stop doesn't click); the card running
    // dry after it is not an underrun.
    void fade_out();
    // Since the last call (any thread): the card's requests, how many found the queue short and by how many frames.
    struct PacedStats {
        uint32_t pulls = 0, underruns = 0;
        uint64_t short_frames = 0;
    };
    PacedStats take_paced_stats();

    struct Pulls;  // what the card's thread sees (audio.cpp)

private:
    SDL_AudioStream* stream_ = nullptr;
    std::unique_ptr<Pulls> pulls_;
    size_t device_frames_ = 0;
    std::array<int16_t, 2> last_{};  // the last frame put
    bool started_ = false;
    unsigned rate_ = 0;
    double smoothed_ms_ = kTargetMs, ratio_ = 1.0;  // the guest thread's own
    std::vector<int16_t> silence_;
    std::atomic<uint64_t> sum_us_{0};
    std::atomic<uint32_t> max_us_{0}, pushes_{0}, empty_{0}, dropped_{0}, ratio_ppm_{1000000};
};

// The playback speed for a queue of `smoothed_ms` against `target_ms`: up to
// 0.5% faster when over, slower when under; 0.4% per target's worth of error,
// so 40 ms too much drains in about 10 s.
inline double audio_rate_ratio(double smoothed_ms, double target_ms) {
    const double err = (smoothed_ms - target_ms) / target_ms;
    return 1.0 + std::clamp(0.004 * err, -0.005, 0.005);
}

// ---- The threads that make sound ----
enum class AudioThread {
    mixer,   // wakes on time and does little: the host's highest priority short of real time (and on Windows the
             // multimedia scheduler's "Pro Audio" class)
    worker,  // makes the sound the mixer waits for (a guest's sound core): above the other threads, below the mixer
};
// For the calling thread. False if the system refused (on Linux without the rights to): it runs on as it was.
bool set_audio_thread_priority(AudioThread role);

// Sleeps until `when`, to within a fraction of a millisecond (on Windows a high-resolution waitable timer: a plain
// sleep there is 1 ms at best, 15.6 ms when the system's timer resolution isn't raised).
void sleep_precisely_until(std::chrono::steady_clock::time_point when);

}  // namespace qilin::host
