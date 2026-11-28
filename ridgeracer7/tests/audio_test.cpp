// The sound mixer's pacing and its ports' blocks, game-free: no window, sound card or game files.
//
// A simulation in virtual time puts the mixer's pacer (qilin/host/audio_pacer.hpp) between a fake sound card (taking
// a request of frames at a time by its own crystal, a little off the host's clock) and a fake game that answers each
// event as Ridge Racer 7 does (audio.cpp): after a while (its sound core's mixing), it copies a block over the one at
// the read index **as it is then**. Each block the game writes carries its number, so the blocks the mixer takes show
// any lost (a late copy overwritten by the next), any taken unwritten (a gap), and the card's queue shows underruns.
//
// The mixer as it was (a sleep to the host's clock, then the block taken whatever its state) is simulated alongside
// and must fail the same checks: the cases named old-* expect it to lose blocks.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <string_view>
#include <vector>

#include "audio_blocks.hpp"
#include "qilin/host/audio_pacer.hpp"

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A small generator of its own, so that every compiler's library gives the same runs.
struct Random {
    uint64_t state;
    uint64_t next() {
        state ^= state << 13, state ^= state >> 7, state ^= state << 17;
        return state;
    }
    double uniform() { return double(next() >> 11) / double(uint64_t{1} << 53); }  // [0, 1)
    int64_t between(int64_t low, int64_t high) { return low + static_cast<int64_t>(uniform() * double(high - low)); }
};

constexpr int64_t kMs = 1'000'000;
constexpr int64_t kPeriod = int64_t{1'000'000'000} * 256 / 48000;
constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
constexpr int kRing = 8;
constexpr int64_t kUnwritten = -1;

struct Setup {
    bool paced = true;                     // the new mixer (else the old: the host's clock, nothing waited for)
    int64_t seconds = 20;
    double drift = 300e-6;                 // the card's crystal against the host's clock
    int64_t pull = 480;                    // frames the card takes at a time
    std::function<int64_t(Random&, int64_t)> wake;     // when a sleep until `t` wakes
    std::function<int64_t(Random&, int64_t)> latency;  // how long the game takes to answer an event at `t`
};

struct Result {
    std::vector<int64_t> taken;  // the blocks taken, by the game's number (kUnwritten: none)
    std::vector<int64_t> take_times;
    uint64_t lost = 0, missed = 0, underruns = 0, forced = 0, resyncs = 0;
    double mean_queue = 0, target = 0;  // the card's queue after each take, from the second second on
    int64_t min_spacing = kNever;       // between two takes, while the game answered every block
};

Result simulate(const Setup& s, uint64_t seed = 0x9E3779B97F4A7C15ull) {
    Random random{seed};
    qilin::host::AudioPacer::Config config;
    config.period_ns = kPeriod;
    config.block_frames = 256;
    config.target_frames = double(s.pull) / 2 + 5 * 256;  // as audio.cpp
    qilin::host::AudioPacer pacer(config);
    Result r;
    r.target = config.target_frames;

    std::array<int64_t, kRing> ring;
    ring.fill(kUnwritten);
    int read_index = 0;
    bool answering = false;
    // The game: events waiting, when it finishes the one it's on, the number of the next block it writes.
    int pending = 0;
    int64_t game_done = kNever, next_block = 0;
    // The card: frames queued, when it next takes, whether it ran dry since the last put.
    double queued = 0;
    const double pull_ns = double(s.pull) / 48000.0 * 1e9 / (1.0 + s.drift);
    double card_next = pull_ns;
    bool prime = true;  // (audio.cpp: at the start, and when the schedule starts again)
    double queue_sum = 0;
    uint64_t queue_n = 0;
    int64_t last_take = -1;

    int64_t mixer_at = 0;
    int64_t old_n = 0;
    const int64_t end = s.seconds * 1000 * kMs;
    pacer.reset(0);

    const auto take = [&](int64_t t, bool forced) {
        const int64_t block = ring[read_index];
        const bool written = block != kUnwritten;
        if (!written && answering) ++r.missed;
        if (written && answering && last_take >= 0) r.min_spacing = std::min(r.min_spacing, t - last_take);
        answering = written;
        r.taken.push_back(block);
        r.take_times.push_back(t);
        ring[read_index] = kUnwritten;
        read_index = (read_index + 1) % kRing;
        // The event: the game starts on it at once if idle.
        if (pending++ == 0) game_done = t + s.latency(random, t);
        // The card: primed with silence (AudioOut::put) when audio.cpp asks, or (the old AudioOut) when it ran empty.
        if ((prime || (!s.paced && queued == 0)) && queued + 256 < config.target_frames) queued = config.target_frames - 256;
        queued += 256;
        if (t >= 1000 * kMs) queue_sum += queued, ++queue_n;
        last_take = t;
        prime = s.paced && pacer.taken(t, forced, queued);
    };

    while (true) {
        const int64_t t = std::min({mixer_at, game_done, static_cast<int64_t>(card_next)});
        if (t >= end) break;
        if (t == static_cast<int64_t>(card_next)) {
            if (queued + 1e-9 < double(s.pull)) ++r.underruns, queued = 0;
            else queued -= double(s.pull);
            card_next += pull_ns;
        } else if (t == game_done) {
            ring[read_index] = next_block++;  // (at the read index as it is now)
            game_done = --pending > 0 ? t + s.latency(random, t) : kNever;
        } else if (s.paced) {
            const bool ready = !answering || ring[read_index] != kUnwritten;
            const auto step = pacer.step(t, ready);
            if (step.kind == qilin::host::AudioPacer::Step::Kind::wait) {
                mixer_at = std::max(t + 1, s.wake(random, step.until));
            } else {
                take(t, step.forced);
                mixer_at = t + 1000;  // (the next step, a microsecond on: the mixing's own time)
            }
        } else {
            // The old mixer: due at n periods by the host's clock, slept to, taken; after 200 ms behind, counted from now.
            take(t, false);
            ++old_n;
            const int64_t due = old_n * kPeriod;
            mixer_at = t > due + 200 * kMs ? t : std::max(t + 1, s.wake(random, due));
        }
    }
    // Lost: a number never taken between two that were.
    int64_t last = -1;
    for (const int64_t block : r.taken) {
        if (block == kUnwritten) continue;
        if (last >= 0 && block > last + 1) r.lost += static_cast<uint64_t>(block - last - 1);
        check(block > last, "a block is taken twice or out of order");
        last = block;
    }
    const auto p = pacer.take_stats();
    r.forced = p.forced, r.resyncs = p.resyncs;
    r.mean_queue = queue_n ? queue_sum / double(queue_n) : 0;
    return r;
}

// Wake-ups: the high-resolution timer (up to half a millisecond late), the system's 1 ms tick, its 15.6 ms one.
int64_t precise_wake(Random& random, int64_t t) { return t + random.between(20'000, 500'000); }
int64_t tick_wake(Random& random, int64_t t, int64_t tick) { return (t / tick + 1) * tick + random.between(0, 200'000); }

// The game: its sound core answers in 1 to 2.5 ms, and one event in twenty takes 6 to 12 ms (longer than a period:
// the core's thread, or the game's, waits for the host).
int64_t game_latency(Random& random, int64_t) {
    if (random.uniform() < 0.05) return random.between(6 * kMs, 12 * kMs);
    return random.between(1 * kMs, 2'500'000);
}

void print(const char* name, const Result& r) {
    std::printf("%-22s %6zu blocks, %4llu lost, %4llu missed, %3llu underruns, %llu forced, %llu restarts, queue %.0f frames (target %.0f), "
                "closest takes %.2f ms\n",
                name, r.taken.size(), static_cast<unsigned long long>(r.lost), static_cast<unsigned long long>(r.missed),
                static_cast<unsigned long long>(r.underruns), static_cast<unsigned long long>(r.forced), static_cast<unsigned long long>(r.resyncs),
                r.mean_queue, r.target, r.min_spacing == kNever ? 0.0 : double(r.min_spacing) / 1e6);
}

Setup realistic(bool paced, std::function<int64_t(Random&, int64_t)> wake) {
    Setup s;
    s.paced = paced;
    s.wake = std::move(wake);
    s.latency = game_latency;
    return s;
}

// ---- the cases ----

void jittery_game() {
    const Result r = simulate(realistic(true, precise_wake));
    print("paced", r);
    check(r.lost == 0 && r.missed == 0, "paced: no block lost or missed by a jittery game");
    check(r.underruns == 0, "paced: the card never runs dry");
    check(r.forced == 0 && r.resyncs == 0, "paced: nothing forced, no restart");
    check(std::fabs(r.mean_queue - r.target) < 256, "paced: the card's queue stays within a block of its target on average");
    check(r.min_spacing >= 1 * kMs, "paced: two blocks are never taken closer than the game's answer");
    // 187.5 blocks a second, by the card's crystal.
    const double rate = double(r.taken.size()) / 20.0;
    check(std::fabs(rate - 187.5 * (1 + 300e-6)) < 0.5, "paced: blocks are taken at the card's rate");
}

void old_pacing_fails() {
    const Result fine = simulate(realistic(false, [](Random& random, int64_t t) { return tick_wake(random, t, 1 * kMs); }));
    print("old, 1 ms timer", fine);
    check(fine.lost > 0 || fine.missed > 0, "the old mixer loses blocks to the same game (1 ms timer)");
    const Result coarse = simulate(realistic(false, [](Random& random, int64_t t) { return tick_wake(random, t, 15'625'000); }));
    print("old, 15.6 ms timer", coarse);
    check(coarse.lost > 1000, "the old mixer loses many blocks with the system's coarse timer (its bursts)");
}

void coarse_timer() {
    // Even with no fine timer (a sleep wakes on the 15.6 ms tick), the paced mixer keeps every block in order; only the
    // card's queue suffers.
    const Result r = simulate(realistic(true, [](Random& random, int64_t t) { return tick_wake(random, t, 15'625'000); }));
    print("paced, 15.6 ms timer", r);
    check(r.lost == 0 && r.missed == 0, "paced, coarse timer: no block lost or missed");
}

void drift() {
    for (const double d : {-1000e-6, 0.0, 1000e-6}) {
        Setup s = realistic(true, precise_wake);
        s.drift = d;
        s.latency = [](Random& random, int64_t) { return random.between(1 * kMs, 2 * kMs); };
        const Result r = simulate(s);
        print(d < 0 ? "paced, card -1000 ppm" : d > 0 ? "paced, card +1000 ppm" : "paced, card exact", r);
        check(r.underruns == 0 && r.lost == 0 && r.missed == 0, "paced: a drifting card is followed without gaps");
        check(std::fabs(r.mean_queue - r.target) < 128, "paced: the queue's average holds against drift");
    }
}

void big_request() {
    // A card that takes 2048 frames (43 ms) at a time: the target grows with it.
    Setup s = realistic(true, precise_wake);
    s.pull = 2048;
    const Result r = simulate(s);
    print("paced, 2048-frame card", r);
    check(r.underruns == 0 && r.lost == 0 && r.missed == 0, "paced: a card with a big request is fed without gaps");
}

void stall(int64_t length, bool expect_forced) {
    // The game takes `length` over one event at 5 s; otherwise answers in 1 to 2 ms.
    Setup s = realistic(true, precise_wake);
    s.latency = [length, done = false](Random& random, int64_t t) mutable {
        if (!done && t >= 5000 * kMs) {
            done = true;
            return length;
        }
        return random.between(1 * kMs, 2 * kMs);
    };
    const Result r = simulate(s);
    print(expect_forced ? "paced, 300 ms stall" : "paced, 40 ms stall", r);
    check(r.resyncs == 0 || expect_forced, "paced: a short stall doesn't restart the schedule");
    check((r.forced > 0) == expect_forced, "paced: a block is taken unwritten only after a long stall");
    if (!expect_forced) check(r.lost == 0 && r.missed == 0, "paced: a 40 ms stall loses nothing (the card's gap is its only mark)");
    // A second after it, everything is in order again: no block lost or missed from then on.
    int64_t last = -1;
    uint64_t trouble = 0;
    for (size_t i = 0; i < r.taken.size(); ++i) {
        if (r.take_times[i] < 6000 * kMs) continue;
        if (r.taken[i] == kUnwritten || (last >= 0 && r.taken[i] != last + 1)) ++trouble;
        last = r.taken[i];
    }
    check(trouble == 0, "paced: a second after a stall, every block is taken in order");
}

void restart() {
    // After a pause the schedule starts again from then: no burst of blocks to catch up.
    qilin::host::AudioPacer::Config config;
    config.period_ns = kPeriod, config.block_frames = 256, config.target_frames = 1500;
    qilin::host::AudioPacer pacer(config);
    pacer.reset(0);
    int64_t t = 0;
    for (int i = 0; i < 10; ++i) {
        const auto step = pacer.step(t, true);
        if (step.kind == qilin::host::AudioPacer::Step::Kind::wait) t = step.until;
        check(pacer.step(t, true).kind == qilin::host::AudioPacer::Step::Kind::take, "restart: a block is taken when due");
        pacer.taken(t, false, 1500.0);
    }
    const int64_t resumed = t + 3000 * kMs;  // three seconds held
    pacer.reset(resumed);
    check(pacer.step(resumed, true).kind == qilin::host::AudioPacer::Step::Kind::take, "restart: taken at once on resuming");
    pacer.taken(resumed, false, 1500.0);
    const auto next = pacer.step(resumed + 1000, true);
    check(next.kind == qilin::host::AudioPacer::Step::Kind::wait && next.until >= resumed + kPeriod * 9 / 10, "restart: the next a period later, no burst");
    const auto stats = pacer.take_stats();
    check(stats.resyncs == 0 && stats.late_wakes == 0, "restart: no restart or lateness counted for the pause");

    // A lateness is counted once per block, by how late the wake-up was.
    pacer.reset(0);
    check(pacer.step(3 * kMs, false).kind == qilin::host::AudioPacer::Step::Kind::wait, "a block not ready is waited for");
    check(pacer.step(4 * kMs, true).kind == qilin::host::AudioPacer::Step::Kind::take, "and taken once ready");
    pacer.taken(4 * kMs, false, 1500.0);
    const auto late = pacer.take_stats();
    check(late.late_wakes == 1 && late.max_late_ns == 3 * kMs && late.waits == 1 && late.max_wait_ns == 1 * kMs, "lateness and waiting are counted");
}

void blocks() {
    namespace audio = rr7::audio;
    std::vector<uint8_t> block(audio::block_bytes(8));
    audio::mark_unwritten(block.data(), block.size());
    check(!audio::written(block.data(), block.size()), "a marked block is unwritten");
    std::memset(block.data(), 0, block.size() / 2);
    check(!audio::written(block.data(), block.size()), "a block half copied over is unwritten");
    audio::mark_unwritten(block.data(), block.size());
    std::memset(block.data() + block.size() / 2, 0, block.size() / 2);
    check(!audio::written(block.data(), block.size()), "a block half copied over from its end is unwritten");
    std::memset(block.data(), 0, block.size());
    check(audio::written(block.data(), block.size()), "a block of silence copied over is written");

    // The down-mix: front left 0.5, centre 0.5, surround right 0.25, back left 0.25 (big-endian floats).
    const auto put = [&](uint32_t frame, uint32_t channel, float v) {
        uint32_t bits = std::bit_cast<uint32_t>(v);
        const uint8_t be[4] = {uint8_t(bits >> 24), uint8_t(bits >> 16), uint8_t(bits >> 8), uint8_t(bits)};
        std::memcpy(block.data() + (size_t{frame} * 8 + channel) * 4, be, 4);
    };
    std::memset(block.data(), 0, block.size());
    put(0, 0, 0.5f), put(0, 2, 0.5f), put(0, 5, 0.25f), put(0, 6, 0.25f), put(0, 3, 1.0f);  // (the bass is left out)
    std::array<float, 512> mix{};
    audio::mix_block(block.data(), 8, 1.0f, mix.data());
    const float h = 0.70710678f;
    check(std::fabs(mix[0] - (0.5f + 0.5f * h + 0.25f * h)) < 1e-6f && std::fabs(mix[1] - (0.5f * h + 0.25f * h)) < 1e-6f, "the 8-channel down-mix");

    std::array<int16_t, 512> out{};
    mix.fill(0.5f);
    mix[10] = 1.5f, mix[11] = -2.0f, mix[12] = std::numeric_limits<float>::quiet_NaN();
    check(audio::to_s16(mix.data(), out.data(), 256) == 3, "samples beyond full scale (and a NaN) are counted");
    check(out[10] == 32767 && out[11] == -32767 && out[12] == 0 && out[0] == 16384, "and clipped (a NaN to silence)");
    mix.fill(1.0f);
    audio::to_s16(mix.data(), out.data(), 256, 1);
    check(out[0] < 200 && out[510] == 32767, "a fade in starts near silence and ends at full");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view which = argc > 1 ? argv[1] : "all";
    const auto run = [&](std::string_view name, const std::function<void()>& f) {
        if (which == "all" || which == name) f();
    };
    run("blocks", blocks);
    run("restart", restart);
    run("jittery", jittery_game);
    run("old", old_pacing_fails);
    run("coarse", coarse_timer);
    run("drift", drift);
    run("big-request", big_request);
    run("stall", [] {
        stall(40 * kMs, false);
        stall(300 * kMs, true);
    });
    if (failures) std::fprintf(stderr, "%d checks failed\n", failures);
    else std::printf("all passed\n");
    return failures ? 1 : 0;
}
