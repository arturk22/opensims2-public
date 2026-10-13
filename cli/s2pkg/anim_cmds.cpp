#include <set>
// s2pkg commands for skeletons, animations and Sims.
#include "anim_cmds.hpp"

#include "formats/anim.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/sim.hpp"
#include "formats/skeleton.hpp"

#include <cctype>
#include <cmath>
#include <map>
#include <optional>
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
        if (b.orient.w < 0.99999f)
            std::printf("    joint orientation q=(%.4f %.4f %.4f %.4f)\n", b.orient.x, b.orient.y, b.orient.z, b.orient.w);
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
    std::printf("heading=%g distance=%g velocity=%g turn=%g stride=%g cycle=%g\n", a.headingOffset,
                a.locomotionDistance, a.velocity, a.turnRotation, a.strideLength, a.cyclePortionCompleted);
    for (const auto& t : a.targets) {
        std::printf("target \"%s\" type=%u channels=%zu ik=%zu\n", t.tag.c_str(), t.animType, t.channels.size(),
                    t.ikChains.size());
        for (const auto& c : t.channels) {
            std::printf("  %-26s attr=%u type=%u dur=%u ik=%d%s hash=0x%08X:", c.name.c_str(), c.attribute(), c.type(),
                        c.duration(), c.ikChain(), c.isDelta() ? " delta" : "", c.nameHash);
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
        // CRC names: the channels', else the bones of the adult, child,
        // toddler and baby skeletons (and with --keys, every CRC field).
        static std::map<uint32_t, std::string> boneNames;
        if (keys && boneNames.empty())
            for (const char* sk : {"auskel_cres", "cuskel_cres", "puskel_cres", "buskel_cres"})
                try {
                    for (const auto& b : loadObjectModel(index, sk).skeleton.bones)
                        boneNames.emplace(nameInstanceHiHash(b.name), std::string(sk).substr(0, 6) + ":" + b.name);
                } catch (const std::exception&) {
                }
        auto crcName = [&](uint32_t crc) -> std::string {
            if (!crc)
                return "0";
            for (const auto& c : t.channels)
                if (c.nameHash == crc)
                    return c.name;
            if (auto it = boneNames.find(crc); it != boneNames.end())
                return it->second;
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

// The root and the feet of an animation on the adult skeleton (IK applied),
// every 1/30 s: where the root is and faces (the yaw of its forward, +Y, axis,
// about +Z, degrees) and both feet. With a speed (m/s, default the header's
// velocity), the animation's frame moves forward at it, as the locomotion
// moves a walker; a planted foot (the lower one, within 0.006 of its lowest)
// should then stand still: the summary gives how far the planted feet slide
// per second of contact (foot slide; 0 is a walk that fits its speed).
int cmdAnimRoot(const char* res, const std::string& name, std::optional<float> speed, const std::string& cres) {
    ResourceIndex index(res);
    auto loc = index.findByName(name, kTypeAnim);
    if (!loc)
        throw FormatError("ANIM not found: " + name);
    const Animation a = parseAnim(index.read(*loc));
    const ObjectModel model = loadObjectModel(index, withSuffix(cres, "_cres"));
    const Skeleton& sk = model.skeleton;
    const Animation::Target* target = a.targets.empty() ? nullptr : &a.targets.front();
    if (!target)
        throw FormatError("ANIM has no target");
    const float v = speed.value_or(a.velocity * Animation::kTicksPerSecond);
    const int root = sk.find("root_rot"), lf = sk.find("l_foot"), rf = sk.find("r_foot");
    if (root < 0 || lf < 0 || rf < 0)
        throw FormatError("skeleton without root_rot/l_foot/r_foot");
    std::printf("%s: %u ticks, frame speed %.4f m/s (header %.4f), turn %.1f deg\n", a.resource.fileName.c_str(),
                a.duration, v, a.velocity * Animation::kTicksPerSecond, a.turnRotation * 180 / 3.14159265f);
    struct Sample { float t; Vec3 l, r; };
    std::vector<Sample> samples;
    const float step = Animation::kTicksPerSecond / 30.0f;
    for (float ticks = 0; ticks <= float(a.duration) + 0.01f; ticks += step) {
        std::vector<Xform> pose = samplePose(*target, sk, ticks);
        applyIk(*target, sk, ticks, pose);
        const auto w = sk.world(pose);
        const float off = v * ticks / Animation::kTicksPerSecond;
        const Vec3 fwd = Xform{w[size_t(root)].q, {0, 0, 0}}.rotate(Vec3{0, 0, 1});
        // The root's facing: its local +Z (the skeleton's forward through
        // root_trans' rest rotation) on the ground.
        const float yaw = std::atan2(-fwd[0], fwd[1]) * 180 / 3.14159265f;
        Vec3 l = w[size_t(lf)].t, r = w[size_t(rf)].t;
        l[1] += off, r[1] += off;
        samples.push_back({ticks, l, r});
        std::printf("t=%5.0f root=(%.3f %.3f %.3f) yaw=%7.1f  l=(%.3f %.3f %.3f) r=(%.3f %.3f %.3f)\n", ticks,
                    w[size_t(root)].t[0], w[size_t(root)].t[1] + off, w[size_t(root)].t[2], yaw, l[0], l[1], l[2], r[0],
                    r[1], r[2]);
    }
    float lowL = 1e9f, lowR = 1e9f;
    for (const auto& s : samples)
        lowL = std::min(lowL, s.l[2]), lowR = std::min(lowR, s.r[2]);
    float slide = 0, contact = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        const auto &p = samples[i - 1], &q = samples[i];
        // The supporting foot: the lower one (relative to its own lowest),
        // when it is down on both samples.
        const bool left = (q.l[2] - lowL) <= (q.r[2] - lowR);
        const Vec3 &a0 = left ? p.l : p.r, &a1 = left ? q.l : q.r;
        const float low = left ? lowL : lowR;
        if (a0[2] - low > 0.006f || a1[2] - low > 0.006f)
            continue;
        slide += std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
        contact += (q.t - p.t) / Animation::kTicksPerSecond;
    }
    std::printf("foot slide: %.4f m over %.2f s of contact (%.4f m/s)\n", slide, contact,
                contact > 0 ? slide / contact : 0.0f);
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
    std::map<std::string, size_t> errors, curves, values, attributes, tags, animTypes;
    std::map<std::string, std::string> examples; // target tag and type: the first few animations
    std::vector<std::string> deltaAnims;         // animations with delta channels
    size_t keys = 0, channels = 0;
    // IK targets: their type and where their frame bone is (the target's
    // own skeleton, another skeleton's bone, none), with examples.
    std::map<std::string, std::set<uint32_t>> skeletonBones; // tag -> bone name CRCs
    std::map<uint32_t, std::string> anyBone;
    for (const char* sk : {"auskel", "cuskel", "puskel", "buskel"})
        try {
            for (const auto& b : loadObjectModel(index, std::string(sk) + "_cres").skeleton.bones) {
                skeletonBones[sk].insert(nameInstanceHiHash(b.name));
                anyBone.emplace(nameInstanceHiHash(b.name), b.name);
            }
        } catch (const std::exception&) {
        }
    std::map<std::string, size_t> ikKinds;
    std::map<std::string, std::string> ikExamples;
    for (const auto& loc : index.ofType(kTypeAnim)) {
        ++total;
        try {
            const Animation a = parseAnim(index.read(loc));
            ++ok;
            size_t deltas = 0, bases = 0;
            for (const auto& t : a.targets)
                for (const auto& c : t.channels)
                    if (c.attribute() <= Animation::Translation)
                        ++(c.isDelta() ? deltas : bases);
            if (deltas)
                deltaAnims.push_back(a.resource.fileName + " flags=" + std::to_string(a.flags) + " delta=" +
                                     std::to_string(deltas) + " base=" + std::to_string(bases) +
                                     " ticks=" + std::to_string(a.duration));
            for (const auto& t : a.targets) {
                std::string tagLower = t.tag;
                for (char& ch : tagLower)
                    ch = char(std::tolower(static_cast<unsigned char>(ch)));
                for (const auto& chain : t.ikChains)
                    for (const auto& g : chain.targets) {
                        const auto own = skeletonBones.find(tagLower);
                        std::string where = !g.crcs[0]                                            ? "no frame"
                                            : own != skeletonBones.end() && own->second.count(g.crcs[0]) ? "own bone"
                                            : anyBone.count(g.crcs[0])                               ? "other skeleton's bone"
                                                                                                     : "unknown";
                        const std::string k = "type " + std::to_string(g.type) + ", " + where;
                        if (++ikKinds[k] <= 4)
                            ikExamples[k] += " " + a.resource.fileName + ":" +
                                             (anyBone.count(g.crcs[0]) ? anyBone[g.crcs[0]] : std::string("?"));
                    }
                ++tags[t.tag];
                std::string lower = t.tag;
                for (char& ch : lower)
                    ch = char(std::tolower(static_cast<unsigned char>(ch)));
                const std::string kind = lower + " type " + std::to_string(t.animType);
                if (++animTypes[kind] <= 3)
                    examples[kind] += " " + a.resource.fileName;
                for (const auto& c : t.channels) {
                    ++channels;
                    ++attributes["attr " + std::to_string(c.attribute()) + " type " + std::to_string(c.type()) +
                                 " x" + std::to_string(c.componentCount()) + (c.isDelta() ? " delta" : "")];
                    if (c.isDelta()) {
                        const std::string dk = kind + " delta attr " + std::to_string(c.attribute());
                        if (++animTypes[dk] <= 3)
                            examples[dk] += " " + a.resource.fileName + ":" + c.name;
                    }
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
    // The targets' animation types (cAnimTarget::SetAnimType; 6 is a delta
    // over the base channels: docs/primitives-mac.md §4), with examples.
    std::printf("target types:\n");
    for (const auto& [k, n] : animTypes)
        std::printf("  %7zu %-28s%s\n", n, k.c_str(), examples[k].c_str());
    std::printf("IK targets (type, frame bone):\n");
    for (const auto& [k, n] : ikKinds)
        std::printf("  %7zu %-34s%s\n", n, k.c_str(), ikExamples[k].c_str());
    std::sort(deltaAnims.begin(), deltaAnims.end());
    std::printf("animations with delta channels (%zu):\n", deltaAnims.size());
    for (const auto& d : deltaAnims)
        std::printf("  %s\n", d.c_str());
    return ok == total ? 0 : 1;
}

} // namespace

int animCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "skeleton" && argc == 4)
        return cmdSkeleton(argv[2], withSuffix(argv[3], "_cres"));
    if (cmd == "anim" && (argc == 4 || argc == 5))
        return cmdAnim(argv[2], withSuffix(argv[3], "_anim"), argc == 5 && std::string(argv[4]) == "--keys");
    if (cmd == "anim-root" && argc >= 4 && argc <= 6)
        return cmdAnimRoot(argv[2], withSuffix(argv[3], "_anim"),
                           argc >= 5 && std::string(argv[4]) != "-" ? std::optional<float>(std::stof(argv[4])) : std::nullopt,
                           argc == 6 ? argv[5] : "auskel");
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
           "       s2pkg anim-root <Res-dir> <anim-name> [speed-m/s|-] [skeleton-cres]  (root, feet, foot slide)\n"
           "       s2pkg anim-sweep <Res-dir>\n"
           "       s2pkg anim-events <Res-dir> [name-filter]\n"
           "       s2pkg sim-parts <Res-dir> [name-filter [--resolve]]\n";
}

} // namespace os2
