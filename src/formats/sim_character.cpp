#include "sim_character.hpp"

#include "dbpf.hpp"
#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "skeleton.hpp"
#include "txmt.hpp"

#include <cctype>

namespace os2 {

namespace {

constexpr uint32_t kTypeAged = 0xAC598EAC, kTypeIdr = 0xAC506764, kTypeGzps = 0xEBCF3E27, kTypeTxmt = 0x49596978,
                   kTypeCres = 0xE519C933;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

uint32_t number(const std::map<std::string, std::string>& p, const std::string& key, uint32_t fallback = 0) {
    auto it = p.find(key);
    if (it == p.end() || it->second.empty())
        return fallback;
    try {
        return uint32_t(std::stoul(it->second, nullptr, 0));
    } catch (const std::logic_error&) {
        return fallback;
    }
}

std::optional<std::vector<uint8_t>> readLocal(DbpfPackage& pkg, uint32_t type, uint32_t instance) {
    for (const auto& e : pkg.entries())
        if (e.tgi.type == type && e.tgi.instance == instance)
            return pkg.read(e);
    return std::nullopt;
}

// "#0x7f861bc1!ss_customFace_..._txmt" -> "ss_customface_..."
std::string bareName(std::string name) {
    if (auto bang = name.find('!'); bang != std::string::npos && name.starts_with("#"))
        name = name.substr(bang + 1);
    if (name.ends_with("_txmt"))
        name.resize(name.size() - 5);
    return lower(name);
}

bool sameKey(const Tgi& a, const Tgi& b) { return a.type == b.type && a.group == b.group && a.instance == b.instance; }

} // namespace

std::map<uint32_t, Tgi> Character::outfit(uint32_t category) const {
    std::map<uint32_t, Tgi> out;
    for (const auto& [key, tgis] : outfits)
        if ((key >> 16) & category)
            if (!tgis.empty())
                out.emplace(key & 0xFFFF, tgis.front());
    return out;
}

std::map<uint32_t, Tgi> Character::wear(uint32_t category) const {
    std::map<uint32_t, Tgi> out = outfit(category);
    if (out.empty())
        return outfit(1);
    if (category != 1)
        for (const auto& [bits, tgi] : outfit(1))
            if (bits == 1 || bits == 2) // hair, face
                out.emplace(bits, tgi);
    return out;
}

std::string Character::prefix(bool unisexChildren) const {
    std::string p;
    if (age & 1)
        p = "p";
    else if (age & 2)
        p = "c";
    else if (age & 4)
        p = "t";
    else if (age & 16)
        p = "e";
    else if (age & 32)
        p = "b";
    else
        p = "a";
    if (unisexChildren && (p == "p" || p == "c" || p == "b"))
        return p + "u";
    return p + (gender & 1 ? "f" : "m");
}

Character readCharacter(DbpfPackage& pkg, ResourceIndex& index) {
    Character c;
    c.file = pkg.path();
    const auto aged = readLocal(pkg, kTypeAged, 1);
    const auto idr = readLocal(pkg, kTypeIdr, 1);
    if (!aged || !idr)
        throw FormatError(c.file.filename().string() + ": no AGED/3IDR 1");
    const auto p = readPropertySet(*aged);
    c.references = parseReferenceList(*idr);
    c.age = number(p, "age", 8);
    c.gender = number(p, "gender", 2);
    if (auto it = p.find("skincolor"); it != p.end() && !it->second.empty())
        c.skinTone = int(std::stoul(it->second.substr(0, 8), nullptr, 16));
    if (p.count("skeletonkeyidx")) {
        const uint32_t i = number(p, "skeletonkeyidx");
        if (i < c.references.size()) {
            // The reference leaves the high instance empty: match the
            // instance against the skeletons' name hashes.
            for (const char* name : {"auskel", "cuskel", "puskel", "buskel"})
                if (nameInstanceHash(std::string(name) + "_cres") == c.references[i].instance)
                    c.skeleton = name;
        }
    }
    const uint32_t n = number(p, "listcnt");
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t key = number(p, "lk" + std::to_string(i));
        const uint32_t count = number(p, "ls" + std::to_string(i), 1);
        for (uint32_t k = 0; k < count; ++k) {
            const std::string le = "le" + std::to_string(i) + "_" + std::to_string(k);
            if (!p.count(le))
                continue;
            const uint32_t ref = number(p, le);
            if (ref < c.references.size())
                c.outfits[key].push_back(c.references[ref]);
        }
    }
    return c;
}

SkinnedModel loadCharacterSim(ResourceIndex& base, const std::vector<SimPart>& parts,
                              const std::filesystem::path& file, uint32_t category) {
    ResourceIndex index({file}, &base);
    DbpfPackage pkg(file);
    const Character ch = readCharacter(pkg, index);

    auto cresLoc = index.findByName(ch.skeleton + "_cres", kTypeCres);
    if (!cresLoc)
        throw FormatError(ch.skeleton + "_cres not found");
    const auto cresBytes = index.read(*cresLoc);
    SkinnedModel sim(Skeleton::fromCres(Rcol(cresBytes)));
    sim.model.name = file.stem().string();
    const std::string tone = "s" + std::to_string(ch.skinTone);
    const std::string skin = ch.prefix(true) + "bodynaked-nude-" + tone + "_txtr";

    // The character file's own parts (face) and their 3IDRs.
    std::map<uint32_t, SimPart> local;
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeGzps)
            continue;
        SimPart sp;
        sp.tgi = e.tgi;
        try {
            sp.properties = readPropertySet(pkg.read(e));
            if (auto idr = readLocal(pkg, kTypeIdr, e.tgi.instance))
                sp.references = parseReferenceList(*idr);
        } catch (const FormatError&) {
            continue;
        }
        local.emplace(e.tgi.instance, std::move(sp));
    }
    // TXMTs of the character file by bare name.
    std::map<std::string, std::string> localMaterials;
    for (const auto& loc : index.ofType(kTypeTxmt))
        if (const std::string n = scenegraphName(index, loc.entry->tgi); !n.empty())
            localMaterials.emplace(bareName(n), n.ends_with("_txmt") ? n.substr(0, n.size() - 5) : n);

    for (const auto& [bits, tgi] : ch.wear(category)) {
        const SimPart* part = nullptr;
        if (tgi.group == 0xFFFFFFFF) {
            if (auto it = local.find(tgi.instance); it != local.end())
                part = &it->second;
        } else {
            for (const auto& sp : parts)
                if (sameKey(sp.tgi, tgi)) {
                    part = &sp;
                    break;
                }
        }
        if (!part) {
            sim.model.warnings.push_back("part " + hexString(tgi.group) + ":" + hexString(tgi.instance) + " (" +
                                         std::to_string(bits) + ") not found");
            continue;
        }
        try {
            ObjectModel m = loadSimPartModel(index, *part);
            if (bits == 2) {
                // The face: its materials are in the character file.
                std::map<std::string, std::string> overrides;
                for (const auto& p : m.parts)
                    if (auto it = localMaterials.find(lower(p.material)); it != localMaterials.end())
                        overrides[p.mesh.name] = it->second;
                if (!overrides.empty()) {
                    const auto cres = part->reference("resourcekeyidx");
                    m = loadObjectModel(index, scenegraphName(index, *cres), overrides);
                }
                for (auto& p : m.parts) {
                    auto it = m.materials.find(p.material);
                    if (it == m.materials.end())
                        continue;
                    const uint32_t layers = uint32_t(std::atoi(it->second.property("numTexturesToComposite").c_str()));
                    for (uint32_t i = 1; i < layers; ++i)
                        if (auto t = it->second.property("baseTexture" + std::to_string(i)); !t.empty())
                            p.overlays.push_back(t + "_txtr");
                }
            } else {
                for (auto& p : m.parts) {
                    auto it = m.materials.find(p.material);
                    if (it != m.materials.end() && it->second.materialType == "SimSkin")
                        p.underlay = skin;
                }
            }
            sim.add(m);
        } catch (const std::exception& e) {
            sim.model.warnings.push_back(part->name() + ": " + e.what());
        }
    }
    sim.model.name = file.stem().string();
    return sim;
}

} // namespace os2
