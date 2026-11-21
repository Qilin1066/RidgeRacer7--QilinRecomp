// cellAudio: the system's sound output, as a game uses it.
//
// A game opens a port: a ring of blocks in memory, each 256 samples of 2
// or 8 channels, as 32-bit floats. 187.5 times a second (every 256 samples
// at 48 kHz) the system takes the next block of every running port, mixes
// them to the output, clears the block, moves the port's read index on,
// and sends an event to the queues the game has named, by which the
// game's sound thread knows to write the next block.
//
// Here one thread does that by the host's clock, and hands the mixed
// samples to the host's sound output (set_audio_sink: main.cpp joins it to
// host/'s AudioOut). The output is stereo (video.cpp); a port of 8
// channels is mixed down to it by the coefficients below, *a stated choice
// (the usual ones: centre and surrounds at -3 dB, no bass channel), to be
// compared with the console's own down-mixer.*
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "events.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

// The library's errors (the SDK's CELL_AUDIO_ERROR_...).
constexpr uint64_t kAlreadyInit = 0x80310701u, kNotInit = 0x80310707u, kParam = 0x80310704u, kPortFull = 0x80310705u, kPortAlreadyRun = 0x80310706u,
                   kPortNotOpen = 0x80310708u, kPortNotRun = 0x8031070bu, kPortOpen = 0x8031070au;
constexpr uint32_t kPorts = 8, kBlockSamples = 256, kRate = 48000;
constexpr uint32_t kReady = 1, kRunning = 2, kClosed = 0x1010;

struct Port {
    uint32_t status = kClosed;
    uint32_t channels = 0, blocks = 0;
    float level = 1.0f;
    uint32_t buffer = 0, size = 0, read_index = 0;  // guest addresses; the read index is 8 bytes
};

std::mutex g_mutex;
bool g_initialised = false;
std::array<Port, kPorts> g_ports;
std::vector<uint64_t> g_notify;  // the keys of the queues to tell
std::function<void(const int16_t*, size_t)> g_sink;
std::once_flag g_started;

// One period: every running port's next block, mixed; then the events.
void mix_once(std::array<float, kBlockSamples * 2>& mix, std::array<int16_t, kBlockSamples * 2>& out) {
    mix.fill(0.0f);
    std::vector<uint64_t> notify;
    bool any = false;
    {
        const std::lock_guard lock(g_mutex);
        for (Port& port : g_ports) {
            if (port.status != kRunning) continue;
            any = true;
            const uint64_t index = load64(port.read_index) % port.blocks;
            const uint32_t block = port.buffer + static_cast<uint32_t>(index) * kBlockSamples * port.channels * 4;
            const auto sample = [&](uint32_t frame, uint32_t channel) { return std::bit_cast<float>(load32(block + (frame * port.channels + channel) * 4)); };
            for (uint32_t i = 0; i < kBlockSamples; ++i) {
                float left, right;
                if (port.channels == 2) {
                    left = sample(i, 0), right = sample(i, 1);
                } else {
                    // Front left and right, centre, bass, surround left and right, back left and right.
                    constexpr float kMinus3dB = 0.70710678f;
                    const float centre = sample(i, 2) * kMinus3dB;
                    left = sample(i, 0) + centre + (sample(i, 4) + sample(i, 6)) * kMinus3dB;
                    right = sample(i, 1) + centre + (sample(i, 5) + sample(i, 7)) * kMinus3dB;
                }
                mix[2 * i] += left * port.level;
                mix[2 * i + 1] += right * port.level;
            }
            std::memset(guest_memory() + block, 0, kBlockSamples * port.channels * 4);
            store64(port.read_index, (index + 1) % port.blocks);
        }
        notify = g_notify;
    }
    if (any) {
        for (size_t i = 0; i < mix.size(); ++i) out[i] = static_cast<int16_t>(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
        g_sink(out.data(), kBlockSamples);
    }
    // The event's own words are not known here (the game's sound thread reads none of them: it only wakes).
    for (const uint64_t key : notify)
        if (const uint32_t queue = event_queue_by_key(key)) send_event(queue, {0, 0, 0, 0});
}

void mixer() {
    using Clock = std::chrono::steady_clock;
    std::array<float, kBlockSamples * 2> mix{};
    std::array<int16_t, kBlockSamples * 2> out{};
    Clock::time_point start = Clock::now();
    for (uint64_t n = 1;; ++n) {
        const Clock::time_point due = start + std::chrono::nanoseconds(n * (1000000000ull * kBlockSamples / kRate));
        // After a long stall (the host slept), start counting again from now and don't play catch-up.
        if (Clock::now() > due + std::chrono::milliseconds(200)) start = Clock::now(), n = 0;
        else std::this_thread::sleep_until(due);
        mix_once(mix, out);
    }
}

// cellAudioInit(), cellAudioQuit()
void cellAudioInit(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    if (g_initialised) {
        c.r[3] = kAlreadyInit;
        return;
    }
    if (!g_sink) fatal("the game starts the sound library, and this run has no sound output joined to it");
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
        const uint32_t size = port.blocks * kBlockSamples * port.channels * 4;
        if (port.size < size) {  // (a port opened again in a size it had before uses the same memory)
            port.buffer = allocate(size, 0x10000);
            port.size = size;
        }
        if (!port.read_index) port.read_index = allocate(8, 8);
        std::memset(guest_memory() + port.buffer, 0, size);
        store64(port.read_index, 0);
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
    store32(config + 24, port->blocks * kBlockSamples * port->channels * 4);
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

void set_audio_sink(std::function<void(const int16_t* left_right, size_t frames)> sink) {
    const std::lock_guard lock(g_mutex);
    g_sink = std::move(sink);
}

std::span<const NativeImport> audio_imports() { return kImports; }

}  // namespace rr7
