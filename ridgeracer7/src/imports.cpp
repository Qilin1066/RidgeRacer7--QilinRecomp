// The imported system functions this runtime provides, by the SDK's names.
//
// One function here for each the game calls, added as the game reaches it:
// a call to one that isn't here stops the game and names it (machine.cpp).
// Each does what the system's function does, as far as the game can tell;
// none answers with a guess. Arguments are in r3 onwards, the result in r3.
#include <cstring>

#include "kernel.hpp"
#include "machine.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

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

// sys_time_get_system_time(): microseconds since the system started.
void sys_time_get_system_time(Cpu& c) { c.r[3] = system_time(); }

// sys_ppu_thread_get_id(sys_ppu_thread_t* id): the calling thread's.
void sys_ppu_thread_get_id(Cpu& c) {
    store64(static_cast<uint32_t>(c.r[3]), current_thread());
    c.r[3] = kOk;
}

// sys_process_exit(status): the process ends.
void sys_process_exit(Cpu& c) { exit_process(static_cast<int>(c.r[3])); }

// cellSysmoduleLoadModule(id), cellSysmoduleUnloadModule(id)
// On a console these load one of the system's libraries into the process. Here every system
// library is part of the runtime already, so there is nothing to load: the call succeeds, and
// each of the library's functions is provided, or stops the game by name, when it is called.
void cellSysmoduleLoadModule(Cpu& c) {
    qilin::rt::log("the game loads system module %#x", static_cast<uint32_t>(c.r[3]));
    c.r[3] = kOk;
}
void cellSysmoduleUnloadModule(Cpu& c) {
    qilin::rt::log("the game unloads system module %#x", static_cast<uint32_t>(c.r[3]));
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"sysPrxForUser", "sys_initialize_tls", sys_initialize_tls},
    {"cellSysmodule", "cellSysmoduleLoadModule", cellSysmoduleLoadModule},
    {"cellSysmodule", "cellSysmoduleUnloadModule", cellSysmoduleUnloadModule},
    {"sysPrxForUser", "sys_time_get_system_time", sys_time_get_system_time},
    {"sysPrxForUser", "sys_ppu_thread_get_id", sys_ppu_thread_get_id},
    {"sysPrxForUser", "sys_process_exit", sys_process_exit},
};

}  // namespace

void (*find_native_import(std::string_view library, std::string_view name))(Cpu&) {
    for (const std::span<const NativeImport> table : {std::span<const NativeImport>(kImports), sync_imports(), sysutil_imports(), pad_imports(), usbd_imports(), mempool_imports(), thread_imports(), gcm_imports(), rtc_imports(), spu_imports(), audio_imports(), np_imports(), net_imports(), fs_imports(), gamedata_imports(), spurs_imports(), font_imports(), video_imports()})
        for (const NativeImport& import : table)
            if (library == import.library && name == import.name) return import.handler;
    return nullptr;
}

}  // namespace rr7
