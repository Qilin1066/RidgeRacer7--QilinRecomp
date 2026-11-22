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
//   qilin id      <name>                     the id a system function of that name has
//   qilin archive <game> [--files]           RR7.DAT's directory, read from the program: its packs, and every file
//   qilin data    <game>                     the game's own files, and the headers of its sound banks and films
//   qilin shaders <game>                     the compiled shader programs the program carries
//   qilin spu     <game>                     the SPU programs the program carries
//   qilin cfg     <game>                     the control-flow pass: functions, switches, system calls, coverage
//   qilin spu-cfg     <game>                 the SPU programs' control flow: functions, coverage, channels
//   qilin spu-insns   <game>                 every word of the SPU programs' code
//   qilin spu-profile <game>                 how often the SPU programs use each instruction
//   qilin spu-disasm  <file> [--base address] [--ignore-reserved]   a file of raw SPU words
//   qilin lift    <game> [--out folder]      lift the PPU code to C++: how many functions the lifter covers, and why not the rest
//   qilin insns   <game>                     every word of the program's code: address, word, instruction
//   qilin profile <game>                     how often the code uses each instruction, and what doesn't decode
//   qilin disasm  <file> [--base address]    a file of raw big-endian PPU words, as `insns` prints them
//
// EBOOT.BIN is encrypted and this project has no keys: `info` reads what its
// header leaves plain, and the rest needs the decrypted program
// (docs/elf-analysis.md says how to make it from your own disc).
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
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

#include "archive.hpp"
#include "bytes.hpp"
#include "cfg.hpp"
#include "elf.hpp"
#include "import_names.hpp"
#include "lift.hpp"
#include "media.hpp"
#include "ppu.hpp"
#include "self.hpp"
#include "sfo.hpp"
#include "spu.hpp"
#include "spu_cfg.hpp"
#include "spu_lift.hpp"
#include "rsx_program.hpp"
#include "shaders.hpp"
#include "spu_image.hpp"
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
               "  qilin id      <function name>\n"
               "  qilin archive <game> [--files]\n"
               "  qilin data    <game>\n"
               "  qilin shaders <game>\n"
               "  qilin spu     <game>\n"
               "  qilin cfg     <game>\n"
               "  qilin spu-cfg     <game>\n"
               "  qilin spu-insns   <game>\n"
               "  qilin spu-profile <game>\n"
               "  qilin spu-disasm  <file of big-endian words> [--base address] [--ignore-reserved]\n"
               "  qilin lift    <game> [--out folder]\n"
               "  qilin insns   <game>\n"
               "  qilin profile <game>\n"
               "  qilin disasm  <file of big-endian words> [--base address]\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> json, out;
    uint32_t base = 0;
    bool list_files = false, ignore_reserved = false, keep_pc = false, keep_registers = false, disasm = false;
};

uint32_t parse_address(std::string_view text) {
    const std::string_view all = text;
    const bool hex = text.starts_with("0x") || text.starts_with("0X");
    if (hex) text.remove_prefix(2);
    uint32_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, hex ? 16 : 10);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) throw Failure{std::format("not an address: {}", all)};
    return value;
}

Args parse_args(std::span<char*> argv) {
    Args a;
    a.command = argv[1];
    a.input = argv[2];
    for (size_t i = 3; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--json" && i + 1 < argv.size()) a.json = argv[++i];
        else if (arg == "--out" && i + 1 < argv.size()) a.out = argv[++i];
        else if (arg == "--base" && i + 1 < argv.size()) a.base = parse_address(argv[++i]);
        else if (arg == "--files") a.list_files = true;
        else if (arg == "--disasm") a.disasm = true;
        else if (arg == "--pc") a.keep_pc = true;
        else if (arg == "--registers") a.keep_pc = a.keep_registers = true;
        else if (arg == "--ignore-reserved") a.ignore_reserved = true;
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
    if (std::ranges::all_of(elf.sections(), [](const ElfSection& s) { return s.name.empty(); }) && !elf.sections().empty())
        std::println("  (no section names: the table of names is empty in this file, as RPCS3 writes it)");
    if (const auto images = find_spu_images(elf)) std::println("  SPU programs carried: {}", images->size());
    else throw Failure{images.error()};
    size_t functions = 0, variables = 0, stubs = 0, named = 0;
    std::println("  imports:");
    for (const ImportLibrary& l : elf.imports()) {
        std::println("    {:28} {:4} functions {:3} variables", l.name, l.functions.size(), l.variables.size());
        functions += l.functions.size();
        variables += l.variables.size() + l.tls_variables;
        for (const ImportFunction& f : l.functions) {
            stubs += f.stub;
            named += !import_name(l.name, f.id).empty();
        }
    }
    std::println("  {} libraries, {} functions, {} variables; {} of the functions' slots point at a stub that calls through them",
                 elf.imports().size(), functions, variables, stubs);
    std::println("  {} of the functions have a name", named);
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
        for (const ImportFunction& f : l.functions) {
            const std::string_view name = import_name(l.name, f.id);
            std::println("  function {:08x}  slot {:#x}  {}  {}", f.id, f.slot,
                         f.stub ? std::format("stub {:#x}", f.initial) : std::format("holds {:#x} (not a stub)", f.initial),
                         name.empty() ? "(no name)" : name);
        }
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

// The program's code: its executable sections, or its executable segments
// if it has no section table. Each is a whole number of words.
struct CodeRange {
    uint32_t address = 0;
    std::span<const uint8_t> bytes;
};

std::vector<CodeRange> code_of(const Elf& elf) {
    std::vector<CodeRange> code;
    const auto add = [&](uint64_t address, uint64_t size, std::string_view what) {
        if (size == 0) return;
        const auto bytes = elf.bytes_at(address, size);
        if (bytes.empty()) throw Failure{std::format("{} at {:#x} ({:#x} bytes) isn't in the file", what, address, size)};
        if (address % 4 || size % 4)
            throw Failure{std::format("{} at {:#x} ({:#x} bytes) isn't a whole number of words", what, address, size)};
        code.push_back({static_cast<uint32_t>(address), bytes});
    };
    for (const ElfSection& s : elf.sections())
        if (s.type == kShtProgbits && (s.flags & kShfAlloc) && (s.flags & kShfExec)) add(s.address, s.size, "code section");
    if (code.empty())
        for (const ElfSegment& s : elf.segments())
            if (s.type == kPtLoad && (s.flags & kPfExec)) add(s.address, s.file_size & ~uint64_t{3}, "code segment");
    if (code.empty()) throw Failure{"the program has no code"};
    return code;
}

void print_insns(const CodeRange& range) {
    std::string out;
    for (size_t i = 0; i + 4 <= range.bytes.size(); i += 4) {
        const uint32_t pc = range.address + static_cast<uint32_t>(i), word = read_be32(range.bytes.data() + i);
        out += std::format("{:08x} {:08x} {}\n", pc, word, ppu::disasm(ppu::decode(word), pc));
        if (out.size() >= 1 << 16) {
            std::fwrite(out.data(), 1, out.size(), stdout);
            out.clear();
        }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
}

int cmd_insns(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    for (const CodeRange& range : code_of(elf)) print_insns(range);
    return 0;
}

int cmd_disasm(const Args& a) {
    const auto file = read_file(a.input);
    if (!file) throw Failure{file.error()};
    if (file->size() % 4) throw Failure{std::format("{} is {} bytes: not a whole number of words", a.input, file->size())};
    print_insns({a.base, *file});
    return 0;
}

// How often each instruction occurs (with its suffixes: "addo." apart from
// "add"), by family, and the words that don't decode. A code section holds
// data too (jump tables, constants), so some invalid words are expected:
// the control-flow pass says which are reached.
int cmd_profile(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    struct Count {
        std::string name;
        size_t count = 0;
        uint32_t first = 0;
    };
    std::vector<std::vector<Count>> by_op(static_cast<size_t>(ppu::Op::count_));
    size_t words = 0, invalid = 0, zero = 0, vector = 0, fp = 0, wide = 0;
    std::vector<std::pair<uint32_t, uint32_t>> first_invalid;
    for (const CodeRange& range : code_of(elf)) {
        for (size_t i = 0; i + 4 <= range.bytes.size(); i += 4) {
            const uint32_t pc = range.address + static_cast<uint32_t>(i), word = read_be32(range.bytes.data() + i);
            const ppu::Insn in = ppu::decode(word);
            ++words;
            if (!in.valid()) {
                ++invalid;
                if (word == 0) ++zero;
                else if (first_invalid.size() < 16) first_invalid.emplace_back(pc, word);
                continue;
            }
            const uint32_t flags = ppu::spec(in.op).flags;
            vector += (flags & ppu::kVector) != 0;
            fp += (flags & ppu::kFloat) != 0;
            wide += (flags & ppu::k64) != 0;
            auto& forms = by_op[static_cast<size_t>(in.op)];
            std::string name = ppu::mnemonic(in);
            auto it = std::ranges::find(forms, name, &Count::name);
            if (it == forms.end()) it = forms.insert(forms.end(), Count{std::move(name), 0, pc});
            ++it->count;
        }
    }
    std::vector<Count> all;
    size_t unused = 0;
    for (size_t i = 1; i < by_op.size(); ++i) {
        unused += by_op[i].empty();
        all.insert(all.end(), by_op[i].begin(), by_op[i].end());
    }
    std::ranges::sort(all, [](const Count& x, const Count& y) { return x.count != y.count ? x.count > y.count : x.name < y.name; });
    std::println("{} words of code: {} decode, {} don't ({} of those are zero)", words, words - invalid, invalid, zero);
    std::println("{} floating-point, {} VMX, {} 64-bit-only instructions", fp, vector, wide);
    std::println("{} of the decoder's {} instructions are used; {} are not", by_op.size() - 1 - unused, by_op.size() - 1, unused);
    for (const Count& c : all) std::println("{:9} {:12} first at {:#x}", c.count, c.name, c.first);
    if (!first_invalid.empty()) {
        std::println("the first non-zero words that don't decode:");
        for (const auto& [pc, word] : first_invalid) std::println("  {:#x} {:08x}", pc, word);
    }
    std::println("not used:");
    std::string line;
    for (size_t i = 1; i < by_op.size(); ++i) {
        if (!by_op[i].empty()) continue;
        const std::string_view name = ppu::spec(static_cast<ppu::Op>(i)).name;
        if (line.size() + name.size() > 96) {
            std::println(" {}", line);
            line.clear();
        }
        line += std::format(" {}", name);
    }
    if (!line.empty()) std::println(" {}", line);
    return 0;
}

// The game's own files (USRDIR): each one's size, and what the headers of the
// sound banks and the films say.
int cmd_data(const Args& a) {
    const GameFiles files = files_of(a);
    if (files.usrdir.empty()) throw Failure{std::format("{} is one file: give the disc's folder", a.input)};
    std::vector<fs::path> paths;
    for (const fs::directory_entry& e : fs::directory_iterator(files.usrdir))
        if (e.is_regular_file()) paths.push_back(e.path());
    std::ranges::sort(paths);
    const auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return s;
    };
    for (const fs::path& path : paths) {
        const std::string name = path.filename().string(), kind = lower(path.extension().string());
        std::println("{:20} {:13} bytes", name, fs::file_size(path));
        if (kind == ".nub") {
            const auto bank = NubBank::load(path);
            if (!bank) throw Failure{bank.error()};
            std::println("  sound bank {:#x}: {} sounds, their data {:#x} bytes at {:#x}", bank->id, bank->sounds.size(), bank->data_size,
                         bank->data_at);
            // The sounds by format: how many, and how long they play in all.
            struct Format {
                NubCodec codec;
                uint16_t channels;
                uint32_t rate, bytes_per_second;
                uint16_t block;
                size_t count = 0;
                uint64_t bytes = 0;
            };
            std::vector<Format> formats;
            for (const NubSound& s : bank->sounds) {
                auto it = std::ranges::find_if(formats, [&](const Format& f) {
                    return f.codec == s.codec && f.channels == s.channels && f.rate == s.sample_rate &&
                           f.bytes_per_second == s.bytes_per_second && f.block == s.block_align;
                });
                if (it == formats.end())
                    it = formats.insert(formats.end(), Format{s.codec, s.channels, s.sample_rate, s.bytes_per_second, s.block_align});
                ++it->count;
                it->bytes += s.data_size;
            }
            for (const Format& f : formats)
                std::println("  {:5} x {}, {} channels, {} Hz, {} bytes a second in blocks of {}: {:.0f} seconds in all", f.count,
                             f.codec == NubCodec::atrac3plus ? "ATRAC3plus" : "16-bit big-endian PCM", f.channels, f.rate,
                             f.bytes_per_second, f.block, static_cast<double>(f.bytes) / f.bytes_per_second);
        } else if (kind == ".pam") {
            const auto film = Pamf::load(path);
            if (!film) throw Failure{film.error()};
            std::println("  film (PAMF {}): {:.1f} seconds, at most {} bytes a second, the stream {:#x} bytes at {:#x}", film->version,
                         film->seconds(), uint64_t{film->mux_rate} * 50, film->data_size, film->data_at);
            for (const PamfStream& s : film->streams) {
                if (s.video()) std::println("  stream {:#04x}: {}, {} x {}", s.stream_id, pamf_coding_name(s.coding), s.width, s.height);
                else std::println("  stream {:#04x} {:#04x}: {}", s.stream_id, s.sub_id, pamf_coding_name(s.coding));
            }
        }
    }
    return 0;
}

// The SPU programs the program carries: where each lies, its name, its size in an SPU's memory.
int cmd_spu(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto images = find_spu_images(elf);
    if (!images) throw Failure{images.error()};
    uint64_t code = 0, bytes = 0;
    for (const SpuImage& image : *images) {
        uint32_t low = kSpuMemory, high = 0;
        for (const SpuSegment& s : image.segments) {
            if (s.type != kPtLoad) continue;
            low = std::min(low, s.address);
            high = std::max(high, s.address + s.memory_size);
        }
        std::println("{:#010x} {:#8x} bytes  entry {:#7x}  memory {:#x}-{:#x}  code {:6} words  {}", image.address, image.size, image.entry, low,
                     high, image.code_size() / 4, image.name.empty() ? "(no name)" : image.name);
        code += image.code_size();
        bytes += image.size;
    }
    std::println("{} SPU programs, {} bytes, {} words of code", images->size(), bytes, code / 4);
    return 0;
}

int cmd_cfg(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto cfg = Cfg::build(elf);
    if (!cfg) throw Failure{cfg.error()};
    std::print("{}", cfg->summary());
    return 0;
}

// The archive's directory, which the program holds: the packs, the files in
// them, and whether RR7.DAT is the size the directory says.
int cmd_archive(const Args& a) {
    const GameFiles files = files_of(a);
    const Elf elf = elf_of(files);
    const auto found = find_archive(elf);
    if (!found) throw Failure{found.error()};
    const Archive& archive = *found;
    size_t compressed = 0, loose = 0;
    uint64_t stored = 0, unpacked = 0, plain = 0;
    for (const ArchivePack& p : archive.packs) {
        if (p.compressed) {
            ++compressed;
            stored += p.stored_size;
            unpacked += p.size;
        } else {
            plain += p.size;
        }
    }
    for (const ArchiveFile& f : archive.files) loose += f.pack == ArchiveFile::kNoPack;
    std::println("the directory: {} files at {:#x}, {} packs at {:#x}", archive.files.size(), archive.files_at, archive.packs.size(),
                 archive.packs_at);
    std::println("{} packs are compressed: {} bytes stored, {} unpacked; {} are stored plain: {} bytes", compressed, stored, unpacked,
                 archive.packs.size() - compressed, plain);
    std::println("{} files are in no pack", loose);
    std::println("the archive is {} bytes by its directory", archive.size());
    if (!files.usrdir.empty()) {
        std::error_code ec;
        const fs::path dat = files.usrdir / "RR7.DAT";
        const uint64_t size = fs::file_size(dat, ec);
        if (ec) throw Failure{std::format("{} isn't there", dat.string())};
        if (size != archive.size()) throw Failure{std::format("{} is {} bytes, and the program's directory describes {}", dat.string(), size, archive.size())};
        std::println("{} is that size", dat.string());
    }
    if (a.list_files) {
        for (const ArchivePack& p : archive.packs) {
            std::println("{} (sector {}, {} sectors{})", p.name, p.sector, p.sectors,
                         p.compressed ? std::format(", {} bytes stored, {} unpacked", p.stored_size, p.size) : std::string(", plain"));
            for (uint32_t i = p.first_file; i < p.first_file + p.files; ++i)
                std::println("  {:#10x} {:9}  {}", archive.files[i].offset, archive.files[i].size, archive.files[i].name);
        }
        return 0;
    }
    // The files by name: how many of each, and their bytes.
    struct Kind {
        std::string name;
        size_t count = 0;
        uint64_t bytes = 0;
    };
    std::vector<Kind> kinds;
    for (const ArchiveFile& f : archive.files) {
        auto it = std::ranges::find(kinds, f.name, &Kind::name);
        if (it == kinds.end()) it = kinds.insert(kinds.end(), Kind{f.name});
        ++it->count;
        it->bytes += f.size;
    }
    std::ranges::sort(kinds, [](const Kind& x, const Kind& y) { return x.bytes > y.bytes; });
    std::println("{} different file names; the largest in all:", kinds.size());
    for (size_t i = 0; i < kinds.size() && i < 30; ++i) std::println("  {:6} x {:24} {:12} bytes", kinds[i].count, kinds[i].name, kinds[i].bytes);
    return 0;
}

int cmd_shaders(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto programs = find_shader_programs(elf);
    if (!programs) throw Failure{programs.error()};
    size_t vertex = 0;
    uint64_t bytes = 0, microcode = 0, parameters = 0;
    for (const ShaderProgram& s : *programs) {
        std::println("{:#010x} {:8} {:6} bytes  {:3} parameters  {:5} bytes of microcode", s.address, s.vertex() ? "vertex" : "fragment", s.size,
                     s.parameters, s.microcode_size);
        vertex += s.vertex();
        bytes += s.size;
        microcode += s.microcode_size;
        parameters += s.parameters;
    }
    std::println("{} compiled shader programs: {} vertex, {} fragment; {} bytes, {} of them microcode; {} parameters", programs->size(), vertex,
                 programs->size() - vertex, bytes, microcode, parameters);

    // Each program decoded and checked against its own header and parameters (rsx_program.hpp).
    std::map<std::string, size_t> used;  // operations, by name, in how many programs
    size_t wrong_programs = 0, instructions = 0;
    for (const ShaderProgram& s : *programs) {
        const auto binary = elf.bytes_at(s.address, s.size);
        const auto p = rsx::parse_cg(binary, s.address);
        if (!p) throw Failure{p.error()};
        const std::vector<std::string> wrong = rsx::check_program(*p);
        std::set<std::string> here;
        std::string text;
        if (p->vertex) {
            for (size_t i = 0; i + 16 <= p->microcode.size(); i += 16) {
                uint32_t w[4];
                for (unsigned k = 0; k < 4; ++k) w[k] = read_be32(p->microcode.data() + i + 4 * k);
                const rsx::VpInsn in = rsx::decode_vp(std::span<const uint32_t, 4>(w, 4));
                if (in.vec_op) here.insert(std::format("vp {}", rsx::vp_vector_name(in.vec_op) ? rsx::vp_vector_name(in.vec_op) : "?"));
                if (in.sca_op) here.insert(std::format("vp {}", rsx::vp_scalar_name(in.sca_op) ? rsx::vp_scalar_name(in.sca_op) : "?"));
                if (in.cond_test || in.cond_update) here.insert("vp (condition codes)");
                if (in.index_const || in.index_input) here.insert("vp (indexed by A0)");
                if (in.saturate) here.insert("vp (saturate)");
                if (in.vec_result && in.vec_temp != 0x3F && in.vec_op) here.insert("vp (output and temp at once)");
                ++instructions;
                text += std::format("    {:3}  {:08x} {:08x} {:08x} {:08x}  {}\n", i / 16, w[0], w[1], w[2], w[3], rsx::disasm(in));
            }
        } else {
            size_t slot = 0;
            for (const rsx::FpInsn& in : rsx::decode_fp(p->microcode)) {
                const char* name = in.branch ? rsx::fp_branch_name(in.op) : rsx::fp_name(in.op);
                here.insert(std::format("fp {}", name ? name : "?"));
                if (!in.branch && in.precision) here.insert(std::format("fp (precision {})", in.precision));
                if (!in.branch && in.scale) here.insert("fp (result scale)");
                if (!in.branch && (in.cond_write || in.cond != 7)) here.insert("fp (condition codes)");
                if (!in.branch && in.out_half) here.insert("fp (fp16 destination)");
                ++instructions;
                text += std::format("    {:3}  {:08x} {:08x} {:08x} {:08x}  {}\n", slot, in.w[0], in.w[1], in.w[2], in.w[3], rsx::disasm(in));
                slot += in.has_constant ? 2 : 1;
            }
        }
        for (const std::string& u : here) ++used[u];
        if (!wrong.empty()) ++wrong_programs;
        if (a.disasm || !wrong.empty()) {
            std::println("{:#010x} {} program, header {}", s.address, p->vertex ? "vertex" : "fragment",
                         [&] { std::string h; for (const uint32_t v : p->header) h += std::format(" {:#x}", v); return h; }());
            for (const rsx::CgParameter& c : p->parameters)
                std::println("    {} {}{}: type {:#x} resource {:#x} index {} {}{}", c.direction == 0x1002 ? "out" : c.direction == 0x1001 ? "in " : "   ",
                             c.variability == 0x1006 ? "uniform " : "", c.name, c.type, c.resource, c.resource_index, c.semantic,
                             c.referenced ? "" : " (unused)");
            for (const std::string& w : wrong) std::println("    WRONG: {}", w);
            if (a.disasm) std::print("{}", text);
        }
    }
    std::println("decoded: {} instructions; {} of {} programs disagree with their own tables", instructions, wrong_programs, programs->size());
    std::println("what the programs use, in how many:");
    for (const auto& [what, n] : used) std::println("  {:4}  {}", n, what);
    return wrong_programs ? 1 : 0;
}

// ---- the SPU programs' code ----

void print_spu_insns(uint32_t address, std::span<const uint8_t> bytes, bool ignore_reserved) {
    std::string out;
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const uint32_t pc = address + static_cast<uint32_t>(i), word = read_be32(bytes.data() + i);
        out += std::format("{:08x} {:08x} {}\n", pc, word, spu::disasm(spu::decode(word, ignore_reserved), pc));
        if (out.size() >= 1 << 16) {
            std::fwrite(out.data(), 1, out.size(), stdout);
            out.clear();
        }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
}

int cmd_spu_disasm(const Args& a) {
    const auto file = read_file(a.input);
    if (!file) throw Failure{file.error()};
    if (file->size() % 4) throw Failure{std::format("{} is {} bytes: not a whole number of words", a.input, file->size())};
    print_spu_insns(a.base, *file, a.ignore_reserved);
    return 0;
}

// Each SPU program's executable sections: the image, the section's address in an SPU's memory, its bytes.
struct SpuCode {
    const SpuImage* image;
    uint32_t address;
    std::span<const uint8_t> bytes;
};

std::vector<SpuCode> spu_code_of(const Elf& elf, const std::vector<SpuImage>& images) {
    std::vector<SpuCode> code;
    for (const SpuImage& image : images)
        for (const SpuSection& s : image.sections) {
            if (s.type != kShtProgbits || !(s.flags & kShfExec) || s.size == 0 || s.name == ".SpuGUID") continue;  // (16 bytes that aren't code)
            const auto bytes = elf.bytes_at(uint64_t{image.address} + s.offset, s.size);
            if (bytes.empty()) throw Failure{std::format("SPU program {}: its section {} isn't in the file", image.name, s.name)};
            code.push_back({&image, s.address, bytes});
        }
    return code;
}

int cmd_spu_insns(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto images = find_spu_images(elf);
    if (!images) throw Failure{images.error()};
    for (const SpuCode& c : spu_code_of(elf, *images)) {
        std::println("# {} {}", c.image->name, c.image->section(".text") && c.image->section(".text")->address == c.address ? ".text" : "(code)");
        std::fflush(stdout);
        print_spu_insns(c.address, c.bytes, false);
    }
    return 0;
}

int cmd_spu_profile(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto images = find_spu_images(elf);
    if (!images) throw Failure{images.error()};
    std::vector<size_t> count(static_cast<size_t>(spu::Op::count_));
    std::vector<size_t> programs_using(count.size());
    size_t words = 0, invalid = 0, zero = 0;
    std::vector<std::string> first_invalid;
    const SpuImage* current = nullptr;
    std::vector<bool> seen(count.size());
    for (const SpuCode& c : spu_code_of(elf, *images)) {
        if (c.image != current) {
            current = c.image;
            seen.assign(count.size(), false);
        }
        for (size_t i = 0; i + 4 <= c.bytes.size(); i += 4) {
            const uint32_t word = read_be32(c.bytes.data() + i);
            const spu::Insn in = spu::decode(word);
            ++words;
            if (!in.valid()) {
                ++invalid;
                if (word == 0) ++zero;
                else if (first_invalid.size() < 16)
                    first_invalid.push_back(std::format("{} {:#x} {:08x}", c.image->name, c.address + i, word));
                continue;
            }
            const auto op = static_cast<size_t>(in.op);
            ++count[op];
            if (!seen[op]) {
                seen[op] = true;
                ++programs_using[op];
            }
        }
    }
    std::vector<size_t> order;
    size_t unused = 0;
    for (size_t i = 1; i < count.size(); ++i) {
        if (count[i]) order.push_back(i);
        else ++unused;
    }
    std::ranges::sort(order, [&](size_t x, size_t y) { return count[x] != count[y] ? count[x] > count[y] : x < y; });
    std::println("{} SPU programs, {} words of code: {} decode, {} don't ({} of those are zero)", images->size(), words, words - invalid, invalid,
                 zero);
    std::println("{} of the decoder's {} instructions are used; {} are not", order.size(), count.size() - 1, unused);
    for (const size_t i : order)
        std::println("{:9} {:10} in {:2} programs", count[i], spu::spec(static_cast<spu::Op>(i)).name, programs_using[i]);
    if (!first_invalid.empty()) {
        std::println("the first non-zero words that don't decode:");
        for (const std::string& line : first_invalid) std::println("  {}", line);
    }
    std::println("not used:");
    std::string line;
    for (size_t i = 1; i < count.size(); ++i) {
        if (count[i]) continue;
        const std::string_view name = spu::spec(static_cast<spu::Op>(i)).name;
        if (line.size() + name.size() > 96) {
            std::println(" {}", line);
            line.clear();
        }
        line += std::format(" {}", name);
    }
    if (!line.empty()) std::println(" {}", line);
    return 0;
}

int cmd_spu_cfg(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto images = find_spu_images(elf);
    if (!images) throw Failure{images.error()};
    std::vector<SpuCfg> programs;
    for (const SpuImage& image : *images) {
        auto cfg = SpuCfg::build(image, elf.bytes_at(image.address, image.size));
        if (!cfg) throw Failure{cfg.error()};
        programs.push_back(std::move(*cfg));
    }
    std::print("{}", spu_cfg_summary(programs));
    return 0;
}

// Lifts every function the control-flow pass found. Says how many the
// lifter's slice covers, and for the rest the reason, most frequent first.
// With --out, writes the lifted code there (derived from the game: keep it
// out of the repository), 256 functions to a file.
int cmd_lift(const Args& a) {
    const Elf elf = elf_of(files_of(a));
    const auto cfg = Cfg::build(elf);
    if (!cfg) throw Failure{cfg.error()};
    struct Reason {
        std::string why;
        size_t functions = 0;
        uint32_t first = 0;
    };
    std::vector<Reason> reasons;
    size_t lifted = 0, words = 0, lifted_words = 0;
    std::vector<std::string> code;
    std::string declarations, stubs, table;
    for (const auto& [start, f] : cfg->functions()) {
        if (a.out) table += std::format("    {{0x{:08X}u, &{}}},\n", start, fn_name(start));
        words += f.words;
        LiftInput input = lift_input(*cfg, f);
        input.keep_pc = a.keep_pc;
        input.keep_registers = a.keep_registers;
        const Lifted l = lift(input);
        if (l.ok) {
            ++lifted;
            lifted_words += f.words;
            if (a.out) {
                code.push_back(l.code);
                declarations += std::format("void {}(Cpu& c);\n", fn_name(start));
            }
            continue;
        }
        if (a.out) {
            // Not lifted: a function that stops the game, saying why, if it is ever called.
            declarations += std::format("void {}(Cpu& c);  // not lifted: {} at {:08X}\n", fn_name(start), l.why, l.why_at);
            stubs += std::format("void {}(Cpu& c) {{ c.hooks->trap(c, 0x{:08X}u, \"this function is not lifted yet: {} at {:08X}\"); }}\n", fn_name(start),
                                 start, l.why, l.why_at);
        }
        auto it = std::ranges::find(reasons, l.why, &Reason::why);
        // Reasons that name an address are one reason.
        if (it == reasons.end()) it = reasons.insert(reasons.end(), Reason{l.why, 0, l.why_at});
        ++it->functions;
    }
    std::ranges::sort(reasons, [](const Reason& x, const Reason& y) { return x.functions > y.functions; });
    std::println("{} of {} functions lifted ({} of {} instructions)", lifted, cfg->functions().size(), lifted_words, words);
    std::println("not lifted, by the first instruction outside the slice:");
    for (size_t i = 0; i < reasons.size() && i < 40; ++i)
        std::println("  {:6}  {} (first at {:#x})", reasons[i].functions, reasons[i].why, reasons[i].first);
    // The SPU programs, each lifted into a file of its own.
    const auto images = find_spu_images(elf);
    if (!images) throw Failure{images.error()};
    std::vector<std::pair<std::string, std::string>> spu_files;  // name, text
    std::string spu_table, spu_declarations;
    {
        std::vector<Reason> spu_reasons;
        size_t spu_lifted = 0, spu_functions = 0, index = 0;
        for (const SpuImage& image : *images) {
            const auto program = SpuCfg::build(image, elf.bytes_at(image.address, image.size));
            if (!program) throw Failure{program.error()};
            const std::string space = std::format("spu_p{:02}", index++);
            std::string own, definitions, cases;
            for (const auto& [start, f] : program->functions()) {
                ++spu_functions;
                own += std::format("void {}(Spu& s);\n", spu_fn_name(start));
                cases += std::format("    case 0x{:05X}u: {}(s); return;\n", start, spu_fn_name(start));
                const SpuLifted l = spu_lift(*program, f);
                if (l.ok) {
                    ++spu_lifted;
                    definitions += "\n" + l.code;
                    continue;
                }
                definitions += std::format("\nvoid {}(Spu& s) {{ s.hooks->halt(s, 0x{:05X}u, \"this function is not lifted yet: {} at {:05X}\"); }}\n",
                                           spu_fn_name(start), start, l.why, l.why_at);
                auto it = std::ranges::find(spu_reasons, l.why, &Reason::why);
                if (it == spu_reasons.end()) it = spu_reasons.insert(spu_reasons.end(), Reason{l.why, 0, l.why_at});
                ++it->functions;
            }
            spu_files.emplace_back(space + ".cpp",
                                   std::format("// Lifted from one of the game's SPU programs by qilin: do not share.\n#include \"qilin/rt/spu.hpp\"\n#include \"qilin/rt/spu_fp.hpp\"\nusing namespace qilin::rt;\n\n"
                                               "namespace {} {{\n\nvoid call(Spu& s, uint32_t target, uint32_t pc);\n{}{}\n"
                                               "// The function at an address the program worked out.\nvoid call(Spu& s, uint32_t target, uint32_t pc) {{\n    switch (target) {{\n{}"
                                               "    default: s.hooks->halt(s, pc, \"a branch through a register to an address that is not a function\");\n    }}\n}}\n\n}}  // namespace {}\n",
                                               space, own, definitions, cases, space));
            spu_declarations += std::format("namespace {} {{ void call(Spu& s, uint32_t target, uint32_t pc); }}\n", space);
            spu_table += std::format("    {{0x{:08X}u, 0x{:X}u, 0x{:05X}u, &{}::call}},\n", image.address, image.size, image.entry, space);
        }
        std::ranges::sort(spu_reasons, [](const Reason& x, const Reason& y) { return x.functions > y.functions; });
        std::println("SPU: {} of {} functions lifted, in {} programs", spu_lifted, spu_functions, images->size());
        for (size_t i = 0; i < spu_reasons.size() && i < 40; ++i)
            std::println("  {:6}  {} (first at {:#x})", spu_reasons[i].functions, spu_reasons[i].why, spu_reasons[i].first);
    }
    if (a.out) {
        const fs::path folder = *a.out;
        std::error_code ec;
        fs::create_directories(folder, ec);
        // A file that already says the same is left alone, so that a build compiles only what changed.
        size_t rewritten = 0;
        const auto write = [&](const fs::path& path, const std::string& text) {
            if (const auto old = read_file(path); old && std::string_view(reinterpret_cast<const char*>(old->data()), old->size()) == text) return;
            std::ofstream f(path, std::ios::binary);
            f << text;
            f.close();
            if (!f) throw Failure{std::format("cannot write {}", path.string())};
            ++rewritten;
        };
        write(folder / "functions.hpp", "// Lifted from the game's program by qilin: do not share.\n#pragma once\n#include <bit>\n#include <cstdint>\n"
                                        "#include \"qilin/rt/cpu.hpp\"\nusing namespace qilin::rt;\n" + declarations);
        size_t files = 0;
        for (size_t i = 0; i < code.size(); i += 256) {
            std::string text = "// Lifted from the game's program by qilin: do not share.\n#include \"functions.hpp\"\n";
            for (size_t k = i; k < code.size() && k < i + 256; ++k) text += "\n" + code[k];
            write(folder / std::format("lifted_{:03}.cpp", files++), text);
        }
        write(folder / "stubs.cpp", "// The functions qilin doesn't lift yet: each stops the game if called.\n#include \"functions.hpp\"\n\n" + stubs);
        // Every function by address, in order, and which program they were lifted from.
        write(folder / "table.cpp", std::format("// Lifted from the game's program by qilin: do not share.\n#include \"functions.hpp\"\n#include \"qilin/rt/lifted.hpp\"\n\n"
                                                "namespace qilin::rt {{\nconst LiftedFunction kLiftedFunctions[] = {{\n{}}};\n"
                                                "const size_t kLiftedFunctionCount = {};\nconst char kLiftedFrom[] = \"{}\";\n}}  // namespace qilin::rt\n",
                                                table, cfg->functions().size(), elf.sha256()));
        for (const auto& [name, text] : spu_files) write(folder / name, text);
        write(folder / "spu_table.cpp", std::format("// Lifted from the game's program by qilin: do not share.\n#include \"qilin/rt/lifted.hpp\"\nusing namespace qilin::rt;\n\n{}\n"
                                                    "namespace qilin::rt {{\nconst LiftedSpuProgram kLiftedSpuPrograms[] = {{\n{}}};\n"
                                                    "const size_t kLiftedSpuProgramCount = {};\n}}  // namespace qilin::rt\n",
                                                    spu_declarations, spu_table, images->size()));
        std::println("{} functions written to {} in {} files, with {} that stop if called, and the table of all {}", code.size(), folder.string(), files,
                     cfg->functions().size() - code.size(), cfg->functions().size());
        std::println("{} files were new or changed", rewritten);
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
        if (a.command == "id") {
            std::println("{:08x} {}", import_id(a.input), a.input);
            return 0;
        }
        if (a.command == "archive") return cmd_archive(a);
        if (a.command == "data") return cmd_data(a);
        if (a.command == "shaders") return cmd_shaders(a);
        if (a.command == "spu") return cmd_spu(a);
        if (a.command == "cfg") return cmd_cfg(a);
        if (a.command == "spu-cfg") return cmd_spu_cfg(a);
        if (a.command == "spu-insns") return cmd_spu_insns(a);
        if (a.command == "spu-profile") return cmd_spu_profile(a);
        if (a.command == "spu-disasm") return cmd_spu_disasm(a);
        if (a.command == "lift") return cmd_lift(a);
        if (a.command == "insns") return cmd_insns(a);
        if (a.command == "profile") return cmd_profile(a);
        if (a.command == "disasm") return cmd_disasm(a);
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
        return 1;
    }
}
