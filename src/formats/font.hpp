#pragma once

#include "image.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace os2 {

struct FontError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// .mxf (TSData/Res/UI/Fonts): an outline font behind a small header,
//   "MXFN", u32 0x005A6202 (same in every file), u8 key,
// then the font file XORed with the key (0x9D in every file), padded with
// junk to a multiple of 10000 bytes. Inside is either a TrueType font (the
// Asian and the symbol fonts) or a PFB Type 1 font (ITC Benguiat Gothic Bold
// and Helvetica Neue Medium, the PT-BR UI fonts). The game draws them with
// Bitstream's T2K, which reads both. Throws FontError.
std::vector<uint8_t> decodeMxf(std::span<const uint8_t> data);

// A glyph outline in font units (y up): contours of lines, quadratic
// (TrueType) and cubic (Type 1) Béziers.
struct GlyphPath {
    enum class Op : uint8_t { Move, Line, Quad, Cubic, Close };
    struct Cmd {
        Op op;
        float x = 0, y = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0; // end point; control points for Quad (x1) and Cubic (x1, x2)
    };
    std::vector<Cmd> cmds;

    void moveTo(float x, float y) { cmds.push_back({Op::Move, x, y}); }
    void lineTo(float x, float y) { cmds.push_back({Op::Line, x, y}); }
    void quadTo(float cx, float cy, float x, float y) { cmds.push_back({Op::Quad, x, y, cx, cy}); }
    void cubicTo(float c1x, float c1y, float c2x, float c2y, float x, float y) {
        cmds.push_back({Op::Cubic, x, y, c1x, c1y, c2x, c2y});
    }
    void close() { cmds.push_back({Op::Close}); }
};

class Font {
public:
    virtual ~Font() = default;

    // TrueType ("\0\1\0\0"/"true") or Type 1 (PFB segments or PFA text).
    static std::unique_ptr<Font> load(std::span<const uint8_t> fontFile);
    // An .mxf file, or a plain font file.
    static std::unique_ptr<Font> loadFile(const std::filesystem::path& path);

    std::string name;          // PostScript or full name
    float unitsPerEm = 1000;
    float ascent = 800, descent = -200; // font units; descent negative
    size_t glyphCount = 0;

    // Glyph for a Unicode code point (Type 1: through its Encoding, which
    // the game's fonts lay out as Windows-1252).
    virtual std::optional<uint32_t> glyph(char32_t codepoint) const = 0;
    virtual float advance(uint32_t glyph) const = 0; // font units
    virtual GlyphPath outline(uint32_t glyph) const = 0;
};

// Antialiased coverage of one glyph at `pixelsPerEm`.
struct GlyphBitmap {
    int width = 0, height = 0;
    int left = 0, top = 0; // bitmap's top-left relative to the pen on the baseline (top: pixels above it)
    float advance = 0;     // pixels
    std::vector<uint8_t> alpha;
};
// Non-zero-ish coverage (absolute accumulated area, clamped) of the outline
// scaled to `pixelsPerEm`, shifted right by `dx` pixels (subpixel position).
GlyphBitmap rasterizeGlyph(const Font& font, uint32_t glyph, float pixelsPerEm, float dx = 0);

// Decodes UTF-8 (invalid bytes become U+FFFD).
std::u32string decodeUtf8(std::string_view s);

// Single-line text layout and rendering.
struct TextMetrics {
    float width = 0;          // advance sum, pixels
    float ascent = 0, descent = 0; // pixels, descent positive
    size_t missing = 0;       // code points without a glyph
};
TextMetrics measureText(const Font& font, std::u32string_view text, float pixelsPerEm, float charSpacing = 0);
// Draws `text` with its pen starting at (x, baseline) in `img`, blending
// `rgba` over the existing pixels by the glyph coverage. Returns the pen's end x.
float drawText(RgbaImage& img, const Font& font, std::u32string_view text, float x, float baseline, float pixelsPerEm,
               const uint8_t rgba[4], float charSpacing = 0);

// FontStyle.ini ([Font Styles]): name = "face[, fallback]", "size", "flags", GUID.
struct FontStyle {
    std::string name;
    std::vector<std::string> faces;
    int size = 11;
    bool bold = false, italic = false;
    std::string flags; // raw, e.g. "bold|aa=bg"
    uint32_t guid = 0;
};
std::vector<FontStyle> parseFontStyles(std::string_view ini);

// The game's fonts directory: styles from FontStyle.ini and faces loaded on
// demand from "<face>.mxf" or "<face> (bold).mxf".
class FontLibrary {
public:
    explicit FontLibrary(const std::filesystem::path& fontsDir);

    const std::vector<FontStyle>& styles() const { return styles_; }
    // By name (case-insensitive) or GUID ("0x8d20fc99"); null if unknown.
    const FontStyle* style(std::string_view nameOrGuid) const;
    // First face of the style that loads; null if none.
    const Font* font(const FontStyle& style);
    const Font* face(const std::string& face, bool bold);

private:
    std::filesystem::path dir_;
    std::vector<FontStyle> styles_;
    std::map<std::string, std::unique_ptr<Font>> faces_; // null if it failed
};

} // namespace os2
