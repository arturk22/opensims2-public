#include "sim.hpp"

#include "binreader.hpp"
#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "scenegraph.hpp"
#include "skeleton.hpp"
#include "txmt.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace os2 {

namespace {

constexpr uint32_t kTypeGzps = 0xEBCF3E27, kTypeIdr = 0xAC506764;

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::optional<ResourceIndex::Location> findTgi(ResourceIndex& index, Tgi tgi) {
    if (auto loc = index.find(tgi))
        return loc;
    tgi.instanceHi = 0;
    return index.find(tgi);
}

} // namespace

std::vector<Tgi> parseReferenceList(std::span<const uint8_t> data) {
    BinReader r(data);
    if (r.u32() != 0xDEADBEEF)
        throw FormatError("3IDR: bad magic");
    const uint32_t version = r.u32();
    if (version != 1 && version != 2)
        throw FormatError("3IDR: unsupported version " + std::to_string(version));
    const uint32_t n = r.u32();
    if (n > 0x10000)
        throw FormatError("3IDR: bad count");
    std::vector<Tgi> out(n);
    for (auto& t : out) {
        t.type = r.u32();
        t.group = r.u32();
        t.instance = r.u32();
        if (version == 2)
            t.instanceHi = r.u32();
    }
    r.expectEnd("3IDR");
    return out;
}

std::string SimPart::property(const std::string& key) const {
    auto it = properties.find(key);
    return it == properties.end() ? std::string() : it->second;
}

uint32_t SimPart::number(const std::string& key) const {
    const auto v = property(key);
    try {
        return v.empty() ? 0 : uint32_t(std::stoul(v, nullptr, 0));
    } catch (const std::logic_error&) {
        return 0;
    }
}

std::optional<Tgi> SimPart::reference(const std::string& indexKey) const {
    if (!properties.count(indexKey))
        return std::nullopt;
    const uint32_t i = number(indexKey);
    if (i >= references.size())
        return std::nullopt;
    return references[i];
}

namespace {

// Runs body(i) for i in [0, n) on up to 8 threads (index reads through
// ResourceIndex::readConcurrent only). An exception in any thread stops the
// others taking more work; every thread is joined and the first exception is
// rethrown here (thrown in a worker it called std::terminate, and one thrown
// on this thread destroyed the joinable workers, which terminates too).
template <class F>
void parallelFor(size_t n, F body) {
    const size_t threads = n < 64 ? 1 : std::min<size_t>(8, std::max(1u, std::thread::hardware_concurrency()));
    std::atomic<size_t> next{0};
    std::mutex errorLock;
    std::exception_ptr error;
    auto run = [&] {
        try {
            for (size_t i; (i = next.fetch_add(1)) < n;)
                body(i);
        } catch (...) {
            next.store(n); // the others stop at their next item
            std::lock_guard lock(errorLock);
            if (!error)
                error = std::current_exception();
        }
    };
    std::vector<std::thread> pool;
    try {
        for (size_t t = 1; t < threads; ++t)
            pool.emplace_back(run);
    } catch (...) { // no thread to be had: this one does the rest
    }
    run();
    for (auto& t : pool)
        t.join();
    if (error)
        std::rethrow_exception(error);
}

} // namespace

// Every GZPS with a 3IDR, in package order. The reading and parsing (about
// 0.4 s in one thread for the base game) runs on several threads; the result
// is the same as reading them in turn.
std::vector<SimPart> loadSimParts(ResourceIndex& index) {
    // Only the 3IDRs of a GZPS matter (a quarter of the 26,000 in the game).
    std::unordered_set<Tgi, TgiHash> gzpsKeys;
    const auto allGzps = index.ofType(kTypeGzps);
    for (const auto& loc : allGzps)
        gzpsKeys.insert(loc.entry->tgi);
    std::vector<ResourceIndex::Location> idrs;
    for (const auto& loc : index.ofType(kTypeIdr)) {
        Tgi key = loc.entry->tgi;
        key.type = kTypeGzps;
        if (gzpsKeys.count(key))
            idrs.push_back(loc);
    }
    std::vector<std::optional<std::vector<Tgi>>> refs(idrs.size());
    parallelFor(idrs.size(), [&](size_t i) {
        try {
            refs[i] = parseReferenceList(ResourceIndex::readConcurrent(idrs[i]));
        } catch (const FormatError&) {
        }
    });
    std::unordered_map<Tgi, std::vector<Tgi>, TgiHash> lists;
    for (size_t i = 0; i < idrs.size(); ++i)
        if (refs[i]) {
            Tgi key = idrs[i].entry->tgi;
            key.type = kTypeGzps;
            lists.emplace(key, std::move(*refs[i]));
        }
    std::vector<ResourceIndex::Location> gzps;
    std::vector<const std::vector<Tgi>*> gzpsRefs;
    for (const auto& loc : allGzps)
        if (auto it = lists.find(loc.entry->tgi); it != lists.end()) {
            gzps.push_back(loc);
            gzpsRefs.push_back(&it->second);
        }
    std::vector<std::optional<SimPart>> parts(gzps.size());
    parallelFor(gzps.size(), [&](size_t i) {
        SimPart p;
        p.tgi = gzps[i].entry->tgi;
        try {
            p.properties = readPropertySet(ResourceIndex::readConcurrent(gzps[i]));
        } catch (const FormatError&) {
            return;
        }
        p.references = *gzpsRefs[i];
        parts[i] = std::move(p);
    });
    std::vector<SimPart> out;
    out.reserve(parts.size());
    for (auto& p : parts)
        if (p)
            out.push_back(std::move(*p));
    return out;
}

const SimPart* findSimPart(const std::vector<SimPart>& parts, std::string_view name) {
    for (const auto& p : parts)
        if (equalsNoCase(p.name(), name))
            return &p;
    return nullptr;
}

std::string scenegraphName(ResourceIndex& index, const Tgi& tgi) {
    auto loc = findTgi(index, tgi);
    if (!loc)
        return {};
    const auto bytes = index.read(*loc);
    Rcol rc(bytes);
    if (rc.blocks.empty())
        return {};
    const auto& b = rc.blocks[0];
    if (b.className == "cResourceNode") {
        const auto n = ResourceNode::parse(b.body);
        return n.typeCode == 1 ? n.resource.fileName : n.graph.fileName;
    }
    if (b.className == "cMaterialDefinition")
        return MaterialDefinition::parse(b.body).resource.fileName;
    if (b.className == "cShape")
        return Shape::parse(b.body).resource.fileName;
    return {};
}

ObjectModel loadSimPartModel(ResourceIndex& index, const SimPart& part) {
    const auto cres = part.reference("resourcekeyidx");
    if (!cres)
        throw FormatError("Sim part " + part.name() + " has no CRES reference");
    const std::string cresName = scenegraphName(index, *cres);
    if (cresName.empty())
        throw FormatError("Sim part " + part.name() + ": CRES not found");
    std::map<std::string, std::string> materials;
    const uint32_t n = part.number("numoverrides");
    for (uint32_t i = 0; i < n; ++i) {
        const std::string prefix = "override" + std::to_string(i);
        const auto subset = part.property(prefix + "subset");
        const auto txmt = part.reference(prefix + "resourcekeyidx");
        if (subset.empty() || !txmt)
            continue;
        std::string name = scenegraphName(index, *txmt);
        if (name.ends_with("_txmt"))
            name.resize(name.size() - 5);
        if (!name.empty())
            materials[subset] = name;
    }
    return loadObjectModel(index, cresName, materials);
}

void markHairPart(ObjectModel& model) {
    for (auto& [name, mat] : model.materials) {
        if (mat.materialType == "SimStandardMaterial")
            mat.materialType = "SimHair";
        mat.properties.emplace_back("os2HairPart", "1");
    }
}

SkinnedModel loadSim(ResourceIndex& index, const std::vector<SimPart>& parts, const SimLook& look) {
    constexpr uint32_t kTypeCres = 0xE519C933;
    auto cresLoc = index.findByName("auskel_cres", kTypeCres);
    if (!cresLoc)
        throw FormatError("auskel_cres not found");
    const auto cresBytes = index.read(*cresLoc);
    SkinnedModel sim(Skeleton::fromCres(Rcol(cresBytes)));
    const std::string tone = "s" + std::to_string(look.skinTone);

    // Body: clothes are SimSkin materials whose transparent texels show the
    // naked body's skin texture (same UV layout for every adult body).
    if (const SimPart* body = findSimPart(parts, look.body)) {
        ObjectModel m = loadSimPartModel(index, *body);
        for (auto& part : m.parts) {
            auto it = m.materials.find(part.material);
            if (it != m.materials.end() && it->second.materialType == "SimSkin")
                part.underlay = look.prefix + "bodynaked-nude-" + tone + "_txtr";
        }
        sim.add(m);
    } else {
        sim.model.warnings.push_back("body part not found: " + look.body);
    }

    // Face: the base head with the skin tone's face texture.
    {
        const std::string face = look.prefix + "face_" + tone;
        ObjectModel m = loadObjectModel(index, look.prefix + "Face", {{"face", face}});
        for (auto& part : m.parts) {
            if (!look.eyes.empty())
                part.overlays.push_back(look.eyes + "_txtr");
            if (!look.brows.empty())
                part.overlays.push_back(look.brows + "_txtr");
        }
        sim.add(m);
    }

    if (!look.hair.empty()) {
        if (const SimPart* hair = findSimPart(parts, look.hair)) {
            ObjectModel m = loadSimPartModel(index, *hair);
            markHairPart(m);
            // The scalp showing through the hair's skin layer: the skin's colour.
            for (auto& part : m.parts) {
                auto it = m.materials.find(part.material);
                if (it != m.materials.end() && it->second.materialType == "SimSkin") {
                    part.underlay = look.prefix + "bodynaked-nude-" + tone + "_txtr";
                    part.underlayFlat = true;
                }
            }
            sim.add(m);
        }
        else
            sim.model.warnings.push_back("hair part not found: " + look.hair);
    }
    sim.model.name = look.body;
    return sim;
}

} // namespace os2
