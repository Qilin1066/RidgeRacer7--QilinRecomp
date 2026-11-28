// When a guest's sound mixer takes its next block: paced by the sound card,
// not by sleeping to the host's clock alone.
//
// A console's sound system takes a block of every port at a fixed rate (on
// the PS3, 256 samples at 48 kHz: every 5.33 ms) and tells the game, which
// writes the next one. Here the blocks go to the host's sound card, whose
// crystal is not the host's clock: a producer paced by the host's clock
// alone slowly fills or drains the card's queue, and one that wakes late
// and then catches up all at once hands the game several blocks in a burst.
//
// The pacer keeps an absolute schedule (a late wake-up doesn't lose time,
// and the block after it is not early: it is simply on time), trims the
// schedule's period by the card's queue (on average `target_frames`
// queued: the period is stretched or shortened by `gain` for each block of
// error, at most `max_change`), and waits for the guest:
//
// - `ready` (step) says whether the guest has written the block that is due.
//   A game that writes the next block only once told the last was taken
//   (as Ridge Racer 7 does: audio.cpp) loses a block whenever the mixer
//   takes one it hasn't written yet. So the pacer waits for it, polling,
//   for up to `answer_wait_ns` past its time: the card's queue is the
//   cushion. Only after that is the block taken unwritten (`forced`), and
//   the schedule goes on from then.
// - Behind by more than `resync_ns` (a long stall, or the host slept), the
//   schedule starts again from now, rather than catching up.
//
// Pure (times are numbers of nanoseconds on any steady clock) and header-
// only: ridgeracer7_audio_test drives it with a fake card and a fake game.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace qilin::host {

class AudioPacer {
public:
    struct Config {
        int64_t period_ns = 0;               // one block's time at the nominal rate
        double block_frames = 0;             // frames in a block
        double target_frames = 0;            // the card's queue aimed for, on average
        int64_t answer_wait_ns = 50'000'000; // how long past its time to wait for a block the guest is late with
        int64_t poll_ns = 250'000;           // how often to look again while waiting
        int64_t resync_ns = 100'000'000;     // behind by more than this: start again from now
        double gain = 0.02;                  // the period's change for each block of queue error
        double max_change = 0.25;            // the period stays within this fraction of the nominal
        double smoothing = 0.05;             // the queue's average takes this much of each reading
        int64_t late_ns = 1'000'000;         // a wake-up later than this is counted as late
    };

    // What the mixer is to do now: wait until `until`, or take the block (`forced`: unwritten, after waiting for it).
    struct Step {
        enum class Kind { wait, take } kind = Kind::wait;
        int64_t until = 0;
        bool forced = false;
    };

    // Counted since the last take_stats().
    struct Stats {
        uint32_t blocks = 0;          // taken
        uint32_t late_wakes = 0;      // woke more than late_ns after a block's time
        int64_t max_late_ns = 0;
        uint32_t waits = 0;           // the guest's block was not ready at its time
        int64_t max_wait_ns = 0;      // the longest wait for it (forced ones included)
        uint32_t forced = 0;          // taken unwritten after answer_wait_ns
        uint32_t resyncs = 0;         // the schedule started again from now
        double min_queued = 0, max_queued = 0, sum_queued = 0;  // the card's queue after each take (frames)
        uint32_t queue_readings = 0;
    };

    explicit AudioPacer(const Config& config) : c_(config), smoothed_(config.target_frames) {}

    // Start, or start again (after a pause): the next block is due now.
    void reset(int64_t now) {
        due_ = now;
        first_look_ = -1;
        smoothed_ = c_.target_frames;
    }

    // A new target (the card's request has become known).
    void set_target_frames(double frames) { c_.target_frames = frames; }

    int64_t due() const { return due_; }
    double smoothed_queue() const { return smoothed_; }
    double period_ratio() const { return ratio_; }

    Step step(int64_t now, bool ready) {
        if (now < due_) return {Step::Kind::wait, due_, false};
        if (first_look_ < 0) {
            first_look_ = now;
            const int64_t late = now - due_;
            if (late > c_.late_ns) ++s_.late_wakes;
            s_.max_late_ns = std::max(s_.max_late_ns, late);
        }
        if (ready) return {Step::Kind::take, now, false};
        const int64_t give_up = first_look_ + c_.answer_wait_ns;
        if (now >= give_up) return {Step::Kind::take, now, true};
        return {Step::Kind::wait, std::min(now + c_.poll_ns, give_up), false};
    }

    // A block was taken at `now` (after step said so) and handed to the card, which then had `queued` frames queued
    // (nullopt: there is no card, and the host's clock alone paces). True if the schedule starts again from now (the
    // block was forced, or the mixer was far behind): the time lost is not caught up, so the card's queue is short and
    // is best primed with silence.
    bool taken(int64_t now, bool forced, std::optional<double> queued) {
        ++s_.blocks;
        if (first_look_ >= 0 && now > first_look_) {
            ++s_.waits;
            s_.max_wait_ns = std::max(s_.max_wait_ns, now - first_look_);
        }
        if (forced) ++s_.forced;
        first_look_ = -1;
        ratio_ = 1.0;
        if (queued) {
            smoothed_ += (*queued - smoothed_) * c_.smoothing;
            ratio_ = 1.0 + std::clamp(c_.gain * (smoothed_ - c_.target_frames) / c_.block_frames, -c_.max_change, c_.max_change);
            s_.min_queued = s_.queue_readings ? std::min(s_.min_queued, *queued) : *queued;
            s_.max_queued = s_.queue_readings ? std::max(s_.max_queued, *queued) : *queued;
            s_.sum_queued += *queued;
            ++s_.queue_readings;
        }
        due_ += std::llround(static_cast<double>(c_.period_ns) * ratio_);
        // After giving up on the guest, on from now: catching up the wait would hand it a burst of blocks.
        if (forced) due_ = std::max(due_, now + c_.period_ns);
        if (now - due_ > c_.resync_ns) {
            due_ = now;
            ++s_.resyncs;
            return true;
        }
        return forced;
    }

    Stats take_stats() {
        const Stats s = s_;
        s_ = {};
        return s;
    }

private:
    Config c_;
    int64_t due_ = 0;
    int64_t first_look_ = -1;  // when the block due was first looked at (its wake-up), or -1
    double smoothed_ = 0, ratio_ = 1.0;
    Stats s_;
};

}  // namespace qilin::host
