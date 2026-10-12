#include "cas_catalog.hpp"

#include "property_set.hpp"
#include "rcol.hpp"
#include "resindex.hpp"
#include "txmt.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace os2::cas {

namespace {

constexpr uint32_t kTypeIdr = 0xAC506764, kTypeTxmt = 0x49596978;

std::string lower(std::string s) {
    for (auto& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool byNameLess(const SimPart* a, const SimPart* b) { return lower(a->name()) < lower(b->name()); }

std::vector<const SimPart*> filter(const std::vector<SimPart>& list, uint32_t ageBit, uint32_t genderBit) {
    std::vector<const SimPart*> out;
    for (const auto& e : list)
        if (fits(e, ageBit, genderBit))
            out.push_back(&e);
    std::stable_sort(out.begin(), out.end(), byNameLess);
    return out;
}

} // namespace

// The style of a hair part: its name without the colour suffix
// ("amhairshort_brown" -> "amhairshort").
std::string hairStyle(const SimPart& p) {
    std::string n = lower(p.name());
    if (auto u = n.rfind('_'); u != std::string::npos)
        n.resize(u);
    return n;
}

std::string agePrefix(uint32_t ageBit, uint32_t genderBit, bool unisexYoung) {
    char a = 'a';
    switch (ageBit) {
    case age::Baby: a = 'b'; break;
    case age::Toddler: a = 'p'; break;
    case age::Child: a = 'c'; break;
    case age::Teen: a = 't'; break;
    case age::Elder: a = 'e'; break;
    default: a = 'a'; break;
    }
    const bool young = a == 'b' || a == 'p' || a == 'c';
    const char g = (young && unisexYoung) ? 'u' : (genderBit == gender::Female ? 'f' : 'm');
    return std::string{a, g};
}

const char* ageName(uint32_t ageBit) {
    switch (ageBit) {
    case age::Baby: return "baby";
    case age::Toddler: return "toddler";
    case age::Child: return "child";
    case age::Teen: return "teen";
    case age::Adult: return "adult";
    case age::Elder: return "elder";
    case age::YoungAdult: return "young adult";
    default: return "?";
    }
}

std::string entryType(const SimPart& e) { return e.property("type"); }
std::string family(const SimPart& e) { return lower(e.property("family")); }

float floatProperty(const SimPart& e, const std::string& key, float fallback) {
    const std::string v = e.property(key);
    if (v.empty())
        return fallback;
    try {
        return std::stof(v);
    } catch (const std::logic_error&) {
        return fallback;
    }
}

float genetic(const SimPart& e) { return floatProperty(e, "genetic"); }

bool fits(const SimPart& e, uint32_t ageBit, uint32_t genderBit) {
    return (e.number("age") & ageBit) && (e.number("gender") & genderBit);
}

std::string toneFamily(int n) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-0000-0000-0000-000000000000", unsigned(n));
    return buf;
}

int toneNumber(std::string_view f) {
    if (f.size() < 36 || f.substr(8) != "-0000-0000-0000-000000000000")
        return 0;
    try {
        return int(std::stoul(std::string(f.substr(0, 8)), nullptr, 16));
    } catch (const std::logic_error&) {
        return 0;
    }
}

int skinToneNumber(std::string_view f) {
    if (int t = toneNumber(f))
        return t;
    if (f.size() == kAlienSkinFamily.size()) {
        bool same = true;
        for (size_t i = 0; i < f.size(); ++i)
            same = same && std::tolower(static_cast<unsigned char>(f[i])) == kAlienSkinFamily[i];
        if (same)
            return 5;
    }
    return 0;
}

CasCatalog CasCatalog::load(ResourceIndex& index) {
    CasCatalog c;
    std::unordered_map<Tgi, std::vector<Tgi>, TgiHash> lists; // by (type 0, group, instance)
    for (const auto& loc : index.ofType(kTypeIdr)) {
        Tgi k = loc.entry->tgi;
        k.type = 0;
        k.instanceHi = 0;
        if (lists.count(k))
            continue;
        try {
            lists.emplace(k, parseReferenceList(index.read(loc)));
        } catch (const FormatError&) {
        }
    }
    const std::pair<uint32_t, std::vector<SimPart>*> kinds[] = {
        {type::SkinTone, &c.skinTones},       {type::HairTone, &c.hairTones},
        {type::FaceArchetype, &c.archetypes}, {type::FaceModifier, &c.modifiers},
        {type::FaceRegion, &c.regions},       {type::FaceNeutral, &c.neutrals},
        {type::TextureOverlay, &c.textureOverlays}, {type::MeshOverlay, &c.meshOverlays},
    };
    for (const auto& [t, list] : kinds) {
        for (const auto& loc : index.ofType(t)) {
            SimPart p;
            p.tgi = loc.entry->tgi;
            try {
                p.properties = readPropertySet(index.read(loc));
            } catch (const FormatError&) {
                continue;
            }
            Tgi k = p.tgi;
            k.type = 0;
            k.instanceHi = 0;
            if (auto it = lists.find(k); it != lists.end())
                p.references = it->second;
            list->push_back(std::move(p));
        }
    }
    c.parts = loadSimParts(index);
    return c;
}

std::vector<const SimPart*> CasCatalog::archetypesFor(uint32_t a, uint32_t g) const { return filter(archetypes, a, g); }
std::vector<const SimPart*> CasCatalog::modifiersFor(uint32_t a, uint32_t g) const { return filter(modifiers, a, g); }
std::vector<const SimPart*> CasCatalog::meshOverlaysFor(uint32_t a, uint32_t g) const { return filter(meshOverlays, a, g); }

const SimPart* CasCatalog::neutralFor(uint32_t a, uint32_t g) const {
    for (const auto& e : neutrals)
        if (fits(e, a, g))
            return &e;
    return nullptr;
}

std::vector<const SimPart*> CasCatalog::overlaysFor(uint32_t subtype, uint32_t a, uint32_t g) const {
    std::vector<const SimPart*> out;
    for (const auto* e : filter(textureOverlays, a, g))
        if (e->number("subtype") == subtype)
            out.push_back(e);
    return out;
}

const SimPart* CasCatalog::recolor(const SimPart& e, std::string_view hairToneFamily) const {
    const std::string fam = family(e);
    const std::vector<SimPart>& list = entryType(e) == "meshoverlay" ? meshOverlays : textureOverlays;
    const uint32_t ages = e.number("age"), genders = e.number("gender");
    for (const auto& o : list)
        if (family(o) == fam && lower(o.property("hairtone")) == hairToneFamily && (o.number("age") & ages) &&
            (o.number("gender") & genders))
            return &o;
    return &e;
}

std::vector<const SimPart*> CasCatalog::outfitParts(uint32_t partBit, uint32_t a, uint32_t g, uint32_t cat,
                                                    bool fat, bool casOnly) const {
    std::vector<const SimPart*> out;
    for (const auto& p : parts) {
        if (p.property("type") != "skin" || !(p.number("outfit") & partBit) || !fits(p, a, g))
            continue;
        if (cat && !(p.number("category") & cat))
            continue;
        if ((p.number("fitness") == 2) != fat)
            continue;
        if (casOnly && (p.number("flags") & 1))
            continue;
        const std::string n = lower(p.name());
        if (n.empty() || n.starts_with("casie_") || n.find("null") != std::string::npos)
            continue;
        if (!p.reference("resourcekeyidx"))
            continue;
        out.push_back(&p);
    }
    std::stable_sort(out.begin(), out.end(), byNameLess);
    return out;
}

std::vector<const SimPart*> CasCatalog::hairStyles(uint32_t a, uint32_t g, std::string_view tone) const {
    std::vector<const SimPart*> out;
    std::vector<std::string> seen;
    for (const auto* p : outfitParts(part::Hair, a, g, 0)) {
        const std::string style = hairStyle(*p);
        if (std::find(seen.begin(), seen.end(), style) != seen.end())
            continue;
        seen.push_back(style);
        out.push_back(hairInTone(*p, tone));
    }
    return out;
}

const SimPart* CasCatalog::hairInTone(const SimPart& hair, std::string_view tone) const {
    const std::string style = hairStyle(hair);
    for (const auto& p : parts)
        if (p.property("type") == "skin" && (p.number("outfit") & part::Hair) && hairStyle(p) == style &&
            lower(p.property("hairtone")) == tone && (p.number("age") & hair.number("age")) &&
            (p.number("flags") & 1) == (hair.number("flags") & 1) && !lower(p.name()).starts_with("casie_"))
            return &p;
    return &hair;
}

const SimPart* CasCatalog::byFamily(const std::vector<SimPart>& list, std::string_view f) const {
    for (const auto& e : list)
        if (family(e) == f)
            return &e;
    return nullptr;
}

const SimPart* CasCatalog::byName(const std::vector<SimPart>& list, std::string_view name) const {
    const std::string n = lower(std::string(name));
    for (const auto& e : list)
        if (lower(e.name()) == n)
            return &e;
    return nullptr;
}

const SimPart* CasCatalog::skinTone(int tone) const { return byFamily(skinTones, toneFamily(tone)); }
const SimPart* CasCatalog::hairTone(int tone) const { return byFamily(hairTones, toneFamily(tone)); }

std::string faceSkinTexture(ResourceIndex& index, const std::string& prefix, int skinTone) {
    const std::string p = lower(prefix), n = std::to_string(skinTone);
    if (auto loc = index.findByName(p + "face_s" + n + "_txmt", kTypeTxmt)) {
        try {
            const auto bytes = index.read(*loc);
            Rcol rc(bytes);
            if (const auto* b = rc.find("cMaterialDefinition"))
                if (auto tex = MaterialDefinition::parse(b->body).property("stdMatBaseTextureName"); !tex.empty())
                    return lower(tex);
        } catch (const std::exception&) {
        }
    }
    return p + "face-s" + n;
}

std::string overlayTexture(ResourceIndex& index, const SimPart& o) {
    if (auto t = o.reference("materialkeyidx")) {
        Tgi k = *t;
        auto loc = index.find(k);
        if (!loc) {
            k.instanceHi = 0;
            loc = index.find(k);
        }
        if (loc && loc->entry->tgi.type == kTypeTxmt) {
            try {
                const auto bytes = index.read(*loc);
                Rcol rc(bytes);
                if (const auto* b = rc.find("cMaterialDefinition"))
                    if (auto tex = MaterialDefinition::parse(b->body).property("stdMatBaseTextureName"); !tex.empty())
                        return tex;
            } catch (const std::exception&) {
            }
        }
    }
    std::string n = lower(o.name());
    if (auto u = n.find('_'); u != std::string::npos)
        for (size_t i = u; i < n.size(); ++i)
            if (n[i] == '_')
                n[i] = '-';
    return n;
}

} // namespace os2::cas
