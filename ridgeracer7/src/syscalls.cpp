// The system calls this runtime provides, by number (the names are
// syscall_names.cpp's). As with the imports: one for each the game makes,
// added as the game reaches it; any other stops the game and names itself.
#include "machine.hpp"

namespace rr7 {

void (*find_native_system_call(uint32_t number))(Cpu&) {
    for (const std::span<const NativeSystemCall> table : {memory_system_calls(), thread_system_calls(), sync_system_calls(), tty_system_calls(), event_system_calls(), timer_system_calls(), spu_system_calls()})
        for (const NativeSystemCall& call : table)
            if (call.number == number) return call.handler;
    return nullptr;
}

}  // namespace rr7
