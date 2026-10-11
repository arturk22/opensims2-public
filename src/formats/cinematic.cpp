#include "formats/cinematic.hpp"

#include "formats/binreader.hpp"
#include "formats/rcol.hpp"

namespace os2 {

namespace {
std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos)
            end = s.size();
        if (end > start)
            out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}
} // namespace

CinematicScene CinematicScene::parse(std::span<const uint8_t> resource) {
    const Rcol rc(resource);
    const Rcol::Block* b = rc.find("cCinematicScene");
    if (!b)
        throw FormatError("no cCinematicScene block");
    BinReader r(b->body);
    CinematicScene s;
    s.version = r.u32();
    s.name = SgResource::read(r).fileName;
    s.cameraAnimation = r.str7();
    if (s.version == 3) { // two strings the game reads and drops
        r.str7();
        r.str7();
    }
    const uint32_t count = r.u32();
    if (count > 64)
        throw FormatError("cinematic with " + std::to_string(count) + " actors");
    for (uint32_t i = 0; i < count; ++i) {
        Actor a;
        a.name = r.str7();
        a.required = r.u8() != 0;
        if (s.version >= 3) {
            a.sgObjectName = r.str7();
            a.renderGroups = r.u32();
            a.autoCreate = r.u8() != 0;
            a.autoOrigin = r.u8() != 0;
            a.autoModel = r.str7();
            a.autoAnimations = splitList(r.str7());
        }
        s.actors.push_back(std::move(a));
    }
    s.originActor = r.u32();
    if (s.version > 1)
        s.sequencedAnimations = r.u32();
    return s;
}

std::string CinematicScene::cameraAnimationResource() const {
    return cameraAnimation.ends_with("_anim") ? cameraAnimation : cameraAnimation + "_anim";
}

} // namespace os2
