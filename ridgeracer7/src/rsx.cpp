// The RSX: a thread that runs the commands the game has written, from
// `get` to `put`, as the console's graphics processor does.
//
// A command is a 32-bit header and its arguments: the header names a
// method (a register of the RSX, by its offset) and says how many
// arguments follow; each argument goes to the next method along, or all to
// the same one. Jumps, calls and returns move `get` about.
//
// Most methods only set state, which is kept here in a table by method;
// the few that do something (draw, clear, write a label) act on that
// state. **A method that isn't in the table of known ones stops the game
// with its number**: none is skipped.
#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <thread>

#include "gcm.hpp"
#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

struct Rsx {
    uint32_t get = 0;
    uint32_t return_to = 0;  // where a call came from, or 0: calls don't nest
    bool in_call = false;
    bool flip_pending = false;  // a flip has been asked for and not yet shown
    std::array<uint32_t, 0x10000 / 4> state{};  // the last argument given to each method
    uint64_t commands = 0;
};
Rsx r;

// A development aid, never a way to play: with RR7_RSX_SURVEY set in the environment, methods not yet
// provided are counted, not run, and at the first flip (or after ten seconds) the list is printed and
// the process stops. It shows the work ahead at once and not one stop at a time.
bool g_survey = false;
struct Surveyed {
    uint64_t count = 0;
    uint32_t first = 0;
};
std::map<uint32_t, Surveyed> g_surveyed;
std::chrono::steady_clock::time_point g_survey_ends;

[[noreturn]] void end_survey() {
    for (const auto& [method, seen] : g_surveyed) qilin::rt::log("survey: method %04x, %llu times, first argument %08x", method, static_cast<unsigned long long>(seen.count), seen.first);
    fatal("the survey of RSX methods is over: %zu not provided", g_surveyed.size());
}

// One method with one argument.
void run_method(uint32_t method, uint32_t argument, uint32_t at) {
    switch (method) {
    case 0x0050:  // NV406E_SET_REFERENCE: the game's own mark of how far the RSX has got, readable beside put and get
        store32(rsx_setup().control + 8, argument);
        return;
    case kMethodWaitFlip:  // (the runtime's own) wait until the last flip has been shown
        if (r.flip_pending) fatal("the RSX is to wait for a flip, and showing pictures isn't provided yet");
        return;
    default:
        if (g_survey) {
            auto& seen = g_surveyed[method];
            if (seen.count++ == 0) seen.first = argument;
            if (method == kMethodFlip || std::chrono::steady_clock::now() > g_survey_ends) end_survey();
            return;
        }
        fatal("the RSX is given method %04x (argument %08x, at offset %08x), which this runtime doesn't provide yet", method, argument, at);
    }
}

void serve() {
    const uint32_t control = rsx_setup().control;
    r.get = load32(control + 4);
    g_survey = std::getenv("RR7_RSX_SURVEY") != nullptr;
    g_survey_ends = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    uint32_t idle = 0;
    for (;;) {
        const uint32_t put = load32(control);
        if (r.get == put) {
            if (g_survey && std::chrono::steady_clock::now() > g_survey_ends) end_survey();
            // Nothing to do. (The console's RSX is told when put moves; here the game's own code moves it
            // in memory, so it is watched: closely at first, then at a millisecond's remove.)
            if (++idle < 2000) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        idle = 0;
        const uint32_t address = rsx_io_address(r.get);
        if (!address) fatal("the RSX's get is at offset %08x, where nothing is mapped (put %08x)", r.get, put);
        const uint32_t header = load32(address);
        uint32_t next;
        if ((header & 0xe0000003u) == 0x20000000u) {  // a jump, as first made
            next = header & 0x1ffffffcu;
            if (next == r.get) {  // to itself: the game's way of holding the RSX here until it writes over it
                if (g_survey && std::chrono::steady_clock::now() > g_survey_ends) end_survey();
                std::this_thread::yield();
                continue;
            }
        } else if ((header & 3) == 1) {  // a jump, as made later
            next = header & 0xfffffffcu;
        } else if ((header & 3) == 2) {  // a call
            if (r.in_call) fatal("the RSX is given a call inside a call, at offset %08x", r.get);
            r.in_call = true;
            r.return_to = r.get + 4;
            next = header & 0xfffffffcu;
        } else if (header == 0x00020000u) {  // a return
            if (!r.in_call) fatal("the RSX is given a return with no call, at offset %08x", r.get);
            r.in_call = false;
            next = r.return_to;
        } else {
            if (header & 0xa0030003u) fatal("the RSX is given a command it can't read: %08x at offset %08x", header, r.get);
            const uint32_t count = (header >> 18) & 0x7ff, method = header & 0xfffc;
            const bool same = (header & 0x40000000u) != 0;  // every argument to the one method
            for (uint32_t i = 0; i < count; ++i) run_method(same ? method : method + 4 * i, load32(address + 4 + 4 * i), r.get);
            next = r.get + 4 + 4 * count;
            ++r.commands;
        }
        r.get = next;
        store32(control + 4, next);
    }
}

}  // namespace

void start_rsx() { std::thread(serve).detach(); }

}  // namespace rr7
