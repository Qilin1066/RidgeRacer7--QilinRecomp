// Unit tests: no game files needed. Run by ctest.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"
#include "elf.hpp"
#include "elf_builder.hpp"
#include "import_names.hpp"
#include "media.hpp"
#include "ppu.hpp"
#include "self.hpp"
#include "sfo.hpp"
#include "sha256.hpp"
#include "spu_image.hpp"
#include "syscall_names.hpp"
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

// ---------------------------------------------------------------- the control-flow pass

void test_cfg() {
    // Unwinding tables: a CIE ("zR", addresses relative to themselves, 8 bytes) and two FDEs.
    Bytes eh(0x60);
    put_be32(eh, 0, 0x14);          // CIE: length
    eh[8] = 1;                      // version
    put_text(eh, 9, "zR");
    eh[12] = 4;                     // code alignment
    eh[13] = 0x78;                  // data alignment (-8)
    eh[14] = 65;                    // the return address's register
    eh[15] = 1;                     // augmentation length
    eh[16] = 0x14;                  // pcrel | udata8
    const auto fde = [&](size_t at, uint32_t begin, uint32_t size) {
        put_be32(eh, at, 0x1c);
        put_be32(eh, at + 4, static_cast<uint32_t>(at + 4));  // back to the CIE at 0
        put_be64(eh, at + 8, uint64_t{begin} - (0x20000 + at + 8));
        put_be64(eh, at + 16, size);
    };
    fde(0x18, 0x10400, 0x44);
    fde(0x38, 0x10000, 0x20);
    const auto ranges = parse_eh_frame(eh, 0x20000);
    CHECK(ranges.has_value());
    if (ranges) {
        CHECK(ranges->size() == 2);
        CHECK((*ranges)[0].begin == 0x10400 && (*ranges)[0].end == 0x10444);
        CHECK((*ranges)[1].begin == 0x10000 && (*ranges)[1].end == 0x10020);
    }
    Bytes bad = eh;
    bad[16] = 0x21;  // an encoding that isn't one
    CHECK(!parse_eh_frame(bad, 0x20000).has_value());
    bad = eh;
    put_be32(bad, 0x18 + 4, 0x10);  // an FDE pointing at no CIE
    CHECK(!parse_eh_frame(bad, 0x20000).has_value());
    bad = eh;
    put_be32(bad, 0x38, 0x100);  // a record longer than the section
    CHECK(!parse_eh_frame(bad, 0x20000).has_value());

    // The test program: its one function and its two import stubs.
    const auto elf = Elf::parse(make_elf());
    CHECK(elf.has_value());
    if (!elf) return;
    const auto cfg = Cfg::build(*elf);
    CHECK(cfg.has_value());
    if (!cfg) {
        std::println(stderr, "{}", cfg.error());
        return;
    }
    const auto& functions = cfg->functions();
    CHECK(functions.contains(elf->entry_code()) && functions.at(elf->entry_code()).origin == Origin::Entry);
    CHECK(functions.contains(kStub0) && functions.at(kStub0).stub && functions.at(kStub0).stub->id == kId0);
    CHECK(functions.contains(kStub1) && functions.at(kStub1).indirect_tails.size() == 1);
    for (const auto& [start, f] : functions) CHECK(f.bad.empty());

    // A function with a switch, reached by a tail call from the entry point:
    // the table's address comes from the TOC, its size from the bound check,
    // and the system calls' numbers from r11 along every path.
    Bytes program = make_elf();
    const uint32_t f = kText + 0x120;
    const size_t at = kTextAt + 0x120;
    put_segment(program, 0, 1, 5, kTextAt, kText, 0x170, 0x170, 0x10000);
    put_section(program, 1, 1, 1, 6, kText, kTextAt, 0x170);
    put_be32(program, kTextAt, 0x48000120);    // b f
    const uint32_t code[] = {
        0x2b830002,  // cmpli cr7,0,r3,2
        0x419d0030,  // bc 12,4*cr7+gt,f+0x34
        0x81628010,  // lwz r11,-32752(r2): the table's address
        0x78691788,  // rldic r9,r3,2,30
        0x7c09582e,  // lwzx r0,r9,r11
        0x7c0007b4,  // extsw r0,r0
        0x7c005a14,  // add r0,r0,r11
        0x7c0903a6,  // mtspr 9,r0
        0x4e800420,  // bcctr 20,lt,0
        0x0000000c,  // the table, at f+0x24: to f+0x30,
        0x00000010,  // f+0x34
        0x00000014,  // and f+0x38
        0x39600321,  // f+0x30: addi r11,0,801
        0x44000002,  // f+0x34: sc (r11 is 801 from above, anything from the branches)
        0x39600193,  // f+0x38: addi r11,0,403
        0x44000002,  // sc
        0x4e800020,  // bclr 20,lt,0
    };
    for (size_t i = 0; i < std::size(code); ++i) put_be32(program, at + 4 * i, code[i]);
    put_be32(program, kDataAt + 16, f + 0x24);  // the TOC's slot for the table
    const auto elf2 = Elf::parse(std::move(program));
    CHECK(elf2.has_value());
    if (!elf2) return;
    const auto cfg2 = Cfg::build(*elf2);
    CHECK(cfg2.has_value());
    if (!cfg2) return;
    CHECK(cfg2->rounds() == 2 && cfg2->functions().contains(f));
    if (!cfg2->functions().contains(f)) return;
    const Function& sw = cfg2->functions().at(f);
    CHECK(sw.origin == Origin::TailCall && sw.bad.empty() && sw.words == 14 && sw.end == f + 0x44 && !sw.runs_off);
    CHECK(sw.tables.size() == 1);
    if (sw.tables.size() == 1) {
        const JumpTable& table = sw.tables[0];
        CHECK(table.at == f + 0x20 && table.table == f + 0x24 && table.proven);
        CHECK(table.targets == std::vector<uint32_t>({f + 0x30, f + 0x34, f + 0x38}));
    }
    CHECK(sw.system_calls.size() == 2);
    if (sw.system_calls.size() == 2) {
        CHECK(sw.system_calls[0].at == f + 0x34 && !sw.system_calls[0].number);
        CHECK(sw.system_calls[1].at == f + 0x3c && sw.system_calls[1].number == 403u);
    }
    CHECK(cfg2->functions().at(kText).tails.contains(f));
    CHECK(cfg2->coverage().tables == 3);
}

// ---------------------------------------------------------------- import names

void test_import_names() {
    CHECK(import_name("sys_fs", 0x718bf5f8) == "cellFsOpen");
    CHECK(import_name("cellGcmSys", 0x15bae46b) == "_cellGcmInitBody");
    CHECK(import_name("sysPrxForUser", 0x1573dc3f) == "sys_lwmutex_lock");
    CHECK(import_name("sys_fs", 0x15bae46b).empty());  // an id belongs to its library
    CHECK(import_name("cellNoSuch", 0x718bf5f8).empty());
    // The method, on ids known from elsewhere (PSL1GHT lists both).
    CHECK(import_id("cellFsOpen") == 0x718bf5f8);
    CHECK(import_id("_cellGcmInitBody") == 0x15bae46b);
    CHECK(import_id("cellFsopen") != 0x718bf5f8);
    // Every name in the table gives its id.
    for (const ImportName& n : import_names()) {
        if (import_id(n.name) != n.id) std::println(stderr, "{} is not {:08x}", n.name, n.id);
        CHECK(import_id(n.name) == n.id);
    }
    // No pair twice, no name twice in a library, and nothing empty.
    const auto names = import_names();
    CHECK(names.size() == 248);
    for (size_t i = 0; i < names.size(); ++i) {
        CHECK(!names[i].library.empty() && !names[i].name.empty());
        for (size_t k = i + 1; k < names.size(); ++k)
            CHECK(names[i].library != names[k].library || (names[i].id != names[k].id && names[i].name != names[k].name));
    }
}

// ---------------------------------------------------------------- system-call names

void test_syscall_names() {
    const auto names = system_call_names();
    CHECK(names.size() == 78);
    for (size_t i = 0; i < names.size(); ++i) {
        CHECK(!names[i].name.empty());
        CHECK(i == 0 || names[i - 1].number < names[i].number);  // in order, none twice
        CHECK(system_call_name(names[i].number) == &names[i]);
    }
    CHECK(system_call_name(801) && system_call_name(801)->name == "sys_fs_open" && system_call_name(801)->confirmed);
    CHECK(system_call_name(341) && !system_call_name(341)->confirmed);
    CHECK(!system_call_name(0) && !system_call_name(46) && !system_call_name(9999));
}

// ---------------------------------------------------------------- SPU programs

// An SPU program: a code segment of four words at 0x3000, a data segment, a
// name note, and the sections naming them. Laid out as the SPU's linker does:
// the section headers last.
Bytes make_spu_elf(std::string_view name = "Test_r.elf") {
    const uint32_t code_at = 0x100, note_at = 0x120, names_at = 0x160, sections_at = 0x1a0;
    const std::string_view names("\0.text\0.note.spu_name\0.shstrtab\0", 31);
    Bytes b(sections_at + 4 * 40);
    const uint8_t ident[7] = {0x7f, 'E', 'L', 'F', 1, 2, 1};
    std::memcpy(b.data(), ident, 7);
    put_be16(b, 16, 2);
    put_be16(b, 18, 23);
    put_be32(b, 20, 1);
    put_be32(b, 24, 0x3004);  // entry
    put_be32(b, 28, 52);      // program headers
    put_be32(b, 32, sections_at);
    put_be16(b, 40, 52);
    put_be16(b, 42, 32);
    put_be16(b, 44, 2);
    put_be16(b, 46, 40);
    put_be16(b, 48, 4);
    put_be16(b, 50, 3);
    const auto segment = [&](size_t i, uint32_t type, uint32_t offset, uint32_t address, uint32_t file, uint32_t memory, uint32_t flags) {
        const size_t at = 52 + i * 32;
        put_be32(b, at, type);
        put_be32(b, at + 4, offset);
        put_be32(b, at + 8, address);
        put_be32(b, at + 16, file);
        put_be32(b, at + 20, memory);
        put_be32(b, at + 24, flags);
    };
    segment(0, 1, code_at, 0x3000, 16, 16, 5);
    segment(1, 4, note_at, 0, 0x20, 0, 4);
    put_be32(b, note_at, 8);
    put_be32(b, note_at + 4, 0x20);
    put_be32(b, note_at + 8, 1);
    put_text(b, note_at + 12, "SPUNAME");
    put_text(b, note_at + 20, name);
    put_text(b, names_at, names);
    const auto section = [&](size_t i, uint32_t name_at, uint32_t type, uint32_t flags, uint32_t address, uint32_t offset, uint32_t size) {
        const size_t at = sections_at + i * 40;
        put_be32(b, at, name_at);
        put_be32(b, at + 4, type);
        put_be32(b, at + 8, flags);
        put_be32(b, at + 12, address);
        put_be32(b, at + 16, offset);
        put_be32(b, at + 20, size);
    };
    section(1, 1, 1, 6, 0x3000, code_at, 16);
    section(2, 7, 7, 0, 0, note_at, 0x34);
    section(3, 22, 3, 0, 0, names_at, 31);
    return b;
}

std::string spu_error(const Bytes& b) {
    auto image = parse_spu_image(b, 0x10052b00);
    return image ? std::string() : image.error();
}

void test_spu_image() {
    Bytes spu = make_spu_elf();
    const size_t size = spu.size();
    spu.resize(size + 0x40);  // what follows it in the program's data
    const auto image = parse_spu_image(spu, 0x10052b00);
    CHECK(image.has_value());
    if (image) {
        CHECK(image->address == 0x10052b00 && image->size == size && image->entry == 0x3004);
        CHECK(image->name == "Test_r.elf" && image->code_size() == 16);
        CHECK(image->segments.size() == 2 && image->segments[0].address == 0x3000);
        CHECK(image->section(".text") && image->section(".text")->address == 0x3000 && !image->section(".data"));
    }
    Bytes bad = spu;
    put_be16(bad, 18, 21);
    CHECK(has(spu_error(bad), "not for the SPU"));
    bad = spu;
    put_be32(bad, 24, 0x4000);
    CHECK(has(spu_error(bad), "entry point"));
    bad = spu;
    put_be32(bad, 52 + 8, 0x3fff8);  // the code would run off the end of an SPU's memory
    CHECK(has(spu_error(bad), "doesn't fit an SPU's memory"));
    bad = spu;
    put_be32(bad, 32, 0x10000);
    CHECK(has(spu_error(bad), "tables run past"));
    bad = spu;
    put_be32(bad, 0x1a0 + 40 + 20, 14);  // code that isn't whole words
    CHECK(has(spu_error(bad), "whole number of words"));
    bad = spu;
    bad[4] = 2;
    CHECK(has(spu_error(bad), "not a 32-bit big-endian ELF"));
    // The test program carries none.
    const auto elf = Elf::parse(make_elf());
    CHECK(elf.has_value());
    if (elf) {
        const auto none = find_spu_images(*elf);
        CHECK(none.has_value() && none->empty());
    }
}

// ---------------------------------------------------------------- sound banks and films

// A bank of two sounds: one of PCM, one of ATRAC3plus.
Bytes make_nub() {
    const uint32_t offsets_at = 0x20, first = 0x30, second = first + 0xd0, data_at = 0x300;
    Bytes b(data_at + 0x40);
    put_be32(b, 0, 0x00020100);
    put_be32(b, 8, 0x2bc);
    put_be32(b, 0xc, 2);
    put_be32(b, 0x10, data_at);
    put_be32(b, 0x14, 0x40);
    put_be32(b, 0x18, offsets_at);
    put_be32(b, 0x1c, first);
    put_be32(b, offsets_at, first);
    put_be32(b, offsets_at + 4, second);
    put_text(b, first, "wav");
    put_be32(b, first + 4, 0x2bc);
    put_be32(b, first + 8, 0);
    put_be32(b, first + 0x14, 0x10);  // size
    put_be32(b, first + 0x18, 0);     // offset
    put_be32(b, first + 0x1c, 0x10);
    put_be16(b, first + 0xbc, 1);
    put_be16(b, first + 0xbe, 1);
    put_be32(b, first + 0xc0, 44100);
    put_be32(b, first + 0xc4, 88200);
    put_be16(b, first + 0xc8, 2);
    put_be16(b, first + 0xca, 16);
    put_text(b, second, "at3");
    put_be32(b, second + 4, 0x2bc);
    put_be32(b, second + 8, 1);
    put_be32(b, second + 0x14, 0x30);
    put_be32(b, second + 0x18, 0x10);
    put_be32(b, second + 0x1c, 0x40);
    const size_t f = second + 0xbc;
    put_le16(b, f, 0xfffe);
    put_le16(b, f + 2, 6);
    put_le32(b, f + 4, 48000);
    put_le32(b, f + 8, 48000);
    put_le16(b, f + 12, 0x800);
    const uint8_t guid[16] = {0xbf, 0xaa, 0x23, 0xe9, 0x58, 0xcb, 0x71, 0x44, 0xa1, 0x19, 0xff, 0xfa, 0x01, 0xe4, 0xce, 0x62};
    std::memcpy(b.data() + f + 24, guid, 16);
    return b;
}

// A film of one MPEG-2 picture stream and one ATRAC3plus sound stream.
Bytes make_pamf() {
    Bytes b(0x800 * 3);
    put_text(b, 0, "PAMF0041");
    put_be32(b, 8, 1);
    put_be32(b, 12, 2);
    put_be32(b, 0x50, 0x34 + 2 * 0x30);
    put_be32(b, 0x58, 90000);          // starts at one second
    put_be32(b, 0x5e, 90000 * 11);     // ends at eleven
    put_be32(b, 0x62, 120000);
    put_be16(b, 0x86, 2);
    b[0x88] = 0x02;
    b[0x8c] = 0xe0;
    put_be16(b, 0xa0, 80);
    put_be16(b, 0xa2, 45);
    put_be16(b, 0xa4, 1280);
    put_be16(b, 0xa6, 720);
    b[0xb8] = 0xdc;
    b[0xbc] = 0xbd;
    return b;
}

std::string nub_error(const Bytes& b) {
    auto bank = NubBank::parse(b, b.size());
    return bank ? std::string() : bank.error();
}

std::string pamf_error(const Bytes& b) {
    auto film = Pamf::parse(b, b.size());
    return film ? std::string() : film.error();
}

void test_media() {
    const Bytes nub = make_nub();
    const auto bank = NubBank::parse(nub, nub.size());
    CHECK(bank.has_value());
    if (bank) {
        CHECK(bank->id == 0x2bc && bank->data_at == 0x300 && bank->data_size == 0x40 && bank->sounds.size() == 2);
        const NubSound& pcm = bank->sounds[0];
        CHECK(pcm.codec == NubCodec::pcm16_be && pcm.channels == 1 && pcm.sample_rate == 44100 && pcm.data_size == 0x10);
        const NubSound& at3 = bank->sounds[1];
        CHECK(at3.codec == NubCodec::atrac3plus && at3.channels == 6 && at3.sample_rate == 48000 && at3.block_align == 0x800);
        CHECK(at3.data_offset == 0x10 && at3.bytes_per_second == 48000);
    }
    Bytes bad = nub;
    put_text(bad, 0x30, "ogg");
    CHECK(has(nub_error(bad), "unknown kind"));
    bad = nub;
    put_be32(bad, 0x30 + 0x14, 0x41);  // the first sound's data runs past the bank's
    CHECK(has(nub_error(bad), "runs past"));
    bad = nub;
    bad[0x30 + 0xd0 + 0xbc + 24] ^= 1;  // not ATRAC3plus's sub-format
    CHECK(has(nub_error(bad), "isn't ATRAC3plus"));
    bad = nub;
    put_be32(bad, 0x14, 0x41);  // more data than the file holds
    CHECK(has(nub_error(bad), "past the file's end"));
    bad = nub;
    put_be32(bad, 0x30 + 8, 5);
    CHECK(has(nub_error(bad), "says it is sound 5"));
    bad = nub;
    put_be16(bad, 0x30 + 0xca, 8);
    CHECK(has(nub_error(bad), "8 bits"));
    CHECK(has(nub_error(Bytes(nub.begin(), nub.begin() + 0x100)), "only"));

    const Bytes pam = make_pamf();
    const auto film = Pamf::parse(pam, pam.size());
    CHECK(film.has_value());
    if (film) {
        CHECK(film->version == "0041" && film->data_at == 0x800 && film->data_size == 0x1000);
        CHECK(film->seconds() == 10.0 && film->mux_rate == 120000 && film->streams.size() == 2);
        CHECK(film->streams[0].coding == kPamfMpeg2Video && film->streams[0].width == 1280 && film->streams[0].height == 720);
        CHECK(film->streams[0].stream_id == 0xe0 && film->streams[0].video());
        CHECK(film->streams[1].coding == kPamfAtrac3plus && film->streams[1].stream_id == 0xbd && !film->streams[1].video());
    }
    bad = pam;
    bad[0xb8] = 0x55;
    CHECK(has(pamf_error(bad), "unknown coding"));
    bad = pam;
    put_be32(bad, 12, 3);
    CHECK(has(pamf_error(bad), "past the file's end"));
    bad = pam;
    put_be16(bad, 0xa4, 1920);
    CHECK(has(pamf_error(bad), "two sizes disagree"));
    bad = pam;
    put_be16(bad, 0x86, 3);
    CHECK(has(pamf_error(bad), "not the layout"));
    bad = pam;
    bad[7] = '2';
    CHECK(has(pamf_error(bad), "version 0042"));
    bad = pam;
    bad[0] = 'X';
    CHECK(has(pamf_error(bad), "not a film"));
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
    test_media();
    test_syscall_names();
    test_cfg();
    test_import_names();
    test_spu_image();
    test_title();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
