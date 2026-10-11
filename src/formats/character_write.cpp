#include "character_write.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"
#include "dbpf.hpp"
#include "lxnr.hpp"
#include "property_set.hpp"
#include "strings.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "scenegraph.hpp"
#include "simantics.hpp"
#include "txmt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <random>

namespace os2::cas {

namespace {

constexpr uint32_t kTypeAged = 0xAC598EAC, kTypeIdr = 0xAC506764, kTypeGzps = 0xEBCF3E27, kTypeCres = 0xE519C933,
                   kTypeShpe = 0xFC6EB1F7, kTypeGmnd = 0x7BA3838C, kTypeGmdc = 0xAC4F8687, kTypeTxmt = 0x49596978,
                   kTypeObjd = 0x4F424A44, kTypeCtss = 0x43545353, kTypeVersion = 0xEBFEE342;
constexpr uint32_t kTemplatePerson = 0x7FD96B54;
constexpr uint32_t kLocal = 0xFFFFFFFF;
constexpr const char* kPrefix = "#0x7fe09489!";

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c;
        }
    }
    return o;
}

Tgi localTgi(uint32_t type, const std::string& bareName) {
    return Tgi{type, kLocal, nameInstanceHash(bareName), nameInstanceHiHash(bareName)};
}

std::optional<ResourceIndex::Location> findAny(ResourceIndex& index, Tgi t) {
    if (auto loc = index.find(t))
        return loc;
    t.instanceHi = 0;
    return index.find(t);
}

// Replaces every length-prefixed (str7, one length byte) occurrence of
// `from` in `body` with `to`.
std::vector<uint8_t> replaceNames(std::span<const uint8_t> body, const std::map<std::string, std::string>& names) {
    std::vector<uint8_t> out;
    out.reserve(body.size() + 64);
    for (size_t i = 0; i < body.size();) {
        bool done = false;
        for (const auto& [from, to] : names) {
            const size_t n = from.size();
            if (n == 0 || n > 127 || i + 1 + n > body.size() || body[i] != n)
                continue;
            if (std::equal(from.begin(), from.end(), body.begin() + long(i) + 1, [](char a, uint8_t b) {
                    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(b);
                })) {
                BinWriter w;
                w.str7(to);
                out.insert(out.end(), w.bytes().begin(), w.bytes().end());
                i += 1 + n;
                done = true;
                break;
            }
        }
        if (!done)
            out.push_back(body[i++]);
    }
    return out;
}

// An RCOL with new links and the names of its blocks replaced.
std::vector<uint8_t> rewriteRcol(std::span<const uint8_t> bytes, const std::vector<Tgi>& links,
                                 const std::map<std::string, std::string>& names) {
    Rcol rc(bytes);
    BinWriter w;
    w.u32(0xFFFF0001);
    w.u32(uint32_t(links.size()));
    for (const auto& l : links) {
        w.u32(l.group);
        w.u32(l.instance);
        w.u32(l.instanceHi);
        w.u32(l.type);
    }
    w.u32(uint32_t(rc.blocks.size()));
    for (const auto& b : rc.blocks)
        w.u32(b.typeId);
    for (const auto& b : rc.blocks) {
        w.str7(b.className);
        w.u32(b.typeId);
        w.raw(replaceNames(b.body, names));
    }
    return std::move(w.bytes());
}

std::vector<uint8_t> writeReferenceList(const std::vector<Tgi>& refs) {
    BinWriter w;
    w.u32(0xDEADBEEF);
    w.u32(2);
    w.u32(uint32_t(refs.size()));
    for (const auto& t : refs) {
        w.u32(t.type);
        w.u32(t.group);
        w.u32(t.instance);
        w.u32(t.instanceHi);
    }
    return std::move(w.bytes());
}

// CTSS / STR# with (language, value, description) entries (format 0xFFFD).
std::vector<uint8_t> writeStrings(const std::vector<std::string>& values) {
    BinWriter w;
    for (int i = 0; i < 64; ++i)
        w.u8(0);
    w.u16(0xFFFD);
    w.u16(uint16_t(values.size()));
    for (const auto& v : values) {
        w.u8(1); // US English, the language every other one falls back to
        w.raw(std::span(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
        w.u8(0);
        w.u8(0); // empty description
    }
    return std::move(w.bytes());
}

std::vector<uint8_t> writeMaterial(const MaterialDefinition& m, uint32_t sgBlockId, uint32_t sgVersion,
                                   const std::string& name) {
    BinWriter body;
    body.u32(m.version);
    body.str7("cSGResource");
    body.u32(sgBlockId);
    body.u32(sgVersion);
    body.str7(name);
    body.str7(m.description);
    body.str7(m.materialType);
    body.u32(uint32_t(m.properties.size()));
    for (const auto& [k, v] : m.properties) {
        body.str7(k);
        body.str7(v);
    }
    if (m.version > 8) {
        body.u32(uint32_t(m.textureNames.size()));
        for (const auto& t : m.textureNames)
            body.str7(t);
    }
    BinWriter w;
    w.u32(0xFFFF0001);
    w.u32(0);
    w.u32(1);
    w.u32(kTypeTxmt);
    w.str7("cMaterialDefinition");
    w.u32(kTypeTxmt);
    w.raw(body.bytes());
    return std::move(w.bytes());
}

void setProperty(MaterialDefinition& m, const std::string& key, const std::string& value) {
    for (auto& [k, v] : m.properties)
        if (k == key) {
            v = value;
            return;
        }
    m.properties.emplace_back(key, value);
}

PropertyValue u32v(uint32_t v) { return {PropertyValue::Uint32, std::to_string(v)}; }
PropertyValue strv(const std::string& v) { return {PropertyValue::String, v}; }

const SimPart* partByName(const CasCatalog& c, const std::string& name) {
    const std::string n = lower(name);
    for (const auto& p : c.parts)
        if (lower(p.name()) == n)
            return &p;
    return nullptr;
}

} // namespace

CharacterSpec readCharacterSpec(DbpfPackage& pkg, uint32_t group, const CasCatalog& catalog) {
    auto read = [&](uint32_t type, uint32_t instance) -> std::optional<std::vector<uint8_t>> {
        for (const auto& e : pkg.entries())
            if (e.tgi.type == type && e.tgi.group == group && e.tgi.instance == instance)
                return pkg.read(e);
        return std::nullopt;
    };
    const auto agedBytes = read(kTypeAged, 1);
    if (!agedBytes)
        throw FormatError("no AGED");
    const auto aged = readPropertySet(*agedBytes);
    std::vector<Tgi> refs;
    if (auto idr = read(kTypeIdr, 1))
        refs = parseReferenceList(*idr);
    auto num = [&](const std::string& k, uint32_t fallback) -> uint32_t {
        auto it = aged.find(k);
        if (it == aged.end() || it->second.empty())
            return fallback;
        try {
            return uint32_t(std::stoul(it->second, nullptr, 0));
        } catch (const std::logic_error&) {
            return fallback;
        }
    };
    auto str = [&](const std::string& k) {
        auto it = aged.find(k);
        std::string v = it == aged.end() ? "" : it->second;
        for (auto& c : v)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    // The game leaves a mesh overlay's GZPS without its family: find the
    // catalog's overlay by the materials of the GZPS's shape (the overlay's
    // TXMT references, by name hash).
    auto meshOverlayOfGzps = [&](uint32_t instance) -> std::string {
        auto idr = read(kTypeIdr, instance);
        if (!idr)
            return {};
        const auto gr = parseReferenceList(*idr);
        if (gr.empty())
            return {};
        std::vector<uint32_t> mats;
        try {
            for (const auto& e : pkg.entries()) {
                if (e.tgi.type != kTypeCres || e.tgi.group != group || e.tgi.instance != gr[0].instance ||
                    e.tgi.instanceHi != gr[0].instanceHi)
                    continue;
                const auto cb = pkg.read(e);
                Rcol cr(cb);
                for (const auto& l : cr.links) {
                    if (l.type != kTypeShpe)
                        continue;
                    for (const auto& se : pkg.entries())
                        if (se.tgi.type == kTypeShpe && se.tgi.instance == l.instance && se.tgi.instanceHi == l.instanceHi) {
                            const auto sb = pkg.read(se);
                            Rcol sr(sb);
                            if (const auto* b = sr.find("cShape"))
                                for (const auto& part : Shape::parse(b->body).parts)
                                    mats.push_back(nameInstanceHash(part.material + "_txmt"));
                        }
                }
                break;
            }
        } catch (const std::exception&) {
            return {};
        }
        if (mats.empty())
            return {};
        for (const auto& o : catalog.meshOverlays) {
            size_t hit = 0;
            for (const auto& ref : o.references)
                if (ref.type == kTypeTxmt && std::find(mats.begin(), mats.end(), ref.instance) != mats.end())
                    ++hit;
            if (hit == mats.size()) {
                std::string f = family(o);
                return f;
            }
        }
        return {};
    };
    CharacterSpec spec;
    spec.age = num("age", age::Adult);
    spec.gender = num("gender", gender::Female);
    spec.fitness = int(num("fitness", 0));
    if (int t = toneNumber(str("skincolor")))
        spec.skinTone = std::clamp(t, 1, 4);
    if (int t = toneNumber(str("haircolor")))
        spec.hairTone = std::clamp(t, 1, 5);
    spec.eyes = str("eyecolor");
    // Outfits: lk<i> = category << 16 | part bits, le<i>_0 = the GZPS's 3IDR index.
    const uint32_t n = num("listcnt", 0);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t lk = num("lk" + std::to_string(i), 0);
        const uint32_t idx = num("le" + std::to_string(i) + "_0", 0xFFFFFFFF);
        const uint32_t cat = lk >> 16, bits = lk & 0xFFFF;
        if (bits == part::Face) {
            // The mesh overlays' GZPS (le<i>_1...): the family we write in them.
            for (uint32_t k = 1; k < num("ls" + std::to_string(i), 1); ++k) {
                const uint32_t r = num("le" + std::to_string(i) + "_" + std::to_string(k), 0xFFFFFFFF);
                if (r >= refs.size() || refs[r].type != kTypeGzps)
                    continue;
                if (auto g = read(kTypeGzps, refs[r].instance)) {
                    const auto gp = readPropertySet(*g);
                    std::string fam;
                    if (auto f = gp.find("family"); f != gp.end())
                        fam = f->second;
                    for (auto& c : fam)
                        c = char(std::tolower(static_cast<unsigned char>(c)));
                    if (!catalog.byFamily(catalog.meshOverlays, fam))
                        fam = meshOverlayOfGzps(refs[r].instance);
                    if (!fam.empty() &&
                        std::find(spec.meshOverlays.begin(), spec.meshOverlays.end(), fam) == spec.meshOverlays.end())
                        spec.meshOverlays.push_back(fam);
                }
            }
            continue;
        }
        if (idx >= refs.size() || !cat)
            continue;
        const Tgi& t = refs[idx];
        for (const auto& p : catalog.parts)
            if (p.tgi.type == t.type && p.tgi.group == t.group && p.tgi.instance == t.instance) {
                // One entry per part bit (hair, top, bottom, body): the lowest
                // category bit set (the lists name a part for several).
                for (uint32_t c = 1; c <= 0x800; c <<= 1)
                    if (cat & c)
                        spec.outfits[c][bits] = p.name();
                break;
            }
    }
    // The face TXMT: the overlays it composites.
    for (const auto& e : pkg.entries()) {
        if (e.tgi.type != kTypeTxmt || e.tgi.group != group)
            continue;
        try {
            const auto bytes = pkg.read(e);
            Rcol rc(bytes);
            const auto* b = rc.find("cMaterialDefinition");
            if (!b)
                continue;
            const auto md = MaterialDefinition::parse(b->body);
            const std::string cnt = md.property("cafNumOverlays");
            if (cnt.empty())
                continue;
            for (int k = 0; k < std::atoi(cnt.c_str()); ++k) {
                std::string f = md.property("cafOverlay" + std::to_string(k));
                for (auto& c : f)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                if (f.empty() || f == spec.eyes ||
                    std::find(spec.overlays.begin(), spec.overlays.end(), f) != spec.overlays.end())
                    continue;
                if (catalog.byFamily(catalog.textureOverlays, f))
                    spec.overlays.push_back(f);
            }
            break;
        } catch (const std::exception&) {
        }
    }
    if (auto lx = read(kTypeLxnr, 1)) {
        try {
            spec.face.structure = std::make_shared<FaceStructure>(FaceStructure::parse(*lx));
        } catch (const std::exception&) {
        }
    }
    if (auto ctss = read(kTypeCtss, 0x7D0)) {
        try {
            const auto st = StringTable::parse(*ctss);
            spec.firstName = st.get(0);
            spec.description = st.get(1);
            spec.lastName = st.get(2);
        } catch (const std::exception&) {
        }
    }
    return spec;
}

uint32_t nextAge(uint32_t a) {
    switch (a) {
    case age::Baby: return age::Toddler;
    case age::Toddler: return age::Child;
    case age::Child: return age::Teen;
    case age::Teen: return age::Adult;
    case age::Adult: return age::Elder;
    default: return 0;
    }
}

int editorAgeIndex(uint32_t a) {
    switch (a) {
    case age::Toddler: return 0;
    case age::Child: return 1;
    case age::Teen: return 2;
    case age::Adult: return 3;
    case age::Elder: return 4;
    case age::Baby: return 5;
    case age::YoungAdult: return 7;
    default: return 3;
    }
}

std::vector<uint8_t> writePropertySetXml(const std::map<std::string, PropertyValue>& props) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString>\n";
    for (const auto& [k, v] : props) {
        const char* tag = v.kind == PropertyValue::Uint32 ? "AnyUint32" : v.kind == PropertyValue::Float32 ? "AnyFloat32" : "AnyString";
        const char* t = v.kind == PropertyValue::Uint32 ? "0xeb61e4f7" : v.kind == PropertyValue::Float32 ? "0xabc78708" : "0x0b8bea18";
        s += "  <" + std::string(tag) + " key=\"" + xmlEscape(k) + "\" type=\"" + t + "\">" + xmlEscape(v.text) + "</" + tag + ">\n";
    }
    s += "</cGZPropertySetString>";
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint32_t> outfitCategories(uint32_t a, uint32_t g) {
    std::vector<uint32_t> out = {category::Everyday};
    if (a == age::Baby)
        return out;
    out.insert(out.end(), {category::Swimwear, category::Pajamas, category::Formal, category::Underwear, category::Naked});
    if (a == age::Adult && g == gender::Female)
        out.push_back(category::Maternity);
    if (a != age::Toddler)
        out.push_back(category::Athletic);
    out.push_back(category::TryOn);
    return out;
}

void completeSpec(const CasCatalog& c, CharacterSpec& spec, uint32_t seed) {
    std::mt19937 rng(seed);
    auto pick = [&](const std::vector<const SimPart*>& list) -> const SimPart* {
        if (list.empty())
            return nullptr;
        return seed ? list[rng() % list.size()] : list.front();
    };
    // Elders' hair is grey (the next-age face of the game's files has hair tone 5).
    const std::string hairTone = toneFamily(spec.age == age::Elder ? 5 : spec.hairTone);
    auto& everyday = spec.outfits[category::Everyday];
    if (!everyday.count(part::Hair)) {
        // Not a bald head or a hat by default.
        std::vector<const SimPart*> styles;
        for (const SimPart* h : c.hairStyles(spec.age, spec.gender, hairTone)) {
            const std::string n = lower(h->name());
            if (n.find("bald") == std::string::npos && n.find("hat") == std::string::npos)
                styles.push_back(h);
        }
        if (styles.empty())
            styles = c.hairStyles(spec.age, spec.gender, hairTone);
        if (const SimPart* h = pick(styles))
            everyday[part::Hair] = h->name();
    }
    if (!everyday.count(part::Body) && !everyday.count(part::Top)) {
        const auto tops = c.outfitParts(part::Top, spec.age, spec.gender);
        const auto bottoms = c.outfitParts(part::Bottom, spec.age, spec.gender);
        const auto bodies = c.outfitParts(part::Body, spec.age, spec.gender);
        if (!tops.empty() && !bottoms.empty() && (!seed || rng() % 2 || bodies.empty())) {
            everyday[part::Top] = pick(tops)->name();
            everyday[part::Bottom] = pick(bottoms)->name();
        } else if (const SimPart* b = pick(bodies)) {
            everyday[part::Body] = b->name();
        }
    }
    if (spec.eyes.empty())
        if (const SimPart* e = c.byName(c.textureOverlays, "uuface_eye_dkbrown"))
            spec.eyes = family(*e);
    bool brows = false;
    for (const auto& f : spec.overlays)
        if (const SimPart* o = c.byFamily(c.textureOverlays, f))
            brows |= o->number("subtype") == overlay::Brows;
    if (!brows && spec.age != age::Baby)
        if (const SimPart* b = pick(c.overlaysFor(overlay::Brows, spec.age, spec.gender)))
            spec.overlays.push_back(family(*b));
    for (uint32_t cat : outfitCategories(spec.age, spec.gender)) {
        if (cat == category::Everyday)
            continue;
        auto& o = spec.outfits[cat];
        if (!o.empty())
            continue;
        if (cat == category::TryOn) {
            o = everyday;
            continue;
        }
        if (cat == category::Naked) {
            const std::string p = agePrefix(spec.age, spec.gender, false);
            const std::string s = "_s" + std::to_string(spec.skinTone);
            const std::string top = "CASIE_" + p + "topnaked_nude" + s, bottom = "CASIE_" + p + "bottomnaked_nude" + s;
            const std::string pu = agePrefix(spec.age, spec.gender, true);
            for (const std::string& t : {top, "CASIE_" + pu + "topnaked_nude" + s})
                if (partByName(c, t)) {
                    o[part::Top] = t;
                    break;
                }
            for (const std::string& b : {bottom, "CASIE_" + pu + "bottomnaked_nude" + s})
                if (partByName(c, b)) {
                    o[part::Bottom] = b;
                    break;
                }
            continue;
        }
        const bool casOnly = cat != category::Maternity;
        if (const SimPart* b = pick(c.outfitParts(part::Body, spec.age, spec.gender, cat, false, casOnly)))
            o[part::Body] = b->name();
    }
}

CharacterWriteResult writeCharacter(ResourceIndex& index, const CasCatalog& catalog, FaceBaker& baker,
                                    const CharacterSpec& spec, const std::string& objectName,
                                    const std::filesystem::path& out, bool faceStructure) {
    CharacterWriteResult res;
    DbpfWriter pkg;
    std::mt19937 rng(std::random_device{}());
    res.guid = spec.guid ? spec.guid : (0x0FD00000u | (rng() & 0xFFFFF));
    char idText[16];
    std::snprintf(idText, sizeof idText, "0x%08x", unsigned(res.guid * 2654435761u));
    const std::string id = idText;
    const std::string skinFamily = toneFamily(spec.skinTone), hairFamily = toneFamily(spec.hairTone);
    const std::string prefix = agePrefix(spec.age, spec.gender, true);

    // The overlays drawn over the face skin, in layer order (eyes first).
    struct Layer {
        const SimPart* entry;
        uint32_t layer;
        int order;
    };
    std::vector<Layer> layers;
    if (const SimPart* e = catalog.byFamily(catalog.textureOverlays, spec.eyes))
        layers.push_back({e, 0, 0});
    for (size_t i = 0; i < spec.overlays.size(); ++i) {
        const SimPart* o = catalog.byFamily(catalog.textureOverlays, spec.overlays[i]);
        if (!o) {
            res.warnings.push_back("no overlay " + spec.overlays[i]);
            continue;
        }
        const uint32_t sub = o->number("subtype");
        if (sub == overlay::Brows || sub == overlay::FacialHair)
            o = catalog.recolor(*o, spec.age == age::Elder ? toneFamily(5) : hairFamily);
        layers.push_back({o, o->number("layer"), int(i) + 1});
    }
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& a, const Layer& b) { return a.layer < b.layer; });

    // One face per age: the current one (GZPS 1) and the next (GZPS 2).
    struct MeshOut {
        Tgi cres;
        uint32_t category = 0;
        std::string family;
    };
    struct FaceOut {
        uint32_t age;
        Tgi cres;
        std::vector<MeshOut> meshes; // the mesh overlays: "<id>_age<n>_<k>_cres", k from 1
    };
    std::vector<FaceOut> faces;
    for (uint32_t a : {spec.age, nextAge(spec.age)}) {
        if (!a || !catalog.neutralFor(a, spec.gender))
            continue;
        const int ai = editorAgeIndex(a);
        const std::string base = id + "_age" + std::to_string(ai) + "_0";
        const std::string cresName = base + "_cres", shpeName = id + "_age" + std::to_string(ai) + "_00_shpe",
                          gmndName = base + "_gmnd", gmdcName = base + "_gmdc";
        const std::string matName = "ss_" + id + "_" + std::to_string(ai) + "_cmpm";
        FaceBaker::Baked baked = baker.bake(spec.face, a, spec.gender);
        for (const auto& w : baked.warnings)
            res.warnings.push_back(baked.neutral + ": " + w);
        const std::string neutral = baked.neutral; // "afFace"
        const Tgi cresT = localTgi(kTypeCres, cresName), shpeT = localTgi(kTypeShpe, shpeName),
                  gmndT = localTgi(kTypeGmnd, gmndName), gmdcT = localTgi(kTypeGmdc, gmdcName),
                  txmtT = localTgi(kTypeTxmt, matName + "_txmt");

        // CRES -> SHPE: the neutral face's chain.
        auto cresLoc = index.findByName(neutral + "_cres", kTypeCres);
        if (!cresLoc)
            throw FormatError(neutral + "_cres not found");
        const auto cresBytes = index.read(*cresLoc);
        Rcol cresRc(cresBytes);
        if (cresRc.links.empty())
            throw FormatError(neutral + "_cres has no shape link");
        auto shpeLoc = findAny(index, cresRc.links[0]);
        if (!shpeLoc)
            throw FormatError(neutral + ": shape not found");
        const auto shpeBytes = index.read(*shpeLoc);
        Rcol shpeRc(shpeBytes);
        std::string oldShpeName, oldGmndName, oldMaterial;
        if (const auto* b = shpeRc.find("cShape")) {
            const Shape sh = Shape::parse(b->body);
            oldShpeName = sh.resource.fileName;
            for (const auto& item : sh.items)
                if (item.lod == 0 || oldGmndName.empty())
                    oldGmndName = item.geometryName;
            if (!sh.parts.empty())
                oldMaterial = sh.parts.front().material;
        }
        auto gmndLoc = index.findByName(oldGmndName, kTypeGmnd);
        if (!gmndLoc)
            throw FormatError(neutral + ": " + oldGmndName + " not found");
        const auto gmndBytes = index.read(*gmndLoc);
        Rcol gmndRc(gmndBytes);
        std::string oldGmdcName;
        {
            Rcol bakedRc(baked.gmdc);
            if (const auto* d = bakedRc.find("cGeometryDataContainer")) {
                BinReader r(d->body);
                r.u32();
                oldGmdcName = SgResource::read(r).fileName;
            }
        }
        const std::string oldCresName = scenegraphName(index, cresLoc->entry->tgi);
        const std::map<std::string, std::string> names = {
            {oldCresName, kPrefix + cresName}, {oldShpeName, kPrefix + shpeName}, {oldGmndName, kPrefix + gmndName},
            {oldGmdcName, kPrefix + gmdcName}, {oldMaterial, matName},
        };
        pkg.add(cresT, rewriteRcol(cresBytes, {shpeT}, names), true);
        pkg.add(shpeT, rewriteRcol(shpeBytes, {}, names), true);
        pkg.add(gmndT, rewriteRcol(gmndBytes, {gmdcT}, names), true);
        pkg.add(gmdcT, rewriteRcol(baked.gmdc, {}, names), true);

        // The face material.
        const std::string skinTex = lower(prefix) + "face-s" + std::to_string(spec.skinTone);
        MaterialDefinition m;
        uint32_t sgId = 0, sgVersion = 2;
        if (auto tl = index.findByName(lower(prefix) + "face_s" + std::to_string(spec.skinTone) + "_txmt", kTypeTxmt)) {
            const auto tb = index.read(*tl);
            Rcol trc(tb);
            if (const auto* b = trc.find("cMaterialDefinition")) {
                m = MaterialDefinition::parse(b->body);
                BinReader r(b->body);
                r.u32();
                r.str7();
                sgId = r.u32();
                sgVersion = r.u32();
            }
        }
        if (m.materialType.empty()) {
            m.version = 11;
            m.materialType = "SimSkin";
        }
        m.materialType = "SimSkin";
        std::vector<std::string> textures = {skinTex};
        std::string composite = lower(prefix) + "face_s" + std::to_string(spec.skinTone);
        for (const auto& l : layers) {
            const std::string t = overlayTexture(index, *l.entry);
            textures.push_back(t);
            std::string u = t;
            std::replace(u.begin(), u.end(), '-', '_');
            composite += "_" + u;
        }
        for (size_t i = 0; i < textures.size(); ++i)
            setProperty(m, "baseTexture" + std::to_string(i), textures[i]);
        setProperty(m, "numTexturesToComposite", std::to_string(textures.size()));
        setProperty(m, "compositeBaseTextureName", composite + "-diffuse");
        setProperty(m, "stdMatBaseTextureName", skinTex);
        setProperty(m, "cafSkinTone", skinFamily);
        setProperty(m, "cafHairTone", hairFamily);
        std::vector<const SimPart*> byChoice;
        for (const auto& l : layers)
            byChoice.push_back(l.entry);
        setProperty(m, "cafNumOverlays", std::to_string(byChoice.size()));
        for (size_t i = 0; i < byChoice.size(); ++i)
            setProperty(m, "cafOverlay" + std::to_string(i), family(*byChoice[i]) + ",");
        setProperty(m, "paramAge", std::to_string(editorAgeIndex(a)));
        setProperty(m, "paramGender", spec.gender == gender::Female ? "1" : "0");
        setProperty(m, "eyeGizmoEnable", "true");
        std::sort(m.properties.begin(), m.properties.end());
        m.textureNames.clear();
        pkg.add(txmtT, writeMaterial(m, sgId, sgVersion, std::string(kPrefix) + matName + "_txmt"), true);

        // The mesh overlays (glasses, beards with a mesh) as the game keeps
        // them: a scenegraph chain of their own per age ("age3_1_cres" ->
        // "age3_10_shpe" -> "age3_1_gmnd" -> "age3_1_gmdc", N001_User00002),
        // the mesh moved with the face; the SHPE keeps the overlay's
        // materials.
        std::vector<MeshOut> meshes;
        int k = 1;
        for (const auto& fam : spec.meshOverlays) {
            const SimPart* o = nullptr;
            const std::string tone = a == age::Elder ? toneFamily(5) : hairFamily;
            for (const auto& e : catalog.meshOverlays)
                if (family(e) == fam && fits(e, a, spec.gender)) {
                    const std::string ht = lower(e.property("hairtone"));
                    const bool toned = !ht.empty() && ht != "00000000-0000-0000-0000-000000000000";
                    if (!o || (toned && ht == tone))
                        o = &e;
                }
            if (!o) {
                res.warnings.push_back("no mesh overlay " + fam + " for this age");
                continue;
            }
            try {
                const auto oc = o->reference("resourcekeyidx");
                auto ocLoc = oc ? findAny(index, *oc) : std::nullopt;
                if (!ocLoc)
                    throw FormatError(o->name() + ": no CRES");
                const auto ocBytes = index.read(*ocLoc);
                Rcol ocRc(ocBytes);
                if (ocRc.links.empty())
                    throw FormatError(o->name() + ": CRES without shape");
                auto osLoc = findAny(index, ocRc.links[0]);
                if (!osLoc)
                    throw FormatError(o->name() + ": shape not found");
                const auto osBytes = index.read(*osLoc);
                Rcol osRc(osBytes);
                std::string oShpe, oGmnd;
                if (const auto* b = osRc.find("cShape")) {
                    const Shape sh = Shape::parse(b->body);
                    oShpe = sh.resource.fileName;
                    for (const auto& item : sh.items)
                        if (item.lod == 0 || oGmnd.empty())
                            oGmnd = item.geometryName;
                }
                auto ogLoc = index.findByName(oGmnd, kTypeGmnd);
                if (!ogLoc)
                    throw FormatError(o->name() + ": " + oGmnd + " not found");
                const auto ogBytes = index.read(*ogLoc);
                Rcol ogRc(ogBytes);
                std::optional<ResourceIndex::Location> odLoc;
                for (const auto& l : ogRc.links)
                    if (l.type == kTypeGmdc)
                        odLoc = findAny(index, l);
                if (!odLoc)
                    throw FormatError(o->name() + ": GMDC not found");
                const auto odBytes = index.read(*odLoc);
                std::string oGmdc;
                {
                    Rcol r(odBytes);
                    if (const auto* d = r.find("cGeometryDataContainer")) {
                        BinReader br(d->body);
                        br.u32();
                        oGmdc = SgResource::read(br).fileName;
                    }
                }
                const std::string ks = std::to_string(k);
                const std::string mb = id + "_age" + std::to_string(ai) + "_" + ks;
                const std::string mc = mb + "_cres", ms = mb + "0_shpe", mn = mb + "_gmnd", md = mb + "_gmdc";
                const Tgi mcT = localTgi(kTypeCres, mc), msT = localTgi(kTypeShpe, ms), mnT = localTgi(kTypeGmnd, mn),
                          mdT = localTgi(kTypeGmdc, md);
                const std::map<std::string, std::string> mnames = {
                    {scenegraphName(index, ocLoc->entry->tgi), kPrefix + mc}, {oShpe, kPrefix + ms},
                    {oGmnd, kPrefix + mn}, {oGmdc, kPrefix + md}};
                pkg.add(mcT, rewriteRcol(ocBytes, {msT}, mnames), true);
                pkg.add(msT, rewriteRcol(osBytes, {}, mnames), true);
                pkg.add(mnT, rewriteRcol(ogBytes, {mdT}, mnames), true);
                pkg.add(mdT, rewriteRcol(FaceBaker::deformToFace(odBytes, baked), {}, mnames), true);
                meshes.push_back({mcT, o->number("category"), fam});
                ++k;
            } catch (const std::exception& e) {
                res.warnings.push_back(std::string("mesh overlay: ") + e.what());
            }
        }
        faces.push_back({a, cresT, std::move(meshes)});
    }
    if (faces.empty())
        throw FormatError("no face for this age");

    // 3IDR 1 (AGED and GZPS 1) and the AGED.
    std::vector<Tgi> refs;
    auto refIndex = [&](const Tgi& t) {
        for (size_t i = 0; i < refs.size(); ++i)
            if (refs[i] == t)
                return uint32_t(i);
        refs.push_back(t);
        return uint32_t(refs.size() - 1);
    };
    const std::string skeleton = spec.age == age::Baby      ? "buskel"
                                 : spec.age == age::Toddler ? "puskel"
                                 : spec.age == age::Child   ? "cuskel"
                                                            : "auskel";
    const Tgi skelT{kTypeCres, kLocalGroup, nameInstanceHash(skeleton + "_cres"), 0};
    const uint32_t skelIdx = refIndex(skelT);
    const uint32_t faceCresIdx = refIndex(Tgi{kTypeCres, kLocal, faces[0].cres.instance, faces[0].cres.instanceHi});
    const uint32_t faceShapeIdx = uint32_t(refs.size());
    refs.push_back(Tgi{});
    const Tgi faceGzps{kTypeGzps, kLocal, 1, 0};
    // The GZPS instances: per age the face, then its mesh overlays (1 face,
    // 2.. overlays, then the next age's), as the game numbers them.
    std::vector<std::vector<uint32_t>> gzpsOf(faces.size());
    {
        uint32_t next = 1;
        for (size_t f = 0; f < faces.size(); ++f)
            for (size_t j = 0; j <= faces[f].meshes.size(); ++j)
                gzpsOf[f].push_back(next++);
    }

    std::map<std::string, PropertyValue> aged;
    uint32_t list = 0;
    for (uint32_t cat : outfitCategories(spec.age, spec.gender)) {
        auto it = spec.outfits.find(cat);
        std::map<uint32_t, std::string> parts = it == spec.outfits.end() ? std::map<uint32_t, std::string>{} : it->second;
        const auto& everyday = spec.outfits.count(category::Everyday) ? spec.outfits.at(category::Everyday)
                                                                       : std::map<uint32_t, std::string>{};
        if (!parts.count(part::Hair) && everyday.count(part::Hair))
            parts[part::Hair] = everyday.at(part::Hair);
        std::map<uint32_t, Tgi> entries;
        for (const auto& [bit, name] : parts) {
            const SimPart* p = partByName(catalog, name);
            if (!p) {
                res.warnings.push_back("no part " + name);
                continue;
            }
            entries[bit] = p->tgi;
        }
        if (entries.empty() || (entries.size() == 1 && entries.count(part::Hair) && cat != category::Everyday))
            continue; // nothing of this category for this age (toddlers' swimwear)
        if (cat != category::Naked)
            entries[part::Face] = faceGzps;
        for (const auto& [bit, tgi] : entries) {
            const std::string i = std::to_string(list++);
            aged["lk" + i] = u32v(cat << 16 | bit);
            aged["le" + i + "_0"] = u32v(refIndex(Tgi{tgi.type, tgi.group, tgi.instance, tgi.instanceHi}));
            uint32_t count = 1;
            if (bit == part::Face) // the face list also names the mesh overlays' GZPS
                for (size_t j = 1; j < gzpsOf[0].size(); ++j)
                    aged["le" + i + "_" + std::to_string(count++)] = u32v(refIndex(Tgi{kTypeGzps, kLocal, gzpsOf[0][j], 0}));
            aged["ls" + i] = u32v(count);
        }
    }
    aged["activecat"] = u32v(1);
    aged["age"] = u32v(spec.age);
    aged["creatorid"] = strv("00000000-0000-0000-0000-000000000000");
    aged["eyecolor"] = strv(spec.eyes);
    aged["fitness"] = u32v(uint32_t(spec.fitness));
    aged["gender"] = u32v(spec.gender);
    aged["haircolor"] = strv(hairFamily);
    aged["listcnt"] = u32v(list);
    aged["skeletonkeyidx"] = u32v(skelIdx);
    aged["skincolor"] = strv(skinFamily);
    aged["species"] = u32v(1);
    aged["stretch"] = u32v(1);
    aged["validparts"] = u32v(65535);

    auto faceGzpsProps = [&](uint32_t a, uint32_t cresIdx, uint32_t shapeIdx) {
        std::map<std::string, PropertyValue> p;
        p["age"] = u32v(a);
        p["category"] = u32v(0);
        p["creator"] = strv("00000000-0000-0000-0000-000000000000");
        p["family"] = strv("00000000-0000-0000-0000-000000000000");
        p["fitness"] = u32v(0);
        p["flags"] = u32v(1);
        p["gender"] = u32v(spec.gender);
        p["genetic"] = {PropertyValue::Float32, "0"};
        p["hairtone"] = strv(a == age::Elder ? toneFamily(5) : hairFamily);
        p["name"] = strv("");
        p["numoverrides"] = u32v(0);
        p["outfit"] = u32v(part::Face);
        p["resourcekeyidx"] = u32v(cresIdx);
        p["shapekeyidx"] = u32v(shapeIdx);
        p["shoe"] = u32v(0);
        p["skintone"] = strv(skinFamily);
        p["species"] = u32v(1);
        p["type"] = strv("skin");
        return p;
    };
    pkg.add(Tgi{kTypeAged, kLocal, 1, 0}, writePropertySetXml(aged), true);
    pkg.add(Tgi{kTypeIdr, kLocal, 1, 0}, writeReferenceList(refs), true);
    pkg.add(faceGzps, writePropertySetXml(faceGzpsProps(faces[0].age, faceCresIdx, faceShapeIdx)), true);
    for (size_t f = 0; f < faces.size(); ++f)
        for (size_t j = 0; j < gzpsOf[f].size(); ++j) {
            const uint32_t inst = gzpsOf[f][j];
            if (inst == 1)
                continue; // shares 3IDR 1 with the AGED, written above
            const Tgi cres = j == 0 ? faces[f].cres : faces[f].meshes[j - 1].cres;
            pkg.add(Tgi{kTypeIdr, kLocal, inst, 0}, writeReferenceList({cres, Tgi{}}), true);
            auto props = faceGzpsProps(faces[f].age, 0, 1);
            if (j > 0) {
                props["category"] = u32v(faces[f].meshes[j - 1].category);
                props["family"] = strv(faces[f].meshes[j - 1].family); // to edit it again (the game leaves it empty)
            }
            pkg.add(Tgi{kTypeGzps, kLocal, inst, 0}, writePropertySetXml(props), true);
        }

    // The Sim's object, from the template person.
    std::optional<Tgi> templateObjd;
    for (const auto& loc : index.ofType(kTypeObjd)) {
        try {
            const auto d = ObjectDefinition::parse(index.read(loc));
            if (d.guid() == kTemplatePerson) {
                templateObjd = loc.entry->tgi;
                break;
            }
        } catch (const std::exception&) {
        }
    }
    if (templateObjd) {
        const auto objdLoc = index.find(*templateObjd);
        DbpfPackage* source = objdLoc->package;
        for (const auto& e : source->entries()) {
            if (e.tgi.group != templateObjd->group)
                continue;
            std::vector<uint8_t> data = source->read(e);
            if (e.tgi.type == kTypeObjd) {
                const std::string fileName = objectName + " - " + spec.firstName;
                std::fill(data.begin(), data.begin() + std::min<size_t>(64, data.size()), 0);
                std::copy_n(fileName.begin(), std::min<size_t>(63, fileName.size()), data.begin());
                auto put16 = [&](size_t off, uint16_t v) {
                    if (off + 1 < data.size())
                        data[off] = uint8_t(v), data[off + 1] = uint8_t(v >> 8);
                };
                put16(0x52, 2); // person
                put16(0x5C, uint16_t(res.guid));
                put16(0x5E, uint16_t(res.guid >> 16));
            }
            pkg.add(Tgi{e.tgi.type, kLocal, e.tgi.instance, e.tgi.instanceHi}, std::move(data), true);
        }
    } else {
        res.warnings.push_back("TemplatePerson (0x7FD96B54) not found: no object written");
    }
    if (faceStructure) {
        const FaceStructure fs = baker.structureOf(spec.face, spec.gender);
        if (!fs.keys.empty())
            pkg.add(Tgi{kTypeLxnr, kLocal, 1, 0}, fs.write(), true);
    }
    if (!spec.portraitPng.empty()) {
        constexpr uint32_t kTypeImg = 0x856DDBAC;
        pkg.add(Tgi{kTypeImg, kLocal, spec.age, 0}, spec.portraitPng, true);
        pkg.add(Tgi{kTypeImg, kLocal, spec.age << 8, 0}, spec.portraitPng, true);
    }
    pkg.add(Tgi{kTypeCtss, kLocal, 0x7D0, 0}, writeStrings({spec.firstName, spec.description, spec.lastName}), true);
    {
        BinWriter w;
        w.u32(0xCBE750E0);
        w.u16(2);
        w.u32(2);
        auto item = [&](const std::string& k, const std::string& v) {
            w.u32(0x0B8BEA18);
            w.u32(uint32_t(k.size()));
            w.raw(std::span(reinterpret_cast<const uint8_t*>(k.data()), k.size()));
            w.u32(uint32_t(v.size()));
            w.raw(std::span(reinterpret_cast<const uint8_t*>(v.data()), v.size()));
        };
        item("0", "The Sims 2");
        item("1", "0.14.0.735");
        pkg.add(Tgi{kTypeVersion, kLocal, 1, 0}, std::move(w.bytes()));
    }
    res.resources = pkg.size();
    std::filesystem::create_directories(out.parent_path().empty() ? "." : out.parent_path());
    pkg.save(out);
    return res;
}

} // namespace os2::cas
