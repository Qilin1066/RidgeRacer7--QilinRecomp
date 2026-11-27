// cellAtrac: the system's streaming ATRAC3plus decoder, which the game uses for its music (BGM.nub), with FFmpeg's decoder
// (cmake/ffmpeg.cmake).
//
// What the game uses of it, read off its music thread (0x3988c0 on) and its reader (0x3984c4):
//   - cellAtracSetDataAndGetMemSize(handle, buffer, buffer size, bytes read, &work memory size) (the game's stream keeps the
//     buffer's size at +12 and adds what its reader copies to +60 before the decoder exists), retried after its reader has
//     read more while it fails, then CreateDecoder; the
//     channels, the most samples a decode gives (it makes three output buffers of channels * that * 4 bytes), the second
//     buffer if one is needed, and the loop's information (with a loop, it sets the loop count to -1: for ever);
//   - Decode(handle, out, &samples, &finished, &frames left) in turn into the three buffers: 0x80610332 is "no data in the
//     buffer yet" (it waits for its reader), 0x80610331 "everything decoded";
//   - GetStreamDataInfo(handle, &where to write, &how much may be, &the file position to read from): the reader copies what
//     it has read there if it fits, and calls AddStreamData with the count.
// The data is a RIFF/WAVE file: "fmt " (extensible, ATRAC3plus; channels, rate, the frame's size), "fact" (the samples,
// then two numbers), "smpl" (a loop's first and last samples), then "data", raw frames.
//
// The game's buffer is a ring of the file as it is read, from the file's start (what was read before the decoder was set
// up). After a loop's end the reader is sent back to the frame before the loop's start (decoded and thrown away, so the
// decoder's overlap is right), as many times as the loop count says.
// *Stated readings:* the decoder's delay is fact's last number, and the loop's samples count from after it; the output is
// interleaved big-endian floats in the system's channel order (L, C, R, Ls, Rs, LFE, as cellAdec's: film.cpp); no second
// buffer is ever needed, since the ring takes the loop's start again.
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace rr7 {

namespace {

using qilin::rt::fatal;

constexpr uint64_t kAllDecoded = 0x80610331u, kNoData = 0x80610332u;
constexpr uint32_t kSamplesPerFrame = 2048;

uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// FFmpeg's objects, freed with their owner.
struct FreeContext {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
struct FreePacket {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
struct FreeFrame {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct Atrac {
    uint32_t buffer = 0, buffer_size = 0;
    bool on_memory = false;  // the whole file is in the buffer, not streamed
    uint32_t data_offset = 0, data_size = 0, frame_bytes = 0, channels = 0, rate = 0;
    uint64_t file_size = 0;
    uint32_t total = 0, delay = 0;                    // samples
    bool has_loop = false;
    uint32_t loop_first = 0, loop_last = 0;           // samples, from after the delay
    int32_t loop_num = 0;                             // -1 for ever
    // The stream: positions in it (the bytes read, in the order read) and in the file.
    uint64_t delivered = 0, consumed = 0;             // stream positions: what the reader has added, what is decoded
    uint64_t read_at = 0, decode_at = 0;              // file positions: the reader's next, the decoder's next frame
    int32_t read_loops = 0, decode_loops = 0;         // loops still to make, by each side
    bool decode_first_pass = true;
    // The decoder, and what each frame goes through on its way in and out: the frame's bytes are followed by the zeroes
    // FFmpeg may read past a packet's end (AV_INPUT_BUFFER_PADDING_SIZE).
    std::unique_ptr<AVCodecContext, FreeContext> context;
    std::unique_ptr<AVPacket, FreePacket> packet;
    std::unique_ptr<AVFrame, FreeFrame> decoded;
    std::vector<uint8_t> frame;
};
std::mutex g_mutex;
std::map<uint32_t, Atrac> g_atracs;  // by the game's CellAtracHandle

Atrac& atrac_of(uint32_t handle) {
    const auto it = g_atracs.find(handle);
    if (it == g_atracs.end()) fatal("an ATRAC decoder %08x that was never set up", handle);
    return it->second;
}

// The first frame of the loop's start, and the end of the frame holding its last sample, in the file.
uint64_t loop_restart(const Atrac& a) {
    const uint32_t frame = (a.delay + a.loop_first) / kSamplesPerFrame;
    return a.data_offset + uint64_t{frame ? frame - 1 : 0} * a.frame_bytes;  // (one frame before, to prime the decoder)
}
uint64_t pass_end(const Atrac& a, int32_t loops_left) {
    if (a.has_loop && loops_left != 0) return a.data_offset + uint64_t{(a.delay + a.loop_last) / kSamplesPerFrame + 1} * a.frame_bytes;
    return a.file_size;
}

// cellAtracSetDataAndGetMemSize(handle, buffer, buffer size, bytes read, uint32_t* work memory)
void cellAtracSetDataAndGetMemSize(Cpu& c) {
    const uint32_t handle = static_cast<uint32_t>(c.r[3]), buffer = static_cast<uint32_t>(c.r[4]), size = static_cast<uint32_t>(c.r[5]),
                   read = static_cast<uint32_t>(c.r[6]), work = static_cast<uint32_t>(c.r[7]);
    if (read > size) fatal("cellAtracSetDataAndGetMemSize: %u bytes read into a buffer of %u", read, size);
    const uint8_t* p = guest_memory() + buffer;
    if (read < 12 || std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) fatal("cellAtracSetDataAndGetMemSize: no RIFF/WAVE file at %08x", buffer);
    Atrac a;
    a.buffer = buffer, a.buffer_size = size;
    uint32_t at = 12;
    bool format = false;
    while (at + 8 <= read) {
        const uint8_t* chunk = p + at;
        const uint32_t length = le32(chunk + 4);
        if (std::memcmp(chunk, "fmt ", 4) == 0) {
            if (le16(chunk + 8) != 0xfffe) fatal("an ATRAC file of format %#x (only extensible ATRAC3plus is taken)", le16(chunk + 8));
            a.channels = le16(chunk + 10), a.rate = le32(chunk + 12), a.frame_bytes = le16(chunk + 20);
            format = true;
        } else if (std::memcmp(chunk, "fact", 4) == 0) {
            a.total = le32(chunk + 8);
            a.delay = le32(chunk + 8 + length - 4);
        } else if (std::memcmp(chunk, "smpl", 4) == 0) {
            if (le32(chunk + 8 + 28) > 1) fatal("an ATRAC file with %u loops", le32(chunk + 8 + 28));
            if (le32(chunk + 8 + 28) == 1) a.has_loop = true, a.loop_first = le32(chunk + 8 + 36 + 8), a.loop_last = le32(chunk + 8 + 36 + 12);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            a.data_offset = at + 8, a.data_size = length;
            break;
        }
        at += 8 + length + (length & 1);
    }
    if (!format || !a.data_offset) {
        // Not enough read yet to see the whole header: the game reads more and asks again (*the error, "read size too
        // small", as recalled: the game only looks for one*).
        c.r[3] = 0x80610313u;
        return;
    }
    if (!a.frame_bytes || !a.total) fatal("cellAtracSetDataAndGetMemSize: an ATRAC file with no frame size or samples");
    a.file_size = uint64_t{a.data_offset} + a.data_size;
    a.on_memory = read >= a.file_size;
    if (!a.on_memory && size < a.frame_bytes) fatal("an ATRAC stream through a buffer of %u bytes, less than a frame of %u", size, a.frame_bytes);
    a.delivered = a.read_at = read;
    a.consumed = a.decode_at = a.data_offset;
    store32(work, 0x10000);
    const std::lock_guard lock(g_mutex);
    g_atracs.insert_or_assign(handle, std::move(a));  // (a decoder it had before is freed)
    c.r[3] = kOk;
}

// cellAtracCreateDecoder(handle, work memory, PPU priority, SPU priority)
void cellAtracCreateDecoder(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_ATRAC3P);
    a.context.reset(codec ? avcodec_alloc_context3(codec) : nullptr);
    a.packet.reset(av_packet_alloc());
    a.decoded.reset(av_frame_alloc());
    if (!a.context || !a.packet || !a.decoded) fatal("FFmpeg's ATRAC3plus decoder can't be made");
    a.context->sample_rate = static_cast<int>(a.rate);
    av_channel_layout_default(&a.context->ch_layout, static_cast<int>(a.channels));
    a.context->block_align = static_cast<int>(a.frame_bytes);
    if (avcodec_open2(a.context.get(), codec, nullptr) < 0) fatal("FFmpeg's ATRAC3plus decoder can't be opened");
    a.frame.assign(size_t{a.frame_bytes} + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    c.r[3] = kOk;
}
// cellAtracDeleteDecoder(handle)
void cellAtracDeleteDecoder(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    atrac_of(static_cast<uint32_t>(c.r[3]));
    g_atracs.erase(static_cast<uint32_t>(c.r[3]));  // (its decoder with it)
    c.r[3] = kOk;
}

void cellAtracGetChannel(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    store32(static_cast<uint32_t>(c.r[4]), atrac_of(static_cast<uint32_t>(c.r[3])).channels);
    c.r[3] = kOk;
}
void cellAtracGetMaxSample(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    atrac_of(static_cast<uint32_t>(c.r[3]));
    store32(static_cast<uint32_t>(c.r[4]), kSamplesPerFrame);
    c.r[3] = kOk;
}
// cellAtracIsSecondBufferNeeded(handle): never (above).
void cellAtracIsSecondBufferNeeded(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    atrac_of(static_cast<uint32_t>(c.r[3]));
    c.r[3] = 0;
}
void cellAtracGetSecondBufferInfo(Cpu&) { fatal("cellAtracGetSecondBufferInfo, though no second buffer is needed"); }
void cellAtracSetSecondBuffer(Cpu&) { fatal("cellAtracSetSecondBuffer, though no second buffer is needed"); }
// cellAtracGetLoopInfo(handle, int* loop count, uint32_t* status): the count set, and 1 if the file has a loop (the game
// then sets the count).
void cellAtracGetLoopInfo(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    store32(static_cast<uint32_t>(c.r[4]), static_cast<uint32_t>(a.loop_num));
    store32(static_cast<uint32_t>(c.r[5]), a.has_loop ? 1 : 0);
    c.r[3] = kOk;
}
// cellAtracSetLoopNum(handle, count): -1 for ever.
void cellAtracSetLoopNum(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    a.loop_num = static_cast<int32_t>(c.r[4]);
    a.read_loops = a.decode_loops = a.loop_num;
    c.r[3] = kOk;
}
void cellAtracGetInternalErrorInfo(Cpu& c) {
    store32(static_cast<uint32_t>(c.r[4]), 0);
    c.r[3] = kOk;
}

// cellAtracGetStreamDataInfo(handle, uint8_t** where, uint32_t* how much, uint32_t* the file position to read)
void cellAtracGetStreamDataInfo(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    const Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    uint32_t writable = 0;
    if (!a.on_memory && a.read_at < a.file_size) {
        const uint64_t contiguous = a.buffer_size - a.delivered % a.buffer_size, room = a.buffer_size - (a.delivered - a.consumed),
                       pass = pass_end(a, a.read_loops) - a.read_at;
        writable = static_cast<uint32_t>(std::min({contiguous, room, pass}));
    }
    store32(static_cast<uint32_t>(c.r[4]), a.buffer + static_cast<uint32_t>(a.on_memory ? 0 : a.delivered % a.buffer_size));
    store32(static_cast<uint32_t>(c.r[5]), writable);
    store32(static_cast<uint32_t>(c.r[6]), static_cast<uint32_t>(a.read_at));
    c.r[3] = kOk;
}
// cellAtracAddStreamData(handle, bytes): the reader has written them where it was told.
void cellAtracAddStreamData(Cpu& c) {
    const uint32_t bytes = static_cast<uint32_t>(c.r[4]);
    const std::lock_guard lock(g_mutex);
    Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    if (a.on_memory) fatal("cellAtracAddStreamData to a decoder whose data is all in memory");
    if (a.delivered + bytes - a.consumed > a.buffer_size || a.read_at + bytes > pass_end(a, a.read_loops))
        fatal("cellAtracAddStreamData of %u bytes, more than it was told it could: delivered %llu, decoded to %llu, buffer %u, reading at %llu, the pass ends at %llu, the data at %u",
              bytes, static_cast<unsigned long long>(a.delivered), static_cast<unsigned long long>(a.consumed), a.buffer_size, static_cast<unsigned long long>(a.read_at),
              static_cast<unsigned long long>(pass_end(a, a.read_loops)), a.data_offset);
    a.delivered += bytes, a.read_at += bytes;
    if (a.read_at == pass_end(a, a.read_loops) && a.has_loop && a.read_loops != 0) {
        a.read_at = loop_restart(a);
        if (a.read_loops > 0) --a.read_loops;
    }
    c.r[3] = kOk;
}

// cellAtracDecode(handle, float* out, uint32_t* samples, uint32_t* finished, int32_t* frames left): one frame.
void cellAtracDecode(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[4]), samples_at = static_cast<uint32_t>(c.r[5]), finished_at = static_cast<uint32_t>(c.r[6]),
                   left_at = static_cast<uint32_t>(c.r[7]);
    const std::lock_guard lock(g_mutex);
    Atrac& a = atrac_of(static_cast<uint32_t>(c.r[3]));
    if (!a.context) fatal("cellAtracDecode before cellAtracCreateDecoder");
    store32(samples_at, 0);
    const uint64_t end = pass_end(a, a.decode_loops);
    if (a.decode_at >= a.file_size || (a.decode_at >= end && !(a.has_loop && a.decode_loops != 0))) {
        store32(finished_at, 1);
        c.r[3] = kAllDecoded;
        return;
    }
    // The frame, from the buffer: at its file position when all is in memory, else at its place in the ring.
    uint8_t* const frame = a.frame.data();
    if (a.on_memory) {
        std::memcpy(frame, guest_memory() + a.buffer + a.decode_at, a.frame_bytes);
    } else {
        if (a.delivered - a.consumed < a.frame_bytes) {
            c.r[3] = kNoData;
            return;
        }
        // Stream positions are file positions, and the frames start after the header, which isn't a whole number of frames:
        // so a frame can run over the ring's end and go on at its start.
        const uint32_t at = static_cast<uint32_t>(a.consumed % a.buffer_size);
        const uint32_t first_part = std::min(a.frame_bytes, a.buffer_size - at);
        std::memcpy(frame, guest_memory() + a.buffer + at, first_part);
        std::memcpy(frame + first_part, guest_memory() + a.buffer, a.frame_bytes - first_part);
        a.consumed += a.frame_bytes;
    }
    // The samples this frame holds, in the decoded stream (with the delay), and those this pass keeps.
    const uint64_t first = (a.decode_at - a.data_offset) / a.frame_bytes * kSamplesPerFrame;
    const uint64_t keep_from = a.delay + (a.decode_first_pass ? 0 : a.loop_first);
    const uint64_t keep_to = a.delay + ((a.has_loop && a.decode_loops != 0) ? uint64_t{a.loop_last} + 1 : a.total);
    AVPacket* const packet = a.packet.get();
    AVFrame* const decoded = a.decoded.get();
    packet->data = frame, packet->size = static_cast<int>(a.frame_bytes);
    if (avcodec_send_packet(a.context.get(), packet) < 0) fatal("FFmpeg's ATRAC3plus decoder refuses a frame of the music");
    uint32_t written = 0;
    while (avcodec_receive_frame(a.context.get(), decoded) == 0) {
        if (decoded->format != AV_SAMPLE_FMT_FLTP) fatal("FFmpeg's ATRAC3plus decoder gives sample format %d", decoded->format);
        const int channels = decoded->ch_layout.nb_channels;
        int from[8];
        for (int k = 0; k < channels; ++k) from[k] = k;
        if (channels == 6) from[1] = 2, from[2] = 1;
        else if (channels > 2) fatal("music of %d channels, whose order isn't known here", channels);
        for (int i = 0; i < decoded->nb_samples; ++i) {
            const uint64_t t = first + static_cast<uint64_t>(i);
            if (t < keep_from || t >= keep_to) continue;
            for (int k = 0; k < channels; ++k) {
                const uint32_t v = std::bit_cast<uint32_t>(reinterpret_cast<const float*>(decoded->extended_data[from[k]])[i]);
                store32(out + (written * static_cast<uint32_t>(channels) + static_cast<uint32_t>(k)) * 4, v);
            }
            ++written;
        }
    }
    // On to the next frame: back to the loop's start after its end, while loops are left.
    a.decode_at += a.frame_bytes;
    if (a.decode_at == end && a.has_loop && a.decode_loops != 0) {
        a.decode_at = loop_restart(a);
        a.decode_first_pass = false;
        if (a.decode_loops > 0) --a.decode_loops;
    }
    store32(samples_at, written);
    store32(finished_at, 0);
    store32(left_at, a.on_memory ? 0xffffffffu : static_cast<uint32_t>((a.delivered - a.consumed) / a.frame_bytes));
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellAtrac", "cellAtracSetDataAndGetMemSize", cellAtracSetDataAndGetMemSize},
    {"cellAtrac", "cellAtracCreateDecoder", cellAtracCreateDecoder},
    {"cellAtrac", "cellAtracDeleteDecoder", cellAtracDeleteDecoder},
    {"cellAtrac", "cellAtracGetChannel", cellAtracGetChannel},
    {"cellAtrac", "cellAtracGetMaxSample", cellAtracGetMaxSample},
    {"cellAtrac", "cellAtracIsSecondBufferNeeded", cellAtracIsSecondBufferNeeded},
    {"cellAtrac", "cellAtracGetSecondBufferInfo", cellAtracGetSecondBufferInfo},
    {"cellAtrac", "cellAtracSetSecondBuffer", cellAtracSetSecondBuffer},
    {"cellAtrac", "cellAtracGetLoopInfo", cellAtracGetLoopInfo},
    {"cellAtrac", "cellAtracSetLoopNum", cellAtracSetLoopNum},
    {"cellAtrac", "cellAtracGetInternalErrorInfo", cellAtracGetInternalErrorInfo},
    {"cellAtrac", "cellAtracGetStreamDataInfo", cellAtracGetStreamDataInfo},
    {"cellAtrac", "cellAtracAddStreamData", cellAtracAddStreamData},
    {"cellAtrac", "cellAtracDecode", cellAtracDecode},
};

}  // namespace

std::span<const NativeImport> atrac_imports() { return kImports; }

}  // namespace rr7
