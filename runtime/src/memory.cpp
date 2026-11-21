#include "qilin/rt/memory.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qilin::rt {

namespace {

constexpr size_t kSpace = size_t{1} << 32;

size_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
}

}  // namespace

AddressSpace::AddressSpace() {
#if defined(_WIN32)
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    base_ = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

AddressSpace::~AddressSpace() {
    if (!base_) return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, kSpace);
#endif
}

bool AddressSpace::commit(uint32_t address, size_t size) {
    if (!base_ || size == 0 || size > kSpace - address) return false;
    const size_t page = page_size();
    const size_t from = address & ~(page - 1);
    const size_t to = (size_t{address} + size + page - 1) & ~(page - 1);
#if defined(_WIN32)
    return VirtualAlloc(base_ + from, to - from, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return mprotect(base_ + from, to - from, PROT_READ | PROT_WRITE) == 0;
#endif
}

}  // namespace qilin::rt
