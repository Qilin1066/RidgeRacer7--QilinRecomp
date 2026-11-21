// The guest's address space: 4 GiB of host addresses reserved in one piece,
// so that a guest address is a host address by one addition, with only what
// the guest has been given backed by memory. An access outside that faults.
//
// Runtime code: no std::format here.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qilin::rt {

class AddressSpace {
public:
    AddressSpace();  // reserves; on failure base() is null
    ~AddressSpace();
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    uint8_t* base() const { return base_; }
    // Backs [address, address + size) with zeroed, readable and writable memory
    // (whole host pages: both ends are rounded outwards). False if the host refuses.
    bool commit(uint32_t address, size_t size);

private:
    uint8_t* base_ = nullptr;
};

}  // namespace qilin::rt
