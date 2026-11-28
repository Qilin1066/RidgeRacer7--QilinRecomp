// Unit tests: no game files needed. Run by ctest.
#include <bitset>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "archive.hpp"
#include "rsx_program.hpp"
#include "rsx_spirv.hpp"
#include "cfg.hpp"
#include "elf.hpp"
#include "elf_builder.hpp"
#include "import_names.hpp"
#include "lift.hpp"
#include "media.hpp"
#include "ppu.hpp"
#include "self.hpp"
#include "sfo.hpp"
#include "sha256.hpp"
#include "spu.hpp"
#include "spu_cfg.hpp"
#include "shaders.hpp"
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

// ---------------------------------------------------------------- the lifter's dataflow
// What each call and return writes back, and which compares a branch reads directly (lift.cpp, "the dataflow").
// The differential test's programs check the results; these check the shapes.
void test_lift_dataflow() {
    const auto lifted = [](std::vector<uint32_t> words) {
        LiftInput in;
        in.start = 0x10000;
        for (size_t i = 0; i < words.size(); ++i) in.insns.emplace_back(0x10000 + 4 * static_cast<uint32_t>(i), ppu::decode(words[i]));
        in.is_function = [](uint32_t a) { return a == 0x10100; };
        const Lifted l = lift(in);
        CHECK(l.ok);
        return l.code;
    };
    const auto has = [](const std::string& text, std::string_view what) { return text.find(what) != std::string::npos; };
    const auto count = [](const std::string& text, std::string_view what) {
        size_t n = 0;
        for (size_t p = text.find(what); p != std::string::npos; p = text.find(what, p + 1)) ++n;
        return n;
    };
    const auto bl = [](uint32_t at) { return 0x48000001u | ((0x10100u - at) & 0x03fffffcu); };
    // A branch on a field set two blocks earlier, reached by two paths from the one compare: both read its operands.
    const std::string two = lifted({0x2f830000, 0x419e000c, 0x38800001, 0x60000000, 0x38a00002, 0x409e0008, 0x38c00003, 0x4e800020});
    CHECK(count(two, "(k7a == k7b)") == 2 && !has(two, "(cr7 & 2u)") && has(two, "k7a = static_cast<uint64_t>(static_cast<int32_t>(r3));"));
    // A field read after a call (which may change it) is read from the field, and the compare keeps no operands.
    const std::string after = lifted({0x2f830000, bl(0x10004), 0x419e0008, 0x38800001, 0x4e800020});
    CHECK(has(after, "(cr7 & 2u)") && !has(after, "k7a"));
    // Fields set by different kinds of compare on two paths (signed, unsigned) can't be read either way.
    const std::string mixed = lifted({0x2f830000, 0x419e000c, 0x2b830005, 0x60000000, 0x419d0008, 0x38c00003, 0x4e800020});
    CHECK(has(mixed, "(cr7 & 4u)"));
    // A floating-point compare and a CR logical operation on its result fold too (fcmpu cr7,f1,f2; cror eq,gt,eq; beq).
    const std::string fp = lifted({0xff811000, 0x4fddf382, 0x419e0008, 0x38800001, 0x4e800020});
    CHECK(has(fp, "kb30 = bit != 0u;") && has(fp, "if ((kb30))") && has(fp, "static_cast<uint32_t>(k7x > k7y)"));
    // mfcr reads every field's local.
    const std::string mfcr = lifted({0x2f830000, 0x7c600026, 0x4e800020});
    CHECK(has(mfcr, "(cr0 << 28) | (cr1 << 24)") && has(mfcr, "(cr7 << 0)"));
    // Only what may have changed is written back: r3 (and LR) before the call, r4 (only read) never, and after the call
    // (whose registers are read again) nothing is left to write back at the return.
    const std::string sync = lifted({0x7c642214, bl(0x10004), 0x4e800020});
    CHECK(has(sync, "c.r[3] = r3; c.lr = lr;  fn_00010100(c);") && !has(sync, "c.r[4] = r4") && has(sync, "\n     return;\n"));
    // Changed on one path to the call and not on the other: written back (the paths' union).
    const std::string paths = lifted({0x2f830000, 0x419e0008, 0x38a00001, bl(0x1000c), 0x4e800020});
    CHECK(has(paths, "c.r[5] = r5;") && !has(paths, "c.r[3] = r3"));
    // The field itself is lazy there: every read of it folds, so the compare keeps only its operands and the
    // write-back forms the field from them.
    CHECK(has(paths, "(compare_s(static_cast<int64_t>(k7a), static_cast<int64_t>(k7b), so) << 0)") && !has(paths, "cr7 = compare_s"));
    // Not where the field's local is read (mcrf cr6,cr7), nor where a read doesn't fold (after a call): set at the compare.
    const std::string copied = lifted({0x2f830000, 0x4f1c0000, 0x419e0008, 0x38800001, 0x4e800020});
    CHECK(has(copied, "cr7 = compare_s(") && has(copied, "cr6 = cr7;") && has(copied, "(cr7 << 0)"));
    CHECK(has(after, "cr7 = compare_s("));
    // A CR logical operation reads its own field's local (it changes one bit of it): set at the compare there.
    CHECK(has(fp, "cr7 = fp::fcompare("));
    // A floating-point compare and branch, alone: lazy, formed at the write-back.
    const std::string fcmp = lifted({0xff811000, 0x419e0008, 0x38800001, 0x4e800020});
    CHECK(has(fcmp, "(fp::fcompare(k7x, k7y) << 0)") && !has(fcmp, "cr7 = fp::fcompare("));
}

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

// ---------------------------------------------------------------- the SPU decoder

std::string spu_text(uint32_t word, uint32_t pc = 0x3000) { return spu::disasm(spu::decode(word), pc); }

void test_spu() {
    using spu::Op;
    // What an SPU function begins and ends with.
    CHECK(spu_text(0x24004080) == "stqd $0,16($1)");
    CHECK(spu_text(0x24ff8081) == "stqd $1,-32($1)");
    CHECK(spu_text(0x1cf80081) == "ai $1,$1,-32");
    CHECK(spu_text(0x34004080) == "lqd $0,16($1)");
    CHECK(spu_text(0x35000000) == "bi $0");
    CHECK(spu_text(0x35080000) == "bid $0");                // interrupts disabled on the way
    CHECK(spu_text(0x00500000) == "syncc" && !spu::decode(0x00480000).valid());  // sync has a C bit and no D
    CHECK(spu::decode(0x35080000).d() && !spu::decode(0x35080000).e());
    // Each length of opcode.
    CHECK(spu_text(0x18010183) == "a $3,$3,$4");            // 11 bits
    CHECK(spu_text(0x76804183) == "csflt $3,$3,154");       // 10
    CHECK(spu_text(0x40800083) == "il $3,1");               // 9
    CHECK(spu_text(0x40ffff83) == "il $3,-1");
    CHECK(spu_text(0x1c004183) == "ai $3,$3,1");            // 8
    CHECK(spu_text(0x42000803) == "ila $3,16");             // 7
    CHECK(spu_text(0x80a10283) == "selb $5,$5,$4,$3");      // 4: the target is the first field
    // Branches and loads take addresses: relative to themselves, or absolute.
    CHECK(spu_text(0x32000200) == "br 0x3010");
    CHECK(spu_text(0x33000803) == "brsl $3,0x3040");
    CHECK(spu_text(0x327ffe00, 0x3010) == "br 0x3000");
    CHECK(spu_text(0x30000800) == "bra 0x40");
    CHECK(spu_text(0x20000183) == "brz $3,0x300c");
    CHECK(spu::decode(0x32000200).relative(0x3000) == 0x3010);
    CHECK(spu::decode(0x327ffe00).relative(0) == 0x3fff0);  // an SPU's memory wraps at 256 KiB
    // Channels, and the rest.
    CHECK(spu_text(0x01a00e83) == "rdch $3,$ch29");
    CHECK(spu_text(0x21a00e83) == "wrch $ch29,$3");
    CHECK(spu_text(0x00200000) == "lnop");
    CHECK(spu_text(0x40200000) == "nop $0");
    CHECK(spu_text(0x00000000) == "stop");
    CHECK(spu_text(0x00002000) == "stop" && spu::decode(0x00002000).u14() == 0x2000);
    // Reserved bits set don't decode, unless asked to look past them.
    CHECK(!spu::decode(0x00200001).valid());                // lnop has no operands
    CHECK(!spu::decode(0x54a04183).valid());                // clz has no rb
    CHECK(spu::decode(0x54a04183, true).op == Op::clz);
    CHECK(!spu::decode(0x07000000).valid());                // no instruction has this opcode
    // Every spec decodes from its own opcode, under its own name.
    for (const spu::Spec& s : spu::specs()) {
        const uint32_t word = static_cast<uint32_t>(s.opcode) << (32 - s.bits);
        const spu::Insn in = spu::decode(word);
        if (in.op != s.op) std::println(stderr, "SPU {} doesn't decode from its own opcode ({:08x})", s.name, word);
        CHECK(in.op == s.op);
        CHECK(spu::disasm(in, 0).starts_with(s.name));
    }
    CHECK(spu::spec(Op::brsl).flags & spu::kCall);
    CHECK(spu::spec(Op::fma).flags & spu::kFloat);
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

// A switch whose out-of-range index calls a function that never returns (`ble` over `bl abort; nop` into the table's
// block): proven only when the pass knows that function doesn't return. `callee_words` is that function's code, at
// f+0x60; `exiting` names import 0 sys_process_exit (its stub at kStub0).
struct NoReturnCase {
    bool proven = false;
    bool callee_returns = true;
    int rounds = 0;
};
constexpr uint32_t kSwitchAt = kText + 0x120, kCallee = kSwitchAt + 0x60;

std::optional<NoReturnCase> switch_after_call(const std::vector<uint32_t>& callee_words, bool exiting) {
    const uint32_t f = kSwitchAt;
    const size_t at = kTextAt + 0x120;
    const std::vector<uint32_t> words = {
        0x2b830002,                          // cmpli cr7,0,r3,2
        0x409d000c,                          // bc 4,4*cr7+gt,f+0x10: in range, to the table's block
        0x48000001u | (kCallee - (f + 8)),   // bl callee
        0x60000000,                          // nop
        0x81628010, 0x78691788, 0x7c09582e, 0x7c0007b4, 0x7c005a14, 0x7c0903a6, 0x4e800420,  // the table's address, ..., bctr
        0x0000000c, 0x00000010, 0x00000014,  // the table, at f+0x2c: to f+0x38, f+0x3c, f+0x40
        0x4e800020, 0x4e800020, 0x4e800020,
    };
    Bytes p = make_elf();
    put_segment(p, 0, 1, 5, kTextAt, kText, 0x200, 0x200, 0x10000);
    put_section(p, 1, 1, 1, 6, kText, kTextAt, 0x200);
    put_be32(p, kTextAt, 0x48000120);  // b f
    for (size_t i = 0; i < words.size(); ++i) put_be32(p, at + 4 * i, words[i]);
    for (size_t i = 0; i < callee_words.size(); ++i) put_be32(p, at + 0x60 + 4 * i, callee_words[i]);
    put_be32(p, kDataAt + 16, f + 0x2c);
    if (exiting) {
        put_text(p, kTextAt + (kName - kText), std::string_view("sysPrxForUser\0", 14));
        put_be32(p, kTextAt + (kIds - kText), import_id("sys_process_exit"));
    }
    const auto e = Elf::parse(std::move(p));
    if (!e) return std::nullopt;
    const auto c = Cfg::build(*e);
    if (!c || !c->functions().contains(f) || c->functions().at(f).tables.size() != 1 || c->functions().at(f).tables[0].targets.size() != 3)
        return std::nullopt;
    NoReturnCase r;
    r.proven = c->functions().at(f).tables[0].proven;
    r.callee_returns = !c->no_return().contains(kCallee);
    r.rounds = c->rounds();
    return r;
}

uint32_t bl_from(uint32_t from, uint32_t to) { return 0x48000001u | ((to - from) & 0x03fffffcu); }

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

    // The bound's compare can be well above its branch (the compiler hoists it past the register saves), and the proof
    // must stop at anything else that writes the same CR field. `extra` goes between `cmplwi cr7,r3,2` and `bgt cr7`.
    const auto bound_proven = [&](const std::vector<uint32_t>& extra) -> std::optional<bool> {
        std::vector<uint32_t> words = {0x2b830002};  // cmpli cr7,0,r3,2
        words.insert(words.end(), extra.begin(), extra.end());
        const size_t branch = words.size(), table = branch + 8, targets = table + 3;
        words.push_back(0x419d0000u | static_cast<uint32_t>(4 * (targets - branch)));  // bc 12,4*cr7+gt to the first target
        for (const uint32_t w : {0x81628010u, 0x78691788u, 0x7c09582eu, 0x7c0007b4u, 0x7c005a14u, 0x7c0903a6u, 0x4e800420u}) words.push_back(w);
        for (uint32_t k = 0; k < 3; ++k) words.push_back(4 * (3 + k));  // offsets from the table to the three targets
        for (int k = 0; k < 3; ++k) words.push_back(0x4e800020);  // blr, blr, blr
        Bytes p3 = make_elf();
        put_segment(p3, 0, 1, 5, kTextAt, kText, 0x200, 0x200, 0x10000);
        put_section(p3, 1, 1, 1, 6, kText, kTextAt, 0x200);
        put_be32(p3, kTextAt, 0x48000120);  // b f
        for (size_t i = 0; i < words.size(); ++i) put_be32(p3, at + 4 * i, words[i]);
        put_be32(p3, kDataAt + 16, f + static_cast<uint32_t>(4 * table));
        const auto elf3 = Elf::parse(std::move(p3));
        if (!elf3) return std::nullopt;
        const auto cfg3 = Cfg::build(*elf3);
        if (!cfg3 || !cfg3->functions().contains(f) || cfg3->functions().at(f).tables.size() != 1) return std::nullopt;
        const JumpTable& t = cfg3->functions().at(f).tables[0];
        if (t.targets.size() != 3) return std::nullopt;
        return t.proven;
    };
    const std::vector<uint32_t> nops(10, 0x60000000);  // ori r0,r0,0
    CHECK(bound_proven({}) == true);
    CHECK(bound_proven(nops) == true);  // ten instructions up: found
    std::vector<uint32_t> other = nops, same = nops;
    other[4] = 0x4f5ac042;  // crnor 26,26,24: writes cr6
    same[4] = 0x4fdee042;   // crnor 30,30,28: writes cr7, so the compare above no longer bounds the branch
    CHECK(bound_proven(other) == true);
    CHECK(bound_proven(same) == false);
    std::vector<uint32_t> fcmp = nops;
    fcmp[2] = 0xff8d0000;  // fcmpu cr7,f13,f0
    CHECK(bound_proven(fcmp) == false);
    std::vector<uint32_t> record = nops;
    record[3] = 0x7c632379;  // or. r3,r3,r4: writes cr0, not cr7
    CHECK(bound_proven(record) == true);

    // Switches whose out-of-range index calls a function that never returns (switch_after_call).
    const uint32_t callee = kCallee;
    // A loop that never ends.
    const auto spins = switch_after_call({0x48000000}, false);  // b .
    CHECK(spins && spins->proven && !spins->callee_returns && spins->rounds == 3);
    // A call of sys_process_exit, then what would follow if it returned.
    const auto exits = switch_after_call({0x7c0802a6, bl_from(callee + 4, kStub0), 0x4e800020}, true);  // mflr r0; bl sys_process_exit; blr
    CHECK(exits && exits->proven && !exits->callee_returns);
    // The same import under another name may return: then so does the callee, and the bound isn't proven.
    const auto other_import = switch_after_call({0x7c0802a6, bl_from(callee + 4, kStub0), 0x4e800020}, false);
    CHECK(other_import && !other_import->proven && other_import->callee_returns);
    // A function that returns on one path.
    const auto returns = switch_after_call({0x2c030000, 0x4d820020, 0x48000000}, false);  // cmpwi r3,0; beqlr; b .
    CHECK(returns && !returns->proven && returns->callee_returns);
    // A jump through CTR might go anywhere: counted as a way back.
    const auto jumps = switch_after_call({0x7c6903a6, 0x4e800420}, false);  // mtctr r3; bctr
    CHECK(jumps && !jumps->proven && jumps->callee_returns);
    // A call that doesn't return ends a path: a callee that only calls the loop never returns either.
    const auto chain = switch_after_call({bl_from(callee, callee + 8), 0x4e800020, 0x48000000}, false);  // bl +8; blr; b .
    CHECK(chain && chain->proven && !chain->callee_returns);
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

// ---------------------------------------------------------------- the archive's directory

// Program data holding a directory: names, then 3 files, then 9 packs (the
// first compressed with two files, the second plain with one, the rest empty).
struct ArchiveData {
    static constexpr uint32_t kAt = 0x10150000, kFiles = 0x100, kPacks = kFiles + 3 * 20;
    Bytes bytes = Bytes(0x400);
    ArchiveData() {
        put_text(bytes, 0x10, "Tex");
        put_text(bytes, 0x18, "Mdl");
        put_text(bytes, 0x20, "Car");
        put_text(bytes, 0x28, "Table");
        file(0, 0, 0x900, 0x10);
        file(1, 0x940, 0x100, 0x18);
        file(2, 0, 0x7ff, 0x18, 2048);
        pack(0, 0, 2, 0, 2, true, 0xc00, 0xa40, 0x20);
        pack(1, 2, 1, 2, 3, false, 0, 0, 0x28);
        for (uint32_t i = 2; i < 9; ++i) {
            const std::string name = "Empty" + std::to_string(i);
            put_text(bytes, 0x30 + 8 * i, name);
            pack(i, 3, 0, 0, 0, false, 0, 0, 0x30 + 8 * i);
        }
    }
    void file(uint32_t i, uint32_t offset, uint32_t size, uint32_t name, uint16_t align = 64) {
        const size_t at = kFiles + 20 * i;
        put_be32(bytes, at, kAt + name);
        put_be16(bytes, at + 8, static_cast<uint16_t>((offset % 2048 + size + 2047) / 2048));
        put_be16(bytes, at + 10, align);
        put_be32(bytes, at + 12, size);
        put_be32(bytes, at + 16, offset);
    }
    void pack(uint32_t i, uint32_t sector, uint16_t sectors, uint16_t first, uint16_t end, bool compressed, uint32_t stored, uint32_t size,
              uint32_t name) {
        const size_t at = kPacks + 36 * i;
        put_be32(bytes, at, kAt + name);
        put_be32(bytes, at + 4, sector);
        put_be16(bytes, at + 8, sectors);
        put_be16(bytes, at + 10, first);
        put_be16(bytes, at + 12, end);
        put_be16(bytes, at + 14, compressed);
        put_be32(bytes, at + 16, stored);
        put_be32(bytes, at + 20, size);
    }
    std::expected<Archive, std::string> find() const {
        const ArchiveMemory memory[] = {{kAt, bytes}};
        return find_archive(memory, [&](uint32_t address) -> std::string {
            if (address < kAt || address - kAt >= bytes.size()) return {};
            return reinterpret_cast<const char*>(bytes.data() + (address - kAt));
        });
    }
    std::string error() const {
        const auto found = find();
        return found ? std::string() : found.error();
    }
};

void test_archive() {
    const ArchiveData data;
    const auto archive = data.find();
    CHECK(archive.has_value());
    if (archive) {
        CHECK(archive->files_at == ArchiveData::kAt + ArchiveData::kFiles && archive->packs_at == ArchiveData::kAt + ArchiveData::kPacks);
        CHECK(archive->files.size() == 3 && archive->packs.size() == 9 && archive->size() == 3 * 2048);
        const ArchivePack& car = archive->packs[0];
        CHECK(car.name == "Car" && car.compressed && car.stored_size == 0xc00 && car.size == 0xa40 && car.files == 2);
        CHECK(car.stored_at() == 2 * 2048 - 0xc00);  // its bytes end with its sectors
        const ArchivePack& table = archive->packs[1];
        CHECK(table.name == "Table" && !table.compressed && table.size == 2048 && table.stored_at() == 2 * 2048 && table.first_file == 2);
        CHECK(archive->files[0].name == "Tex" && archive->files[0].pack == 0 && archive->files[0].size == 0x900);
        CHECK(archive->files[1].name == "Mdl" && archive->files[1].offset == 0x940 && archive->files[2].pack == 1);
        CHECK(archive->packs[2].files == 0 && archive->packs[2].sectors == 0);
    }
    ArchiveData bad;
    bad.file(1, 0x8c0, 0x100, 0x18);  // overlaps the file before it
    CHECK(has(bad.error(), "overlaps"));
    bad = ArchiveData();
    bad.file(1, 0x980, 0x100, 0x18);  // runs past the pack's unpacked bytes
    CHECK(has(bad.error(), "runs past"));
    bad = ArchiveData();
    bad.file(0, 0, 0x900, 0x10, 32);
    CHECK(has(bad.error(), "aligned to 32"));
    bad = ArchiveData();
    bad.pack(0, 0, 2, 0, 2, true, 0x700, 0xa40, 0x20);  // too few stored bytes to reach back into its first sector
    CHECK(has(bad.error(), "stored bytes"));
    bad = ArchiveData();
    put_text(bad.bytes, 0x28, "Car");  // two packs of one name
    bad.bytes[0x2b] = 0;
    CHECK(has(bad.error(), "two packs"));
    bad = ArchiveData();
    put_be32(bad.bytes, ArchiveData::kFiles, 0);  // a file with no name
    CHECK(has(bad.error(), "no name"));
    bad = ArchiveData();
    bad.pack(1, 5, 1, 2, 3, false, 0, 0, 0x28);  // the packs don't follow one another: no table
    CHECK(has(bad.error(), "wasn't found"));
}

// ---------------------------------------------------------------- compiled shaders

void test_shaders() {
    // The test program with room made in its data for two compiled programs.
    const auto with_shaders = [](uint32_t second_microcode_at) {
        Bytes program = make_elf();
        const size_t at = kDataAt + 0x20;
        put_segment(program, 1, 1, 6, kDataAt, kData, 0x80, 0x100, 0x10000);
        const auto shader = [&](size_t where, uint32_t profile, uint32_t size, uint32_t microcode_at) {
            put_be32(program, where, profile);
            put_be32(program, where + 4, 6);
            put_be32(program, where + 8, size);
            put_be32(program, where + 12, 1);
            put_be32(program, where + 16, 0x20);
            put_be32(program, where + 20, 0x20);
            put_be32(program, where + 24, 8);
            put_be32(program, where + 28, microcode_at);
        };
        shader(at, 7003, 0x30, 0x28);
        shader(at + 0x30, 7004, 0x30, second_microcode_at);
        return Elf::parse(std::move(program));
    };
    const auto elf = with_shaders(0x28);
    CHECK(elf.has_value());
    if (!elf) return;
    const auto programs = find_shader_programs(*elf);
    CHECK(programs.has_value());
    if (programs) {
        CHECK(programs->size() == 2);
        if (programs->size() == 2) {
            CHECK((*programs)[0].address == kData + 0x20 && (*programs)[0].vertex() && (*programs)[0].size == 0x30);
            CHECK((*programs)[1].address == kData + 0x50 && !(*programs)[1].vertex() && (*programs)[1].microcode_size == 8);
        }
    }
    const auto broken = with_shaders(0x2c);  // the second one's microcode runs past its end
    CHECK(broken.has_value());
    if (broken) {
        const auto found = find_shader_programs(*broken);
        CHECK(!found && has(found.error(), "microcode is outside"));
    }
    const auto plain = Elf::parse(make_elf());
    CHECK(plain && find_shader_programs(*plain).has_value() && find_shader_programs(*plain)->empty());
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

// ---------------------------------------------------------------- the SPU control-flow pass

void test_spu_cfg() {
    // The test SPU program with real code in its four words: a function that
    // returns, the entry point calling it and returning, and padding.
    Bytes file = make_spu_elf();
    put_be32(file, 0x100, 0x35000000);  // 0x3000: bi $0
    put_be32(file, 0x104, 0x337fff80);  // 0x3004 (the entry point): brsl $0,0x3000
    put_be32(file, 0x108, 0x35000000);  // 0x3008: bi $0
    put_be32(file, 0x10c, 0x00200000);  // 0x300c: lnop
    const auto image = parse_spu_image(file, 0x10052b00);
    CHECK(image.has_value());
    if (!image) return;
    const auto cfg = SpuCfg::build(*image, file);
    CHECK(cfg.has_value());
    if (!cfg) return;
    CHECK(cfg->functions().size() == 2 && cfg->rejected().empty());
    CHECK(cfg->functions().contains(0x3004) && cfg->functions().at(0x3004).origin == SpuOrigin::Entry);
    CHECK(cfg->functions().at(0x3004).calls.contains(0x3000) && cfg->functions().at(0x3004).words == 2);
    CHECK(cfg->functions().contains(0x3000) && cfg->functions().at(0x3000).origin == SpuOrigin::Call);
    CHECK(cfg->coverage().code == 3 && cfg->coverage().padding == 1 && cfg->coverage().unknown == 0);

    // The same with the call taken out: the first function is code nothing reaches, found as a gap... but it is
    // the section's first word, with nothing before it to make the boundary clean, so it stays unknown.
    put_be32(file, 0x104, 0x40200000);  // nop
    const auto alone = SpuCfg::build(*image, file);
    CHECK(alone && alone->functions().size() == 1 && alone->coverage().unknown == 1);

    // Code after the entry function's return is a gap, and a function.
    put_be32(file, 0x100, 0x35000000);  // 0x3000: bi $0   (unknown, as above)
    put_be32(file, 0x104, 0x35000000);  // 0x3004 (entry): bi $0
    put_be32(file, 0x108, 0x00200000);  // lnop
    put_be32(file, 0x10c, 0x35000000);  // 0x300c: bi $0, reached by nothing
    const auto gap = SpuCfg::build(*image, file);
    CHECK(gap && gap->functions().contains(0x300c) && gap->functions().at(0x300c).origin == SpuOrigin::Gap);

    // A word that isn't an instruction, reached from the entry point, is reported.
    put_be32(file, 0x104, 0x07000000);
    const auto broken = SpuCfg::build(*image, file);
    CHECK(broken && broken->functions().at(0x3004).bad.size() == 1);
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
    // Written out and read again, it is the same; and an empty text and a number survive.
    Sfo more = *sfo;
    more.entries.push_back({"EMPTY", false, 0, ""});
    more.entries.push_back({"NUMBER", true, 0xfedcba98u, {}});
    const auto again = Sfo::parse(more.bytes());
    CHECK(again.has_value());
    if (again) {
        CHECK(again->entries.size() == more.entries.size());
        for (size_t i = 0; i < again->entries.size() && i < more.entries.size(); ++i)
            CHECK(again->entries[i].key == more.entries[i].key && again->entries[i].integer == more.entries[i].integer &&
                  again->entries[i].number == more.entries[i].number && again->entries[i].text == more.entries[i].text);
    }
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

// ---------------------------------------------------------------- the RSX's programs

// Instructions built field by field from the documented layout (rsx_program.hpp), not taken from any program.
void put_be32_back(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s));
}

void test_rsx_program() {
    using namespace qilin::rsx;
    // MAD R3.xz, -v[2].yzwx, c[300], R5 (the vector unit), and RSQ o[TC1].w, R5.x (the scalar unit), last.
    {
        const auto source = [](uint32_t type, uint32_t temp, uint32_t x, uint32_t y, uint32_t z, uint32_t w, bool negate) {
            return type | (temp << 2) | (w << 8) | (z << 10) | (y << 12) | (x << 14) | (negate ? 1u << 16 : 0u);
        };
        const uint32_t s0 = source(2, 0, 1, 2, 3, 0, true), s1 = source(3, 0, 0, 1, 2, 3, false), s2 = source(1, 5, 0, 0, 0, 0, false);
        uint32_t w[4] = {};
        w[0] = (3u << 15) | (7u << 10) | (0x1bu << 2);           // the vector unit's temp 3; the condition "always", swizzle xyzw
        w[1] = (uint32_t{kScaRsq} << 27) | (uint32_t{kVecMad} << 22) | (300u << 12) | (2u << 8) | (s0 >> 9);
        w[2] = ((s0 & 0x1ff) << 23) | (s1 << 6) | (s2 >> 11);
        w[3] = ((s2 & 0x7ff) << 21) | (1u << 17) | (0xau << 13) | (1u << 12) | (8u << 2) | 1u;  // scalar w to the output, vector xz; output 8 (TC1); last
        const VpInsn in = decode_vp(std::span<const uint32_t, 4>(w, 4));
        CHECK(in.unknown == (std::array<uint32_t, 4>{}));
        CHECK(in.vec_op == kVecMad && in.sca_op == kScaRsq && in.constant == 300 && in.input == 2 && in.last);
        CHECK(in.vec_temp == 3 && in.vec_mask == 0xa && in.sca_result && in.sca_mask == 1 && in.dest == 8);
        CHECK(in.src[0].type == VpRegister::input && in.src[0].negate && in.src[0].swizzle == (std::array<uint8_t, 4>{1, 2, 3, 0}));
        CHECK(in.src[1].type == VpRegister::constant && in.src[2].type == VpRegister::temp && in.src[2].temp == 5);
        CHECK(disasm(in) == "MAD R3.xz, -v[2].yzwx, c[300], R5.xxxx  +  RSQ o[TC1].w, R5.xxxx  (last)");
    }
    // ADD R1.y, H2, {1.0, 0, 0, 0}.xxxx: a constant inline after it; then MOV R0, f[COL0], the end. Stored with halves exchanged.
    {
        const auto source = [](uint32_t type, uint32_t index, bool half, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
            return type | (index << 2) | (half ? 1u << 8 : 0u) | (x << 9) | (y << 11) | (z << 13) | (w << 15);
        };
        const uint32_t words[] = {
            (uint32_t{kFpAdd} << 24) | (1u << 10) | (1u << 1),  // R1, mask y (bit 10)
            source(0, 2, true, 0, 1, 2, 3) | (7u << 18) | (0xe4u << 21),
            source(2, 0, false, 0, 0, 0, 0),
            0,
            0x3f800000u, 0, 0, 0,
            (uint32_t{kFpMov} << 24) | (1u << 13) | (0xfu << 9) | 1u,  // input COL0, all of R0, the end
            source(1, 0, false, 0, 1, 2, 3) | (7u << 18) | (0xe4u << 21),
            0,
            0x7fcu << 19,
        };
        Bytes code;
        for (const uint32_t v : words) {
            const uint32_t stored = (v << 16) | (v >> 16);
            code.push_back(static_cast<uint8_t>(stored >> 24)), code.push_back(static_cast<uint8_t>(stored >> 16));
            code.push_back(static_cast<uint8_t>(stored >> 8)), code.push_back(static_cast<uint8_t>(stored));
        }
        size_t used = 0;
        const std::vector<FpInsn> insns = decode_fp(code, &used);
        CHECK(insns.size() == 2 && used == 12);
        if (insns.size() == 2) {
            CHECK(insns[0].op == kFpAdd && insns[0].mask == 4 && insns[0].has_constant && insns[0].constant[0] == 0x3f800000u && insns[0].src[0].half);
            CHECK(insns[1].op == kFpMov && insns[1].end && insns[1].input == 1 && insns[1].input_displacement == 0x7fc);
            CHECK(disasm(insns[0]) == "ADDR R1.y, H2, {0x3f800000, 0x0, 0x0, 0x0}.xxxx");
            CHECK(disasm(insns[1]) == "MOVR R0, f[COL0]  (end)");
        }
    }
}

// The translation of synthetic programs: a vertex program that moves an input to the position and a texture
// coordinate, and a fragment program that reads it and a constant; refusals by name.
void test_rsx_spirv() {
    using namespace qilin::rsx;
    const auto vp_source = [](uint32_t type, uint32_t temp) { return type | (temp << 2) | (0x1bu << 8); };  // swizzle xyzw
    const auto vp_word = [&](uint32_t op, uint32_t input, uint32_t dest, bool last) {
        const uint32_t s0 = vp_source(2, 0);
        return std::array<uint32_t, 4>{(1u << 30) | (0x3fu << 15) | (7u << 10) | (0x1bu << 2), (op << 22) | (input << 8) | (s0 >> 9), (s0 & 0x1ff) << 23,
                                       (0xfu << 13) | (dest << 2) | (last ? 1u : 0u)};
    };
    CgProgram vp;
    vp.vertex = true;
    vp.header = {2, 0, 1, 1, 0, 0};
    for (const auto& w : {vp_word(kVecMov, 0, 0, false), vp_word(kVecMov, 0, 7, true)})
        for (const uint32_t v : w) put_be32_back(vp.microcode, v);
    const auto vt = translate(vp);
    CHECK(vt.has_value());
    if (vt) {
        CHECK(vt->spirv.size() > 5 && vt->spirv[0] == 0x07230203u && vt->inputs == 1 && vt->outputs == ((1u << 0) | (1u << 7)));
    }
    // MUL R0, f[TEX0], {2, 2, 2, 2}: a constant inline; the end.
    CgProgram fp;
    fp.header = {2, 0, 0, 0x10000, 0x200, 0, 0};
    const uint32_t words[] = {(uint32_t{kFpMul} << 24) | (4u << 13) | (0xfu << 9) | 1u, 1u | (0xe4u << 9) | (7u << 18) | (0xe4u << 21), 2u | (0xe4u << 9), 0x7fcu << 19,
                              0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u};
    for (const uint32_t v : words) put_be32_back(fp.microcode, (v << 16) | (v >> 16));
    const auto ft = translate(fp);
    CHECK(ft.has_value());
    if (ft) CHECK(ft->inputs == (1u << 4) && ft->outputs == 1 && ft->constant_slots == std::vector<uint32_t>{16});
    // The key leaves the inline constant out: another value, the same key.
    CgProgram other = fp;
    other.microcode[16] ^= 0x55;
    CHECK(program_key(other) == program_key(fp));
    // An operation the translation doesn't take is refused by name.
    CgProgram kil = fp;
    kil.microcode[2] = kFpKil;  // (word 0 is stored with its halves exchanged: its top byte, the opcode, is the stored word's third)
    const auto refused = translate(kil);
    CHECK(!refused.has_value() && refused.error().find("KIL") != std::string::npos);
}

// ---------------------------------------------------------------- the lifter and C++ exceptions
// (The unwinding program in the differential test runs them; these hold the shapes the lifter takes and refuses.)

// ---------------------------------------------------------------- what a callee reads and writes
// register_effects over a few functions, and a call narrowed by them (lift.cpp). The differential test's programs of
// three functions check the results; these check the shapes.
void test_lift_effects() {
    const auto input = [](uint32_t at, std::vector<uint32_t> words) {
        LiftInput in;
        in.start = at;
        for (size_t i = 0; i < words.size(); ++i) in.insns.emplace_back(at + 4 * static_cast<uint32_t>(i), ppu::decode(words[i]));
        in.is_function = [](uint32_t a) { return a == 0x10100 || a == 0x10200 || a == 0x10300 || a == 0x10400; };
        return in;
    };
    const auto has = [](const std::string& text, std::string_view what) { return text.find(what) != std::string::npos; };
    const std::vector<LiftInput> program = {
        input(0x10200, {0x38a30001, 0x4e800020}),  // leaf: addi r5,r3,1; blr
        input(0x10100, {0x48000101, 0x4e800020}),  // mid: bl leaf; blr
        input(0x10300, {0x44000002, 0x4e800020}),  // sc; blr
        input(0x10400, {0x4bfffd01, 0x4e800020}),  // bl mid (from 0x10400); blr
    };
    const EffectsTable effects = register_effects(program);
    const auto slots = [](std::initializer_list<size_t> bits) {
        std::bitset<kRegisterSlots> b;
        for (const size_t n : bits) b.set(n);
        return b;
    };
    constexpr size_t kLr = 104;
    CHECK(effects.size() == 4);
    CHECK(effects.at(0x10200).reads == slots({3}) && effects.at(0x10200).writes == slots({5}));
    CHECK(effects.at(0x10100).reads == slots({3}) && effects.at(0x10100).writes == slots({5, kLr}));  // (bl sets LR)
    CHECK(effects.at(0x10300).reads.all() && effects.at(0x10300).writes.all());                        // a system call: everything
    CHECK(effects.at(0x10400).reads == slots({3}) && effects.at(0x10400).writes == slots({5, kLr}));
    // A call to a function not among them: everything.
    const EffectsTable alone = register_effects({input(0x10100, {0x48000101, 0x4e800020})});
    CHECK(alone.at(0x10100).reads.all() && alone.at(0x10100).writes.all());
    // The caller: addi r3,r3,1; addi r4,r4,1; bl leaf; addi r6,r5,0; blr. Before the call only r3 is written back (r4
    // and LR wait for the return), after it only r5 is read again.
    LiftInput caller = input(0x10000, {0x38630001, 0x38840001, 0x480001f9, 0x38c50000, 0x4e800020});
    caller.effects = &effects;
    const Lifted narrowed = lift(caller);
    CHECK(narrowed.ok && narrowed.narrowed == 1);
    CHECK(has(narrowed.code, "c.r[3] = r3;  fn_00010200(c); r5 = c.r[5]; ") && has(narrowed.code, "c.r[4] = r4;") && has(narrowed.code, "c.lr = lr;"));
    CHECK(!has(narrowed.code, "r4 = c.r[4];") || has(narrowed.code, "uint64_t r4 = c.r[4];"));
    // Without the effects (or with --registers), the call syncs everything.
    caller.effects = nullptr;
    const Lifted full = lift(caller);
    CHECK(full.ok && full.narrowed == 0 && has(full.code, "fn_00010200(c); r3 = c.r[3]; r4 = c.r[4]; r5 = c.r[5]; r6 = c.r[6]; lr = c.lr;"));
}

void test_lift_exceptions() {
    const auto input = [](std::vector<uint32_t> words, std::vector<uint32_t> pads = {}) {
        LiftInput in;
        in.start = 0x10000;
        for (size_t i = 0; i < words.size(); ++i) in.insns.emplace_back(0x10000 + 4 * static_cast<uint32_t>(i), ppu::decode(words[i]));
        in.landing_pads = std::move(pads);
        in.is_function = [](uint32_t) { return false; };
        return in;
    };
    const auto has = [](const std::string& text, std::string_view what) { return text.find(what) != std::string::npos; };
    // __builtin_eh_return's epilogue, either operand order: a throw when the adjustment isn't 0.
    for (const uint32_t add : {0x7c215214u, 0x7c2a0a14u}) {  // add r1,r1,r10; add r1,r10,r1
        const Lifted l = lift(input({add, 0x4e800020}));
        CHECK(l.ok && l.eh_returns == 1 && !l.catches && has(l.code, "if (r10 != 0ull)") && has(l.code, "eh_return(c)"));
    }
    // The stack pointer moved by a register anywhere else is a shape the lifter doesn't know: not lifted, by name.
    const Lifted apart = lift(input({0x7c215214, 0x60000000, 0x4e800020}));  // a nop between the add and the return
    CHECK(!apart.ok && has(apart.why, "__builtin_eh_return") && apart.why_at == 0x10000);
    const Lifted conditional = lift(input({0x7c215214, 0x4d9e0020, 0x4e800020}));  // beqlr cr7
    CHECK(!conditional.ok && has(conditional.why, "__builtin_eh_return"));
    const Lifted record = lift(input({0x7c215215, 0x4e800020}));  // add. r1,r1,r10
    CHECK(!record.ok && has(record.why, "__builtin_eh_return"));
    const Lifted plain = lift(input({0x7c632214, 0x4e800020}));  // add r3,r3,r4: nothing to do with it
    CHECK(plain.ok && plain.eh_returns == 0 && !has(plain.code, "eh_return"));
    // A function with a landing pad: lifted twice, from its start and from its pads, under the runtime's try.
    const Lifted pad = lift(input({0x38600001, 0x4e800020, 0x38600002, 0x4e800020}, {0x10008}));
    CHECK(pad.ok && pad.catches && pad.eh_returns == 0);
    CHECK(has(pad.code, "static void fn_00010000_start(Cpu& c)") && has(pad.code, "static void fn_00010000_pad(Cpu& c, uint32_t pad)") &&
          has(pad.code, "case 0x00010008u: goto L_00010008;") && has(pad.code, "kPads[] = {0x00010008u}") &&
          has(pad.code, "run_with_landing_pads(c, &fn_00010000_start, &fn_00010000_pad, kPads)"));
    const Lifted none = lift(input({0x38600001, 0x4e800020}));
    CHECK(none.ok && !none.catches && !has(none.code, "run_with_landing_pads") && !has(none.code, "_pad"));
    // A pad the function doesn't reach can't be entered: refused, not ignored.
    const Lifted missing = lift(input({0x38600001, 0x4e800020}, {0x10008}));
    CHECK(!missing.ok && has(missing.why, "landing pad") && missing.why_at == 0x10008);
}

int main() {
    test_lift_effects();
    test_lift_exceptions();
    test_lift_dataflow();
    test_sha256();
    test_ppu();
    test_spu();
    test_elf();
    test_self();
    test_sfo();
    test_media();
    test_spu_cfg();
    test_shaders();
    test_archive();
    test_rsx_program();
    test_rsx_spirv();
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
