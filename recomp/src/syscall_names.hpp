// Names for the system calls the game makes directly (the `sc` instruction,
// with the call's number in r11).
//
// Unlike an imported function's id, a system call's number says nothing
// about its name, so a name can't be checked by computation: it is a label
// for people, and the runtime goes by the number. `confirmed` says whether an
// independent, permissively licensed source pairs the number with the same
// operation (PSL1GHT's headers, MIT: docs/elf-analysis.md); the others are
// from public descriptions of the system-call table, recalled, and are to be
// confirmed by what the call does when the runtime meets it. The names are
// in the system's own style. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace qilin {

struct SystemCallName {
    uint32_t number;
    std::string_view name;
    bool confirmed;
};

// Every named system call, by number.
std::span<const SystemCallName> system_call_names();

// The call's entry; null if the table doesn't have it.
const SystemCallName* system_call_name(uint32_t number);

}  // namespace qilin
