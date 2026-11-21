#include "kernel.hpp"

#include <bit>
#include <cstring>

namespace rr7 {

namespace {
thread_local uint32_t t_thread = 0;
}

uint32_t current_thread() { return t_thread; }
void set_current_thread(uint32_t id) { t_thread = id; }

uint32_t load32(uint32_t address) {
    uint32_t v;
    std::memcpy(&v, guest_memory() + address, 4);
    return std::byteswap(v);
}
void store32(uint32_t address, uint32_t value) {
    const uint32_t v = std::byteswap(value);
    std::memcpy(guest_memory() + address, &v, 4);
}
uint64_t load64(uint32_t address) {
    uint64_t v;
    std::memcpy(&v, guest_memory() + address, 8);
    return std::byteswap(v);
}
void store64(uint32_t address, uint64_t value) {
    const uint64_t v = std::byteswap(value);
    std::memcpy(guest_memory() + address, &v, 8);
}

}  // namespace rr7
