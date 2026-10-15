// effects: the game's particle effects (Res/Effects/effects.package).
#include "effects_cmds.hpp"

#include "formats/dbpf.hpp"
#include "formats/effects.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"
#include "render/particle_system.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace os2 {

namespace {

fx::EffectsLibrary load(const std::string& path) {
    DbpfPackage pkg(path);
    for (const auto& e : pkg.entries())
        if (e.tgi.type == fx::kTypeEffects)
            return fx::EffectsLibrary::parse(pkg.read(e));
    throw std::runtime_error("no effects resource in " + path);
}

std::string curveText(const fx::Curve& c) {
    std::string s = "[";
    for (size_t i = 0; i < c.size(); ++i) {
        char b[32];
        std::snprintf(b, sizeof b, "%s%g", i ? " " : "", c[i]);
        s += b;
    }
    return s + "]";
}

const char* blockName(fx::Block b) {
    switch (b) {
    case fx::Block::Visual: return "effect";
    case fx::Block::Particles: return "particles";
    case fx::Block::MetaParticles: return "metaParticles";
    case fx::Block::Decal: return "decal";
    case fx::Block::Sequence: return "sequence";
    case fx::Block::Sound: return "sound";
    case fx::Block::Camera: return "camera";
    case fx::Block::Model: return "model";
    case fx::Block::Screen: return "screen";
    case fx::Block::Water: return "water";
    }
    return "?";
}

void printParticles(const fx::Particles& p, const char* indent) {
    std::printf("%sv%u flags=%llx life=%g..%g preroll=%g rate=%s over %gs cycles=%u delay=%g..%g trigger=%g..%g\n", indent,
                p.version, (unsigned long long)p.flags, p.life[0], p.life[1], p.preRoll, curveText(p.rateCurve).c_str(),
                p.rateCurveTime, p.rateCurveCycles, p.rateDelay[0], p.rateDelay[1], p.rateTrigger[0], p.rateTrigger[1]);
    std::printf("%svolume=[%g %g %g]-[%g %g %g] torus=%g dir=[%g %g %g]-[%g %g %g] speed=%g..%g\n", indent,
                p.emitVolume.lo[0], p.emitVolume.lo[1], p.emitVolume.lo[2], p.emitVolume.hi[0], p.emitVolume.hi[1],
                p.emitVolume.hi[2], p.torusWidth, p.emitDirection.lo[0], p.emitDirection.lo[1], p.emitDirection.lo[2],
                p.emitDirection.hi[0], p.emitDirection.hi[1], p.emitDirection.hi[2], p.emitSpeed[0], p.emitSpeed[1]);
    std::printf("%ssize=%s vary %g aspect=%s rotX=%s alpha=%s vary %g colors=%zu", indent, curveText(p.sizeCurve).c_str(),
                p.sizeVary, curveText(p.aspectCurve).c_str(), curveText(p.rotateCurveX).c_str(),
                curveText(p.alphaCurve).c_str(), p.alphaVary, p.colors.size());
    for (auto& c : p.colors)
        std::printf(" (%g %g %g)", c[0], c[1], c[2]);
    std::printf("\n%stexture=%s tiles=%ux%u align=%u draw=%u layer=%g frames=%u+%u@%g\n", indent, p.material.c_str(),
                p.tileU, p.tileV, p.alignment, unsigned(p.drawType), p.layer, p.frameStart, p.frameCount, p.frameSpeed);
    std::printf("%sforce=[%g %g %g] globalWind=%g bomb=%g at [%g %g %g] drag=%g stretch=%g screw=%g wiggles=%zu "
                "walk=%g..%g/%g..%g turn %g %g rotate %u x[%g %g] +%g, x[%g %g] +%g\n",
                indent, p.force[0], p.force[1], p.force[2], p.globalWind, p.bomb, p.bombCenter[0], p.bombCenter[1],
                p.bombCenter[2], p.drag, p.stretch, p.screw, p.wiggles.size(), p.randomWalkDelay[0],
                p.randomWalkDelay[1], p.randomWalkStrength[0], p.randomWalkStrength[1], p.randomWalkTurnX,
                p.randomWalkTurnY, p.rotateCount, p.rotateRange[0][0], p.rotateRange[0][1], p.rotateOffset[0],
                p.rotateRange[1][0], p.rotateRange[1][1], p.rotateOffset[1]);
    if (!p.attractorStrength.empty())
        std::printf("%sattractor=[%g %g %g] %s range %g\n", indent, p.attractor[0], p.attractor[1], p.attractor[2],
                    curveText(p.attractorStrength).c_str(), p.attractorRange);
}

void printEffect(const fx::EffectsLibrary& lib, uint32_t index, int depth) {
    if (index >= lib.visuals.size() || depth > 6)
        return;
    const std::string pad(size_t(depth) * 2 + 2, ' ');
    for (const auto& c : lib.visuals[index].components) {
        std::printf("%s%s \"%s\" #%d flags=0x%X scale=%g offset=[%g %g %g] lod=%u-%u emit=%g/%g size=%g/%g alpha=%g/%g sel=%u/%u time=%g\n",
                    pad.c_str(), blockName(c.type), c.name.c_str(), c.index, c.flags, c.scale, c.offset[0], c.offset[1],
                    c.offset[2], c.lodBegin, c.lodEnd, c.emitScale[0], c.emitScale[1], c.sizeScale[0], c.sizeScale[1],
                    c.alphaScale[0], c.alphaScale[1], c.selectionGroup, c.selectionChance, c.timeScale);
        const std::string in = pad + "    ";
        if (c.transformFlags || c.rotation != std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1})
            std::printf("%s  transform flags=%u rot=[%g %g %g | %g %g %g | %g %g %g]\n", pad.c_str(), c.transformFlags,
                        c.rotation[0], c.rotation[1], c.rotation[2], c.rotation[3], c.rotation[4], c.rotation[5],
                        c.rotation[6], c.rotation[7], c.rotation[8]);
        switch (c.type) {
        case fx::Block::Visual: printEffect(lib, uint32_t(c.index), depth + 1); break;
        case fx::Block::Particles:
            if (size_t(c.index) < lib.particles.size())
                printParticles(lib.particles[size_t(c.index)], in.c_str());
            break;
        case fx::Block::MetaParticles:
            if (size_t(c.index) < lib.metaParticles.size()) {
                const auto& m = lib.metaParticles[size_t(c.index)];
                std::printf("%sbase=%s flags=%llx life=%g..%g preroll=%g rate=%s over %gs cycles=%u size=%s alpha=%s "
                            "dir=[%g %g %g]-[%g %g %g] speed=%g..%g force=[%g %g %g] %g %g\n",
                            in.c_str(), m.baseEffect.c_str(), (unsigned long long)m.flags, m.life[0], m.life[1],
                            m.preRoll, curveText(m.rateCurve).c_str(), m.rateCurveTime, m.rateCurveCycles,
                            curveText(m.sizeCurve).c_str(), curveText(m.alphaCurve).c_str(), m.emitDirection.lo[0],
                            m.emitDirection.lo[1], m.emitDirection.lo[2], m.emitDirection.hi[0], m.emitDirection.hi[1],
                            m.emitDirection.hi[2], m.emitSpeed[0], m.emitSpeed[1], m.force[0], m.force[1], m.force[2],
                            m.globalWind, m.bomb);
                std::printf("%sdelay=%g..%g trigger=%g..%g volume=[%g %g %g]-[%g %g %g] align=%u bomb at [%g %g %g] drag=%g screw=%g collider=%s wiggles=%zu",
                            in.c_str(), m.rateDelay[0], m.rateDelay[1], m.rateTrigger[0], m.rateTrigger[1], m.emitVolume.lo[0], m.emitVolume.lo[1], m.emitVolume.lo[2], m.emitVolume.hi[0],
                            m.emitVolume.hi[1], m.emitVolume.hi[2], m.alignment, m.bombCenter[0], m.bombCenter[1], m.bombCenter[2],
                            m.drag, m.screw, m.colliderName.c_str(), m.wiggles.size());
                for (const auto& w : m.wiggles)
                    std::printf(" (%g [%g %g %g] [%g %g %g])", w.timeRate, w.rateDirection[0], w.rateDirection[1],
                                w.rateDirection[2], w.wiggleDirection[0], w.wiggleDirection[1], w.wiggleDirection[2]);
                for (const auto& c : m.colliders)
                    std::printf(" collider{%x %s [%g %g %g] [%g %g %g] [%g %g %g] %g %g %g %g %s %s}", c.flags,
                                c.name.c_str(), c.v[0][0], c.v[0][1], c.v[0][2], c.v[1][0], c.v[1][1], c.v[1][2], c.v[2][0],
                                c.v[2][1], c.v[2][2], c.f[0], c.f[1], c.f[2], c.f[3], c.effect1.c_str(), c.effect2.c_str());
                for (const auto& w : m.walks)
                    std::printf(" walk=%g..%g/%g..%g turn %g %g %g", w.delay[0], w.delay[1], w.strength[0],
                                w.strength[1], w.turn[0], w.turn[1], w.turn[2]);
                std::printf(" prefer=[%g %g %g] damp=%g bankWind=%g bank=%g attractor=[%g %g %g] %s %g path=%zu\n",
                            m.walkPreferDirection[0], m.walkPreferDirection[1], m.walkPreferDirection[2], m.alignDamp,
                            m.alignBankWind, m.alignBank, m.attractor[0], m.attractor[1], m.attractor[2],
                            curveText(m.attractorStrength).c_str(), m.attractorRange, m.path.size());
                if (auto i = lib.indexOf(m.baseEffect))
                    printEffect(lib, *i, depth + 2);
            }
            break;
        case fx::Block::Sequence:
            if (size_t(c.index) < lib.sequences.size())
                for (const auto& it : lib.sequences[size_t(c.index)].items) {
                    std::printf("%s@%g..%g %s\n", in.c_str(), it.activateTime[0], it.activateTime[1], it.effect.c_str());
                    if (auto i = lib.indexOf(it.effect))
                        printEffect(lib, *i, depth + 2);
                }
            break;
        case fx::Block::Model:
            if (size_t(c.index) < lib.models.size())
                std::printf("%smodel=%s\n", in.c_str(), lib.models[size_t(c.index)].model.c_str());
            break;
        case fx::Block::Sound:
            if (size_t(c.index) < lib.sounds.size()) {
                const auto& s = lib.sounds[size_t(c.index)];
                std::printf("%saudio=%08X flags=%X update=%g play=%g volume=%g\n", in.c_str(), s.audioId, s.flags,
                            s.locationUpdateDelta, s.playTime, s.volume);
            }
            break;
        case fx::Block::Screen:
            if (size_t(c.index) < lib.screens.size()) {
                const auto& s = lib.screens[size_t(c.index)];
                std::printf("%smode=%u flags=0x%X length=%g delay=%g strength=%s texture=%s colors=%zu", in.c_str(), s.mode,
                            s.flags, s.length, s.delay, curveText(s.strength).c_str(), s.texture.c_str(), s.colors.size());
                for (auto& col : s.colors)
                    std::printf(" (%g %g %g)", col[0], col[1], col[2]);
                std::printf("\n");
            }
            break;
        case fx::Block::Decal:
            if (size_t(c.index) < lib.decals.size()) {
                const auto& d = lib.decals[size_t(c.index)];
                std::printf("%stexture=%s draw=%u lifeType=%u life=%g size=%s alpha=%s rotate=%s aspect=%s repeat=%g colors=%zu\n",
                            in.c_str(), d.texture.c_str(), d.drawType, d.lifeType, d.life, curveText(d.sizeCurve).c_str(),
                            curveText(d.alphaCurve).c_str(), curveText(d.rotateCurve).c_str(),
                            curveText(d.aspectCurve).c_str(), d.textureRepeat, d.colors.size());
            }
            break;
        default: break;
        }
    }
}

int cmdEffects(const std::string& path, const std::vector<std::string>& names) {
    const auto lib = load(path);
    if (names.empty()) {
        std::printf("particles %zu, meta %zu, decals %zu, sequences %zu, sounds %zu, cameras %zu, models %zu, "
                    "screens %zu, waters %zu, effects %zu, names %zu\n",
                    lib.particles.size(), lib.metaParticles.size(), lib.decals.size(), lib.sequences.size(),
                    lib.sounds.size(), lib.cameras, lib.models.size(), lib.screens.size(), lib.waters, lib.visuals.size(),
                    lib.names.size());
        std::map<int, int> versions, draws, aligns;
        for (const auto& p : lib.particles) {
            ++versions[p.version];
            ++draws[int(p.drawType)];
            ++aligns[p.alignment];
        }
        for (auto [v, n] : versions)
            std::printf("  particle version %d: %d\n", v, n);
        for (auto [v, n] : draws)
            std::printf("  draw type %d: %d\n", v, n);
        for (auto [v, n] : aligns)
            std::printf("  alignment %d: %d\n", v, n);
        // Flag bits (the binary's std::bitset<38> at cTSParticlesDescription
        // +0x158; docs/effects-mac.md §1.2), with a few components using each.
        std::map<int, std::string> blockName;
        for (const auto& v : lib.visuals)
            for (const auto& c : v.components)
                if (c.type == fx::Block::Particles && !blockName.count(c.index))
                    blockName[c.index] = c.name;
        for (int b = 0; b < 32; ++b) {
            int n = 0;
            std::string some;
            for (size_t i = 0; i < lib.particles.size(); ++i)
                if (lib.particles[i].flags >> b & 1)
                    if (++n <= 4)
                        some += " " + blockName[int(i)];
            if (n)
                std::printf("  flag bit %d: %d%s\n", b, n, some.c_str());
        }
        std::map<size_t, int> attractorCurves;
        for (const auto& p : lib.particles)
            ++attractorCurves[p.attractorStrength.size()];
        for (auto [n, c] : attractorCurves)
            std::printf("  attractor curve of %zu points: %d\n", n, c);
        for (int b = 0; b < 32; ++b) {
            int n = 0;
            for (const auto& v : lib.visuals)
                n += int(v.flags >> b & 1);
            if (n)
                std::printf("  effect flag bit %d: %d\n", b, n);
        }
        for (int b = 0; b < 64; ++b) {
            int n = 0;
            for (const auto& m : lib.metaParticles)
                n += int(m.flags >> b & 1);
            if (n)
                std::printf("  meta flag bit %d: %d\n", b, n);
        }
        std::vector<std::pair<uint32_t, std::string>> sorted;
        for (const auto& [n, i] : lib.names)
            sorted.emplace_back(i, n);
        std::sort(sorted.begin(), sorted.end());
        for (const auto& [i, n] : sorted)
            std::printf("%5u %s\n", i, n.c_str());
        return 0;
    }
    for (const auto& n : names) {
        auto i = lib.indexOf(n);
        if (!i) {
            std::printf("%s: no such effect\n", n.c_str());
            continue;
        }
        std::printf("%s (#%u) flags=0x%X\n", n.c_str(), *i, lib.visuals[*i].flags);
        printEffect(lib, *i, 0);
    }
    return 0;
}

// STR# tables under `res` whose strings name effects in the library.
int cmdEffectTables(const std::string& res, const std::string& fxPath, uint32_t onlyGroup, uint32_t onlyInstance) {
    const auto lib = load(fxPath);
    ResourceIndex index(res);
    for (const auto& loc : index.ofType(0x53545223)) {
        StringTable t;
        try {
            t = StringTable::parse(index.read(loc));
        } catch (const std::exception&) {
            continue;
        }
        const size_t n = t.count(1);
        size_t hits = 0;
        for (size_t i = 0; i < n; ++i)
            if (lib.indexOf(t.get(i, 1)))
                ++hits;
        if (onlyGroup && loc.entry->tgi.group == onlyGroup && loc.entry->tgi.instance == onlyInstance) {
            for (size_t i = 0; i < n; ++i)
                std::printf("%4zu %c %s\n", i, lib.indexOf(t.get(i, 1)) ? '*' : ' ', t.get(i, 1).c_str());
            continue;
        }
        if (hits && !onlyGroup)
            std::printf("%08X %04X %zu/%zu %s: %s\n", loc.entry->tgi.group, unsigned(loc.entry->tgi.instance), hits, n,
                        t.fileName.c_str(), t.get(0, 1).c_str());
    }
    return 0;
}

// Runs an effect for `seconds` at the origin and prints its particles.
// "<name>" starts hard (with the pre-roll), "<name>~soft" soft.
int cmdEffectSim(const std::string& path, const std::string& spec, float seconds) {
    const auto lib = load(path);
    fx::EffectSystem sys(lib);
    const bool soft = spec.ends_with("~soft");
    const std::string name = soft ? spec.substr(0, spec.size() - 5) : spec;
    const int h = sys.start(name, {}, !soft);
    if (h < 0) {
        std::printf("%s: no such effect\n", name.c_str());
        return 1;
    }
    for (float t = 0; t < seconds; t += 1.0f / 30)
        sys.update(1.0f / 30);
    std::vector<fx::ParticleQuad> quads;
    sys.collect(quads);
    std::printf("%s after %gs: %zu particles\n", name.c_str(), seconds, quads.size());
    for (size_t i = 0; i < quads.size(); i += std::max<size_t>(1, quads.size() / 24)) {
        const auto& q = quads[i];
        std::printf("  pos (%.2f %.2f %.2f) size %.3fx%.3f rot %.2f rgba (%.2f %.2f %.2f %.2f) v (%.2f %.2f %.2f) %s\n",
                    q.position[0], q.position[1], q.position[2], q.width, q.height, q.rotation, q.color[0], q.color[1],
                    q.color[2], q.color[3], q.velocity[0], q.velocity[1], q.velocity[2],
                    q.texture ? q.texture->c_str() : "");
    }
    return 0;
}

} // namespace

int effectsCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "effects" && argc >= 3)
        return cmdEffects(argv[2], {argv + 3, argv + argc});
    if (cmd == "effect-sim" && (argc == 4 || argc == 5))
        return cmdEffectSim(argv[2], argv[3], argc == 5 ? std::stof(argv[4]) : 2.0f);
    if (cmd == "effect-tables" && (argc == 4 || argc == 5))
    {
        const std::string g = argc == 5 ? argv[4] : "0";
        const auto colon = g.find(':');
        return cmdEffectTables(argv[2], argv[3], uint32_t(std::stoul(g.substr(0, colon), nullptr, 16)),
                               colon == std::string::npos ? 0x8F : uint32_t(std::stoul(g.substr(colon + 1), nullptr, 16)));
    }
    return -1;
}

const char* effectsUsage() {
    return "       s2pkg effects <effects.package> [effect-name...]\n"
           "       s2pkg effect-sim <effects.package> <effect-name> [seconds]\n"
           "       s2pkg effect-tables <Res-dir> <effects.package> [group-hex[:STR#-hex]: list that table, 0x8F by default]\n";
}

} // namespace os2
