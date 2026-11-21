// The imported system functions this runtime provides, by the SDK's names.
//
// One function here for each the game calls, added as the game reaches it:
// a call to one that isn't here stops the game and names it (machine.cpp).
// Each does what the system's function does, as far as the game can tell;
// none answers with a guess. Arguments are in r3 onwards, the result in r3.
#include <cstring>

#include "machine.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

constexpr uint64_t kOk = 0;  // CELL_OK

// sys_initialize_tls(thread id, the thread-local image's address, its size in the file, its size in memory)
//
// Gives the calling thread its thread-local storage: a copy of the image,
// zeroes after it, and r13 pointing 0x7000 past its start (compiled code
// reaches a thread-local variable at r13 - 0x7000 + its offset).
void sys_initialize_tls(Cpu& c) {
    const uint32_t image = static_cast<uint32_t>(c.r[4]), file_size = static_cast<uint32_t>(c.r[5]), memory_size = static_cast<uint32_t>(c.r[6]);
    if (c.r[13] != 0) return;  // already set up (the start-up code calls it once for the first thread)
    if (file_size > memory_size) qilin::rt::fatal("sys_initialize_tls: an image of %u bytes for %u bytes of thread-local storage", file_size, memory_size);
    const uint32_t block = allocate(memory_size, 16);
    std::memcpy(guest_memory() + block, guest_memory() + image, file_size);
    c.r[13] = block + 0x7000u;
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_initialize_tls", sys_initialize_tls},
};

}  // namespace

std::span<const NativeImport> native_imports() { return kImports; }

}  // namespace rr7
