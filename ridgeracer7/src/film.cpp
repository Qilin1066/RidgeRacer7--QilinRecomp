// The films: cellPamf (the PAMF file's header), cellDmux (the program stream cut into access units) and libvdec (MPEG-2
// pictures decoded), with FFmpeg's libavcodec doing the parsing and decoding (cmake/ffmpeg.cmake).
//
// What the game uses of each, and the layouts of what it reads, are read off its own film player (0x2d0000 to 0x2d3900):
//   - a stream's info for MPEG-2: byte 0 the profile and level (1 main at main level, 2 at high 1440, 3 at high: the game
//     compares it with 1), byte 3 the frame-rate code, halfwords at 10 and 12 the picture's width and height;
//   - the demultiplexer's and a stream's messages: a word, the type, first (0: the chunk is done; 0: an access unit found);
//   - cellDmuxGetAu hands back pointers, and the game reads an access unit's address and size from words 0 and 4;
//   - the picture decoder's messages: 0 an access unit done (the game releases it), 1 a picture out;
//   - a picture item: the MPEG-2 info's address at 72, its height a halfword at 2 of that; "empty" is 0x80610104;
//   - a picture asked for as format 0 with colour matrix 1 and an alpha byte: A, R, G, B bytes (BT.709); format 3, YUV
//     4:2:0 planar, into a buffer or with none, to drop pictures.
// The PAMF header's fields are in docs/data-files.md. *Stated:* a stream entry's byte 0x12 holds the frame-rate code in its
// low four bits (every film's stream has code 4 there and in its own sequence header, so this isn't told apart from other
// readings); the ES filter ids are this runtime's own (the game only hands them back to cellDmuxEnableEs).
//
// Not yet: ATRAC3plus sound streams (enabling one stops the game, by name).
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace rr7 {

namespace {

using qilin::rt::fatal;

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void store16(uint32_t at, uint16_t v) { guest_memory()[at] = static_cast<uint8_t>(v >> 8), guest_memory()[at + 1] = static_cast<uint8_t>(v); }
void put64(uint32_t at, uint64_t v) { store32(at, static_cast<uint32_t>(v >> 32)), store32(at + 4, static_cast<uint32_t>(v)); }

// Calls the game's function(arguments...) through its descriptor, on a guest thread of the runtime's.
template <typename... A>
void callback(Cpu& c, uint32_t function, A... arguments) {
    const uint64_t values[] = {static_cast<uint64_t>(arguments)...};
    for (size_t i = 0; i < sizeof...(A); ++i) c.r[3 + i] = values[i];
    call_descriptor(c, function);
}

// ---------------------------------------------------------------- cellPamf

// The stream types (the game's switch on them: 0 AVC, 1 MPEG-2, 2 ATRAC3plus, 3 LPCM, 4 AC3) and the PAMF codings.
constexpr uint8_t kTypeMpeg2 = 1, kTypeAtrac3plus = 2;
uint8_t type_of_coding(uint8_t coding) {
    switch (coding) {
    case 0x1b: return 0;
    case 0x02: return kTypeMpeg2;
    case 0xdc: return kTypeAtrac3plus;
    default: fatal("a film's stream of coding %#x, which this runtime doesn't know", coding);
    }
}

// Whether a stream of a coding is of a type asked for: the types themselves, or 20 any picture stream and 21 any sound
// (*the SDK's group types, as recalled: the game asks for 20 when it looks for its film's picture*).
bool of_type(uint8_t coding, uint8_t type) {
    const uint8_t t = type_of_coding(coding);
    if (type == 20) return t <= kTypeMpeg2;
    if (type == 21) return t >= kTypeAtrac3plus;
    return t == type;
}

struct Reader {
    uint32_t header = 0;  // the PAMF header, in guest memory
    int current = -1;     // the stream chosen
};
std::mutex g_mutex;  // the readers, demultiplexers and decoders' maps
std::map<uint32_t, Reader> g_readers;  // by the game's CellPamfReader

uint32_t streams(uint32_t header) { return be16(guest_memory() + header + 0x86); }
const uint8_t* entry(uint32_t header, uint32_t i) { return guest_memory() + header + 0x88 + 0x30 * i; }
void check_header(uint32_t header) {
    if (std::memcmp(guest_memory() + header, "PAMF", 4) != 0) fatal("the game reads a film's header at %08x, which isn't one", header);
}

// cellPamfGetHeaderSize(header, file size, uint64_t* size)
void cellPamfGetHeaderSize(Cpu& c) {
    const uint32_t header = static_cast<uint32_t>(c.r[3]);
    check_header(header);
    put64(static_cast<uint32_t>(c.r[5]), uint64_t{load32(header + 8)} * 2048);
    c.r[3] = kOk;
}
// cellPamfGetStreamOffsetAndSize(header, file size, uint64_t* offset, uint64_t* size)
void cellPamfGetStreamOffsetAndSize(Cpu& c) {
    const uint32_t header = static_cast<uint32_t>(c.r[3]);
    check_header(header);
    put64(static_cast<uint32_t>(c.r[5]), uint64_t{load32(header + 8)} * 2048);
    put64(static_cast<uint32_t>(c.r[6]), uint64_t{load32(header + 12)} * 2048);
    c.r[3] = kOk;
}
// cellPamfReaderInitialize(CellPamfReader* reader, header, file size, attribute)
void cellPamfReaderInitialize(Cpu& c) {
    const uint32_t reader = static_cast<uint32_t>(c.r[3]), header = static_cast<uint32_t>(c.r[4]);
    check_header(header);
    if (c.r[6] != 0) fatal("cellPamfReaderInitialize with attribute %llx", static_cast<unsigned long long>(c.r[6]));
    const std::lock_guard lock(g_mutex);
    g_readers[reader] = Reader{header, -1};
    c.r[3] = kOk;
}
Reader& reader_at(uint32_t address) {
    const auto it = g_readers.find(address);
    if (it == g_readers.end()) fatal("a PAMF reader at %08x that was never made", address);
    return it->second;
}
// The film's first and last presentation times: cellPamfReaderGetPresentationStartTime / EndTime(reader, CellCodecTimeStamp*)
void presentation_time(Cpu& c, uint32_t offset) {
    const std::lock_guard lock(g_mutex);
    const Reader& r = reader_at(static_cast<uint32_t>(c.r[3]));
    const uint8_t* p = guest_memory() + r.header + offset;  // 48 bits
    const uint32_t at = static_cast<uint32_t>(c.r[4]);
    store32(at, be16(p)), store32(at + 4, be32(p + 2));
    c.r[3] = kOk;
}
void cellPamfReaderGetPresentationStartTime(Cpu& c) { presentation_time(c, 0x56); }
void cellPamfReaderGetPresentationEndTime(Cpu& c) { presentation_time(c, 0x5c); }

// cellPamfReaderGetNumberOfSpecificStreams(reader, type)
void cellPamfReaderGetNumberOfSpecificStreams(Cpu& c) {
    const uint8_t type = static_cast<uint8_t>(c.r[4]);
    if (type > 4 && type != 20 && type != 21) fatal("cellPamfReaderGetNumberOfSpecificStreams for type %u, which this runtime doesn't take yet", type);
    const std::lock_guard lock(g_mutex);
    const Reader& r = reader_at(static_cast<uint32_t>(c.r[3]));
    uint32_t n = 0;
    for (uint32_t i = 0; i < streams(r.header); ++i) n += of_type(entry(r.header, i)[0], type);
    c.r[3] = n;
}
// cellPamfReaderSetStreamWithTypeAndIndex(reader, type, index): the stream's number among all.
void cellPamfReaderSetStreamWithTypeAndIndex(Cpu& c) {
    const uint8_t type = static_cast<uint8_t>(c.r[4]);
    const uint32_t index = static_cast<uint32_t>(c.r[5]);
    const std::lock_guard lock(g_mutex);
    Reader& r = reader_at(static_cast<uint32_t>(c.r[3]));
    for (uint32_t i = 0, k = 0; i < streams(r.header); ++i)
        if (of_type(entry(r.header, i)[0], type) && k++ == index) {
            r.current = static_cast<int>(i);
            c.r[3] = i;
            return;
        }
    fatal("cellPamfReaderSetStreamWithTypeAndIndex: no stream %u of type %u", index, type);
}
// The chosen stream's channel: its place among the streams of its type.
uint32_t channel_of(const Reader& r) {
    const uint8_t type = type_of_coding(entry(r.header, static_cast<uint32_t>(r.current))[0]);
    uint32_t k = 0;
    for (int i = 0; i < r.current; ++i) k += type_of_coding(entry(r.header, static_cast<uint32_t>(i))[0]) == type;
    return k;
}
// cellPamfReaderGetStreamTypeAndChannel(reader, uint8_t* type, uint8_t* channel)
void cellPamfReaderGetStreamTypeAndChannel(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const Reader& r = reader_at(static_cast<uint32_t>(c.r[3]));
    if (r.current < 0) fatal("cellPamfReaderGetStreamTypeAndChannel with no stream chosen");
    guest_memory()[static_cast<uint32_t>(c.r[4])] = type_of_coding(entry(r.header, static_cast<uint32_t>(r.current))[0]);
    guest_memory()[static_cast<uint32_t>(c.r[5])] = static_cast<uint8_t>(channel_of(r));
    c.r[3] = kOk;
}
// cellPamfStreamTypeToEsFilterId(type, channel, CellCodecEsFilterId*): four words, this runtime's own (above): the program
// stream's id and sub-id, as cellDmuxEnableEs takes them back.
void cellPamfStreamTypeToEsFilterId(Cpu& c) {
    const uint8_t type = static_cast<uint8_t>(c.r[3]), channel = static_cast<uint8_t>(c.r[4]);
    const uint32_t at = static_cast<uint32_t>(c.r[5]);
    uint32_t id = 0, sub = 0;
    if (type == kTypeMpeg2) id = 0xe0u + channel;
    else if (type == kTypeAtrac3plus) id = 0xbd, sub = channel;
    else fatal("cellPamfStreamTypeToEsFilterId for type %u, which this runtime doesn't take yet", type);
    store32(at, id), store32(at + 4, sub), store32(at + 8, 0), store32(at + 12, 0);
    c.r[3] = kOk;
}
// cellPamfReaderGetStreamInfo(reader, info, size)
void cellPamfReaderGetStreamInfo(Cpu& c) {
    const uint32_t info = static_cast<uint32_t>(c.r[4]), size = static_cast<uint32_t>(c.r[5]);
    const std::lock_guard lock(g_mutex);
    const Reader& r = reader_at(static_cast<uint32_t>(c.r[3]));
    if (r.current < 0) fatal("cellPamfReaderGetStreamInfo with no stream chosen");
    const uint8_t* e = entry(r.header, static_cast<uint32_t>(r.current));
    std::memset(guest_memory() + info, 0, size);
    switch (type_of_coding(e[0])) {
    case kTypeMpeg2: {
        if (size < 14) fatal("cellPamfReaderGetStreamInfo: %u bytes for an MPEG-2 stream's info", size);
        uint8_t level;
        switch (e[0x10]) {  // MPEG-2's profile and level
        case 0x48: level = 1; break;
        case 0x46: level = 2; break;
        case 0x44: level = 3; break;
        default: fatal("a film's MPEG-2 stream of profile and level %#x", e[0x10]);
        }
        guest_memory()[info] = level;
        guest_memory()[info + 3] = e[0x12] & 0xf;
        store16(info + 10, be16(e + 0x1c)), store16(info + 12, be16(e + 0x1e));
        break;
    }
    case kTypeAtrac3plus:
        if (size < 5) fatal("cellPamfReaderGetStreamInfo: %u bytes for an ATRAC3plus stream's info", size);
        store32(info, 48000);  // *the films' sound is taken to be at the music's rate*
        guest_memory()[info + 4] = e[0x12];  // channels (docs/data-files.md)
        break;
    default: fatal("cellPamfReaderGetStreamInfo for a stream of coding %#x", e[0]);
    }
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- cellDmux

constexpr uint64_t kVdecEmpty = 0x80610104u;

struct Au {
    uint32_t address = 0, size = 0;  // in the stream's memory
    uint32_t info = 0;               // its CellDmuxAuInfo, in guest memory
};
struct Es {
    uint32_t handle = 0, demuxer = 0;
    uint32_t id = 0, sub = 0;          // the filter: program stream id and sub-id
    uint32_t memory = 0, memory_size = 0, used_from = 0, used_to = 0;  // a ring of access units in the game's memory
    uint32_t callback = 0, argument = 0, message = 0;
    std::deque<Au> ready, taken;       // found and not yet got; got and not yet released
    std::vector<uint8_t> pending;      // the elementary stream not yet cut
    AVCodecParserContext* parser = nullptr;
    AVCodecContext* parser_context = nullptr;
    uint32_t info_slots = 0, next_slot = 0;
};
struct Demuxer {
    uint32_t handle = 0, callback = 0, argument = 0, message = 0;
    std::map<uint32_t, std::shared_ptr<Es>> es;  // by handle
    std::deque<std::pair<uint32_t, uint32_t>> chunks;  // address, size
    bool closing = false;
    std::condition_variable changed;
    std::thread thread;
};
std::map<uint32_t, std::shared_ptr<Demuxer>> g_demuxers;
std::map<uint32_t, std::shared_ptr<Es>> g_es;
uint32_t g_next_handle = 0x0d000001u;

// Room for an access unit of `size` bytes in the stream's ring, or 0.
uint32_t ring_room(Es& e, uint32_t size) {
    if (e.ready.empty() && e.taken.empty()) e.used_from = e.used_to = 0;
    if (e.used_to >= e.used_from) {
        if (e.memory_size - e.used_to >= size) return e.memory + e.used_to;
        if (e.used_from > size) return e.memory;  // wrap
        return 0;
    }
    return e.used_from - e.used_to > size ? e.memory + e.used_to : 0;
}

// One access unit cut from the stream: into the ring (waiting for room), announced to the game.
void found(Cpu& cpu, Demuxer& d, Es& e, const uint8_t* data, uint32_t size, std::unique_lock<std::mutex>& lock) {
    if (size + 16 > e.memory_size) fatal("a film's access unit of %u bytes, larger than its stream's memory (%u)", size, e.memory_size);
    uint32_t at = 0;
    d.changed.wait(lock, [&] { return d.closing || (at = ring_room(e, size)) != 0; });
    if (d.closing) return;
    std::memcpy(guest_memory() + at, data, size);
    e.used_to = at - e.memory + size;
    Au au{at, size, e.info_slots + 32 * (e.next_slot++ % 64)};
    std::memset(guest_memory() + au.info, 0, 32);
    store32(au.info, at), store32(au.info + 4, size);
    e.ready.push_back(au);
    store32(e.message, 0);  // an access unit found
    lock.unlock();
    callback(cpu, e.callback, d.handle, e.handle, e.message, e.argument);
    lock.lock();
}

// The elementary stream's new bytes, cut into access units (MPEG-2 pictures by FFmpeg's parser).
void cut(Cpu& cpu, Demuxer& d, Es& e, const uint8_t* data, size_t size, bool end, std::unique_lock<std::mutex>& lock) {
    while (size > 0 || end) {
        uint8_t* out = nullptr;
        int out_size = 0;
        const int used = av_parser_parse2(e.parser, e.parser_context, &out, &out_size, data, static_cast<int>(size), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) fatal("FFmpeg's MPEG video parser fails on a film's stream");
        data += used, size -= static_cast<size_t>(used);
        if (out_size > 0) {
            const std::vector<uint8_t> unit(out, out + out_size);  // (the parser's buffer is its own)
            found(cpu, d, e, unit.data(), static_cast<uint32_t>(unit.size()), lock);
            if (d.closing) return;
        }
        if (end && size == 0 && out_size == 0) break;
    }
}

// A chunk of the program stream: its packs, and the packets of the streams enabled.
void demux_chunk(Cpu& cpu, Demuxer& d, const uint8_t* p, uint32_t size, std::unique_lock<std::mutex>& lock) {
    uint32_t i = 0;
    while (i + 4 <= size) {
        if (be32(p + i) >> 8 != 1) fatal("a film's program stream has no start code at %u of a chunk", i);
        const uint8_t code = p[i + 3];
        if (code == 0xba) {  // pack header (MPEG-2): 14 bytes and its stuffing
            i += 14 + (p[i + 13] & 7);
            continue;
        }
        if (code == 0xb9) {  // the program's end: what the parsers hold is the last access unit
            for (auto& [handle, e] : d.es)
                if (e->parser) cut(cpu, d, *e, nullptr, 0, true, lock);
            i += 4;
            continue;
        }
        const uint32_t length = be16(p + i + 4);
        if (i + 6 + length > size) fatal("a film's packet runs past the end of the chunk the game gave (a packet across chunks isn't taken yet)");
        if (code == 0xe0 + (code & 0xf) || code == 0xbd) {
            const uint8_t* packet = p + i + 6;
            const uint32_t payload_at = 3 + packet[2];  // MPEG-2 PES header
            if (code == 0xbd) {
                bool wanted = false;
                for (const auto& [handle, e] : d.es) wanted = wanted || e->id == 0xbd;
                if (wanted) fatal("a film's sound stream, which this runtime doesn't take yet (ATRAC3plus)");
            } else {
                for (auto& [handle, e] : d.es)
                    if (e->id == code) cut(cpu, d, *e, packet + payload_at, length - payload_at, false, lock);
            }
        }
        i += 6 + length;  // (system header 0xbb, padding 0xbe, private 2 0xbf and streams not enabled: passed over)
    }
}

void demuxer_thread(std::shared_ptr<Demuxer> d) {
    Cpu cpu;
    become_guest_thread(cpu, 0x10000, 0);
    std::unique_lock lock(g_mutex);
    for (;;) {
        d->changed.wait(lock, [&] { return d->closing || !d->chunks.empty(); });
        if (d->closing) return;
        const auto [address, size] = d->chunks.front();
        demux_chunk(cpu, *d, guest_memory() + address, size, lock);
        if (d->closing) return;
        d->chunks.pop_front();
        store32(d->message, 0);  // the chunk is done
        lock.unlock();
        callback(cpu, d->callback, d->handle, d->message, d->argument);
        lock.lock();
    }
}

std::shared_ptr<Demuxer> demuxer_of(uint32_t handle) {
    const auto it = g_demuxers.find(handle);
    if (it == g_demuxers.end()) fatal("a demultiplexer %08x that was never opened", handle);
    return it->second;
}
std::shared_ptr<Es> es_of(uint32_t handle) {
    const auto it = g_es.find(handle);
    if (it == g_es.end()) fatal("a demultiplexer's stream %08x that was never enabled", handle);
    return it->second;
}

// cellDmuxQueryAttr(CellDmuxType*, CellDmuxAttr* {memory size, version upper, lower}): this demultiplexer keeps nothing
// in the game's memory but a few messages.
void cellDmuxQueryAttr(Cpu& c) {
    const uint32_t type = static_cast<uint32_t>(c.r[3]), attr = static_cast<uint32_t>(c.r[4]);
    if (load32(type) != 1) fatal("cellDmuxQueryAttr for stream type %u (only PAMF, 1, is taken)", load32(type));
    store32(attr, 0x10000), store32(attr + 4, 0), store32(attr + 8, 0);
    c.r[3] = kOk;
}
// cellDmuxOpen(CellDmuxType*, CellDmuxResource* {memory, size, ...}, CellDmuxCb* {function, argument}, uint32_t* handle)
void cellDmuxOpen(Cpu& c) {
    const uint32_t type = static_cast<uint32_t>(c.r[3]), resource = static_cast<uint32_t>(c.r[4]), cb = static_cast<uint32_t>(c.r[5]);
    if (load32(type) != 1) fatal("cellDmuxOpen for stream type %u (only PAMF, 1, is taken)", load32(type));
    auto d = std::make_shared<Demuxer>();
    d->callback = load32(cb), d->argument = load32(cb + 4);
    d->message = load32(resource);  // (16 bytes of the memory the game gave)
    const std::lock_guard lock(g_mutex);
    d->handle = g_next_handle++;
    g_demuxers[d->handle] = d;
    d->thread = std::thread(demuxer_thread, d);
    store32(static_cast<uint32_t>(c.r[6]), d->handle);
    c.r[3] = kOk;
}
// cellDmuxClose(handle)
void cellDmuxClose(Cpu& c) {
    std::shared_ptr<Demuxer> d;
    {
        const std::lock_guard lock(g_mutex);
        d = demuxer_of(static_cast<uint32_t>(c.r[3]));
        d->closing = true;
        d->changed.notify_all();
    }
    d->thread.join();
    const std::lock_guard lock(g_mutex);
    for (auto& [handle, e] : d->es) {
        av_parser_close(e->parser);
        avcodec_free_context(&e->parser_context);
        g_es.erase(handle);
    }
    g_demuxers.erase(d->handle);
    c.r[3] = kOk;
}
// cellDmuxSetStream(handle, address, size, discontinuity, user data): demultiplexed on the demultiplexer's thread.
void cellDmuxSetStream(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto d = demuxer_of(static_cast<uint32_t>(c.r[3]));
    d->chunks.emplace_back(static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(c.r[5]));
    d->changed.notify_all();
    c.r[3] = kOk;
}
// cellDmuxQueryEsAttr(type, filter, specific info, CellDmuxEsAttr* {memory size}): room for a few pictures.
void cellDmuxQueryEsAttr(Cpu& c) {
    store32(static_cast<uint32_t>(c.r[6]), 4u << 20);
    c.r[3] = kOk;
}
// cellDmuxEnableEs(handle, filter, CellDmuxEsResource* {memory, size}, CellDmuxEsCb* {function, argument}, specific info, uint32_t* es handle)
void cellDmuxEnableEs(Cpu& c) {
    const uint32_t filter = static_cast<uint32_t>(c.r[4]), resource = static_cast<uint32_t>(c.r[5]), cb = static_cast<uint32_t>(c.r[6]);
    auto e = std::make_shared<Es>();
    e->id = load32(filter), e->sub = load32(filter + 4);
    if (e->id == 0xbd) fatal("the game enables a film's sound stream, which this runtime doesn't take yet (ATRAC3plus)");
    if ((e->id & 0xf0) != 0xe0) fatal("cellDmuxEnableEs with filter %08x %08x, which isn't one of cellPamfStreamTypeToEsFilterId's", e->id, e->sub);
    e->memory = load32(resource), e->memory_size = load32(resource + 4);
    e->callback = load32(cb), e->argument = load32(cb + 4);
    // The stream's own records: its message and 64 access units' infos, from the runtime's memory.
    e->message = allocate(16 + 64 * 32, 16);
    e->info_slots = e->message + 16;
    e->parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    e->parser_context = avcodec_alloc_context3(nullptr);
    if (!e->parser || !e->parser_context) fatal("FFmpeg's MPEG video parser can't be made");
    const std::lock_guard lock(g_mutex);
    const auto d = demuxer_of(static_cast<uint32_t>(c.r[3]));
    e->handle = g_next_handle++;
    e->demuxer = d->handle;
    d->es[e->handle] = e;
    g_es[e->handle] = e;
    store32(static_cast<uint32_t>(c.r[8]), e->handle);
    c.r[3] = kOk;
}
// cellDmuxDisableEs(es), cellDmuxResetEs(es): its access units dropped (and with Disable, the stream).
void cellDmuxResetEs(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto e = es_of(static_cast<uint32_t>(c.r[3]));
    e->ready.clear(), e->taken.clear();
    av_parser_close(e->parser);
    e->parser = av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
    demuxer_of(e->demuxer)->changed.notify_all();
    c.r[3] = kOk;
}
void cellDmuxDisableEs(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto e = es_of(static_cast<uint32_t>(c.r[3]));
    const auto d = demuxer_of(e->demuxer);
    d->es.erase(e->handle);
    g_es.erase(e->handle);
    av_parser_close(e->parser);
    avcodec_free_context(&e->parser_context);
    d->changed.notify_all();
    c.r[3] = kOk;
}
// cellDmuxGetAu(es, CellDmuxAuInfo** info, void** specific info): the next access unit found, now the game's.
void cellDmuxGetAu(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto e = es_of(static_cast<uint32_t>(c.r[3]));
    if (e->ready.empty()) fatal("cellDmuxGetAu with no access unit found (the library's answer to that isn't known here)");
    const Au au = e->ready.front();
    e->ready.pop_front();
    e->taken.push_back(au);
    store32(static_cast<uint32_t>(c.r[4]), au.info);
    if (c.r[5]) store32(static_cast<uint32_t>(c.r[5]), 0);
    c.r[3] = kOk;
}
// cellDmuxReleaseAu(es): the oldest access unit got is done with; its room in the ring is free.
void cellDmuxReleaseAu(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto e = es_of(static_cast<uint32_t>(c.r[3]));
    if (e->taken.empty()) fatal("cellDmuxReleaseAu with no access unit got");
    e->taken.pop_front();
    const Au* next = !e->taken.empty() ? &e->taken.front() : !e->ready.empty() ? &e->ready.front() : nullptr;
    e->used_from = next ? next->address - e->memory : e->used_to;
    demuxer_of(e->demuxer)->changed.notify_all();
    c.r[3] = kOk;
}

// ---------------------------------------------------------------- libvdec

struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> y, u, v;  // 4:2:0, rows packed
};
struct Decoder {
    uint32_t handle = 0, callback = 0, argument = 0;
    uint32_t item = 0;  // the CellVdecPicItem and its CellVdecMpeg2Info, in the runtime's memory
    AVCodecContext* context = nullptr;
    std::deque<std::vector<uint8_t>> units;  // access units to decode
    std::deque<Picture> pictures;
    bool closing = false;
    std::condition_variable changed;
    std::thread thread;
};
std::map<uint32_t, std::shared_ptr<Decoder>> g_decoders;

void decoder_thread(std::shared_ptr<Decoder> d) {
    Cpu cpu;
    become_guest_thread(cpu, 0x10000, 0);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::unique_lock lock(g_mutex);
    for (;;) {
        d->changed.wait(lock, [&] { return d->closing || !d->units.empty(); });
        if (d->closing) break;
        std::vector<uint8_t> unit = std::move(d->units.front());
        d->units.pop_front();
        lock.unlock();
        packet->data = unit.data(), packet->size = static_cast<int>(unit.size());
        if (avcodec_send_packet(d->context, packet) < 0) fatal("FFmpeg's MPEG-2 decoder refuses a film's access unit");
        std::vector<Picture> out;
        while (avcodec_receive_frame(d->context, frame) == 0) {
            if (frame->format != AV_PIX_FMT_YUV420P) fatal("a film's picture in pixel format %d (4:2:0 is taken)", frame->format);
            Picture p{static_cast<uint32_t>(frame->width), static_cast<uint32_t>(frame->height), {}, {}, {}};
            const auto plane = [&](int k, uint32_t w, uint32_t h, std::vector<uint8_t>& to) {
                to.resize(size_t{w} * h);
                for (uint32_t row = 0; row < h; ++row) std::memcpy(to.data() + size_t{row} * w, frame->data[k] + size_t{row} * static_cast<size_t>(frame->linesize[k]), w);
            };
            plane(0, p.width, p.height, p.y);
            plane(1, (p.width + 1) / 2, (p.height + 1) / 2, p.u);
            plane(2, (p.width + 1) / 2, (p.height + 1) / 2, p.v);
            out.push_back(std::move(p));
        }
        callback(cpu, d->callback, d->handle, 0, 0, d->argument);  // the access unit is done
        for (Picture& p : out) {
            {
                const std::lock_guard picture_lock(g_mutex);
                d->pictures.push_back(std::move(p));
            }
            callback(cpu, d->callback, d->handle, 1, 0, d->argument);  // a picture out
        }
        lock.lock();
    }
    av_frame_free(&frame);
    av_packet_free(&packet);
}

std::shared_ptr<Decoder> decoder_of(uint32_t handle) {
    const auto it = g_decoders.find(handle);
    if (it == g_decoders.end()) fatal("a picture decoder %08x that was never opened", handle);
    return it->second;
}

// The codec types (the SDK's, *as recalled*: 0 MPEG-2): only MPEG-2 is taken, and anything else stops the game by number.
void check_codec(uint32_t type) {
    if (load32(type) != 0) fatal("the game opens a picture decoder for codec type %u (only MPEG-2, taken to be 0, is provided)", load32(type));
}
// cellVdecQueryAttr(CellVdecType* {codec, profile and level}, CellVdecAttr* {memory size, command depth, version upper, lower})
void cellVdecQueryAttr(Cpu& c) {
    check_codec(static_cast<uint32_t>(c.r[3]));
    const uint32_t attr = static_cast<uint32_t>(c.r[4]);
    store32(attr, 0x10000), store32(attr + 4, 4), store32(attr + 8, 0), store32(attr + 12, 0);
    c.r[3] = kOk;
}
// cellVdecOpen(type, CellVdecResource*, CellVdecCb* {function, argument}, uint32_t* handle)
void cellVdecOpen(Cpu& c) {
    check_codec(static_cast<uint32_t>(c.r[3]));
    const uint32_t cb = static_cast<uint32_t>(c.r[5]);
    auto d = std::make_shared<Decoder>();
    d->callback = load32(cb), d->argument = load32(cb + 4);
    d->item = allocate(256, 16);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    d->context = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!d->context || avcodec_open2(d->context, codec, nullptr) < 0) fatal("FFmpeg's MPEG-2 decoder can't be opened");
    const std::lock_guard lock(g_mutex);
    d->handle = g_next_handle++;
    g_decoders[d->handle] = d;
    d->thread = std::thread(decoder_thread, d);
    store32(static_cast<uint32_t>(c.r[6]), d->handle);
    c.r[3] = kOk;
}
// cellVdecStartSeq(handle)
void cellVdecStartSeq(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    decoder_of(static_cast<uint32_t>(c.r[3]));
    c.r[3] = kOk;
}
// cellVdecClose(handle)
void cellVdecClose(Cpu& c) {
    std::shared_ptr<Decoder> d;
    {
        const std::lock_guard lock(g_mutex);
        d = decoder_of(static_cast<uint32_t>(c.r[3]));
        d->closing = true;
        d->changed.notify_all();
    }
    d->thread.join();
    avcodec_free_context(&d->context);
    const std::lock_guard lock(g_mutex);
    g_decoders.erase(d->handle);
    c.r[3] = kOk;
}
// cellVdecDecodeAu(handle, mode, CellVdecAuInfo* {address, size, ...}): copied now (the game releases it once done).
void cellVdecDecodeAu(Cpu& c) {
    const uint32_t info = static_cast<uint32_t>(c.r[5]);
    if (c.r[4] != 0) fatal("cellVdecDecodeAu in mode %llu (only normal decoding, 0, is taken)", static_cast<unsigned long long>(c.r[4]));
    const uint32_t address = load32(info), size = load32(info + 4);
    const std::lock_guard lock(g_mutex);
    const auto d = decoder_of(static_cast<uint32_t>(c.r[3]));
    d->units.emplace_back(guest_memory() + address, guest_memory() + address + size);
    d->changed.notify_all();
    c.r[3] = kOk;
}
// cellVdecGetPicItem(handle, const CellVdecPicItem** item): the next picture's item.
void cellVdecGetPicItem(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const auto d = decoder_of(static_cast<uint32_t>(c.r[3]));
    if (d->pictures.empty()) {
        c.r[3] = kVdecEmpty;
        return;
    }
    const Picture& p = d->pictures.front();
    const uint32_t item = d->item, info = d->item + 128;
    std::memset(guest_memory() + item, 0, 256);
    store32(item + 8, p.width * p.height * 3 / 2);  // its size as 4:2:0
    store32(item + 12, 1);                          // from one access unit
    store32(item + 72, info);
    store16(info, static_cast<uint16_t>(p.width)), store16(info + 2, static_cast<uint16_t>(p.height));
    store32(static_cast<uint32_t>(c.r[4]), item);
    c.r[3] = kOk;
}
// cellVdecGetPicture(handle, const CellVdecPicFormat* {format, colour matrix, alpha}, buffer): the next picture, converted.
// YUV is taken as limited range; chroma is taken from the nearest sample (*a stated choice, for speed*).
void cellVdecGetPicture(Cpu& c) {
    const uint32_t format = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[5]);
    Picture p;
    {
        const std::lock_guard lock(g_mutex);
        const auto d = decoder_of(static_cast<uint32_t>(c.r[3]));
        if (d->pictures.empty()) {
            c.r[3] = kVdecEmpty;
            return;
        }
        p = std::move(d->pictures.front());
        d->pictures.pop_front();
    }
    c.r[3] = kOk;
    if (!out) return;  // dropped
    const uint32_t type = load32(format), matrix = load32(format + 4);
    const uint8_t alpha = guest_memory()[format + 8];
    if (type == 3) {
        // YUV 4:2:0 planar: Y, then U, then V, rows packed (*U before V as I420 has them: stated, to be held to the picture*).
        uint8_t* to = guest_memory() + out;
        std::memcpy(to, p.y.data(), p.y.size());
        std::memcpy(to + p.y.size(), p.u.data(), p.u.size());
        std::memcpy(to + p.y.size() + p.u.size(), p.v.data(), p.v.size());
        return;
    }
    if (type != 0) fatal("cellVdecGetPicture in format %u into a buffer (ARGB, 0, and YUV 4:2:0 planar, 3, are taken)", type);
    if (matrix > 1) fatal("cellVdecGetPicture with colour matrix %u", matrix);
    // Fixed point, 16 bits of fraction: BT.601 (0) or BT.709 (1), limited range.
    const int32_t ry = 76309, rv = matrix ? 117489 : 104597, gu = matrix ? -13975 : -25675, gv = matrix ? -34925 : -53279, bu = matrix ? 138438 : 132201;
    uint8_t* to = guest_memory() + out;
    const auto clamp = [](int32_t v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    const uint32_t cw = (p.width + 1) / 2;
    for (uint32_t y = 0; y < p.height; ++y)
        for (uint32_t x = 0; x < p.width; ++x) {
            const int32_t Y = (p.y[size_t{y} * p.width + x] - 16) * ry, U = p.u[size_t{y / 2} * cw + x / 2] - 128, V = p.v[size_t{y / 2} * cw + x / 2] - 128;
            uint8_t* q = to + (size_t{y} * p.width + x) * 4;
            q[0] = alpha;
            q[1] = clamp((Y + rv * V + 32768) >> 16);
            q[2] = clamp((Y + gu * U + gv * V + 32768) >> 16);
            q[3] = clamp((Y + bu * U + 32768) >> 16);
        }
}

constexpr NativeImport kImports[] = {
    {"cellPamf", "cellPamfGetHeaderSize", cellPamfGetHeaderSize},
    {"cellPamf", "cellPamfGetStreamOffsetAndSize", cellPamfGetStreamOffsetAndSize},
    {"cellPamf", "cellPamfReaderInitialize", cellPamfReaderInitialize},
    {"cellPamf", "cellPamfReaderGetPresentationStartTime", cellPamfReaderGetPresentationStartTime},
    {"cellPamf", "cellPamfReaderGetPresentationEndTime", cellPamfReaderGetPresentationEndTime},
    {"cellPamf", "cellPamfReaderGetNumberOfSpecificStreams", cellPamfReaderGetNumberOfSpecificStreams},
    {"cellPamf", "cellPamfReaderSetStreamWithTypeAndIndex", cellPamfReaderSetStreamWithTypeAndIndex},
    {"cellPamf", "cellPamfReaderGetStreamTypeAndChannel", cellPamfReaderGetStreamTypeAndChannel},
    {"cellPamf", "cellPamfStreamTypeToEsFilterId", cellPamfStreamTypeToEsFilterId},
    {"cellPamf", "cellPamfReaderGetStreamInfo", cellPamfReaderGetStreamInfo},
    {"cellDmux", "cellDmuxQueryAttr", cellDmuxQueryAttr},
    {"cellDmux", "cellDmuxOpen", cellDmuxOpen},
    {"cellDmux", "cellDmuxClose", cellDmuxClose},
    {"cellDmux", "cellDmuxSetStream", cellDmuxSetStream},
    {"cellDmux", "cellDmuxQueryEsAttr", cellDmuxQueryEsAttr},
    {"cellDmux", "cellDmuxEnableEs", cellDmuxEnableEs},
    {"cellDmux", "cellDmuxDisableEs", cellDmuxDisableEs},
    {"cellDmux", "cellDmuxResetEs", cellDmuxResetEs},
    {"cellDmux", "cellDmuxGetAu", cellDmuxGetAu},
    {"cellDmux", "cellDmuxReleaseAu", cellDmuxReleaseAu},
    {"libvdec", "cellVdecQueryAttr", cellVdecQueryAttr},
    {"libvdec", "cellVdecOpen", cellVdecOpen},
    {"libvdec", "cellVdecStartSeq", cellVdecStartSeq},
    {"libvdec", "cellVdecClose", cellVdecClose},
    {"libvdec", "cellVdecDecodeAu", cellVdecDecodeAu},
    {"libvdec", "cellVdecGetPicItem", cellVdecGetPicItem},
    {"libvdec", "cellVdecGetPicture", cellVdecGetPicture},
};

}  // namespace

std::span<const NativeImport> film_imports() { return kImports; }

}  // namespace rr7
