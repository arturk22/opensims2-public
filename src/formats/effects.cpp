#include "formats/effects.hpp"

#include "formats/binreader.hpp"

#include <algorithm>
#include <cctype>

namespace os2::fx {

namespace {

std::string str32(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > 4096)
        throw FormatError("effects: string too long");
    auto b = r.bytes(n);
    return std::string(reinterpret_cast<const char*>(b.data()), n);
}
V2 v2(BinReader& r) { return {r.f32(), r.f32()}; }
V3 v3(BinReader& r) { return {r.f32(), r.f32(), r.f32()}; }
Box box(BinReader& r) { return {v3(r), v3(r)}; }
Curve curve(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > 4096)
        throw FormatError("effects: curve too long");
    Curve c(n);
    for (auto& x : c)
        x = r.f32();
    return c;
}
std::vector<V3> vectors(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > 4096)
        throw FormatError("effects: vector list too long");
    std::vector<V3> v(n);
    for (auto& x : v)
        x = v3(r);
    return v;
}
std::vector<Wiggle> wiggles(BinReader& r) {
    std::vector<Wiggle> w(r.u32());
    for (auto& x : w)
        x = {r.f32(), v3(r), v3(r)};
    return w;
}
// Collider records: flags, name, three vectors, four floats, two names (the
// base game has no optional byte/float that OpenTS2 reads for later
// versions). Only the first name is kept.
std::vector<ColliderInfo> colliders(BinReader& r) {
    const uint32_t n = r.u32();
    if (n > 256)
        throw FormatError("effects: too many colliders");
    std::vector<ColliderInfo> out(n);
    for (auto& c : out) {
        c.flags = r.u32();
        c.name = str32(r);
        for (auto& v : c.v)
            v = v3(r);
        for (auto& f : c.f)
            f = r.f32();
        c.effect1 = str32(r);
        c.effect2 = str32(r);
    }
    return out;
}

struct Emission {
    V2 rateDelay, rateTrigger;
    Box dir;
    V2 speed;
    Box vol;
    float torus = -1;
    Curve rate;
    float rateTime = 0;
    uint32_t cycles = 0;
};
Emission emission(BinReader& r, bool torus, bool cycles32) {
    Emission e;
    e.rateDelay = v2(r);
    e.rateTrigger = v2(r);
    e.dir = box(r);
    e.speed = v2(r);
    e.vol = box(r);
    if (torus)
        e.torus = r.f32();
    e.rate = curve(r);
    e.rateTime = r.f32();
    e.cycles = cycles32 ? r.u32() : r.u16();
    return e;
}

Particles readParticles(BinReader& r) {
    Particles p;
    p.version = r.u16();
    if (p.version >= 5)
        throw FormatError("effects: particle version " + std::to_string(p.version) + " not supported");
    p.flags = r.u32();
    p.life = v2(r);
    p.preRoll = r.f32();
    auto e = emission(r, p.version > 2, false);
    p.rateDelay = e.rateDelay;
    p.rateTrigger = e.rateTrigger;
    p.emitDirection = e.dir;
    p.emitSpeed = e.speed;
    p.emitVolume = e.vol;
    p.torusWidth = e.torus;
    p.rateCurve = std::move(e.rate);
    p.rateCurveTime = e.rateTime;
    p.rateCurveCycles = e.cycles;
    p.rateSpeedScale = r.f32();
    p.sizeCurve = curve(r);
    p.sizeVary = r.f32();
    p.aspectCurve = curve(r);
    p.aspectVary = r.f32();
    p.rotateAxis = v3(r);
    v3(r);
    p.rotateCount = r.u8();
    for (auto& range : p.rotateRange) { // (version < 6: one float each)
        const float x = r.f32();
        range = x > 0 ? V2{-x, x} : V2{1, 1};
    }
    p.rotateOffset[0] = r.f32();
    p.rotateOffset[1] = r.f32();
    p.rotateCurveX = curve(r);
    p.rotateCurveY = curve(r);
    p.alphaCurve = curve(r);
    p.alphaVary = r.f32();
    p.colors = vectors(r);
    p.colorVary = v3(r);
    p.material = str32(r);
    p.tileU = r.u8();
    p.tileV = r.u8();
    p.alignment = r.u8();
    const uint8_t draw = r.u8();
    if (draw > 7)
        throw FormatError("effects: bad draw type " + std::to_string(draw));
    p.drawType = DrawType(draw);
    p.layer = r.f32();
    p.frameSpeed = r.f32();
    p.frameStart = r.u8();
    p.frameCount = r.u8();
    p.force = v3(r);
    p.globalWind = r.f32();
    p.bomb = r.f32();
    p.bombCenter = v3(r);
    p.drag = r.f32();
    p.stretch = r.f32();
    p.screw = r.f32();
    p.wiggles = wiggles(r);
    for (auto& b : p.bloom)
        b = r.u8();
    p.colliders = colliders(r);
    p.colliderName = p.colliders.empty() ? std::string() : p.colliders[0].name;
    str32(r);
    p.terrainBounce = r.f32();
    r.skip(4 * 4);
    p.killHeight = r.f32();
    r.skip(2 * 4);
    v2(r);
    p.randomWalkDelay = v2(r);
    p.randomWalkStrength = v2(r);
    p.randomWalkTurnX = r.f32();
    p.randomWalkTurnY = r.f32();
    // version < 8: attractor (vector, strength curve, range)
    p.attractor = v3(r);
    p.attractorStrength = curve(r);
    p.attractorRange = r.f32();
    return p;
}

MetaParticles readMeta(BinReader& r) {
    MetaParticles m;
    m.version = r.u16();
    if (m.version >= 3)
        throw FormatError("effects: meta particle version " + std::to_string(m.version) + " not supported");
    // std::bitset<34>: the first u32 holds bits 32-33, the second bits 0-31.
    const uint32_t high = r.u32(), low = r.u32();
    m.flags = low | (uint64_t(high & 3) << 32);
    m.life = v2(r);
    m.preRoll = r.f32();
    auto e = emission(r, m.version > 1, true);
    m.rateDelay = e.rateDelay;
    m.rateTrigger = e.rateTrigger;
    m.emitDirection = e.dir;
    m.emitSpeed = e.speed;
    m.emitVolume = e.vol;
    m.torusWidth = e.torus;
    m.rateCurve = std::move(e.rate);
    m.rateCurveTime = e.rateTime;
    m.rateCurveCycles = e.cycles;
    m.sizeCurve = curve(r);
    m.sizeVary = r.f32();
    m.aspectCurve = curve(r);
    m.aspectVary = r.f32();
    m.rotateCurve = curve(r);
    m.rotateVary = r.f32();
    m.rotateOffset = r.f32();
    m.colors = vectors(r);
    v3(r);
    m.alphaCurve = curve(r);
    r.f32();
    m.baseEffect = str32(r);
    m.alignment = r.u8();
    m.force = v3(r);
    m.globalWind = r.f32();
    m.bomb = r.f32();
    m.bombCenter = v3(r);
    m.drag = r.f32();
    m.screw = r.f32();
    m.wiggles = wiggles(r);
    r.skip(4);  // bloom
    m.colliders = colliders(r);
    m.colliderName = m.colliders.empty() ? std::string() : m.colliders[0].name;
    r.skip(8 * 4); // terrain
    v2(r);
    for (auto& w : m.walks) {
        w.delay = v2(r);
        w.strength = v2(r);
        for (float& t : w.turn)
            t = r.f32();
    }
    m.walkPreferDirection = v3(r);
    m.alignDamp = r.f32();
    m.alignBankWind = r.f32();
    m.alignBank = r.f32();
    m.attractor = v3(r);
    m.attractorStrength = curve(r);
    m.attractorRange = r.f32();
    const uint32_t points = r.u32();
    if (points > 4096)
        throw FormatError("effects: too many path points");
    m.path.resize(points);
    for (auto& t : m.path)
        t = {v3(r), v3(r), r.f32()};
    m.tractorReset = r.f32();
    return m;
}

Decal readDecal(BinReader& r) {
    if (r.u16() != 1)
        throw FormatError("effects: decal version");
    Decal d;
    r.u32();
    d.texture = str32(r);
    d.drawType = r.u8();
    d.lifeType = r.u8();
    d.life = r.f32();
    d.rotateCurve = curve(r);
    d.sizeCurve = curve(r);
    d.alphaCurve = curve(r);
    d.colors = vectors(r);
    d.aspectCurve = curve(r);
    r.skip(3 * 4);
    d.textureRepeat = r.f32();
    d.textureOffset = v2(r);
    return d;
}

Sequence readSequence(BinReader& r) {
    if (r.u16() != 1)
        throw FormatError("effects: sequence version");
    Sequence s;
    s.items.resize(r.u32());
    for (auto& it : s.items) {
        it.activateTime = v2(r);
        it.effect = str32(r);
    }
    s.flags = r.u32();
    return s;
}

void readCamera(BinReader& r) {
    if (r.u16() != 1)
        throw FormatError("effects: camera version");
    r.skip(4 + 4 + 4);
    curve(r);
    curve(r);
    r.skip(4 + 1);
    for (int i = 0; i < 7; ++i)
        curve(r);
    r.skip(2 + 4);
    str32(r);
}

void readWater(BinReader& r) {
    if (r.u16() != 1)
        throw FormatError("effects: water version");
    const uint32_t flags = r.u32();
    r.skip(flags == 0 ? 8 : 1 + 1 + 2 + 16);
    r.f32();
}

Component readComponent(BinReader& r, uint16_t version) {
    Component c;
    c.name = str32(r);
    c.type = Block(r.u8());
    c.flags = r.u32();
    c.transformFlags = r.u8();
    c.scale = r.f32();
    for (auto& x : c.rotation)
        x = r.f32();
    c.offset = v3(r);
    c.lodBegin = r.u8();
    c.lodEnd = r.u8();
    c.emitScale = v2(r);
    c.sizeScale = v2(r);
    c.alphaScale = v2(r);
    if (version < 3)
        r.skip(4);
    c.selectionGroup = r.u16();
    c.selectionChance = r.u16();
    c.timeScale = r.f32();
    c.index = r.i32();
    return c;
}

std::string lower(std::string_view s) {
    std::string o(s);
    for (auto& ch : o)
        ch = char(std::tolower(uint8_t(ch)));
    return o;
}

template <class T, class F> std::vector<T> list(BinReader& r, F read) {
    const uint32_t n = r.u32();
    if (n > 100000)
        throw FormatError("effects: block count too large");
    std::vector<T> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const size_t at = r.pos();
        try {
            v.push_back(read(r));
        } catch (const FormatError& e) {
            throw FormatError(std::string(e.what()) + " (block " + std::to_string(i) + " at " + std::to_string(at) + ")");
        }
    }
    return v;
}

} // namespace

float sampleCurve(const Curve& c, float t, float fallback) {
    if (c.empty())
        return fallback;
    if (c.size() == 1)
        return c[0];
    t = std::clamp(t, 0.0f, 1.0f) * float(c.size() - 1);
    const size_t i = std::min(size_t(t), c.size() - 2);
    const float f = t - float(i);
    return c[i] + (c[i + 1] - c[i]) * f;
}

V3 sampleColors(const std::vector<V3>& c, float t) {
    if (c.empty())
        return {1, 1, 1};
    if (c.size() == 1)
        return c[0];
    t = std::clamp(t, 0.0f, 1.0f) * float(c.size() - 1);
    const size_t i = std::min(size_t(t), c.size() - 2);
    const float f = t - float(i);
    V3 o;
    for (int k = 0; k < 3; ++k)
        o[k] = c[i][k] + (c[i + 1][k] - c[i][k]) * f;
    return o;
}

EffectsLibrary EffectsLibrary::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    if (r.u16() != 0)
        throw FormatError("effects: unknown version");
    EffectsLibrary lib;
    lib.particles = list<Particles>(r, readParticles);
    lib.metaParticles = list<MetaParticles>(r, readMeta);
    lib.decals = list<Decal>(r, readDecal);
    if (r.u32() != 0 || r.u32() != 0)
        throw FormatError("effects: brush/scrubber blocks present");
    lib.sequences = list<Sequence>(r, readSequence);
    lib.sounds = list<Sound>(r, [](BinReader& r) {
        if (r.u16() != 1)
            throw FormatError("effects: sound version");
        Sound s;
        s.flags = r.u32();
        s.audioId = r.u32();
        s.locationUpdateDelta = r.f32();
        s.playTime = r.f32();
        s.volume = r.f32();
        return s;
    });
    lib.cameras = r.u32();
    for (size_t i = 0; i < lib.cameras; ++i)
        readCamera(r);
    if (r.u32() != 0)
        throw FormatError("effects: game effects present");
    lib.models = list<Model>(r, [](BinReader& r) {
        if (r.u16() != 1)
            throw FormatError("effects: model version");
        Model m;
        m.model = str32(r);
        m.size = r.f32();
        m.color = v3(r);
        m.alpha = r.f32();
        return m;
    });
    lib.screens = list<Screen>(r, [](BinReader& r) {
        if (r.u16() != 1)
            throw FormatError("effects: screen version");
        Screen s;
        s.mode = r.u8();
        s.flags = r.u32();
        s.colors = vectors(r);
        s.strength = curve(r);
        s.length = r.f32();
        s.delay = r.f32();
        s.texture = str32(r);
        return s;
    });
    lib.waters = r.u32();
    for (size_t i = 0; i < lib.waters; ++i)
        readWater(r);
    if (r.u32() != 0)
        throw FormatError("effects: light effects present");
    const uint16_t vver = r.u16();
    lib.visuals = list<VisualEffect>(r, [vver](BinReader& r) {
        VisualEffect v;
        v.flags = r.u32();
        r.u16();
        if (vver < 3)
            r.u16();
        else
            r.u32();
        r.skip(4 + 4 + 8);
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n; ++i)
            v.components.push_back(readComponent(r, vver));
        return v;
    });
    for (;;) {
        auto name = str32(r);
        const uint32_t id = r.u32();
        if (name == "end")
            break;
        lib.names[lower(name)] = id;
    }
    // A component that is a whole effect names it: its stored index is 0
    // (the base game's "aquarium_fish_effects" lists aquarium_effects_fish1..6,
    // all with index 0, the construction cursor's dust). Resolved by name.
    for (auto& v : lib.visuals)
        for (auto& c : v.components)
            if (c.type == Block::Visual)
                if (auto i = lib.indexOf(c.name))
                    c.index = int32_t(*i);
    return lib;
}

std::optional<uint32_t> EffectsLibrary::indexOf(std::string_view name) const {
    auto it = names.find(lower(name));
    if (it == names.end() || it->second >= visuals.size())
        return std::nullopt;
    return it->second;
}

const VisualEffect* EffectsLibrary::find(std::string_view name) const {
    auto i = indexOf(name);
    return i ? &visuals[*i] : nullptr;
}

} // namespace os2::fx
