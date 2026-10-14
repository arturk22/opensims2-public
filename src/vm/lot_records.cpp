// The records of a lot save outside the object module (lot_records.hpp).

#include "lot_records.hpp"
#include "formats/log_once.hpp"

#include "formats/binreader.hpp"
#include "formats/binwriter.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/model.hpp"
#include "formats/resindex.hpp"
#include "formats/neighborhood.hpp"
#include "formats/scene_anim.hpp"
#include "thread_state.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>

namespace os2::vm {

namespace {

constexpr uint32_t kTypeSceneObject = 0xFA1C39F7, kTypeRouting = 0xBC66BAEC;
constexpr size_t kFileNameSize = 64;

// Where a cObject keeps its position and rotation (7 floats, twice: before
// and after the definition's name), as Lot's reader walks it.
struct SceneLayout {
    size_t first = 0, second = 0; // byte offsets; second = 0 if the copy is not there
    std::string definition;       // "Trash Can - Outside_objt"
};

std::optional<SceneLayout> sceneLayout(std::span<const uint8_t> bytes) {
    try {
        BinReader r(bytes);
        r.skip(kFileNameSize);
        uint32_t version = 0;
        for (int depth = 0;; ++depth) {
            if (depth == 8)
                return std::nullopt;
            r.u32();
            version = r.u32();
            if (r.str7() == "cObject")
                break;
        }
        r.str7();
        for (uint32_t n = r.u32(); n; --n) {
            r.str7();
            if (version >= 0x11)
                r.u32();
            for (uint32_t m = r.u32(); m; --m) {
                r.str7();
                r.str7();
            }
        }
        SceneLayout l;
        l.first = r.pos();
        r.skip(28);
        r.u32();
        l.definition = r.str7();
        if (r.remaining() >= 28 && std::memcmp(&bytes[l.first], &bytes[r.pos()], 28) == 0)
            l.second = r.pos();
        return l;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

// A cObject with its model list rewritten for a design choice (the Design
// tool's, subset -> material): one entry, the model's name, 12 (v0x11), the
// (material, subset) pairs, as the game writes an object's.
std::optional<std::vector<uint8_t>> withDesign(std::span<const uint8_t> bytes,
                                               const std::map<std::string, std::string>& design) {
    try {
        BinReader r(bytes);
        r.skip(kFileNameSize);
        uint32_t version = 0;
        for (int depth = 0;; ++depth) {
            if (depth == 8)
                return std::nullopt;
            r.u32();
            version = r.u32();
            if (r.str7() == "cObject")
                break;
        }
        const std::string model = r.str7();
        const size_t from = r.pos();
        for (uint32_t n = r.u32(); n; --n) {
            r.str7();
            if (version >= 0x11)
                r.u32();
            for (uint32_t m = r.u32(); m; --m) {
                r.str7();
                r.str7();
            }
        }
        const size_t to = r.pos();
        if (model.empty())
            return std::nullopt;
        BinWriter w;
        w.u32(1);
        w.str7(model);
        if (version >= 0x11)
            w.u32(12);
        w.u32(uint32_t(design.size()));
        for (const auto& [subset, material] : design) {
            w.str7(material);
            w.str7(subset);
        }
        std::vector<uint8_t> out(bytes.begin(), bytes.begin() + std::ptrdiff_t(from));
        out.insert(out.end(), w.bytes().begin(), w.bytes().end());
        out.insert(out.end(), bytes.begin() + std::ptrdiff_t(to), bytes.end());
        return out;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

std::array<float, 7> readPose(std::span<const uint8_t> bytes, size_t at) {
    std::array<float, 7> p{};
    std::memcpy(p.data(), &bytes[at], 28);
    return p;
}

void writePose(std::vector<uint8_t>& bytes, const SceneLayout& l, const std::array<float, 7>& p) {
    std::memcpy(&bytes[l.first], p.data(), 28);
    if (l.second)
        std::memcpy(&bytes[l.second], p.data(), 28);
}

// A cPerson record (cPerson v3 < cLocomotable v4 < cAnimatable v0x10 <
// cObject v0x11, outermost first): its cObject's model list, the pose, the
// definition, the pose again, then the subclass data (the skeleton's name,
// its bones' pose, the animations, the locomotion state).
struct PersonLayout {
    size_t models = 0;     // the cObject's model count (after its empty model name)
    size_t pose = 0;       // the first pose (after the model list)
    size_t definition = 0; // the u32 before the definition's name
    size_t tail = 0;       // after the second pose
    std::string skeleton;  // "auskel", "cuskel", ...
};

std::optional<PersonLayout> personLayout(std::span<const uint8_t> bytes) {
    try {
        BinReader r(bytes);
        r.skip(kFileNameSize);
        bool person = false;
        uint32_t version = 0;
        for (int depth = 0;; ++depth) {
            if (depth == 8)
                return std::nullopt;
            r.u32();
            version = r.u32();
            const std::string cls = r.str7();
            person = person || cls == "cPerson";
            if (cls == "cObject")
                break;
        }
        if (!person)
            return std::nullopt;
        r.str7();
        PersonLayout l;
        l.models = r.pos();
        for (uint32_t n = r.u32(); n; --n) {
            r.str7();
            if (version >= 0x11)
                r.u32();
            for (uint32_t m = r.u32(); m; --m) {
                r.str7();
                r.str7();
            }
        }
        l.pose = r.pos();
        r.skip(28);
        l.definition = r.pos();
        r.u32();
        r.str7();
        r.skip(28);
        l.tail = r.pos();
        l.skeleton = r.str7();
        return l;
    } catch (const FormatError&) {
        return std::nullopt;
    }
}

// The cPerson of a Sim that has none (one that just moved in, created during
// play), made from `templ`, another Sim's of the same skeleton: the
// template's model list without its material choices (the game builds those
// composited materials and the meshes, named by number, when it dresses the
// Sim from its character file: inferred), the Sim's pose and definition, and
// the template's subclass data with its pose replaced (a rest pose of the
// skeleton and an idle locomotion state).
std::vector<uint8_t> personFromTemplate(std::span<const uint8_t> templ, const PersonLayout& l,
                                        const std::string& definition, const std::array<float, 7>& pose) {
    BinWriter w;
    w.raw(templ.subspan(0, l.models));
    {
        BinReader r(templ);
        r.seek(l.models);
        const uint32_t n = r.u32();
        w.u32(n);
        for (uint32_t i = 0; i < n; ++i) {
            w.str7(r.str7());
            w.u32(r.u32());
            for (uint32_t m = r.u32(); m; --m) {
                r.str7();
                r.str7();
            }
            w.u32(0);
        }
    }
    const auto old = readPose(templ, l.pose);
    const std::span<const uint8_t> poseBytes(reinterpret_cast<const uint8_t*>(pose.data()), 28);
    w.raw(poseBytes);
    {
        BinReader r(templ);
        r.seek(l.definition);
        w.u32(r.u32());
    }
    w.str7(definition);
    w.raw(poseBytes);
    // The subclass data keeps copies of the pose (the locomotion state's
    // position and rotation): the Sim's there too.
    std::vector<uint8_t> tail(templ.begin() + std::ptrdiff_t(l.tail), templ.end());
    for (size_t i = 0; i + 28 <= tail.size(); ++i)
        if (std::memcmp(&tail[i], old.data(), 28) == 0) {
            std::memcpy(&tail[i], pose.data(), 28);
            i += 27;
        }
    for (size_t i = 0; i + 12 <= tail.size(); ++i)
        if (std::memcmp(&tail[i], old.data(), 12) == 0) {
            std::memcpy(&tail[i], pose.data(), 12);
            i += 11;
        }
    w.raw(tail);
    return std::move(w.bytes());
}

// The skeleton a Sim of person data 58 "Person Age" has (lifestage values:
// 1 baby, 2 toddler, 3-15 child, 16 and up teen, adult, elder).
std::string skeletonForAge(int16_t age) {
    if (age >= 16)
        return "auskel";
    if (age >= 3)
        return "cuskel";
    if (age == 2)
        return "puskel";
    return "buskel";
}

int16_t directionOf(const LotObjects::State& s) {
    return s.data.size() > size_t(objdata::Direction) ? s.data[objdata::Direction] : 0;
}

bool inWorld(const LotObjects::State& s) { return s.x >= 0 && s.y >= 0; }

// The pose the game gives a state: position in tiles with the floor's
// elevation plus the object's (out of the world: the origin), rotation
// about Z by 90° - 45° × direction. What did not change keeps the old
// pose's values (the game's quaternions have either sign).
std::array<float, 7> poseOf(const Lot& world, const LotObjects::State& s, const LotObjects::State* was,
                            const std::array<float, 7>* old) {
    std::array<float, 7> p = old ? *old : std::array<float, 7>{0, 0, 0, 0, 0, 0, 1};
    if (!inWorld(s)) {
        p[0] = p[1] = p[2] = 0;
    } else {
        p[0] = s.x;
        p[1] = s.y;
        if (old && was && inWorld(*was) && was->level == s.level)
            p[2] = (*old)[2] - was->elevation + s.elevation;
        else
            p[2] = (world.hasLevel(s.level - 1) ? world.elevationAt(s.level - 1, s.x, s.y) : 3.0f * float(s.level - 1)) +
                   s.elevation;
    }
    if (!old || !was || directionOf(*was) != directionOf(s)) {
        const double a = (90.0 - 45.0 * directionOf(s)) * M_PI / 180.0;
        p[3] = p[4] = 0;
        p[5] = float(std::sin(a / 2));
        p[6] = float(std::cos(a / 2));
    }
    return p;
}

bool moved(const LotObjects::State& a, const LotObjects::State& b) {
    return a.x != b.x || a.y != b.y || a.level != b.level || a.elevation != b.elevation ||
           directionOf(a) != directionOf(b);
}

std::vector<uint8_t> writeRelationship(uint32_t unknown, const std::vector<int32_t>& values,
                                       const std::vector<int32_t>& stale) {
    BinWriter w;
    w.u32(unknown);
    w.u32(uint32_t(values.size()));
    for (int32_t v : values)
        w.i32(v);
    for (int32_t v : stale)
        w.i32(v);
    return std::move(w.bytes());
}

// The object's scene graph record with its animation queue saying what
// the VM plays (scene_anim.hpp), as the game's cAnimatable saves it: the
// overlays (Animate Overlay's idles, faces, carry poses; Animate Object's)
// and a Sim's Animate Sim its Main waits on (its request ID is the waiting
// frame's primitive state).
// The record's animation requests are replaced (its look-ats stay); with
// `clear` all of them (a record copied from another Sim). Nothing when the
// record has no queue that reads, or the queue already says the same.
std::optional<std::vector<uint8_t>> withAnimation(VM& vm, const Object& o, std::span<const uint8_t> bytes, bool clear) {
    const auto queue = readSceneAnimatable(bytes);
    if (!queue)
        return std::nullopt;
    const AnimationState& a = o.animation;
    const float msPerTick = float(1000.0 / vm.clock.ticksPerSecond);
    const int32_t now = int32_t(double(vm.ticks) * 1000.0 / vm.clock.ticksPerSecond);
    auto guidOf = [&](ObjectId id) {
        const Object* x = vm.object(id);
        return x ? x->guid() : 0u;
    };
    std::vector<AnimSimRequest> wanted;
    for (const Overlay& ov : a.overlays) {
        AnimSimRequest r;
        r.id = 0x60000000; // every overlay's in the game's lots
        r.name = ov.name;
        r.level = ov.level;
        r.channel = animChannelName(ov.level, false);
        r.arType = 4;
        r.oeType = uint32_t(ov.oeType);
        r.loops = uint32_t(std::max(ov.loops, 0));
        r.stackObject = uint16_t(ov.stackObject);
        r.eventTree = ov.eventTree;
        r.target = uint32_t(uint16_t(ov.ikObject));
        r.objectGuid = guidOf(ov.codeOwner);
        if (ov.eventTree && ov.me != o.id)
            r.movedObject = uint32_t(uint16_t(ov.me));
        if (r.channel == "obj") {
            // Animate Object's: its event tree's context.
            r.target = 0;
            for (const auto& ev : a.overlayEvents)
                if (ev.name == ov.name) {
                    r.eventTree = ev.eventTree;
                    r.stackObject = uint16_t(ev.stackObject);
                    r.objectGuid = guidOf(ev.codeOwner);
                    r.movedObject = ev.me != o.id ? uint32_t(uint16_t(ev.me)) : 0;
                }
        }
        r.current = now;
        r.length = ov.length;
        r.played = ov.played;
        r.speed = ov.perTick > 0 ? ov.perTick / msPerTick : 1.0f;
        r.heading = ov.headingOffset;
        r.mirrored = ov.flipped;
        wanted.push_back(std::move(r));
    }
    if (auto t = vm.threads.find(o.id); a.playing() && a.requestId && t != vm.threads.end() &&
                                        t->second.status == Thread::Status::Running && !t->second.stack.empty() &&
                                        a.depth == t->second.stack.size() &&
                                        uint32_t(t->second.stack.back().primitiveState) == a.requestId) {
        const Frame& f = t->second.stack.back();
        const auto& operands = f.behavior->instructions[f.pc].operands;
        AnimSimRequest r;
        r.id = a.requestId;
        r.name = a.name;
        r.level = a.level;
        r.stackObject = uint16_t(f.stackObject);
        r.eventTree = a.eventTree;
        // Animate Sim's flags (byte 2): 0x08 interruptible, 0x40 no blend
        // out, 0x80 no blend in.
        r.interruptible = operands[2] & 0x08;
        r.blendOut = !(operands[2] & 0x40);
        r.blendIn = !(operands[2] & 0x80);
        r.target = uint32_t(uint16_t(a.eventObject));
        r.objectGuid = guidOf(f.codeOwner);
        r.current = now;
        r.length = vm.animations.seconds(a.name).value_or(0.0f) * 1000.0f;
        r.played = a.playedTicks;
        r.speed = a.speed;
        r.weight = a.weight;
        r.heading = a.headingOffset;
        r.mirrored = a.flipped;
        wanted.push_back(std::move(r));
    }
    // The same requests as far as they had played (a lot loaded and saved
    // again): the record stays. Animate Sim requests no frame waits on
    // (the game keeps finished ones) do not count.
    if (!clear) {
        std::vector<std::pair<std::string, float>> have, want;
        for (const auto& q : queue->requests)
            if (q.kind == 1 && (!q.blocking() || (a.requestId && q.id == a.requestId)))
                have.emplace_back(q.name, q.played);
        for (const auto& r : wanted)
            want.emplace_back(r.name, r.played);
        if (have == want)
            return std::nullopt;
    }
    std::vector<std::vector<uint8_t>> requests;
    if (!clear)
        for (const auto& q : queue->requests)
            if (q.kind != 1)
                requests.emplace_back(bytes.begin() + std::ptrdiff_t(q.begin), bytes.begin() + std::ptrdiff_t(q.end));
    for (const auto& r : wanted)
        requests.push_back(writeAnimSimRequest(r));
    return withSceneRequests(bytes, *queue, requests, std::max(queue->counter, vm.animationCounter));
}

} // namespace

RecordsResult captureLotRecords(VM& vm, const Lot& world, DbpfPackage& source, const LotObjects& before,
                                LotObjects& lot, const std::vector<std::filesystem::path>& templatePackages) {
    RecordsResult out;
    std::map<uint32_t, const DbpfEntry*> scene, routing;
    std::map<uint32_t, const DbpfEntry*> relationships;
    for (const auto& e : source.entries()) {
        if (e.tgi.type == kTypeSceneObject)
            scene[e.tgi.instance] = &e;
        else if (e.tgi.type == kTypeRouting)
            routing[e.tgi.instance] = &e;
        else if (e.tgi.type == hoodtype::SREL)
            relationships[e.tgi.instance] = &e;
    }
    auto addRecord = [&](uint32_t type, uint32_t group, uint32_t instance, std::vector<uint8_t> bytes) {
        lot.otherRecords.push_back({type, group, instance, std::move(bytes)});
    };

    // Scene graph objects.
    std::map<std::string, std::vector<uint8_t>> templates; // definition name -> a cObject of it
    std::map<std::string, std::vector<uint8_t>> people;    // skeleton -> a cPerson of it
    bool templatesRead = false;
    auto readTemplates = [&] {
        if (templatesRead)
            return;
        templatesRead = true;
        for (const auto& path : templatePackages) {
            try {
                DbpfPackage pkg(path);
                for (const auto& e : pkg.entries())
                    if (e.tgi.type == kTypeSceneObject) {
                        auto bytes = pkg.read(e);
                        if (auto p = personLayout(bytes); p && !p->skeleton.empty())
                            people.emplace(p->skeleton, bytes);
                        if (auto l = sceneLayout(bytes); l && !l->definition.empty())
                            templates.emplace(l->definition, std::move(bytes));
                    }
            } catch (const std::exception& err) {
                os2::logOnce("person templates", path.filename().string(), err);
            }
        }
    };
    auto templateOf = [&](const std::string& definition) -> const std::vector<uint8_t>* {
        readTemplates();
        auto it = templates.find(definition);
        return it == templates.end() ? nullptr : &it->second;
    };
    for (const auto& s : lot.objects) {
        const LotObjects::State* was = before.state(s.id);
        const bool known = was && before.guidOf(s.id) == lot.guidOf(s.id);
        const uint32_t instance = uint32_t(uint16_t(s.id));
        auto it = scene.find(instance);
        // A Sim that arrived on this load (formats/lot_arrivals.hpp) is in
        // `before` but has no cPerson yet: it gets one below.
        const Object* designed = vm.object(s.id);
        const bool redesigned = designed && designed->design;
        if (known && !(s.person && it == scene.end())) {
            if (it == scene.end())
                continue;
            if (!moved(s, *was) && !redesigned) {
                // An object that did not move: its animation queue.
                if (designed)
                    if (auto b = withAnimation(vm, *designed, source.read(*it->second), false)) {
                        addRecord(kTypeSceneObject, it->second->tgi.group, instance, std::move(*b));
                        ++out.animations;
                    }
                continue;
            }
            auto bytes = source.read(*it->second);
            if (redesigned)
                if (auto d = withDesign(bytes, *designed->design))
                    bytes = std::move(*d);
            const auto l = sceneLayout(bytes);
            if (!l)
                continue;
            const auto old = readPose(bytes, l->first);
            if (moved(s, *was))
                writePose(bytes, *l, poseOf(world, s, was, &old));
            if (designed)
                if (auto b = withAnimation(vm, *designed, bytes, false)) {
                    bytes = std::move(*b);
                    ++out.animations;
                }
            addRecord(kTypeSceneObject, it->second->tgi.group, instance, std::move(bytes));
            ++out.sceneMoved;
            continue;
        }
        // A new object: only the lead part of a multi-tile object (or a
        // single-tile one, or a Sim) has a cObject naming its definition.
        const Object* o = vm.object(s.id);
        if (!o || !o->definition || (s.kind == LotObjects::Kind::MultiTile && multiTileGroup(*o) != o->id))
            continue;
        const std::string name = o->definition->fileName + "_objt";
        const std::vector<uint8_t>* t = templateOf(name);
        std::vector<uint8_t> bytes;
        if (t) {
            bytes = *t;
        } else if (s.person) {
            // A Sim's is a cPerson: one of another Sim of its skeleton, made
            // its own (personFromTemplate).
            readTemplates();
            const int16_t age = o->person ? o->person->data[persondata::PersonAge] : int16_t(0);
            std::string skeleton = skeletonForAge(age);
            auto pt = people.find(skeleton);
            if (pt == people.end())
                pt = people.find("auskel");
            if (pt == people.end()) {
                out.warnings.push_back("Sim " + std::to_string(s.id) + ": no cPerson of a " + skeleton + " Sim to copy");
                continue;
            }
            const auto pl = personLayout(pt->second);
            bytes = personFromTemplate(pt->second, *pl, name, poseOf(world, s, nullptr, nullptr));
            // Not the other Sim's animations: its own, if it plays one.
            if (auto b = withAnimation(vm, *o, bytes, true))
                bytes = std::move(*b);
            addRecord(kTypeSceneObject, 0xFFFFFFFF, instance, std::move(bytes));
            ++out.sceneNew;
            ++out.people;
            continue;
        } else {
            // No lot has one (an object bought in Buy mode): a bare cObject
            // as the game writes for objects without an animatable part
            // (364 of N001_Lot11's 834, "Social - Talk - Chat_objt"): no
            // name, class cObject version 0x11, the model with no design
            // choices, the pose, 1, the definition, the pose again, 0.
            // Inferred: the cAnimatable data of animated objects is left out.
            std::string model;
            if (ResourceIndex* index = vm.code().index())
                model = objectCresName(*index, Tgi{kTypeObjd, o->group, o->definitionInstance, 0});
            // A graphic the VM set (data 0: "change graphic") names the model
            // in the object's STR# 0x85 (the stairs' railings).
            if (o->data[0] > 0)
                if (const StringTable* names = vm.code().strings(o->group, 0x85))
                    if (std::string g = names->get(size_t(o->data[0]), lang::EnglishUS); !g.empty())
                        model = g;
            if (model.size() > 5 && model.compare(model.size() - 5, 5, "_cres") == 0)
                model.resize(model.size() - 5);
            BinWriter w;
            w.raw(std::vector<uint8_t>(kFileNameSize, 0));
            w.u32(kTypeSceneObject);
            w.u32(0x11);
            w.str7("cObject");
            w.str7(model);
            w.u32(model.empty() ? 0 : 1);
            if (!model.empty()) {
                w.str7(model);
                w.u32(12);
                w.u32(0);
            }
            for (int k = 0; k < 6; ++k)
                w.f32(0);
            w.f32(1);
            w.u32(1);
            w.str7(name);
            for (int k = 0; k < 6; ++k)
                w.f32(0);
            w.f32(1);
            w.u32(0);
            bytes = std::move(w.bytes());
            out.warnings.push_back("object " + std::to_string(s.id) + ": no cObject of \"" + name +
                                   "\" in the other lots to copy; wrote a bare one");
        }
        if (o->design)
            if (auto d = withDesign(bytes, *o->design))
                bytes = std::move(*d);
        // Not the copied object's animations: its own.
        if (auto b = withAnimation(vm, *o, bytes, true))
            bytes = std::move(*b);
        const auto l = sceneLayout(bytes);
        auto pose = poseOf(world, s, nullptr, nullptr);
        if (o->position.hasHeight()) // Build mode's stair steps: at their own height
            pose[2] = o->position.height;
        writePose(bytes, *l, pose);
        addRecord(kTypeSceneObject, 0xFFFFFFFF, instance, std::move(bytes));
        ++out.sceneNew;
    }

    // The VM's own state of the threads (thread_state.hpp): written for the
    // objects that have some, and the record of an object that no longer has
    // any left out (it would not fit its next stack anyway).
    {
        std::set<uint32_t> had;
        for (const auto& e : source.entries())
            if (e.tgi.type == kThreadStateType)
                had.insert(e.tgi.instance);
        addRecord(kThreadStateType, 0xFFFFFFFF, 0, writeLotState(vm));
        for (const auto& s : lot.objects) {
            const uint32_t instance = uint32_t(uint16_t(s.id));
            const Object* o = vm.object(s.id);
            if (auto bytes = o ? writeThreadState(vm, *o) : std::nullopt) {
                addRecord(kThreadStateType, 0xFFFFFFFF, instance, std::move(*bytes));
                ++out.threadStates;
            } else if (had.count(instance)) {
                lot.deletedRecords.push_back({kThreadStateType, 0xFFFFFFFF, instance, {}});
            }
        }
    }

    // Routing states of new Sims: idle, as the lot's own idle ones (else the
    // base game's version 4 with nothing set).
    std::vector<uint8_t> idle;
    for (const auto& [_, e] : routing) {
        auto bytes = source.read(*e);
        // Idle: after the class name, only zeros.
        BinReader r(bytes);
        try {
            r.skip(kFileNameSize + 8);
            r.str7();
            bool zeros = r.remaining() > 0;
            for (size_t i = r.pos(); i < bytes.size(); ++i)
                zeros = zeros && bytes[i] == 0;
            if (zeros) {
                idle = std::move(bytes);
                break;
            }
        } catch (const FormatError&) {
        }
    }
    if (idle.empty()) {
        BinWriter w;
        w.raw(std::vector<uint8_t>(kFileNameSize, 0));
        w.u32(kTypeRouting);
        w.u32(4);
        w.str7("cTSRoutingObjectAggregate");
        w.raw(std::vector<uint8_t>(12, 0));
        idle = std::move(w.bytes());
    }
    for (const auto& s : lot.objects)
        if (s.kind == LotObjects::Kind::Person && !routing.count(uint32_t(uint16_t(s.id)))) {
            addRecord(kTypeRouting, 0xFFFFFFFF, uint32_t(uint16_t(s.id)), idle);
            ++out.routing;
        }

    // Relationships: the lot's SRELs become exactly the VM's relationships
    // with a lot object on one side at least, keyed as loadLot keys them
    // (two values: ownership, "to" a neighbor ID; otherwise object IDs on
    // both sides, a Sim by its neighbor ID). Unchanged records stay as
    // they are; records whose relationship the VM no longer has go.
    using Key = std::pair<VM::RelationshipKey, VM::RelationshipKey>;
    auto isSim = [&](int16_t id) {
        const Object* o = vm.object(id);
        return o && o->person;
    };
    auto trimmed = [](std::vector<int32_t> v) {
        while (!v.empty() && v.back() == 0)
            v.pop_back();
        return v;
    };
    std::map<Key, std::vector<uint32_t>> instancesOf;
    std::map<uint32_t, hood::Relationship> read;
    for (const auto& [instance, e] : relationships) {
        try {
            auto rel = hood::Relationship::parse(source.read(*e), instance);
            const auto from = int16_t(rel.from), to = int16_t(rel.to);
            Key k;
            if (rel.values.size() == 2) {
                k = {{false, from}, {true, to}};
            } else {
                if (isSim(from) && isSim(to))
                    continue; // the neighborhood's
                k = {{false, from}, {false, to}};
            }
            instancesOf[k].push_back(instance);
            read.emplace(instance, std::move(rel));
        } catch (const std::exception& err) {
            os2::logOnce("lot relationships", std::to_string(instance), err);
        }
    }
    auto objectIdOf = [&](const VM::RelationshipKey& k) -> int16_t {
        if (!k.neighbor)
            return k.id;
        const Object* sim = vm.neighborSim(k.id);
        return sim ? int16_t(sim->id) : int16_t(0);
    };
    std::set<uint32_t> kept;
    // First the relationships the lot had (their records), then new ones.
    for (int pass = 0; pass < 2; ++pass)
    for (const auto& [k, v16] : vm.relationships) {
        if (k.first.neighbor && k.second.neighbor)
            continue;
        std::vector<int32_t> values(v16.begin(), v16.end());
        if (trimmed(values).empty())
            continue;
        auto it = instancesOf.find(k);
        if ((it != instancesOf.end()) != (pass == 0))
            continue;
        if (it != instancesOf.end()) {
            for (uint32_t instance : it->second) {
                kept.insert(instance);
                const auto& rel = read.at(instance);
                std::vector<int32_t> out32;
                for (size_t i = 0; i < values.size(); ++i)
                    out32.push_back(i < rel.values.size() && int16_t(rel.values[i]) == values[i] ? rel.values[i]
                                                                                              : values[i]);
                // The VM keeps at most 32 variables: the rest stay.
                for (size_t i = out32.size(); i < rel.values.size() && values.size() >= kMaxRelationshipVars; ++i)
                    out32.push_back(rel.values[i]);
                // As many as were saved while only zeros differ; the count
                // tells ownership (2) from the rest.
                if (trimmed(out32) == trimmed(rel.values))
                    continue;
                const bool ownership = rel.values.size() == 2;
                if (ownership)
                    out32.resize(2, 0);
                else if (out32.size() == 2)
                    out32.push_back(0);
                addRecord(hoodtype::SREL, relationships.at(instance)->tgi.group, instance,
                          writeRelationship(rel.unknown, out32, rel.stale));
                ++out.relationshipsChanged;
            }
            continue;
        }
        // Sim-to-Sim ones keyed by object (a Sim with no neighbor ID)
        // would read back as the neighborhood's: not saved here.
        if (!k.first.neighbor && !k.second.neighbor && isSim(k.first.id) && isSim(k.second.id))
            continue;
        const int16_t from = objectIdOf(k.first);
        const bool ownership = k.second.neighbor;
        const int16_t to = ownership ? k.second.id : objectIdOf(k.second);
        if (from <= 0 || to <= 0)
            continue;
        if (ownership)
            values.resize(2, 0);
        else if (values.size() == 2)
            values.push_back(0);
        const uint32_t instance = uint32_t(uint16_t(from)) << 16 | uint16_t(to);
        if (kept.count(instance)) {
            out.warnings.push_back("relationship " + std::to_string(from) + " -> " + std::to_string(to) +
                                   ": an object one and an ownership one share the record; one kept");
            continue;
        }
        kept.insert(instance);
        addRecord(hoodtype::SREL, 0xFFFFFFFF, instance, writeRelationship(2, values, {}));
        ++out.relationshipsNew;
    }
    // (Records of only zeros mean what no record means: they stay.)
    for (const auto& [instance, rel] : read)
        if (!kept.count(instance) && !trimmed(rel.values).empty()) {
            lot.deletedRecords.push_back({hoodtype::SREL, relationships.at(instance)->tgi.group, instance, {}});
            ++out.relationshipsGone;
        }
    return out;
}

} // namespace os2::vm
