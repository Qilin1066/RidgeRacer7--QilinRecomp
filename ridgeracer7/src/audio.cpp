// cellAudio: the system's sound output, as a game uses it.
//
// A game opens a port: a ring of blocks in memory, each 256 samples of 2
// or 8 channels, as 32-bit floats. 187.5 times a second (every 256 samples
// at 48 kHz) the system takes the next block of every running port, mixes
// them to the output, clears the block, moves the port's read index on,
// and sends an event to the queues the game has named, by which the
// game's sound thread knows to write the next one.
//
// How this game writes them (read off its sound thread, 0x391ae8): woken by
// the event, it tells its sound core (the SPU program bnusCoreSpu.elf) to
// mix, waits for the core's answer, then copies the 8 KiB it mixed over the
// block **at the read index as it is then**. So it has one period to answer
// in: if the read index has moved on before its copy, the copy lands one
// block late, the block it should have filled plays empty, and the next
// event's copy overwrites the late one (a gap, then a lost block).
//
// Here one thread (the mixer) does the system's part, paced by the sound
// card (qilin/host/audio_pacer.hpp): on an absolute schedule trimmed to keep
// the card's queue near a target, at the host's highest priority, with
// precise sleeps. A taken block is filled with a pattern, not zeroes
// (audio_blocks.hpp), so the mixer sees whether the game has written the
// next; when the game is late, it waits for it (the card's queue is the
// cushion), up to 50 ms, before taking it unwritten. The mixed sound goes
// to host/'s AudioOut (set_audio_output: main.cpp gives it). The output is
// stereo (video.cpp); a port of 8 channels is mixed down to it by
// audio_blocks.hpp's coefficients.
//
// What it saw is logged once a second when anything went wrong (late
// wake-ups, waits for the game, blocks taken unwritten, the card running
// dry, samples clipped), each second with RR7_AUDIO_VERBOSE=1, and summed up
// at the process's end.
//
// Runtime code: no std::format.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "audio_blocks.hpp"
#include "events.hpp"
#include "kernel.hpp"
#include "pause.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/audio_pacer.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;
using audio::kBlockSamples;

// The library's errors (the SDK's CELL_AUDIO_ERROR_...).
constexpr uint64_t kAlreadyInit = 0x80310701u, kNotInit = 0x80310707u, kParam = 0x80310704u, kPortFull = 0x80310705u, kPortAlreadyRun = 0x80310706u,
                   kPortNotOpen = 0x80310708u, kPortNotRun = 0x8031070bu, kPortOpen = 0x8031070au;
constexpr uint32_t kPorts = 8, kRate = 48000;
constexpr uint32_t kReady = 1, kRunning = 2, kClosed = 0x1010;
// The card's queue aimed for: half its request (the queue is a saw-tooth, falling by a request at a time) and this
// many blocks besides, which is what a late game may use up before the card runs dry (26.7 ms).
constexpr double kCushionBlocks = 5;

struct Port {
    uint32_t status = kClosed;
    uint32_t channels = 0, blocks = 0;
    float level = 1.0f;
    uint32_t buffer = 0, size = 0, read_index = 0;  // guest addresses; the read index is 8 bytes
    bool answering = false;  // the game wrote the last block taken: the next is waited for
};

std::mutex g_mutex;
bool g_initialised = false;
std::array<Port, kPorts> g_ports;
std::vector<uint64_t> g_notify;  // the keys of the queues to tell
std::function<std::unique_ptr<qilin::host::AudioOut>()> g_output;
bool g_output_set = false;
std::once_flag g_started;
const bool g_verbose = std::getenv("RR7_AUDIO_VERBOSE") != nullptr;

uint8_t* block_of(const Port& port) {
    const uint64_t index = load64(port.read_index) % port.blocks;
    return guest_memory() + port.buffer + static_cast<uint32_t>(index * audio::block_bytes(port.channels));
}

// Whether every running port the game answers has its next block written (under g_mutex).
bool blocks_ready() {
    for (const Port& port : g_ports)
        if (port.status == kRunning && port.answering && !audio::written(block_of(port), audio::block_bytes(port.channels))) return false;
    return true;
}

// The run's counters: each second's, and the sums (read at the process's end, from another thread).
struct Counts {
    uint64_t blocks = 0, late_wakes = 0, waits = 0, forced = 0, missed = 0, resyncs = 0, underruns = 0, short_frames = 0, clipped = 0;
    int64_t max_late_ns = 0, max_wait_ns = 0;
};
std::mutex g_totals_mutex;
Counts g_totals;

void add(Counts& to, const Counts& c) {
    to.blocks += c.blocks, to.late_wakes += c.late_wakes, to.waits += c.waits, to.forced += c.forced, to.missed += c.missed, to.resyncs += c.resyncs;
    to.underruns += c.underruns, to.short_frames += c.short_frames, to.clipped += c.clipped;
    to.max_late_ns = std::max(to.max_late_ns, c.max_late_ns), to.max_wait_ns = std::max(to.max_wait_ns, c.max_wait_ns);
}

bool troubled(const Counts& c) { return c.late_wakes || c.waits || c.forced || c.missed || c.resyncs || c.underruns || c.clipped; }

void log_counts(const char* when, const Counts& c) {
    using ull = unsigned long long;
    qilin::rt::log("sound%s: %llu blocks; %llu late wake-ups (worst %.2f ms); waited %llu times for the game's block (worst %.2f ms), %llu taken unwritten "
                   "after waiting, %llu missed in all; the card ran dry %llu times (%llu frames short); the schedule restarted %llu times; %llu samples clipped",
                   when, ull{c.blocks}, ull{c.late_wakes}, double(c.max_late_ns) / 1e6, ull{c.waits}, double(c.max_wait_ns) / 1e6, ull{c.forced}, ull{c.missed},
                   ull{c.underruns}, ull{c.short_frames}, ull{c.resyncs}, ull{c.clipped});
}

// One period: every running port's next block, mixed and marked as taken; then the events (to the queues in
// `notify`, which is the mixer's own, kept from period to period). Returns how many blocks were taken unwritten from
// ports whose game had been answering (the game missed its time).
uint32_t take_blocks(std::array<float, kBlockSamples * 2>& mix, std::vector<uint64_t>& notify) {
    mix.fill(0.0f);
    uint32_t missed = 0;
    {
        const std::lock_guard lock(g_mutex);
        for (Port& port : g_ports) {
            if (port.status != kRunning) continue;
            const size_t bytes = audio::block_bytes(port.channels);
            uint8_t* const block = block_of(port);
            const bool written = audio::written(block, bytes);
            if (written) audio::mix_block(block, port.channels, port.level, mix.data());
            else if (port.answering) ++missed;
            port.answering = written;
            audio::mark_unwritten(block, bytes);
            store64(port.read_index, (load64(port.read_index) + 1) % port.blocks);
        }
        notify.assign(g_notify.begin(), g_notify.end());
    }
    // The event's own words are not known here (the game's sound thread reads none of them: it only wakes).
    for (const uint64_t key : notify)
        if (const uint32_t queue = event_queue_by_key(key)) send_event(queue, {0, 0, 0, 0});
    return missed;
}

void mixer() {
    using Clock = std::chrono::steady_clock;
    const auto now_ns = [] { return int64_t{std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()}; };
    if (!qilin::host::set_audio_thread_priority(qilin::host::AudioThread::mixer))
        qilin::rt::log("sound: the system refused the mixer's thread a higher priority; it runs at the usual one");
    const std::unique_ptr<qilin::host::AudioOut> out = g_output ? g_output() : nullptr;
    const auto target_for = [](size_t frames) { return double(frames) / 2 + kCushionBlocks * kBlockSamples; };
    size_t pull = out ? out->pull_frames() : 0;
    qilin::host::AudioPacer::Config config;
    config.period_ns = int64_t{1'000'000'000} * kBlockSamples / kRate;  // (5,333,333 ns: 62 parts in a billion short, which the trim takes up)
    config.block_frames = kBlockSamples;
    config.target_frames = target_for(pull);
    qilin::host::AudioPacer pacer(config);
    if (out)
        qilin::rt::log("sound: out at %u Hz; the card takes %zu frames at a time; %.0f frames (%.1f ms) are kept queued for it", kRate, pull, config.target_frames,
                       config.target_frames * 1000.0 / kRate);
    else
        qilin::rt::log("sound: no output (RR7_QUIET): the ports are taken by the host's clock");

    std::array<float, kBlockSamples * 2> mix{};
    std::array<int16_t, kBlockSamples * 2> samples{};
    std::vector<uint64_t> notify;
    Counts second;
    int64_t next_report = now_ns() + 1'000'000'000;
    bool fade_in = false, prime = true;  // (the card's queue is primed with silence at the start, after a pause or a stall)
    pacer.reset(now_ns());
    for (;;) {
        // Held while the settings menu pauses the game (pause.hpp), here, before a block is taken: the game's ports
        // aren't read on, and its sound thread, which writes a block each time one is taken, waits. The card is given
        // a fade to silence first, and the schedule starts again afterwards, the first block faded in.
        if (pause_requested()) {
            if (out) out->fade_out();
            if (hold_while_paused(HoldPoint::sound)) {
                pacer.reset(now_ns());
                fade_in = prime = true;
                next_report = now_ns() + 1'000'000'000;
            }
            continue;
        }
        bool ready;
        {
            const std::lock_guard lock(g_mutex);
            ready = blocks_ready();
        }
        const qilin::host::AudioPacer::Step step = pacer.step(now_ns(), ready);
        if (step.kind == qilin::host::AudioPacer::Step::Kind::wait) {
            qilin::host::sleep_precisely_until(Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(step.until))));
            continue;
        }
        second.missed += take_blocks(mix, notify);
        second.clipped += audio::to_s16(mix.data(), samples.data(), kBlockSamples, fade_in ? 1 : 0);
        fade_in = false;
        std::optional<double> queued;
        if (out) {
            out->put(samples.data(), kBlockSamples, prime ? static_cast<size_t>(config.target_frames) : 0);
            queued = static_cast<double>(out->queued_frames());
        }
        const int64_t now = now_ns();
        prime = pacer.taken(now, step.forced, queued);
        if (now < next_report) continue;

        // Once a second: what went wrong, if anything.
        next_report = now + 1'000'000'000;
        const qilin::host::AudioPacer::Stats p = pacer.take_stats();
        second.blocks = p.blocks, second.late_wakes = p.late_wakes, second.max_late_ns = p.max_late_ns, second.waits = p.waits;
        second.max_wait_ns = p.max_wait_ns, second.forced = p.forced, second.resyncs = p.resyncs;
        if (out) {
            const qilin::host::AudioOut::PacedStats o = out->take_paced_stats();
            second.underruns = o.underruns, second.short_frames = o.short_frames;
            // The card's request, once it has made some (its buffer's size was only a guess): the target follows it (not
            // the few frames a resampler's margin may add or take).
            if (const size_t now_pull = out->pull_frames(); now_pull > pull + 32 || now_pull + 32 < pull) {
                pull = now_pull;
                config.target_frames = target_for(pull);
                pacer.set_target_frames(config.target_frames);
                qilin::rt::log("sound: the card takes %zu frames at a time; %.0f frames (%.1f ms) are kept queued for it", pull, config.target_frames,
                               config.target_frames * 1000.0 / kRate);
            }
        }
        if (g_verbose || troubled(second)) log_counts("", second);
        if (g_verbose && p.queue_readings)
            qilin::rt::log("sound: the card's queue %.1f to %.1f ms, %.1f on average; the period at %.5f of its own", p.min_queued * 1000.0 / kRate,
                           p.max_queued * 1000.0 / kRate, p.sum_queued / p.queue_readings * 1000.0 / kRate, pacer.period_ratio());
        {
            const std::lock_guard lock(g_totals_mutex);
            add(g_totals, second);
        }
        second = {};
    }
}

// cellAudioInit(), cellAudioQuit()
void cellAudioInit(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (g_initialised) {
        c.r[3] = kAlreadyInit;
        return;
    }
    if (!g_output_set) fatal("the game starts the sound library, and this run has no sound output joined to it");
    g_initialised = true;
    std::call_once(g_started, [] { std::thread(mixer).detach(); });
    c.r[3] = kOk;
}
void cellAudioQuit(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInit;
        return;
    }
    for (Port& port : g_ports) port.status = kClosed;
    g_notify.clear();
    g_initialised = false;
    c.r[3] = kOk;
}

// cellAudioPortOpen(CellAudioPortParam* {channels, blocks, attributes (8 bytes each), level (a float)}, uint32_t* port)
void cellAudioPortOpen(Cpu& c) {
    const uint32_t param = static_cast<uint32_t>(c.r[3]), out = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInit;
        return;
    }
    if (!param || !out) {
        c.r[3] = kParam;
        return;
    }
    const uint64_t channels = load64(param), blocks = load64(param + 8), attributes = load64(param + 16);
    const float level = std::bit_cast<float>(load32(param + 24));
    if ((channels != 2 && channels != 8) || (blocks != 8 && blocks != 16 && blocks != 32))
        fatal("cellAudioPortOpen: %llu channels in %llu blocks, which this runtime doesn't know the library's answer to", static_cast<unsigned long long>(channels),
              static_cast<unsigned long long>(blocks));
    // The attributes: bit 0x1000 says the level given is to be used (otherwise it is 1); others are not known here.
    constexpr uint64_t kInitialLevel = 0x1000;
    if (attributes & ~kInitialLevel) fatal("cellAudioPortOpen: attributes %#llx, some of which this runtime doesn't know", static_cast<unsigned long long>(attributes));
    for (uint32_t i = 0; i < kPorts; ++i) {
        Port& port = g_ports[i];
        if (port.status != kClosed) continue;
        port.channels = static_cast<uint32_t>(channels);
        port.blocks = static_cast<uint32_t>(blocks);
        port.level = (attributes & kInitialLevel) ? level : 1.0f;
        const uint32_t size = port.blocks * static_cast<uint32_t>(audio::block_bytes(port.channels));
        if (port.size < size) {  // (a port opened again in a size it had before uses the same memory)
            port.buffer = allocate(size, 0x10000);
            port.size = size;
        }
        if (!port.read_index) port.read_index = allocate(8, 8);
        audio::mark_unwritten(guest_memory() + port.buffer, size);  // (no block written yet: audio_blocks.hpp)
        store64(port.read_index, 0);
        port.answering = false;
        port.status = kReady;
        store32(out, i);
        qilin::rt::log("the game opens sound port %u: %u channels, %u blocks, level %g", i, port.channels, port.blocks, static_cast<double>(port.level));
        c.r[3] = kOk;
        return;
    }
    c.r[3] = kPortFull;
}

Port* open_port(Cpu& c) {
    if (!g_initialised) {
        c.r[3] = kNotInit;
        return nullptr;
    }
    const uint32_t number = static_cast<uint32_t>(c.r[3]);
    if (number >= kPorts) {
        c.r[3] = kParam;
        return nullptr;
    }
    if (g_ports[number].status == kClosed) {
        c.r[3] = kPortNotOpen;
        return nullptr;
    }
    return &g_ports[number];
}

// cellAudioGetPortConfig(port, CellAudioPortConfig* config): where the read index is, the port's status (1 ready, 2
// running), its channels and blocks (8 bytes each), its buffer's size and address.
void cellAudioGetPortConfig(Cpu& c) {
    const uint32_t config = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_mutex);
    const Port* port = open_port(c);
    if (!port) return;
    store32(config, port->read_index);
    store32(config + 4, port->status);
    store64(config + 8, port->channels);
    store64(config + 16, port->blocks);
    store32(config + 24, port->blocks * static_cast<uint32_t>(audio::block_bytes(port->channels)));
    store32(config + 28, port->buffer);
    c.r[3] = kOk;
}

// cellAudioPortStart(port), cellAudioPortStop(port), cellAudioPortClose(port)
void cellAudioPortStart(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Port* port = open_port(c);
    if (!port) return;
    if (port->status == kRunning) {
        c.r[3] = kPortAlreadyRun;
        return;
    }
    port->status = kRunning;
    port->answering = false;  // (until the game is seen to write a block)
    c.r[3] = kOk;
}
void cellAudioPortStop(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Port* port = open_port(c);
    if (!port) return;
    if (port->status != kRunning) {
        c.r[3] = kPortNotRun;
        return;
    }
    port->status = kReady;
    c.r[3] = kOk;
}
void cellAudioPortClose(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Port* port = open_port(c);
    if (!port) return;
    port->status = kClosed;
    c.r[3] = kOk;
}

// cellAudioSetNotifyEventQueue(key), cellAudioRemoveNotifyEventQueue(key): a queue (by the key it was made with) to
// send an event to each time the ports' blocks have been taken.
void cellAudioSetNotifyEventQueue(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInit;
        return;
    }
    if (std::ranges::find(g_notify, c.r[3]) != g_notify.end()) {
        c.r[3] = kPortOpen;  // (already set: the library's own code for that isn't known here, and this one is not seen by the game)
        return;
    }
    g_notify.push_back(c.r[3]);
    c.r[3] = kOk;
}
void cellAudioRemoveNotifyEventQueue(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kNotInit;
        return;
    }
    if (std::erase(g_notify, c.r[3]) == 0) {
        c.r[3] = kParam;
        return;
    }
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellAudio", "cellAudioInit", cellAudioInit},
    {"cellAudio", "cellAudioQuit", cellAudioQuit},
    {"cellAudio", "cellAudioPortOpen", cellAudioPortOpen},
    {"cellAudio", "cellAudioGetPortConfig", cellAudioGetPortConfig},
    {"cellAudio", "cellAudioPortStart", cellAudioPortStart},
    {"cellAudio", "cellAudioPortStop", cellAudioPortStop},
    {"cellAudio", "cellAudioPortClose", cellAudioPortClose},
    {"cellAudio", "cellAudioSetNotifyEventQueue", cellAudioSetNotifyEventQueue},
    {"cellAudio", "cellAudioRemoveNotifyEventQueue", cellAudioRemoveNotifyEventQueue},
};

}  // namespace

void set_audio_output(std::function<std::unique_ptr<qilin::host::AudioOut>()> open) {
    const std::lock_guard lock(g_mutex);
    g_output = std::move(open);
    g_output_set = true;
}

void audio_report() {
    Counts totals;
    {
        const std::lock_guard lock(g_totals_mutex);
        totals = g_totals;
    }
    if (totals.blocks) log_counts(", over the run", totals);
}

std::span<const NativeImport> audio_imports() { return kImports; }

}  // namespace rr7
