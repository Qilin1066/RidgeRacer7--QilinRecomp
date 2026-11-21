// cellFont and cellFontFT: the system's font library, which opens fonts
// (the console's own sets, or one the game hands it in memory) and draws
// their glyphs for the game to copy into its own buffers.
//
// The console's library is built on FreeType, and so is this (fetched and
// pinned by CMake). The console's own fonts are the player's to supply, as
// the game is: the `data/font` folder of a PS3's system software
// (docs/runtime.md says how to get it). Nothing stands in for them: a font
// set that is asked for and not there stops the game.
//
// **Stated choices, to be compared with the console's output (RPCS3):**
// glyphs are loaded unhinted; "scale pixel" is taken as the em's size in
// pixels; a rendered glyph is placed on whole pixels.
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "kernel.hpp"
#include "qilin/rt/report.hpp"

namespace rr7 {

namespace {

using qilin::rt::fatal;

// The library's errors (the SDK's CELL_FONT_ERROR_...).
constexpr uint64_t kInvalidParameter = 0x80540002u, kUninitialised = 0x80540003u, kAlreadyInitialised = 0x80540004u, kRendererUnbound = 0x80540021u;

constexpr uint32_t kFontMagic = 0x51464e54u, kRendererMagic = 0x5146524eu;  // what marks the game's structures as holding one of ours

struct Font {
    std::vector<std::vector<uint8_t>> files;  // a set's fonts, as read (FreeType reads from these)
    std::vector<FT_Face> faces;               // in the order a glyph is looked for
    float scale_w = 0, scale_h = 0;
    uint32_t renderer = 0;  // bound to this renderer, or 0
};
struct Renderer {
    uint32_t image = 0, capacity = 0;  // where a rendered glyph is left for the game, in guest memory
};

std::mutex g_mutex;  // FreeType's library object is used by one thread at a time
bool g_initialised = false;
uint32_t g_library = 0;  // the rasteriser's handle, in guest memory: the game holds a pointer to it
FT_Library g_freetype = nullptr;
std::map<uint32_t, Font> g_fonts;
std::map<uint32_t, Renderer> g_renderers;
uint32_t g_next_id = 1;

// The font sets this runtime knows, by the SDK's number for them, and the firmware's files each is made of.
struct FontSet {
    uint32_t type;
    const char* files[4];
};
constexpr FontSet kFontSets[] = {
    {0xffffffffu, {nullptr}},  // (none yet: each is added when the game asks for it)
};

float float_at(uint32_t address) { return std::bit_cast<float>(load32(address)); }
void store_float(uint32_t address, float value) { store32(address, std::bit_cast<uint32_t>(value)); }

Font* font_of(uint32_t structure) {
    if (!structure || load32(structure) != kFontMagic) return nullptr;
    const auto it = g_fonts.find(load32(structure + 4));
    return it == g_fonts.end() ? nullptr : &it->second;
}
Renderer* renderer_of(uint32_t structure) {
    if (!structure || load32(structure) != kRendererMagic) return nullptr;
    const auto it = g_renderers.find(load32(structure + 4));
    return it == g_renderers.end() ? nullptr : &it->second;
}

void apply_scale(Font& font) {
    for (const FT_Face face : font.faces)
        if (FT_Set_Char_Size(face, std::lround(font.scale_w * 64), std::lround(font.scale_h * 64), 72, 72))
            fatal("the font library can't set a font to %g by %g pixels", static_cast<double>(font.scale_w), static_cast<double>(font.scale_h));
}

// The face of the font that has this character, with its glyph loaded; null if none has it.
FT_Face load_glyph(Font& font, uint32_t code, bool render) {
    for (const FT_Face face : font.faces) {
        const FT_UInt index = FT_Get_Char_Index(face, code);
        if (!index) continue;
        if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) fatal("the font library can't load the glyph of character U+%04X", code);
        if (render && FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) fatal("the font library can't draw the glyph of character U+%04X", code);
        return face;
    }
    return nullptr;
}

// CellFontGlyphMetrics: width, height; then bearing x, bearing y and advance, horizontal and vertical.
void store_metrics(uint32_t out, const FT_Glyph_Metrics& m) {
    const float values[] = {m.width / 64.0f,        m.height / 64.0f,       m.horiBearingX / 64.0f, m.horiBearingY / 64.0f,
                            m.horiAdvance / 64.0f, m.vertBearingX / 64.0f, m.vertBearingY / 64.0f, m.vertAdvance / 64.0f};
    for (uint32_t i = 0; i < 8; ++i) store_float(out + 4 * i, values[i]);
}

// cellFontInitializeWithRevision(revision, CellFontConfig* config)
// The configuration: a buffer for the library's file cache and its size, how many fonts the game may
// have open at once and the memory for them, and flags.
void cellFontInitializeWithRevision(Cpu& c) {
    const uint32_t config = static_cast<uint32_t>(c.r[4]);
    if (!config) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (g_initialised) {
        c.r[3] = kAlreadyInitialised;
        return;
    }
    if (FT_Init_FreeType(&g_freetype)) fatal("FreeType wouldn't start");
    g_initialised = true;
    c.r[3] = kOk;
}

// cellFontInitLibraryFreeTypeWithRevision(revision, CellFontLibraryConfigFT* config, const CellFontLibrary** library)
// Makes the rasteriser the library draws with, and hands back a handle to it.
void cellFontInitLibraryFreeTypeWithRevision(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[5]);
    if (!c.r[4] || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const std::lock_guard lock(g_mutex);
    if (!g_initialised) {
        c.r[3] = kUninitialised;
        return;
    }
    if (!g_library) g_library = allocate(16, 16);
    store32(out, g_library);
    c.r[3] = kOk;
}

// cellFontCreateRenderer(library, CellFontRendererConfig* config, CellFontRenderer* renderer)
// A renderer is what draws glyphs and holds the last one drawn. (The configuration is its buffer's
// sizes on a console; here the buffer is the runtime's and grows as glyphs need.)
void cellFontCreateRenderer(Cpu& c) {
    const uint32_t out = static_cast<uint32_t>(c.r[5]);
    if (!c.r[3] || !c.r[4] || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const std::lock_guard lock(g_mutex);
    const uint32_t id = g_next_id++;
    g_renderers.emplace(id, Renderer{});
    store32(out, kRendererMagic);
    store32(out + 4, id);
    c.r[3] = kOk;
}

// cellFontDestroyRenderer(renderer)
void cellFontDestroyRenderer(Cpu& c) {
    const uint32_t structure = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    if (!renderer_of(structure)) {
        c.r[3] = kInvalidParameter;
        return;
    }
    g_renderers.erase(load32(structure + 4));
    store32(structure, 0);
    c.r[3] = kOk;
}

uint32_t add_font(uint32_t structure, Font font) {
    const uint32_t id = g_next_id++;
    g_fonts.emplace(id, std::move(font));
    store32(structure, kFontMagic);
    store32(structure + 4, id);
    return id;
}

// cellFontOpenFontset(library, CellFontType* {type, map}, CellFont* font): one of the console's own sets of fonts.
void cellFontOpenFontset(Cpu& c) {
    const uint32_t type_at = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[5]);
    if (!c.r[3] || !type_at || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const uint32_t type = load32(type_at), map = load32(type_at + 4);
    const FontSet* set = nullptr;
    for (const FontSet& s : kFontSets)
        if (s.type == type) set = &s;
    if (!set) fatal("the game opens the console's font set %#x (map %#x), which this runtime doesn't know yet", type, map);
    if (map != 0) fatal("the game opens font set %#x with character map %#x, which this runtime doesn't know (only Unicode, 0)", type, map);
    if (game().flash.empty())
        fatal("the game needs the console's own fonts, and no system software folder was given or found: start with --flash <dev_flash> (docs/runtime.md says how to get one)");
    const std::lock_guard lock(g_mutex);
    Font font;
    for (const char* name : set->files) {
        if (!name) break;
        const std::filesystem::path path = game().flash / "data" / "font" / name;
        std::FILE* file = std::fopen(path.string().c_str(), "rb");
        if (!file) fatal("%s is not there: the console's fonts are needed whole", path.string().c_str());
        std::vector<uint8_t> bytes;
        uint8_t block[65536];
        for (size_t n; (n = std::fread(block, 1, sizeof block, file)) > 0;) bytes.insert(bytes.end(), block, block + n);
        std::fclose(file);
        font.files.push_back(std::move(bytes));
    }
    for (const auto& bytes : font.files) {
        FT_Face face;
        if (FT_New_Memory_Face(g_freetype, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &face)) fatal("FreeType can't read one of the fonts of set %#x", type);
        font.faces.push_back(face);
    }
    add_font(out, std::move(font));
    c.r[3] = kOk;
}

// cellFontOpenFontMemory(library, address, size, sub-font number, unique id, CellFont* font): a font file the game holds in memory.
// (It is read where it is, as on the console: the game keeps it there while the font is open.)
void cellFontOpenFontMemory(Cpu& c) {
    const uint32_t address = static_cast<uint32_t>(c.r[4]), size = static_cast<uint32_t>(c.r[5]), sub = static_cast<uint32_t>(c.r[6]), out = static_cast<uint32_t>(c.r[8]);
    if (!c.r[3] || !address || !size || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const std::lock_guard lock(g_mutex);
    FT_Face face;
    if (FT_New_Memory_Face(g_freetype, guest_memory() + address, static_cast<FT_Long>(size), static_cast<FT_Long>(sub), &face))
        fatal("FreeType can't read the font the game holds at %08x (%u bytes, sub-font %u)", address, size, sub);
    Font font;
    font.faces.push_back(face);
    add_font(out, std::move(font));
    c.r[3] = kOk;
}

// cellFontCloseFont(font)
void cellFontCloseFont(Cpu& c) {
    const uint32_t structure = static_cast<uint32_t>(c.r[3]);
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(structure);
    if (!font) {
        c.r[3] = kInvalidParameter;
        return;
    }
    for (const FT_Face face : font->faces) FT_Done_Face(face);
    g_fonts.erase(load32(structure + 4));
    store32(structure, 0);
    c.r[3] = kOk;
}

// cellFontSetScalePixel(font, float width, float height)
void cellFontSetScalePixel(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    if (!font) {
        c.r[3] = kInvalidParameter;
        return;
    }
    font->scale_w = static_cast<float>(c.f[1]);
    font->scale_h = static_cast<float>(c.f[2]);
    apply_scale(*font);
    c.r[3] = kOk;
}

// cellFontBindRenderer(font, renderer), cellFontUnbindRenderer(font)
void cellFontBindRenderer(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    const uint32_t renderer = static_cast<uint32_t>(c.r[4]);
    if (!font || !renderer_of(renderer)) {
        c.r[3] = kInvalidParameter;
        return;
    }
    font->renderer = load32(renderer + 4);
    c.r[3] = kOk;
}
void cellFontUnbindRenderer(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    if (!font) {
        c.r[3] = kInvalidParameter;
        return;
    }
    font->renderer = 0;
    c.r[3] = kOk;
}

// cellFontRenderSurfaceInit(CellFontRenderSurface* surface, buffer, bytes to a row, bytes to a pixel, width, height)
// The surface: buffer, bytes to a row, bytes to a pixel, width, height, then the scissor's x0, y0, x1, y1.
void cellFontRenderSurfaceInit(Cpu& c) {
    const uint32_t surface = static_cast<uint32_t>(c.r[3]);
    for (uint32_t i = 0; i < 5; ++i) store32(surface + 4 * i, static_cast<uint32_t>(c.r[4 + i]));
    store32(surface + 20, 0);
    store32(surface + 24, 0);
    store32(surface + 28, static_cast<uint32_t>(c.r[7]));
    store32(surface + 32, static_cast<uint32_t>(c.r[8]));
}

// cellFontRenderSurfaceSetScissor(surface, x0, y0, width, height): kept within the surface.
void cellFontRenderSurfaceSetScissor(Cpu& c) {
    const uint32_t surface = static_cast<uint32_t>(c.r[3]);
    const int32_t width = static_cast<int32_t>(load32(surface + 12)), height = static_cast<int32_t>(load32(surface + 16));
    const int32_t x0 = std::clamp(static_cast<int32_t>(c.r[4]), 0, width), y0 = std::clamp(static_cast<int32_t>(c.r[5]), 0, height);
    const int32_t x1 = std::clamp(x0 + static_cast<int32_t>(c.r[6]), x0, width), y1 = std::clamp(y0 + static_cast<int32_t>(c.r[7]), y0, height);
    store32(surface + 20, static_cast<uint32_t>(x0));
    store32(surface + 24, static_cast<uint32_t>(y0));
    store32(surface + 28, static_cast<uint32_t>(x1));
    store32(surface + 32, static_cast<uint32_t>(y1));
}

// cellFontGetHorizontalLayout(font, CellFontHorizontalLayout* {baseline's y, line height, effect height})
void cellFontGetHorizontalLayout(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    const uint32_t out = static_cast<uint32_t>(c.r[4]);
    if (!font || font->faces.empty() || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    const FT_Size_Metrics& m = font->faces[0]->size->metrics;
    store_float(out, m.ascender / 64.0f);
    store_float(out + 4, m.height / 64.0f);
    store_float(out + 8, (m.ascender - m.descender) / 64.0f);
    c.r[3] = kOk;
}

// cellFontGetCharGlyphMetrics(font, character, CellFontGlyphMetrics* metrics): the glyph's outline, at the font's scale.
// cellFontGetRenderCharGlyphMetrics: the same of the glyph as the bound renderer would draw it.
void glyph_metrics(Cpu& c, bool rendered) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    const uint32_t code = static_cast<uint32_t>(c.r[4]), out = static_cast<uint32_t>(c.r[5]);
    if (!font || !out) {
        c.r[3] = kInvalidParameter;
        return;
    }
    if (rendered && !font->renderer) {
        c.r[3] = kRendererUnbound;
        return;
    }
    const FT_Face face = load_glyph(*font, code, false);
    if (!face) fatal("the game asks the font library for character U+%04X, which the font doesn't have: the library's answer to that isn't known here", code);
    store_metrics(out, face->glyph->metrics);
    c.r[3] = kOk;
}
void cellFontGetCharGlyphMetrics(Cpu& c) { glyph_metrics(c, false); }
void cellFontGetRenderCharGlyphMetrics(Cpu& c) { glyph_metrics(c, true); }

// cellFontRenderCharGlyphImage(font, character, surface, float x, float y, CellFontGlyphMetrics* metrics, CellFontImageTransInfo* info)
//
// Draws the glyph with its origin at (x, y) and leaves it with the renderer, one byte of coverage to
// a pixel. It does not write to the surface: `info` tells the game what to copy where, already cut to
// the surface's scissor: the image's address, bytes to a row, width and height, then the address in
// the surface of its first pixel and the surface's bytes to a row. (Read off the game's own copying loop.)
void cellFontRenderCharGlyphImage(Cpu& c) {
    const std::lock_guard lock(g_mutex);
    Font* font = font_of(static_cast<uint32_t>(c.r[3]));
    const uint32_t code = static_cast<uint32_t>(c.r[4]), surface = static_cast<uint32_t>(c.r[5]), metrics = static_cast<uint32_t>(c.r[8]),
                   info = static_cast<uint32_t>(c.r[9]);
    const float x = static_cast<float>(c.f[1]), y = static_cast<float>(c.f[2]);
    if (!font || !surface) {
        c.r[3] = kInvalidParameter;
        return;
    }
    if (!font->renderer) {
        c.r[3] = kRendererUnbound;
        return;
    }
    Renderer& renderer = g_renderers[font->renderer];
    const FT_Face face = load_glyph(*font, code, true);
    if (!face) fatal("the game has the font library draw character U+%04X, which the font doesn't have: the library's answer to that isn't known here", code);
    const FT_GlyphSlot glyph = face->glyph;
    if (metrics) store_metrics(metrics, glyph->metrics);
    if (!info) {
        c.r[3] = kOk;
        return;
    }
    const FT_Bitmap& bitmap = glyph->bitmap;
    const uint32_t width = bitmap.width, height = bitmap.rows;
    // The image, in guest memory, rows packed.
    if (width * height > renderer.capacity) {
        renderer.capacity = std::max<uint32_t>(width * height, 0x4000);
        renderer.image = allocate(renderer.capacity, 16);
    }
    for (uint32_t row = 0; row < height; ++row) std::memcpy(guest_memory() + renderer.image + row * width, bitmap.buffer + static_cast<ptrdiff_t>(row) * bitmap.pitch, width);
    // Where it goes, and what the scissor leaves of it.
    const int32_t left = static_cast<int32_t>(std::floor(x)) + glyph->bitmap_left, top = static_cast<int32_t>(std::floor(y)) - glyph->bitmap_top;
    const int32_t x0 = static_cast<int32_t>(load32(surface + 20)), y0 = static_cast<int32_t>(load32(surface + 24)), x1 = static_cast<int32_t>(load32(surface + 28)),
                  y1 = static_cast<int32_t>(load32(surface + 32));
    const int32_t cut_left = std::max(left, x0), cut_top = std::max(top, y0);
    const int32_t cut_right = std::min(left + static_cast<int32_t>(width), x1), cut_bottom = std::min(top + static_cast<int32_t>(height), y1);
    const uint32_t row_bytes = load32(surface + 4), pixel_bytes = load32(surface + 8);
    if (cut_right <= cut_left || cut_bottom <= cut_top) {
        for (uint32_t i = 0; i < 6; ++i) store32(info + 4 * i, 0);
        store32(info, renderer.image);
        store32(info + 4, width);
        store32(info + 16, load32(surface));
        store32(info + 20, row_bytes);
    } else {
        store32(info, renderer.image + static_cast<uint32_t>(cut_top - top) * width + static_cast<uint32_t>(cut_left - left));
        store32(info + 4, width);
        store32(info + 8, static_cast<uint32_t>(cut_right - cut_left));
        store32(info + 12, static_cast<uint32_t>(cut_bottom - cut_top));
        store32(info + 16, load32(surface) + static_cast<uint32_t>(cut_top) * row_bytes + static_cast<uint32_t>(cut_left) * pixel_bytes);
        store32(info + 20, row_bytes);
    }
    c.r[3] = kOk;
}

constexpr NativeImport kImports[] = {
    {"cellFont", "cellFontInitializeWithRevision", cellFontInitializeWithRevision},
    {"cellFontFT", "cellFontInitLibraryFreeTypeWithRevision", cellFontInitLibraryFreeTypeWithRevision},
    {"cellFont", "cellFontCreateRenderer", cellFontCreateRenderer},
    {"cellFont", "cellFontDestroyRenderer", cellFontDestroyRenderer},
    {"cellFont", "cellFontOpenFontset", cellFontOpenFontset},
    {"cellFont", "cellFontOpenFontMemory", cellFontOpenFontMemory},
    {"cellFont", "cellFontCloseFont", cellFontCloseFont},
    {"cellFont", "cellFontSetScalePixel", cellFontSetScalePixel},
    {"cellFont", "cellFontBindRenderer", cellFontBindRenderer},
    {"cellFont", "cellFontUnbindRenderer", cellFontUnbindRenderer},
    {"cellFont", "cellFontRenderSurfaceInit", cellFontRenderSurfaceInit},
    {"cellFont", "cellFontRenderSurfaceSetScissor", cellFontRenderSurfaceSetScissor},
    {"cellFont", "cellFontGetHorizontalLayout", cellFontGetHorizontalLayout},
    {"cellFont", "cellFontGetCharGlyphMetrics", cellFontGetCharGlyphMetrics},
    {"cellFont", "cellFontGetRenderCharGlyphMetrics", cellFontGetRenderCharGlyphMetrics},
    {"cellFont", "cellFontRenderCharGlyphImage", cellFontRenderCharGlyphImage},
};

}  // namespace

std::span<const NativeImport> font_imports() { return kImports; }

}  // namespace rr7
