#include "qilin/host/audio.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace qilin::host {

namespace {
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "fatal: %s: %s\n", what, SDL_GetError());
    std::fflush(nullptr);
    std::_Exit(70);
}
}  // namespace

// What the card's thread sees of its requests (on_pull), for put() and take_paced_stats().
struct AudioOut::Pulls {
    std::atomic<bool> expecting{false};  // sound is being put: the queue running dry is an underrun
    std::atomic<uint32_t> pulls{0}, underruns{0}, last_frames{0};
    std::atomic<uint64_t> short_frames{0};
};

namespace {
// Called by SDL on the card's thread before it takes from the stream, with the stream locked (SDL's locks are
// re-entrant, so its queue can be read here). `total` is what it takes, in the stream's own bytes (with a few frames
// of the resampler's margin when the card's rate differs); a queue short of it by more than that margin ran dry.
void SDLCALL on_pull(void* userdata, SDL_AudioStream* stream, int, int total) {
    auto& p = *static_cast<AudioOut::Pulls*>(userdata);
    const int queued = SDL_GetAudioStreamQueued(stream);
    constexpr int kFrameBytes = 4, kMarginFrames = 32;
    p.pulls.fetch_add(1, std::memory_order_relaxed);
    p.last_frames.store(static_cast<uint32_t>(total / kFrameBytes), std::memory_order_relaxed);
    if (!p.expecting.load(std::memory_order_relaxed) || queued < 0) return;
    const int short_bytes = total - queued;
    if (short_bytes > kMarginFrames * kFrameBytes) {
        p.underruns.fetch_add(1, std::memory_order_relaxed);
        p.short_frames.fetch_add(static_cast<uint64_t>(short_bytes / kFrameBytes), std::memory_order_relaxed);
    }
}
}  // namespace

AudioOut::AudioOut(unsigned rate) : pulls_(std::make_unique<Pulls>()) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) die("SDL audio can't start");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) die("no sound output device");
    rate_ = rate;
    // The card's buffer, in this stream's frames: what it takes at a time until it has said.
    SDL_AudioSpec device{};
    int device_frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream_), &device, &device_frames) && device.freq > 0 && device_frames > 0)
        device_frames_ = static_cast<size_t>(std::llround(double(device_frames) * double(rate) / double(device.freq)));
    if (!SDL_SetAudioStreamGetCallback(stream_, on_pull, pulls_.get())) die("the sound output can't be watched");
    if (!SDL_ResumeAudioStreamDevice(stream_)) die("the sound output device won't start");
}

AudioOut::~AudioOut() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void AudioOut::push(const int16_t* lr, size_t frames, unsigned rate) {
    if (rate != rate_) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, static_cast<int>(rate)};
        if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) die("the sound rate can't change");
        rate_ = rate;
    }
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) die("the sound queue can't be read");
    double ms = double(queued) / 4.0 * 1000.0 / double(rate);
    if (ms > kDropMs) {
        SDL_ClearAudioStream(stream_);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        ms = 0, smoothed_ms_ = kTargetMs;
    }
    if (queued == 0) {
        // Ran dry (or the start): a cushion of silence first, so the device has
        // the target's worth queued at once rather than gap after small gap.
        empty_.fetch_add(1, std::memory_order_relaxed);
        const double sound_ms = double(frames) * 1000.0 / double(rate);
        if (sound_ms < kTargetMs) {
            silence_.assign(static_cast<size_t>((kTargetMs - sound_ms) * double(rate) / 1000.0) * 2, 0);
            if (!SDL_PutAudioStreamData(stream_, silence_.data(), static_cast<int>(silence_.size() * 2)))
                die("sound can't be queued");
            ms = kTargetMs - sound_ms, smoothed_ms_ = kTargetMs;
        }
    }
    // Smoothed over about 50 pushes (a quarter second at 5 ms each): the drift is slow.
    smoothed_ms_ += (ms - smoothed_ms_) * 0.02;
    const double ratio = audio_rate_ratio(smoothed_ms_, kTargetMs);
    if (std::fabs(ratio - ratio_) >= 0.0001) {  // SDL rebuilds its resampler: not for every push
        if (!SDL_SetAudioStreamFrequencyRatio(stream_, static_cast<float>(ratio))) die("the sound speed can't change");
        ratio_ = ratio;
        ratio_ppm_.store(static_cast<uint32_t>(std::lround(ratio * 1e6)), std::memory_order_relaxed);
    }
    const auto us = static_cast<uint32_t>(ms * 1000.0);
    sum_us_.fetch_add(us, std::memory_order_relaxed);
    pushes_.fetch_add(1, std::memory_order_relaxed);
    uint32_t m = max_us_.load(std::memory_order_relaxed);
    while (us > m && !max_us_.compare_exchange_weak(m, us, std::memory_order_relaxed)) {}
    if (!SDL_PutAudioStreamData(stream_, lr, static_cast<int>(frames * 4))) die("sound can't be queued");
}

AudioOut::Stats AudioOut::take_stats() {
    Stats s;
    s.pushes = pushes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = sum_us_.exchange(0, std::memory_order_relaxed);
    s.average_ms = s.pushes ? double(sum) / double(s.pushes) / 1000.0 : 0;
    s.max_ms = max_us_.exchange(0, std::memory_order_relaxed) / 1000.0;
    s.empty = empty_.exchange(0, std::memory_order_relaxed);
    s.dropped = dropped_.exchange(0, std::memory_order_relaxed);
    s.ratio = ratio_ppm_.load(std::memory_order_relaxed) / 1e6;
    return s;
}

void AudioOut::put(const int16_t* lr, size_t frames, size_t prime_frames) {
    pulls_->expecting.store(true, std::memory_order_relaxed);
    const size_t queued = prime_frames ? queued_frames() : 0;
    if (queued + frames < prime_frames) {
        silence_.assign((prime_frames - queued - frames) * 2, 0);
        if (!SDL_PutAudioStreamData(stream_, silence_.data(), static_cast<int>(silence_.size() * 2))) die("sound can't be queued");
    }
    if (!frames) return;
    if (!SDL_PutAudioStreamData(stream_, lr, static_cast<int>(frames * 4))) die("sound can't be queued");
    last_ = {lr[2 * (frames - 1)], lr[2 * (frames - 1) + 1]};
    started_ = true;
}

size_t AudioOut::queued_frames() const {
    const int queued = SDL_GetAudioStreamQueued(stream_);
    if (queued < 0) die("the sound queue can't be read");
    return static_cast<size_t>(queued) / 4;
}

size_t AudioOut::pull_frames() const {
    const uint32_t last = pulls_->last_frames.load(std::memory_order_relaxed);
    return last ? last : device_frames_;
}

void AudioOut::fade_out() {
    pulls_->expecting.store(false, std::memory_order_relaxed);
    if (!started_) return;
    constexpr size_t kFrames = 256;  // 5.3 ms at 48 kHz
    silence_.resize(kFrames * 2);
    for (size_t i = 0; i < kFrames; ++i) {
        const double k = 1.0 - double(i + 1) / double(kFrames);
        silence_[2 * i] = static_cast<int16_t>(std::lround(last_[0] * k));
        silence_[2 * i + 1] = static_cast<int16_t>(std::lround(last_[1] * k));
    }
    if (!SDL_PutAudioStreamData(stream_, silence_.data(), static_cast<int>(silence_.size() * 2))) die("sound can't be queued");
    last_ = {};
}

AudioOut::PacedStats AudioOut::take_paced_stats() {
    PacedStats s;
    s.pulls = pulls_->pulls.exchange(0, std::memory_order_relaxed);
    s.underruns = pulls_->underruns.exchange(0, std::memory_order_relaxed);
    s.short_frames = pulls_->short_frames.exchange(0, std::memory_order_relaxed);
    return s;
}

bool set_audio_thread_priority(AudioThread role) {
    const bool mixer = role == AudioThread::mixer;
    bool ok = SDL_SetCurrentThreadPriority(mixer ? SDL_THREAD_PRIORITY_TIME_CRITICAL : SDL_THREAD_PRIORITY_HIGH);
#ifdef _WIN32
    // The multimedia class scheduler: a "Pro Audio" thread isn't starved by the system's busy threads. (Loaded as
    // SDL's own sound thread loads it: avrt.dll is not linked.)
    if (mixer) {
        using SetCharacteristics = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
        static const SetCharacteristics set_characteristics = [] {
            const HMODULE avrt = LoadLibraryW(L"avrt.dll");
            return avrt ? reinterpret_cast<SetCharacteristics>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))) : nullptr;
        }();
        DWORD task = 0;
        ok = set_characteristics && set_characteristics(L"Pro Audio", &task) && ok;
    }
#endif
    return ok;
}

void sleep_precisely_until(std::chrono::steady_clock::time_point when) {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    // SDL_DelayNS is a high-resolution waitable timer on Windows, nanosleep elsewhere.
    SDL_DelayNS(static_cast<Uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count()));
}

}  // namespace qilin::host
