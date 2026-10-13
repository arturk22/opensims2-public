#include "app/game_cursors.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace os2 {

namespace {

uint16_t u16(std::span<const uint8_t> d, size_t at) { return uint16_t(d[at] | d[at + 1] << 8); }
uint32_t u32(std::span<const uint8_t> d, size_t at) { return uint32_t(u16(d, at)) | uint32_t(u16(d, at + 2)) << 16; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

} // namespace

std::optional<CursorImage> decodeCursor(std::span<const uint8_t> d) {
    if (d.size() < 6 || u16(d, 0) != 0 || (u16(d, 2) != 2 && u16(d, 2) != 1))
        return std::nullopt;
    const int count = u16(d, 4);
    // The entry with the most colours, then the largest.
    struct Pick {
        int w, h, hx, hy, bpp;
        size_t off;
    };
    std::optional<Pick> best;
    for (int i = 0; i < count; ++i) {
        const size_t e = 6 + 16 * size_t(i);
        if (e + 16 > d.size())
            return std::nullopt;
        const uint32_t off = u32(d, e + 12);
        if (off + 40 > d.size() || u32(d, off) < 40)
            continue;
        Pick p{d[e] ? d[e] : 256, d[e + 1] ? d[e + 1] : 256, u16(d, e + 4), u16(d, e + 6), int(u16(d, off + 14)), off};
        p.w = int(int32_t(u32(d, off + 4))); // the bitmap's own size wins
        p.h = int(int32_t(u32(d, off + 8))) / 2;
        if (p.w <= 0 || p.h <= 0 || p.w > 256 || p.h > 256)
            continue;
        if (!best || p.bpp > best->bpp || (p.bpp == best->bpp && p.w * p.h > best->w * best->h))
            best = p;
    }
    if (!best)
        return std::nullopt;
    const Pick& p = *best;
    if (p.bpp != 1 && p.bpp != 4 && p.bpp != 8 && p.bpp != 24 && p.bpp != 32)
        return std::nullopt;
    const size_t header = u32(d, p.off);
    uint32_t colours = u32(d, p.off + 32);
    if (p.bpp <= 8 && (colours == 0 || colours > (1u << p.bpp)))
        colours = 1u << p.bpp;
    if (p.bpp > 8)
        colours = 0;
    const size_t palette = p.off + header;
    const size_t stride = size_t((p.w * p.bpp + 31) / 32) * 4, maskStride = size_t((p.w + 31) / 32) * 4;
    const size_t xorAt = palette + 4 * size_t(colours), andAt = xorAt + stride * size_t(p.h);
    if (andAt + maskStride * size_t(p.h) > d.size())
        return std::nullopt;

    CursorImage img;
    img.width = p.w, img.height = p.h;
    img.hotX = std::clamp(p.hx, 0, p.w - 1), img.hotY = std::clamp(p.hy, 0, p.h - 1);
    img.rgba.assign(size_t(p.w) * size_t(p.h) * 4, 0);
    for (int y = 0; y < p.h; ++y) {
        const size_t row = size_t(p.h - 1 - y); // bottom-up
        for (int x = 0; x < p.w; ++x) {
            uint8_t* out = &img.rgba[(size_t(y) * size_t(p.w) + size_t(x)) * 4];
            const bool masked = d[andAt + row * maskStride + size_t(x / 8)] >> (7 - x % 8) & 1;
            uint8_t b = 0, g = 0, r = 0, a = 255;
            const size_t px = xorAt + row * stride;
            if (p.bpp <= 8) {
                uint32_t idx = 0;
                if (p.bpp == 8)
                    idx = d[px + size_t(x)];
                else if (p.bpp == 4)
                    idx = x % 2 ? d[px + size_t(x / 2)] & 15 : d[px + size_t(x / 2)] >> 4;
                else
                    idx = d[px + size_t(x / 8)] >> (7 - x % 8) & 1;
                if (idx >= colours)
                    idx = 0;
                b = d[palette + 4 * idx], g = d[palette + 4 * idx + 1], r = d[palette + 4 * idx + 2];
                if (masked) {
                    // Screen XOR colour: black keeps the screen (transparent);
                    // a 1-bit white inverts it (drawn black here).
                    if (p.bpp == 1 && idx != 0)
                        r = g = b = 0;
                    else
                        a = 0;
                }
            } else {
                const size_t q = px + size_t(x) * size_t(p.bpp / 8);
                b = d[q], g = d[q + 1], r = d[q + 2];
                if (p.bpp == 32)
                    a = d[q + 3];
                if (masked)
                    a = 0;
            }
            if (a == 0)
                r = g = b = 0;
            out[0] = r, out[1] = g, out[2] = b, out[3] = a;
        }
    }
    return img;
}

namespace {
struct CursorEntry {
    const char* name;
    int id;
};
// The order of GameCursor: the name of the file and the id in SetupCursors.
constexpr CursorEntry kCursors[] = {
    {"arrow", 1},           {"hourglass", 0xF},        {"simsmove", 0x19},         {"simsplace", 0x1A},
    {"simsrotate", 0x1B},   {"eyedropperTool", 0x38},  {"designTool", 0x39},       {"railingtool", 0x3A},
    {"simsdelete", 0x15},   {"simslevel", 0x16},       {"simslower", 0x17},        {"simsraise", 0x18},
    {"simsrotatenw", 0x1C}, {"simsrotatene", 0x1D},    {"simsrotatesw", 0x1E},     {"simsrotatese", 0x1F},
    {"left", 0x20},         {"right", 0x21},           {"up", 0x22},               {"down", 0x23},
    {"upright", 0x24},      {"upleft", 0x25},          {"downright", 0x26},        {"downleft", 0x27},
    {"pan", 0x2A},          {"livenothing", 0x2B},     {"liveperson", 0x2C},       {"liveobjectavail", 0x2D},
    {"liveobjectunavail", 0x2E}, {"movein", 0x30},     {"bulldoze", 0x31},         {"evictFamily", 0x32},
    {"moveLotToCatalog", 0x34},
};
static_assert(sizeof(kCursors) / sizeof(kCursors[0]) == size_t(GameCursor::Count), "one entry per GameCursor");
} // namespace

const char* gameCursorName(GameCursor c) {
    return size_t(c) < size_t(GameCursor::Count) ? kCursors[size_t(c)].name : "arrow";
}

int gameCursorId(GameCursor c) { return size_t(c) < size_t(GameCursor::Count) ? kCursors[size_t(c)].id : 1; }

std::filesystem::path findCursorFile(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        files.push_back(e.path());
    for (const char* suffix : {"_8.cur", "_1.cur", ".cur"}) {
        const std::string want = lower(name + suffix);
        for (const auto& f : files)
            if (lower(f.filename().string()) == want)
                return f;
    }
    return {};
}

namespace {
GameCursor edgeCursor(int x, int y) {
    if (x < 0)
        return y > 0 ? GameCursor::ScrollUpLeft : y < 0 ? GameCursor::ScrollDownLeft : GameCursor::ScrollLeft;
    if (x > 0)
        return y > 0 ? GameCursor::ScrollUpRight : y < 0 ? GameCursor::ScrollDownRight : GameCursor::ScrollRight;
    return y > 0 ? GameCursor::ScrollUp : GameCursor::ScrollDown;
}
GameCursor frontCursor(int front) {
    switch (front) {
    case 0: return GameCursor::RotateNW;
    case 1: return GameCursor::RotateSW;
    case 2: return GameCursor::RotateSE;
    case 3: return GameCursor::RotateNE;
    }
    return GameCursor::Rotate;
}
} // namespace

GameCursor chooseCursor(const CursorContext& c) {
    using Ctx = CursorContext;
    if (c.loading)
        return GameCursor::Hourglass;
    if (c.overUi)
        return GameCursor::Arrow;
    if (c.panning)
        return GameCursor::Pan;
    if (c.scrollX || c.scrollY)
        return edgeCursor(c.scrollX, c.scrollY);
    switch (c.mode) {
    case Ctx::Mode::Buy:
        if (c.holding) // the tutorial's picture (step 20): the open hand over the held chair
            return !c.pressing ? GameCursor::Move : c.turned ? GameCursor::Rotate : GameCursor::Place;
        switch (c.buyTool) {
        case Ctx::BuyTool::Eyedropper: return GameCursor::Eyedropper;
        case Ctx::BuyTool::Design: return GameCursor::Design;
        case Ctx::BuyTool::Hand: return c.overObject ? GameCursor::Move : GameCursor::Arrow;
        }
        break;
    case Ctx::Mode::Build:
        switch (c.buildTool) {
        case Ctx::BuildTool::Design: return GameCursor::Design;
        case Ctx::BuildTool::Railing: return GameCursor::Railing;
        case Ctx::BuildTool::Raise: return GameCursor::Raise;
        case Ctx::BuildTool::Lower: return GameCursor::Lower;
        case Ctx::BuildTool::Level: return GameCursor::Level;
        case Ctx::BuildTool::Roof: return c.buildDragging ? GameCursor::Rotate : GameCursor::Arrow;
        case Ctx::BuildTool::Object: return c.holding ? GameCursor::Move : GameCursor::Arrow;
        case Ctx::BuildTool::Other: break;
        }
        break;
    case Ctx::Mode::Live:
        switch (c.liveHover) {
        case Ctx::LiveHover::Person: return GameCursor::LivePerson;
        case Ctx::LiveHover::Object: return GameCursor::LiveObjectAvail;
        case Ctx::LiveHover::ObjectUnavailable: return GameCursor::LiveObjectUnavail;
        case Ctx::LiveHover::None: return GameCursor::LiveNothing;
        }
        break;
    case Ctx::Mode::Hood:
        if (c.lotInHand) // RotateLot: the front the drag chose
            return c.lotPressed ? frontCursor(c.lotFront) : GameCursor::Arrow;
        switch (c.hoodTool) {
        case Ctx::HoodTool::MoveLot:
            return !c.overLot ? GameCursor::Arrow : c.lotPressed ? GameCursor::Place : GameCursor::Move;
        case Ctx::HoodTool::Decorate: return GameCursor::Move;
        case Ctx::HoodTool::Eyedropper: return GameCursor::Eyedropper;
        case Ctx::HoodTool::MoveIn: return c.overLot ? GameCursor::MoveIn : GameCursor::Arrow;
        case Ctx::HoodTool::Evict: return c.overLot ? GameCursor::EvictFamily : GameCursor::Arrow;
        case Ctx::HoodTool::Bulldoze: return c.overLot ? GameCursor::Bulldoze : GameCursor::Arrow;
        case Ctx::HoodTool::ToBin: return c.overLot ? GameCursor::MoveLotToCatalog : GameCursor::Arrow;
        case Ctx::HoodTool::None: break;
        }
        break;
    case Ctx::Mode::Other: break;
    }
    return GameCursor::Arrow;
}

} // namespace os2
