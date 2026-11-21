// The console's terminal (system calls 402 and 403): where a program's
// printf goes. What the game writes is shown in the runtime's log, a line
// at a time, marked as the game's own words.
#include <mutex>
#include <string>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

std::mutex g_mutex;
std::string g_line;

// sys_tty_write(channel, buffer, length, uint32_t* written)
void sys_tty_write(Cpu& c) {
    const uint32_t buffer = static_cast<uint32_t>(c.r[4]), length = static_cast<uint32_t>(c.r[5]), written = static_cast<uint32_t>(c.r[6]);
    {
        const std::lock_guard lock(g_mutex);
        for (uint32_t i = 0; i < length; ++i) {
            const char ch = static_cast<char>(guest_memory()[buffer + i]);
            if (ch == '\n') {
                qilin::rt::log("[the game] %s", g_line.c_str());
                g_line.clear();
            } else if (ch != '\r') {
                g_line += ch;
            }
        }
    }
    if (written) store32(written, length);
    c.r[3] = kOk;
}

// sys_tty_read(channel, buffer, length, uint32_t* read): nobody is typing.
void sys_tty_read(Cpu& c) {
    if (const uint32_t read = static_cast<uint32_t>(c.r[6])) store32(read, 0);
    c.r[3] = kOk;
}

constexpr NativeSystemCall kCalls[] = {
    {402, sys_tty_read},
    {403, sys_tty_write},
};

}  // namespace

std::span<const NativeSystemCall> tty_system_calls() { return kCalls; }

}  // namespace rr7
