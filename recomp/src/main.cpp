// qilin: Ridge Racer 7 (PS3) static recompiler (work in progress).
//
// <game> is your copy however you hold it: the disc's folder (the one holding
// PS3_GAME), PS3_GAME or USRDIR inside it, EBOOT.BIN, or the decrypted
// program (EBOOT.elf).
//
//   qilin info    <game> [--json out.json]   the disc's description, EBOOT.BIN's plain header, and
//                                            the program: hashes, supported build, entry point,
//                                            segments, sections, imports by library
//   qilin imports <game>                     every library imported and each function id taken from it
//   qilin exports <game>                     what the program offers the system
//   qilin sfo     <game>                     every entry of PARAM.SFO
//
// EBOOT.BIN is encrypted and this project has no keys: `info` reads what its
// header leaves plain, and the rest needs the decrypted program
// (docs/elf-analysis.md says how to make it from your own disc).
// Exit status is non-zero on any error; nothing fails silently.
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "elf.hpp"
#include "self.hpp"
#include "sfo.hpp"
#include "title.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;
namespace fs = std::filesystem;

namespace {

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage (<game>: the disc's folder, EBOOT.BIN, or the decrypted EBOOT.elf):\n"
               "  qilin info    <game> [--json out.json]\n"
               "  qilin imports <game>\n"
               "  qilin exports <game>\n"
               "  qilin sfo     <game>\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> json;
};

Args parse_args(std::span<char*> argv) {
    Args a;
    a.command = argv[1];
    a.input = argv[2];
    for (size_t i = 3; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--json" && i + 1 < argv.size()) a.json = argv[++i];
        else throw Failure{std::format("unknown argument {}", arg)};
    }
    return a;
}

GameFiles files_of(const Args& a) {
    auto files = find_game(a.input);
    if (!files) throw Failure{files.error()};
    return *files;
}

Elf elf_of(const GameFiles& files) {
    auto elf = load_game_elf(files);
    if (!elf) throw Failure{elf.error()};
    return std::move(*elf);
}

std::string segment_type(uint32_t type) {
    switch (type) {
        case kPtLoad: return "load";
        case kPtTls: return "tls";
        case kPtProcParam: return "process";
        case kPtProcPrx: return "prx";
        default: return std::format("{:#x}", type);
    }
}

std::string permissions(uint32_t flags) {
    return std::format("{}{}{}", flags & kPfRead ? 'r' : '-', flags & kPfWrite ? 'w' : '-', flags & kPfExec ? 'x' : '-');
}

std::string known(std::string_view sha256, bool self) {
    const KnownBuild* b = known_build(sha256);
    if (b && b->self == self) return std::format("supported build: {}", b->name);
    return "not a build this port has recorded";
}

void print_layout(const ElfLayout& l, const std::vector<SelfSegment>* stored) {
    std::println("  program headers:");
    for (size_t i = 0; i < l.segments.size(); ++i) {
        const ElfSegment& s = l.segments[i];
        std::print("    {:2} {:8} {} {:#010x}  in file {:#9x}  in memory {:#9x}  flags {:#x}", i, segment_type(s.type),
                   permissions(s.flags), s.address, s.file_size, s.memory_size, s.flags);
        if (stored && (*stored)[i].encryption == 1) std::print("  encrypted");
        std::println("");
    }
    if (l.sections.empty()) return;
    std::println("  sections:");
    for (const ElfSection& s : l.sections) {
        if (s.type == kShtNull) continue;
        std::println("    {:2} {:#010x} {:#9x}  {}{}{} {}{}", s.index, s.address, s.size, s.flags & kShfAlloc ? 'a' : '-',
                     s.flags & kShfWrite ? 'w' : '-', s.flags & kShfExec ? 'x' : '-', s.type == kShtNobits ? "(zero-filled) " : "",
                     s.name);
    }
}

std::string json_text(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += std::format("\\{}", c);
        else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else out += c;
    }
    return out + "\"";
}

void write_json(const std::string& path, const std::optional<Sfo>& sfo, const std::optional<Self>& self, const Elf* elf) {
    std::string j = "{\n";
    j += std::format("  \"qilin\": {}", json_text(QILIN_VERSION));
    if (sfo) {
        j += ",\n  \"sfo\": {";
        for (size_t i = 0; i < sfo->entries.size(); ++i) {
            const SfoEntry& e = sfo->entries[i];
            j += std::format("{}\n    {}: {}", i ? "," : "", json_text(e.key), e.integer ? std::to_string(e.number) : json_text(e.text));
        }
        j += "\n  }";
    }
    const auto layout = [&](const ElfLayout& l) {
        std::string s = std::format("\"entry\": {}, \"segments\": [", l.entry);
        for (size_t i = 0; i < l.segments.size(); ++i) {
            const ElfSegment& g = l.segments[i];
            s += std::format("{}\n      {{\"type\": {}, \"flags\": {}, \"address\": {}, \"file_size\": {}, \"memory_size\": {}}}", i ? "," : "",
                             g.type, g.flags, g.address, g.file_size, g.memory_size);
        }
        s += "],\n    \"sections\": [";
        for (size_t i = 0; i < l.sections.size(); ++i) {
            const ElfSection& c = l.sections[i];
            s += std::format("{}\n      {{\"name\": {}, \"type\": {}, \"flags\": {}, \"address\": {}, \"size\": {}}}", i ? "," : "",
                             json_text(c.name), c.type, c.flags, c.address, c.size);
        }
        return s + "]";
    };
    if (self) {
        j += std::format(",\n  \"self\": {{\n    \"sha256\": {}, \"size\": {}, \"key_revision\": {}, \"type\": {},\n    ",
                         json_text(self->sha256), self->file_size, self->key_revision, self->self_type);
        j += layout(self->elf) + ",\n    \"libraries\": [";
        for (size_t i = 0; i < self->libraries.size(); ++i) {
            const SdkLibrary& l = self->libraries[i];
            j += std::format("{}\n      {{\"name\": {}, \"version\": {}, \"objects\": {}}}", i ? "," : "", json_text(l.name),
                             json_text(l.version), l.objects);
        }
        j += "]\n  }";
    }
    if (elf) {
        j += std::format(",\n  \"elf\": {{\n    \"sha256\": {}, \"size\": {}, \"entry_code\": {}, \"toc\": {},\n    ",
                         json_text(elf->sha256()), elf->file_size(), elf->entry_code(), elf->entry_toc());
        j += layout(elf->layout());
        if (const auto& p = elf->process_param())
            j += std::format(",\n    \"process\": {{\"sdk_version\": {}, \"priority\": {}, \"stack_size\": {}, \"malloc_page_size\": {}}}",
                             p->sdk_version, p->primary_priority, p->primary_stack_size, p->malloc_page_size);
        j += ",\n    \"imports\": [";
        for (size_t i = 0; i < elf->imports().size(); ++i) {
            const ImportLibrary& l = elf->imports()[i];
            j += std::format("{}\n      {{\"library\": {}, \"functions\": [", i ? "," : "", json_text(l.name));
            for (size_t k = 0; k < l.functions.size(); ++k)
                j += std::format("{}{{\"id\": {}, \"slot\": {}, \"stub\": {}}}", k ? ", " : "", l.functions[k].id, l.functions[k].slot,
                                 l.functions[k].stub ? l.functions[k].initial : 0);
            j += std::format("], \"variables\": {}}}", l.variables.size());
        }
        j += "]\n  }";
    }
    j += "\n}\n";
    std::ofstream f(path, std::ios::binary);
    f << j;
    f.close();
    if (!f) throw Failure{std::format("cannot write {}", path)};
}

int cmd_info(const Args& a) {
    const GameFiles files = files_of(a);
    std::optional<Sfo> sfo;
    if (!files.sfo.empty()) {
        auto read = Sfo::load(files.sfo);
        if (!read) throw Failure{read.error()};
        sfo = std::move(*read);
        std::println("{}", files.sfo.string());
        std::println("  title          {}", sfo->text("TITLE").value_or("(none)"));
        std::println("  title id       {}", sfo->text("TITLE_ID").value_or("(none)"));
        std::println("  version        {}", sfo->text("VERSION").value_or("(none)"));
        std::println("  system version {}", sfo->text("PS3_SYSTEM_VER").value_or("(none)"));
        if (const auto r = sfo->number("RESOLUTION")) std::println("  picture        {}", sfo_resolutions(*r));
        if (const auto s = sfo->number("SOUND_FORMAT")) std::println("  sound          {}", sfo_sound_formats(*s));
    }
    std::optional<Self> self;
    if (!files.self.empty()) {
        auto read = Self::load(files.self);
        if (!read) throw Failure{read.error()};
        self = std::move(*read);
        std::println("{}", files.self.string());
        std::println("  SHA-256        {}", self->sha256);
        std::println("  {}", known(self->sha256, true));
        std::println("  SELF: {} bytes, key revision {:#x}, type {}, authentication id {:#018x}", self->file_size, self->key_revision,
                     self->self_type, self->authentication_id);
        std::println("  what its header leaves plain (the program itself is encrypted):");
        std::println("  entry point's descriptor {:#x}", self->elf.entry);
        print_layout(self->elf, &self->segments);
        if (!self->libraries.empty()) {
            std::println("  linked from these SDK libraries (name, version, object files):");
            for (const SdkLibrary& l : self->libraries) std::println("    {:36} {:8} {:4}", l.name, l.version, l.objects);
        }
    }
    if (files.elf.empty()) {
        if (a.json) write_json(*a.json, sfo, self, nullptr);
        if (files.self.empty()) return 0;  // PARAM.SFO alone
        std::fflush(stdout);
        throw Failure{how_to_decrypt(files.self)};
    }
    const Elf elf = elf_of(files);
    std::println("{}", files.elf.string());
    std::println("  SHA-256        {}", elf.sha256());
    std::println("  {}", known(elf.sha256(), false));
    if (self) std::println("  the same program as EBOOT.BIN's plain headers describe");
    std::println("  entry point    descriptor {:#x}: code {:#x}, r2 {:#x}", elf.entry_descriptor(), elf.entry_code(), elf.entry_toc());
    if (const auto& p = elf.process_param())
        std::println("  process        SDK version {:#x}, main thread priority {}, stack {:#x}, malloc page size {:#x}", p->sdk_version,
                     p->primary_priority, p->primary_stack_size, p->malloc_page_size);
    if (const auto& t = elf.tls())
        std::println("  thread-local   at {:#x}: {:#x} bytes initialised, {:#x} in all, aligned to {}", t->address, t->file_size,
                     t->memory_size, t->align);
    print_layout(elf.layout(), nullptr);
    size_t functions = 0, variables = 0, stubs = 0;
    std::println("  imports:");
    for (const ImportLibrary& l : elf.imports()) {
        std::println("    {:28} {:4} functions {:3} variables", l.name, l.functions.size(), l.variables.size());
        functions += l.functions.size();
        variables += l.variables.size() + l.tls_variables;
        for (const ImportFunction& f : l.functions) stubs += f.stub;
    }
    std::println("  {} libraries, {} functions, {} variables; {} of the functions' slots point at a stub that calls through them",
                 elf.imports().size(), functions, variables, stubs);
    size_t exported = 0;
    for (const ExportLibrary& l : elf.exports()) exported += l.functions.size() + l.variables.size();
    std::println("  exports: {} entries, {} symbols", elf.exports().size(), exported);
    if (a.json) write_json(*a.json, sfo, self, &elf);
    return 0;
}

int cmd_imports(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    for (const ImportLibrary& l : elf.imports()) {
        std::println("{} (version {:#x}, attributes {:#x}, entry at {:#x})", l.name, l.version, l.attributes, l.entry);
        for (const ImportFunction& f : l.functions)
            std::println("  function {:08x}  slot {:#x}  {}", f.id, f.slot,
                         f.stub ? std::format("stub {:#x}", f.initial) : std::format("holds {:#x} (not a stub)", f.initial));
        for (const ImportVariable& v : l.variables) std::println("  variable {:08x}  slot {:#x}", v.id, v.slot);
        if (l.tls_variables) std::println("  {} thread-local variables", l.tls_variables);
    }
    return 0;
}

int cmd_exports(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    for (const ExportLibrary& l : elf.exports()) {
        std::println("{} (version {:#x}, attributes {:#x})", l.name.empty() ? "(the program's own)" : l.name, l.version, l.attributes);
        for (const ExportedSymbol& s : l.functions) std::println("  function {:08x}  {:#x}", s.id, s.address);
        for (const ExportedSymbol& s : l.variables) std::println("  variable {:08x}  {:#x}", s.id, s.address);
    }
    return 0;
}

int cmd_sfo(const Args& a) {
    const GameFiles files = files_of(a);
    if (files.sfo.empty()) throw Failure{std::format("no PARAM.SFO found for {}", a.input)};
    auto sfo = Sfo::load(files.sfo);
    if (!sfo) throw Failure{sfo.error()};
    for (const SfoEntry& e : sfo->entries) {
        if (e.integer) std::println("{:16} {} ({:#x})", e.key, e.number, e.number);
        else std::println("{:16} {}", e.key, e.text);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::println("qilin " QILIN_VERSION);
        return 0;
    }
    if (argc < 3) return usage();
    try {
        const Args a = parse_args(std::span(argv, static_cast<size_t>(argc)));
        if (a.command == "info") return cmd_info(a);
        if (a.command == "imports") return cmd_imports(a);
        if (a.command == "exports") return cmd_exports(a);
        if (a.command == "sfo") return cmd_sfo(a);
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
        return 1;
    }
}
