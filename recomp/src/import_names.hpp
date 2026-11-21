// Names for the system functions the game imports.
//
// A PS3 program identifies an imported function by a 32-bit id, not a name.
// The id is computed from the name: the first four bytes, read
// little-endian, of the SHA-1 of the name followed by a fixed 16-byte
// suffix. So a name can be checked against an id, but an id can't be turned
// back into a name: names have to be proposed. Every name here is the SDK's
// own name for the function, and the tests recompute every id from its name.
// (The suffix is a constant of the naming scheme, the same for every PS3
// program. It is not a key: it decrypts nothing and protects nothing.)
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace qilin {

struct ImportName {
    std::string_view library;
    uint32_t id;
    std::string_view name;
};

// Every named import, by library (in the program's order), then by id.
std::span<const ImportName> import_names();

// The id of a function of this name.
uint32_t import_id(std::string_view name);

// The function's name; empty if the table doesn't have it.
std::string_view import_name(std::string_view library, uint32_t id);

}  // namespace qilin
