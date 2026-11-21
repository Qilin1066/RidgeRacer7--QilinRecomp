#include "qilin/rt/report.hpp"

#include <cstdarg>
#include <cstdlib>
#include <mutex>

namespace qilin::rt {

namespace {

std::FILE* g_log = nullptr;
std::mutex g_mutex;

void write_line(const char* prefix, const char* format, std::va_list arguments) {
    const std::lock_guard lock(g_mutex);
    for (std::FILE* out : {stderr, g_log}) {
        if (!out) continue;
        std::va_list copy;
        va_copy(copy, arguments);
        std::fputs(prefix, out);
        std::vfprintf(out, format, copy);
        std::fputc('\n', out);
        std::fflush(out);
        va_end(copy);
    }
}

}  // namespace

void set_log_file(std::FILE* file) {
    const std::lock_guard lock(g_mutex);
    g_log = file;
}

void log(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    write_line("", format, arguments);
    va_end(arguments);
}

void fatal(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    write_line("stopped: ", format, arguments);
    va_end(arguments);
    std::exit(70);
}

}  // namespace qilin::rt
