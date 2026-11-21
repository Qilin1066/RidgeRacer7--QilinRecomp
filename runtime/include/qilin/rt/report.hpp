// Where the runtime's messages go, and how it stops.
//
// Everything goes to stderr and, once a log file is set, to it too: on
// Windows the game's console closes with it, so the file is what is left to
// read. `fatal` is how the runtime fails loudly: one message saying what
// was asked of it that it can't do, and the process ends with exit code 70.
//
// Runtime code: no std::format here (printf-style, checked by the compiler
// where it can).
#pragma once

#include <cstdio>

#if defined(__GNUC__) || defined(__clang__)
#define QILIN_PRINTF(format_index, first_argument) __attribute__((format(printf, format_index, first_argument)))
#else
#define QILIN_PRINTF(format_index, first_argument)
#endif

namespace qilin::rt {

// Mirror the messages to `file` (open for writing; nullptr: stderr only).
void set_log_file(std::FILE* file);

// One line (the newline is added).
void log(const char* format, ...) QILIN_PRINTF(1, 2);

// One line, then the end: exit code 70.
[[noreturn]] void fatal(const char* format, ...) QILIN_PRINTF(1, 2);

}  // namespace qilin::rt
