// s2pkg commands for skeletons, animations and Sims.
#include "anim_cmds.hpp"

#include "formats/anim.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/skeleton.hpp"

#include <cmath>
#include <map>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeCres = 0xE519C933, kTypeAnim = 0xFB00791E;

std::string withSuffix(std::string s, const char* suffix) {
    if (!s.ends_with(suffix))
        s += suffix;
    return s;
}

// Largest deviation of world(rest) ∘ bind from the identity, over a few test points.
float bindError(const Xform& world, const Xform& bind) {
    const Xform m = world.then(bind);
    float err = 0;
    for (const Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
        const Vec3 q = m.apply(p);
        err = std::max({err, std::fabs(q[0] - p[0]), std::fabs(q[1] - p[1]), std::fabs(q[2] - p[2])});
    }
    return err;
}

int cmdSkeleton(const char* res, const std::string& cresName) {
    ResourceIndex index(res);
    const ObjectModel model = loadObjectModel(index, cresName);
    const Skeleton& sk = model.skeleton;
    const auto world = sk.world(sk.restPose());
    std::printf("%s: %zu bones\n", sk.name.c_str(), sk.bones.size());
    for (size_t i = 0; i < sk.bones.size(); ++i) {
        const auto& b = sk.bones[i];
        int depth = 0;
        for (int p = b.parent; p >= 0; p = sk.bones[size_t(p)].parent)
            ++depth;
        char joint[16] = "-";
        if (b.joint >= 0)
            std::snprintf(joint, sizeof joint, "%d", b.joint);
        std::printf("%3zu %*s%-*s joint=%-3s local t=(%.4f %.4f %.4f) q=(%.4f %.4f %.4f %.4f)  world t=(%.4f %.4f %.4f)\n",
                    i, depth * 2, "", 28 - depth * 2, b.name.c_str(), joint, b.rest.t[0], b.rest.t[1], b.rest.t[2],
                    b.rest.q.x, b.rest.q.y, b.rest.q.z, b.rest.q.w, world[i].t[0], world[i].t[1], world[i].t[2]);
    }
    // Skinned parts: bone ids used, their bones, and how well the GMDC bind
    // poses match the skeleton's rest pose.
    for (const auto& part : model.parts) {
        if (part.mesh.bones.empty())
            continue;
        std::vector<bool> used(part.bindPoses.size(), false);
        for (const auto& v : part.mesh.bones)
            for (auto b : v)
                if (b != 0xFF && b < used.size())
                    used[b] = true;
        size_t count = 0, missing = 0;
        float worst = 0;
        std::string worstBone;
        for (size_t j = 0; j < used.size(); ++j) {
            if (!used[j])
                continue;
            ++count;
            const int bone = sk.findJoint(int(j));
            if (bone < 0) {
                ++missing;
                continue;
            }
            const float e = bindError(world[size_t(bone)], part.bindPoses[j]);
            if (e > worst)
                worst = e, worstBone = sk.bones[size_t(bone)].name;
        }
        std::printf("part %s/%s: %zu vertices, %zu bones used of %zu bind poses, %zu without a joint, "
                    "max bind error %.2g (%s)\n",
                    part.shape.c_str(), part.mesh.name.c_str(), part.mesh.positions.size(), count,
                    part.bindPoses.size(), missing, worst, worstBone.c_str());
    }
    return 0;
}

Animation parseAnim(std::span<const uint8_t> bytes) {
    Rcol rc(bytes);
    const auto* b = rc.find("cAnimResourceConst");
    if (!b)
        throw FormatError("ANIM has no cAnimResourceConst");
    return Animation::parse(b->body);
}

const char* curveName(Animation::Curve c) {
    switch (c) {
    case Animation::Curve::Baked: return "baked";
    case Animation::Curve::Continuous: return "continuous";
    case Animation::Curve::Discontinuous: return "discontinuous";
    }
    return "?";
}

const char* valueName(Animation::Value v) {
    static const char* names[] = {"8.7", "9.7", "5.10", "5.11", "3.12", "3.13", "float"};
    return names[size_t(v)];
}

int cmdAnim(const char* res, const std::string& name, bool keys) {
    ResourceIndex index(res);
    auto loc = index.findByName(name, kTypeAnim);
    if (!loc)
        throw FormatError("ANIM not found: " + name);
    const Animation a = parseAnim(index.read(*loc));
    std::printf("%s: v%u duration=%u ticks (%.2f s) flags=0x%02X priority=%u locomotion=%u skeleton=\"%s\" data=\"%s\"\n",
                a.resource.fileName.c_str(), a.version, a.duration, a.duration / Animation::kTicksPerSecond, a.flags,
                a.priority, a.locomotionType, a.skeletonTag.c_str(), a.dataString.c_str());
    std::printf("heading=%g distance=%g velocity=%g turn=%g stride=%g\n", a.headingOffset, a.locomotionDistance,
                a.velocity, a.turnRotation, a.strideLength);
    for (const auto& t : a.targets) {
        std::printf("target \"%s\" type=%u channels=%zu ik=%zu\n", t.tag.c_str(), t.animType, t.channels.size(),
                    t.ikChains.size());
        for (const auto& c : t.channels) {
            std::printf("  %-26s attr=%u type=%u dur=%u ik=%d hash=0x%08X:", c.name.c_str(), c.attribute(), c.type(),
                        c.duration(), c.ikChain(), c.nameHash);
            for (const auto& comp : c.components)
                std::printf(" [%zu %s %s]", comp.keys.size(), curveName(comp.curve), valueName(comp.value));
            std::printf("\n");
            if (keys)
                for (size_t k = 0; k < c.components.size(); ++k) {
                    std::printf("    %zu:", k);
                    for (const auto& key : c.components[k].keys)
                        std::printf(" %u=%.4g", key.time, key.value);
                    std::printf("\n");
                }
        }
        // CRC names: the channels' (and with --keys, every CRC field).
        auto crcName = [&](uint32_t crc) -> std::string {
            if (!crc)
                return "0";
            for (const auto& c : t.channels)
                if (c.nameHash == crc)
                    return c.name;
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%08X", crc);
            return buf;
        };
        for (const auto& ch : t.ikChains) {
            std::printf("  ik chain 0x%08X strategy=0x%08X begin=0x%08X end=0x%08X targets=%zu\n", ch.nameCrc,
                        ch.strategy, ch.crcs[0], ch.crcs[2], ch.targets.size());
            if (!keys)
                continue;
            std::printf("    begin/mirror/end/mirror/twist/twistbone/mirror/weight:");
            for (uint32_t c : ch.crcs)
                std::printf(" %s", crcName(c).c_str());
            std::printf("\n");
            for (const auto& g : ch.targets) {
                std::printf("    target index=%u chain=%u type=%u bone/mirror/rot/rot2/trans/contact:", g.targetIndex,
                            g.chainIndex, g.type);
                for (uint32_t c : g.crcs)
                    std::printf(" %s", crcName(c).c_str());
                std::printf("\n");
            }
        }
    }
    for (const auto& e : a.events)
        std::printf("event t=%u type=%d \"%s\"\n", e.ticks, e.type, e.data.c_str());
    return 0;
}

// Names of the ANIM resources containing `filter`, with duration and targets.
int cmdAnimList(const char* res, const std::string& filter) {
    ResourceIndex index(res);
    for (const auto& loc : index.ofType(kTypeAnim)) {
        const Animation a = parseAnim(index.read(loc));
        if (a.resource.fileName.find(filter) == std::string::npos)
            continue;
        std::printf("%-50s %6u ticks", a.resource.fileName.c_str(), a.duration);
        for (const auto& t : a.targets)
            std::printf(" %s(%zu)", t.tag.c_str(), t.channels.size());
        std::printf("\n");
    }
    return 0;
}

// One line per ANIM event (name, duration, locomotion distance/velocity,
// event tick, type and data with newlines as '|'), for sweeps that relate the
// tick to sounds and other timed data.
int cmdAnimEvents(const char* res, const std::string& filter) {
    ResourceIndex index(res);
    for (const auto& loc : index.ofType(kTypeAnim)) {
        const Animation a = parseAnim(index.read(loc));
        if (a.resource.fileName.find(filter) == std::string::npos)
            continue;
        for (const auto& e : a.events) {
            std::string data = e.data;
            std::replace(data.begin(), data.end(), '\n', '|');
            std::printf("%s\t%u\t%g\t%g\t%u\t%d\t%s\n", a.resource.fileName.c_str(), a.duration,
                        a.locomotionDistance, a.velocity, e.ticks, e.type, data.c_str());
        }
    }
    return 0;
}

// Catalog Sim parts (GZPS + 3IDR) whose name contains `filter`.
int cmdSimParts(const char* res, const std::string& filter, bool resolve) {
    ResourceIndex index(res);
    const auto parts = loadSimParts(index);
    size_t shown = 0;
    for (const auto& p : parts) {
        if (p.name().find(filter) == std::string::npos)
            continue;
        ++shown;
        std::printf("%-44s age=%-3u gender=%u outfit=%-3u category=%-3u type=%s flags=%u refs=%zu", p.name().c_str(),
                    p.number("age"), p.number("gender"), p.number("outfit"), p.number("category"),
                    p.property("type").c_str(), p.number("flags"), p.references.size());
        if (resolve) {
            if (auto c = p.reference("resourcekeyidx"))
                std::printf(" cres=%s", scenegraphName(index, *c).c_str());
            for (uint32_t i = 0; i < p.number("numoverrides"); ++i) {
                const std::string k = "override" + std::to_string(i);
                if (auto t = p.reference(k + "resourcekeyidx"))
                    std::printf(" %s=%s", p.property(k + "subset").c_str(), scenegraphName(index, *t).c_str());
            }
        }
        std::printf("\n");
    }
    std::printf("%zu of %zu parts\n", shown, parts.size());
    return 0;
}

// Decodes every ANIM resource and summarises what was found.
int cmdAnimSweep(const char* res) {
    ResourceIndex index(res);
    size_t total = 0, ok = 0;
    std::map<std::string, size_t> errors, curves, values, attributes, tags;
    size_t keys = 0, channels = 0;
    for (const auto& loc : index.ofType(kTypeAnim)) {
        ++total;
        try {
            const Animation a = parseAnim(index.read(loc));
            ++ok;
            for (const auto& t : a.targets) {
                ++tags[t.tag];
                for (const auto& c : t.channels) {
                    ++channels;
                    ++attributes["attr " + std::to_string(c.attribute()) + " type " + std::to_string(c.type()) +
                                 " x" + std::to_string(c.componentCount())];
                    for (const auto& comp : c.components) {
                        ++curves[curveName(comp.curve)];
                        ++values[valueName(comp.value)];
                        keys += comp.keys.size();
                    }
                }
            }
        } catch (const std::exception& e) {
            ++errors[e.what()];
        }
    }
    std::printf("%zu of %zu ANIM resources decoded; %zu channels, %zu keys\n", ok, total, channels, keys);
    auto dump = [](const char* what, const std::map<std::string, size_t>& m, size_t limit) {
        std::printf("%s:\n", what);
        std::vector<std::pair<size_t, std::string>> v;
        for (const auto& [k, n] : m)
            v.emplace_back(n, k);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < limit; ++i)
            std::printf("  %7zu %s\n", v[i].first, v[i].second.c_str());
    };
    dump("errors", errors, 20);
    dump("curves", curves, 10);
    dump("values", values, 10);
    dump("channel kinds", attributes, 30);
    dump("target tags", tags, 15);
    return ok == total ? 0 : 1;
}

} // namespace

int animCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "skeleton" && argc == 4)
        return cmdSkeleton(argv[2], withSuffix(argv[3], "_cres"));
    if (cmd == "anim" && (argc == 4 || argc == 5))
        return cmdAnim(argv[2], withSuffix(argv[3], "_anim"), argc == 5 && std::string(argv[4]) == "--keys");
    if (cmd == "anim-list" && (argc == 3 || argc == 4))
        return cmdAnimList(argv[2], argc == 4 ? argv[3] : "");
    if (cmd == "sim-parts" && argc >= 3 && argc <= 5)
        return cmdSimParts(argv[2], argc >= 4 ? argv[3] : "", argc == 5 && std::string(argv[4]) == "--resolve");
    if (cmd == "anim-events" && (argc == 3 || argc == 4))
        return cmdAnimEvents(argv[2], argc == 4 ? argv[3] : "");
    if (cmd == "anim-sweep" && argc == 3)
        return cmdAnimSweep(argv[2]);
    (void)kTypeCres;
    return -1;
}

const char* animUsage() {
    return "       s2pkg skeleton <Res-dir> <cres-name>\n"
           "       s2pkg anim <Res-dir> <anim-name> [--keys]\n"
           "       s2pkg anim-list <Res-dir> [name-filter]\n"
           "       s2pkg anim-sweep <Res-dir>\n"
           "       s2pkg anim-events <Res-dir> [name-filter]\n"
           "       s2pkg sim-parts <Res-dir> [name-filter [--resolve]]\n";
}

} // namespace os2
