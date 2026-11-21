// Names for the system functions the game imports.
//
// A PS3 program identifies an imported function by a 32-bit id, not a name.
// The id is computed from the name (the first four bytes of a SHA-1 of the
// name and a fixed suffix), so a name can be checked against an id but an id
// can't be turned back into a name: names have to be proposed. Every name
// here is the SDK's own name for the function, and each was checked by
// computing its id (docs/elf-analysis.md says how and with what). The table
// holds the pairs, not the computation: the suffix is not in this
// repository. Runtime code: no std::format.
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

// The function's name; empty if the table doesn't have it.
std::string_view import_name(std::string_view library, uint32_t id);

}  // namespace qilin
