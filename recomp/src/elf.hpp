// PS3 program (ELF) loader.
//
// A PS3 game's program is a big-endian 64-bit PowerPC ELF (machine 21,
// OS/ABI 0x66), though an LV2 process's addresses are 32 bits wide: every
// pointer the program keeps in memory is four bytes. What the PS3 adds:
//
//   - two program headers of its own: the process parameters (0x60000001:
//     the SDK version, the main thread's priority and stack size) and the
//     PRX parameters (0x60000002: where the import and export tables are);
//   - imports: one 44-byte entry per system library, naming it and pointing
//     at a table of function ids (32 bits each: the PS3 names nothing in a
//     program, it identifies) and a table of slots, one per function, that
//     the system's loader fills with the address of the function's
//     descriptor. The program reaches a library through these and through
//     nothing else, apart from system calls (the `sc` instruction);
//   - function descriptors: a function pointer is the address of two words,
//     the code's address and the table-of-contents register's value (r2).
//     The ELF's entry point is the address of one.
//
// The disc holds the program encrypted (EBOOT.BIN, a SELF: self.hpp). This
// loader reads the decrypted program, which the player makes from their own
// disc (docs/elf-analysis.md says how). Every table is validated when the
// file is opened. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

inline constexpr uint32_t kPtLoad = 1, kPtTls = 7, kPtProcParam = 0x60000001, kPtProcPrx = 0x60000002;
inline constexpr uint32_t kPfExec = 1, kPfWrite = 2, kPfRead = 4;
inline constexpr uint32_t kShtNull = 0, kShtProgbits = 1, kShtStrtab = 3, kShtNobits = 8;
inline constexpr uint64_t kShfWrite = 1, kShfAlloc = 2, kShfExec = 4;
inline constexpr uint16_t kEmPpc64 = 21, kEtExec = 2;
inline constexpr uint8_t kOsAbiCellLv2 = 0x66;
inline constexpr uint32_t kProcParamMagic = 0x13bcc5f6, kPrxParamMagic = 0x1b434cec;
inline constexpr size_t kImportEntrySize = 0x2c, kExportEntrySize = 0x1c;

struct ElfSegment {
    uint32_t type = 0, flags = 0;
    uint64_t offset = 0, address = 0, physical = 0, file_size = 0, memory_size = 0, align = 0;

    bool contains(uint64_t a) const { return a >= address && a - address < memory_size; }
};

struct ElfSection {
    unsigned index = 0;
    std::string name;  // empty where the string table can't be read (a SELF's plain headers)
    uint32_t name_offset = 0, type = 0;
    uint64_t flags = 0, address = 0, offset = 0, size = 0;
    uint32_t link = 0, info = 0;
    uint64_t align = 0, entry_size = 0;
};

// The ELF header and its two tables: what an ELF and a SELF's plain copy share.
struct ElfLayout {
    uint8_t os_abi = 0;
    uint16_t type = 0, machine = 0;
    uint32_t flags = 0;
    uint64_t entry = 0, program_headers_at = 0, section_headers_at = 0;
    uint16_t section_names = 0;  // index of the sections' string table
    std::vector<ElfSegment> segments;
    std::vector<ElfSection> sections;  // names not filled in
};

// `header_at`: where the ELF header is in `file`. `segments_at` and
// `sections_at`: where the two tables are (in an ELF, the header's own
// offsets; a SELF keeps them elsewhere).
std::expected<ElfLayout, std::string> parse_elf_layout(std::span<const uint8_t> file, uint64_t header_at,
                                                       std::optional<uint64_t> segments_at = {},
                                                       std::optional<uint64_t> sections_at = {});

struct ProcessParam {
    uint32_t size = 0, version = 0, sdk_version = 0;
    int32_t primary_priority = 0;
    uint32_t primary_stack_size = 0, malloc_page_size = 0, ppc_seg = 0;
};

struct PrxParam {
    uint32_t size = 0, version = 0;
    uint32_t exports_start = 0, exports_end = 0, imports_start = 0, imports_end = 0;
};

struct ImportFunction {
    uint32_t id = 0;       // the function's id in its library
    uint32_t slot = 0;     // the word the loader fills with the function's descriptor
    uint32_t initial = 0;  // what the file holds in the slot
    bool stub = false;     // `initial` is the address of code that calls through this slot
};

struct ImportVariable {
    uint32_t id = 0, slot = 0;
};

struct ImportLibrary {
    std::string name;
    uint32_t entry = 0;  // the address of its entry in the import table
    uint16_t version = 0, attributes = 0;
    std::vector<ImportFunction> functions;
    std::vector<ImportVariable> variables;
    uint16_t tls_variables = 0;
};

struct ExportedSymbol {
    uint32_t id = 0, address = 0;
};

struct ExportLibrary {
    std::string name;  // empty: the program's own entry
    uint16_t version = 0, attributes = 0;
    std::vector<ExportedSymbol> functions, variables;
};

struct TlsImage {
    uint32_t address = 0, file_size = 0, memory_size = 0, align = 0;
};

class Elf {
public:
    static std::expected<Elf, std::string> parse(std::vector<uint8_t> file);
    static std::expected<Elf, std::string> load(const std::filesystem::path& path);

    const ElfLayout& layout() const { return layout_; }
    const std::vector<ElfSegment>& segments() const { return layout_.segments; }
    const std::vector<ElfSection>& sections() const { return layout_.sections; }
    const ElfSection* section(std::string_view name) const;
    const std::string& sha256() const { return sha256_; }
    size_t file_size() const { return file_.size(); }

    // The entry point: its descriptor's address, the code's, and r2's value.
    uint32_t entry_descriptor() const { return static_cast<uint32_t>(layout_.entry); }
    uint32_t entry_code() const { return entry_code_; }
    uint32_t entry_toc() const { return entry_toc_; }

    const std::optional<ProcessParam>& process_param() const { return process_param_; }
    const std::optional<PrxParam>& prx_param() const { return prx_param_; }
    const std::optional<TlsImage>& tls() const { return tls_; }
    const std::vector<ImportLibrary>& imports() const { return imports_; }
    const std::vector<ExportLibrary>& exports() const { return exports_; }

    // The file's bytes for `size` bytes at a program address; empty if any of
    // them isn't in the file (zero-filled memory, or no segment there).
    std::span<const uint8_t> bytes_at(uint64_t address, uint64_t size) const;
    std::optional<uint32_t> read32(uint64_t address) const;
    bool executable(uint64_t address) const;

private:
    std::optional<std::string> read_tables();

    std::vector<uint8_t> file_;
    ElfLayout layout_;
    std::string sha256_;
    uint32_t entry_code_ = 0, entry_toc_ = 0;
    std::optional<ProcessParam> process_param_;
    std::optional<PrxParam> prx_param_;
    std::optional<TlsImage> tls_;
    std::vector<ImportLibrary> imports_;
    std::vector<ExportLibrary> exports_;
};

// The whole of a file; an error names it.
std::expected<std::vector<uint8_t>, std::string> read_file(const std::filesystem::path& path);

// "0x4bb1a0": addresses and sizes in messages (no std::format in runtime code).
std::string hex(uint64_t value);

}  // namespace qilin
