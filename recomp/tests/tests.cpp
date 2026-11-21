// Unit tests: no game files needed. Run by ctest.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "elf.hpp"
#include "elf_builder.hpp"
#include "ppu.hpp"
#include "self.hpp"
#include "sfo.hpp"
#include "sha256.hpp"
#include "title.hpp"

using namespace qilin;
using namespace elfb;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

// ---------------------------------------------------------------- SHA-256

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string_view abc = "abc";
    CHECK(sha256_hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Padding edges: the length fits in the last block, just doesn't, and exact blocks.
    CHECK(sha256_hex(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(sha256_hex(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
}

// ---------------------------------------------------------------- the PPU decoder

std::string text_of(uint32_t word, uint32_t pc = 0x10000) { return ppu::disasm(ppu::decode(word), pc); }

void test_ppu() {
    using ppu::Op;
    // What every function of a PS3 program begins and ends with.
    CHECK(text_of(0x7c0802a6) == "mfspr r0,8");            // mflr r0
    CHECK(text_of(0xf821ff91) == "stdu r1,-112(r1)");
    CHECK(text_of(0xf8010080) == "std r0,128(r1)");
    CHECK(text_of(0xe8410028) == "ld r2,40(r1)");
    CHECK(text_of(0xe8630002) == "lwa r3,0(r3)");
    CHECK(text_of(0x4e800020) == "bclr 20,lt,0");          // blr
    CHECK(text_of(0x4e800421) == "bcctrl 20,lt,0");        // bctrl
    CHECK(text_of(0x48000011) == "bl 0x10010");
    CHECK(text_of(0x4bfffff0) == "b 0xfff0");
    CHECK(text_of(0x419e0008) == "bc 12,4*cr7+eq,0x10008");
    CHECK(text_of(0x44000002) == "sc 0");
    CHECK(text_of(0x60000000) == "ori r0,r0,0");           // nop
    CHECK(text_of(0x38600001) == "addi r3,0,1");           // li r3,1
    CHECK(text_of(0x7c7f1b78) == "or r31,r3,r3");          // mr r31,r3
    CHECK(text_of(0x7c632214) == "add r3,r3,r4");
    CHECK(text_of(0x7c632615) == "addo. r3,r3,r4");
    // 64 bits.
    CHECK(text_of(0x786b0020) == "rldicl r11,r3,0,32");    // clrldi r11,r3,32
    CHECK(text_of(0x786307c6) == "rldicr r3,r3,32,31");    // sldi r3,r3,32
    CHECK(text_of(0x7c6307b4) == "extsw r3,r3");
    CHECK(text_of(0x7c63fe76) == "sradi r3,r3,63");
    CHECK(text_of(0x2c230000) == "cmpi cr0,1,r3,0");       // cmpdi r3,0
    CHECK(text_of(0x7f832040) == "cmpl cr7,0,r3,r4");      // cmplw cr7,r3,r4
    CHECK(text_of(0x7c0004ac) == "sync 0");
    CHECK(text_of(0x7c2004ac) == "sync 1");                // lwsync
    CHECK(!ppu::decode(0x7c6004ac).valid());               // no fourth kind of sync
    // The condition register, whole and by one field.
    CHECK(text_of(0x7d800026) == "mfcr r12");
    CHECK(text_of(0x7d908120) == "mtocrf 8,r12");
    CHECK(text_of(0x7d808120) == "mtcrf 8,r12");
    CHECK(!ppu::decode(0x7d918120).valid());               // mtocrf names one field, not two
    CHECK(text_of(0x7c6c42e6) == "mftb r3,268");
    CHECK(!ppu::decode(0x7c6102e6).valid());               // mftb reads the time base and nothing else
    // VMX.
    CHECK(text_of(0x10000484) == "vor v0,v0,v0");
    CHECK(text_of(0x7c0000ce) == "lvx v0,0,r0");
    CHECK(text_of(0x7c2329ce) == "stvx v1,r3,r5");
    CHECK(text_of(0x7c03240e) == "lvlx v0,r3,r4");
    CHECK(text_of(0x1023038c) == "vspltisw v1,3");
    CHECK(text_of(0x103f038c) == "vspltisw v1,-1");
    CHECK(text_of(0x1022186e) == "vmaddfp v1,v2,v1,v3");
    CHECK(text_of(0x102218ac) == "vsldoi v1,v2,v3,2");
    CHECK(text_of(0x10221c06) == "vcmpequb. v1,v2,v3");
    CHECK(text_of(0x1020128c) == "vspltw v1,v2,0");
    CHECK(!ppu::decode(0x1024128c).valid());               // a word has four elements, not five
    // Reserved bits and invalid forms don't decode.
    CHECK(text_of(0x00000000) == ".long 0x0");
    CHECK(ppu::decode(0x7c632215).valid());                // add. is fine
    CHECK(!ppu::decode(0x7c0802a7).valid());               // mfspr with bit 31 set
    CHECK(!ppu::decode(0xe8630003).valid());               // opcode 58 has three forms
    CHECK(!ppu::decode(0xe8630001).valid());               // ldu r3,0(r3): the base is the target
    CHECK(!ppu::decode(0xf8600001).valid());               // stdu with no base
    CHECK(!ppu::decode(0x4c000064).valid());               // rfi: not on a 64-bit processor
    CHECK(!ppu::decode(0x7c0000a6).valid());               // mfmsr: the supervisor's
    CHECK(!ppu::decode(0x4e200020).valid());               // bclr whose BO has a reserved bit set
    // Properties the passes go by.
    CHECK(ppu::decode(0x4e800020).op == Op::bclr);
    CHECK(ppu::spec(Op::bclr).flags & ppu::kBranch);
    CHECK(ppu::spec(Op::lvx).flags & ppu::kVector);
    CHECK(ppu::spec(Op::ld).flags & ppu::k64);
    CHECK(ppu::decode(0x48000011).branch_target(0x10000) == 0x10010);
    CHECK(ppu::decode(0x48000013).branch_target(0x10000) == 0x10);
    // Every spec decodes from its own match bits, under its own name (some need an operand set to be a valid form).
    for (const ppu::Spec& s : ppu::specs()) {
        uint32_t word = s.match;
        if (s.flags & (ppu::kRaNonZero | ppu::kRaNotZeroOrRd | ppu::kLmw | ppu::kLswi)) word |= 1u << 16;
        if (s.flags & ppu::kLmw) word |= 31u << 21;
        if (s.flags & ppu::kLswi) word |= 4u << 11;
        if (s.flags & ppu::kLswx) word |= 1u << 21;
        if (s.flags & (ppu::kValidBo | ppu::kBoNoDecrement)) word |= 20u << 21;
        if (s.flags & ppu::kOneCrField) word |= 1u << 12;
        if (s.flags & ppu::kTimeBase) word |= 12u << 16 | 8u << 11;
        const ppu::Insn in = ppu::decode(word);
        if (in.op != s.op) std::println(stderr, "{} doesn't decode from its own bits ({:08x})", s.name, word);
        CHECK(in.op == s.op);
        CHECK(ppu::mnemonic(in) == s.name);
    }
}

// ---------------------------------------------------------------- the program

// The error parsing `file` gives; empty if it parses.
std::string elf_error(Bytes file) {
    auto elf = Elf::parse(std::move(file));
    return elf ? std::string() : elf.error();
}

bool has(const std::string& text, std::string_view part) { return text.find(part) != text.npos; }

void test_elf() {
    const Bytes file = make_elf();
    auto parsed = Elf::parse(file);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::println(stderr, "  {}", parsed.error());
        return;
    }
    const Elf& elf = *parsed;
    CHECK(elf.sha256() == sha256_hex(file));
    CHECK(elf.entry_descriptor() == kEntry && elf.entry_code() == kText && elf.entry_toc() == kToc);
    CHECK(elf.segments().size() == kSegments);
    CHECK(elf.sections().size() == 4 && elf.sections()[1].name == ".text" && elf.sections()[3].name == ".shstrtab");
    CHECK(elf.section(".data") && elf.section(".data")->address == kData);
    CHECK(!elf.section(".nothing"));
    CHECK(elf.executable(kText) && !elf.executable(kData));

    // Memory: the file's bytes where it has them, nothing in zero-filled memory or outside.
    CHECK(elf.read32(kText) == 0x4e800020u);
    CHECK(elf.read32(kData + kDataSize - 4) == 0xdeadbeefu);
    CHECK(!elf.read32(kData + kDataSize - 2));
    CHECK(!elf.read32(kData + kDataSize));
    CHECK(!elf.read32(0x30000));
    CHECK(elf.bytes_at(kText, kTextSize).size() == kTextSize && elf.bytes_at(kText, kTextSize + 1).empty());

    CHECK(elf.process_param() && elf.process_param()->sdk_version == 0x00102000 && elf.process_param()->primary_priority == 1001 &&
          elf.process_param()->primary_stack_size == 0x10000 && elf.process_param()->malloc_page_size == 0x100000);
    CHECK(elf.tls() && elf.tls()->address == kTlsData && elf.tls()->file_size == 4 && elf.tls()->memory_size == 0x40);

    CHECK(elf.imports().size() == 1);
    if (elf.imports().size() == 1) {
        const ImportLibrary& lib = elf.imports()[0];
        CHECK(lib.name == "cellTest" && lib.version == 1 && lib.attributes == 9 && lib.entry == kImport);
        CHECK(lib.functions.size() == 2 && lib.variables.empty());
        if (lib.functions.size() == 2) {
            CHECK(lib.functions[0].id == kId0 && lib.functions[0].slot == kSlots && lib.functions[0].initial == kStub0);
            CHECK(lib.functions[1].id == kId1 && lib.functions[1].slot == kSlots + 4 && lib.functions[1].initial == kStub1);
            CHECK(lib.functions[0].stub && lib.functions[1].stub);
        }
    }
    CHECK(elf.exports().size() == 1 && elf.exports()[0].name.empty() && elf.exports()[0].functions.size() == 1 &&
          elf.exports()[0].functions[0].id == kExportId && elf.exports()[0].functions[0].address == kEntry);

    // A slot whose stub loads another slot is reported, not trusted.
    {
        Bytes b = file;
        put_be32(b, kDataAt + 8, kStub1);
        auto e = Elf::parse(b);
        CHECK(e && !e->imports()[0].functions[0].stub && e->imports()[0].functions[1].stub);
    }
    // A slot address whose low half is negative as lwz reads it.
    {
        Bytes b(64);
        put_stub(b, 0, 0x1234fff0);
        CHECK(b[6] == 0x12 && b[7] == 0x35 && b[10] == 0xff && b[11] == 0xf0);
    }

    // What isn't a PS3 program says why.
    const auto with = [&](size_t at, uint8_t value) {
        Bytes b = file;
        b[at] = value;
        return elf_error(std::move(b));
    };
    CHECK(has(with(0, 0), "not an ELF"));
    CHECK(has(with(4, 1), "64-bit big-endian"));
    CHECK(has(with(5, 1), "64-bit big-endian"));
    CHECK(has(with(7, 0), "not a PS3 program"));
    CHECK(has(with(19, 20), "not a PowerPC64"));
    CHECK(has(with(17, 3), "not an executable"));
    CHECK(has(with(55, 32), "program header size"));
    CHECK(has(with(59, 40), "section header size"));
    CHECK(has(with(63, 9), "name table"));
    const auto text = [](uint32_t address) { return kTextAt + (address - kText); };
    CHECK(has(with(text(kProcParam) + 4, 0), "process parameters' magic"));
    CHECK(has(with(text(kPrxParam) + 4, 0), "PRX parameters' magic"));
    CHECK(has(with(text(kImport), 0x2d), "expected 44"));
    CHECK(has(with(text(kExport), 0x1d), "expected 28"));
    CHECK(has(with(text(kName) + 8, 1), "library name"));            // a name that isn't text
    CHECK(has(with(text(kImport) + 16, 0x7f), "library name"));      // a name outside the file
    CHECK(has(with(text(kImport) + 20, 0x7f), "function ids"));
    CHECK(has(with(text(kImport) + 24, 0x7f), "function slots"));
    CHECK(has(with(text(kImport) + 7, 9), "function"));              // more functions than the tables hold
    CHECK(has(with(kDataAt, 0x7f), "not code"));                     // the entry descriptor's code address
    CHECK(has(with(24 + 7, 0x80), "entry point"));                   // an entry point outside the file
    // Segments: past the file, overlapping, outside 32 bits.
    {
        Bytes b = file;
        put_be64(b, kSegmentsAt + 32, 0x100000);
        put_be64(b, kSegmentsAt + 40, 0x100000);
        CHECK(has(elf_error(b), "past the end of the file"));
        b = file;
        put_be64(b, kSegmentsAt + 56 + 16, kText + 0x100);
        CHECK(has(elf_error(b), "overlaps"));
        b = file;
        put_be64(b, kSegmentsAt + 56 + 16, 0xfffffff0);
        CHECK(has(elf_error(b), "32-bit address space"));
        b = file;
        put_be64(b, kSegmentsAt + 56 + 40, 4);
        CHECK(has(elf_error(b), "more bytes in the file"));
    }
    // Any truncation is an error or a smaller valid file, never a crash.
    for (size_t n = 0; n < file.size(); ++n) (void)Elf::parse(Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(n)));
    CHECK(has(elf_error(Bytes(file.begin(), file.begin() + 63)), "too short"));
}

// ---------------------------------------------------------------- the SELF

void test_self() {
    const Bytes elf_file = make_elf();
    const Bytes file = make_self(elf_file);
    CHECK(Self::is_self(file) && !Self::is_self(elf_file));
    auto parsed = Self::parse(file);
    CHECK(parsed.has_value());
    if (!parsed) {
        std::println(stderr, "  {}", parsed.error());
        return;
    }
    const Self& self = *parsed;
    CHECK(self.sha256 == sha256_hex(file) && self.file_size == file.size());
    CHECK(self.key_revision == 1 && self.self_type == 4 && self.authentication_id == 0x1010000001000003);
    CHECK(self.elf.entry == kEntry && self.elf.segments.size() == kSegments && self.elf.sections.size() == 4);
    CHECK(self.elf.sections[1].address == kText && self.elf.sections[1].name.empty());
    CHECK(self.segments.size() == kSegments && self.segments[0].offset == kSelfHeader + kTextAt && self.segments[0].encryption == 1 &&
          self.segments[2].encryption == 0);
    CHECK(self.libraries.size() == 3);
    if (self.libraries.size() == 3) {
        CHECK(self.libraries[0].name == "crt0" && self.libraries[0].version == "p102002" && self.libraries[0].objects == 1);
        CHECK(self.libraries[1].name == "libc" && self.libraries[1].objects == 2);
        CHECK(self.libraries[2].name == "libgcm_sys_stub");
    }

    // The decrypted program is recognised as this SELF's, and another isn't.
    auto elf = Elf::parse(elf_file);
    CHECK(elf && differs_from(self, elf->layout()).empty());
    {
        Bytes other = elf_file;
        put_be64(other, kSegmentsAt + 56 + 40, kDataMemory + 0x100);
        auto e = Elf::parse(other);
        CHECK(e && differs_from(self, e->layout()) == "program header 1");
        other = elf_file;
        put_be64(other, 24, kData + 8);  // another descriptor (a slot holding a stub's address)
        e = Elf::parse(other);
        CHECK(e && has(differs_from(self, e->layout()), "entry point"));
    }
    // The ELF loader says what a SELF is instead of calling it damaged.
    auto as_elf = Elf::parse(file);
    CHECK(!as_elf && has(as_elf.error(), "encrypted"));

    const auto error = [](const Bytes& b) {
        auto s = Self::parse(b);
        return s ? std::string() : s.error();
    };
    CHECK(has(error(elf_file), "not a SELF"));
    {
        Bytes b = file;
        b[11] = 3;
        CHECK(has(error(b), "not a SELF"));
        b = file;
        b.push_back(0);
        CHECK(has(error(b), "truncated"));
        b = file;
        b[0x90] = 0;
        CHECK(has(error(b), "copy of the ELF header"));
        b = file;
        put_be32(b, 0xd0 + kSegments * 56 + 16, 7);
        CHECK(has(error(b), "unknown compression"));
    }
    CHECK(has(error(make_self(elf_file, "crt0:p102002libc")), "malformed"));
    CHECK(has(error(make_self(elf_file, "crt0:")), "malformed"));
    CHECK(has(error(make_self(elf_file, ":p1")), "malformed"));
    for (size_t n = 0; n < file.size(); ++n) CHECK(!Self::parse(std::span(file).first(n)));

    auto stamps = parse_sdk_stamps("libm:p102002libstdc++:p102002libm:p102002libsysutil_np_matching_int_stub_03:p102002");
    CHECK(stamps && stamps->size() == 3 && (*stamps)[0].objects == 2 && (*stamps)[1].name == "libstdc++" &&
          (*stamps)[2].name == "libsysutil_np_matching_int_stub_03");
    CHECK(parse_sdk_stamps("") && parse_sdk_stamps("")->empty());
}

// ---------------------------------------------------------------- PARAM.SFO

void test_sfo() {
    const Bytes file = make_sfo({{"BOOTABLE", "#1"}, {"RESOLUTION", "#29"}, {"SOUND_FORMAT", "#263"}, {"TITLE", "A Test Game"},
                                 {"TITLE_ID", "TEST00001"}, {"VERSION", "01.02"}});
    auto sfo = Sfo::parse(file);
    CHECK(sfo.has_value());
    if (!sfo) return;
    CHECK(sfo->entries.size() == 6);
    CHECK(sfo->text("TITLE") == "A Test Game" && sfo->text("TITLE_ID") == "TEST00001" && sfo->text("VERSION") == "01.02");
    CHECK(sfo->number("RESOLUTION") == 29u && sfo->number("BOOTABLE") == 1u);
    CHECK(!sfo->text("RESOLUTION") && !sfo->number("TITLE") && !sfo->text("NOTHING"));
    CHECK(sfo_resolutions(29) == "480, 720, 1080, 480 (16:9)");
    CHECK(sfo_resolutions(0) == "none" && sfo_resolutions(0x40) == "bit 6");
    CHECK(sfo_sound_formats(263) == "LPCM 2.0, LPCM 5.1, Dolby Digital");
    CHECK(sfo_sound_formats(0x211) == "LPCM 2.0, LPCM 7.1, unknown bits 0x200");
    CHECK(sfo_sound_formats(0x202) == "DTS");

    CHECK(!Sfo::parse(make_elf()));
    {
        Bytes b = file;
        put_le16(b, 20 + 2, 0x0999);
        auto bad = Sfo::parse(b);
        CHECK(!bad && has(bad.error(), "unknown format"));
        b = file;
        put_le32(b, 20 + 16 + 12, 0x10000);
        bad = Sfo::parse(b);
        CHECK(!bad && has(bad.error(), "outside the file"));
    }
    for (size_t n = 0; n < file.size(); ++n) (void)Sfo::parse(std::span(file).first(n));
    for (size_t n = 0; n < 20; ++n) CHECK(!Sfo::parse(std::span(file).first(n)));
}

// ---------------------------------------------------------------- finding the game

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void test_title() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "qilin-rr7-title-test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path usrdir = root / "PS3_GAME" / "USRDIR";
    fs::create_directories(usrdir);
    const Bytes elf = make_elf();
    write(usrdir / "EBOOT.BIN", make_self(elf));
    write(root / "PS3_GAME" / "PARAM.SFO", make_sfo({{"TITLE", "A Test Game"}}));

    // Every way of pointing at the copy finds the same files.
    for (const fs::path& given : {root, root / "PS3_GAME", usrdir, usrdir / "EBOOT.BIN"}) {
        auto files = find_game(given);
        CHECK(files.has_value());
        if (!files) continue;
        CHECK(files->self == usrdir / "EBOOT.BIN" && files->sfo == root / "PS3_GAME" / "PARAM.SFO" && files->elf.empty());
        // Only the encrypted program: the error says how to make the other.
        auto program = load_game_elf(*files);
        CHECK(!program && has(program.error(), "RPCS3") && has(program.error(), "EBOOT.elf"));
    }
    CHECK(!find_game(root / "nothing"));
    CHECK(!find_game(root / "PS3_GAME" / "nothing.bin"));
    {
        fs::create_directories(root / "empty");
        auto none = find_game(root / "empty");
        CHECK(!none && has(none.error(), "no EBOOT.BIN"));
    }

    // With the decrypted program beside it (whatever its name's case).
    write(usrdir / "eboot.ELF", elf);
    for (const fs::path& given : {root, usrdir / "EBOOT.BIN", usrdir / "eboot.ELF"}) {
        auto files = find_game(given);
        CHECK(files && files->elf == usrdir / "eboot.ELF" && files->self == usrdir / "EBOOT.BIN");
        if (!files) continue;
        auto program = load_game_elf(*files);
        CHECK(program && program->entry_code() == kText);
    }
    // A decrypted program that isn't this EBOOT.BIN's is refused.
    {
        Bytes other = elf;
        put_be64(other, kSegmentsAt + 56 + 40, kDataMemory + 0x100);
        write(usrdir / "eboot.ELF", other);
        auto files = find_game(root);
        auto program = files ? load_game_elf(*files) : std::unexpected(std::string());
        CHECK(!program && has(program.error(), "is not the program in"));
    }
    // PARAM.SFO alone.
    {
        auto files = find_game(root / "PS3_GAME" / "PARAM.SFO");
        CHECK(files && files->self.empty() && files->elf.empty() && !files->sfo.empty());
    }
    fs::remove_all(root, ec);

    const KnownBuild* disc = known_build("095ed6cb8e90538309b57662fb19da1ba2c4bdbac5fd40dc0be6122e76d4c0db");
    CHECK(disc && disc->self);
    CHECK(!known_build("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

}  // namespace

int main() {
    test_sha256();
    test_ppu();
    test_elf();
    test_self();
    test_sfo();
    test_title();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
