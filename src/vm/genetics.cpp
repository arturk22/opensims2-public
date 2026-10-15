#include "genetics.hpp"

#include <algorithm>
#include <sstream>

namespace os2::genetics {

namespace {

const std::string kEmpty;

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// One allele of a parent: dominant or recessive, 50/50.
const std::string& oneOf(const Allele& a, const Random& random) { return random(2) == 0 ? a.first : a.second; }

} // namespace

Dna Dna::fromProperties(const std::map<std::string, std::string>& sdna) {
    Dna d;
    for (const auto& [k, v] : sdna) {
        uint32_t key = 0;
        try {
            key = uint32_t(std::stoul(k));
        } catch (const std::logic_error&) {
            continue;
        }
        if (key & gene::Recessive)
            d.alleles[key & ~gene::Recessive].second = v;
        else
            d.alleles[key].first = v;
    }
    return d;
}

std::map<std::string, std::string> Dna::toProperties() const {
    std::map<std::string, std::string> out;
    for (const auto& [t, a] : alleles) {
        out[std::to_string(t)] = a.first;
        out[std::to_string(t | gene::Recessive)] = a.second;
    }
    return out;
}

const std::string& Dna::dominant(uint32_t type) const {
    auto it = alleles.find(type);
    return it == alleles.end() ? kEmpty : it->second.first;
}

std::string Dna::expressed(uint32_t type) const {
    if (type == gene::Skin && !dominant(gene::SkinBetween).empty())
        return dominant(gene::SkinBetween);
    return dominant(type);
}

std::vector<std::string> Dna::dominantRegions() const {
    std::vector<std::string> out;
    std::stringstream ss(dominant(gene::DominantRegions));
    for (std::string r; std::getline(ss, r, ',');)
        if (!r.empty())
            out.push_back(r);
    return out;
}

std::string setupDominantRegions(bool female, const Random& random) {
    struct Threshold {
        const char* region;
        uint32_t woman, man;
    };
    static const Threshold table[] = {{"nose", 50, 66},       {"brow", 50, 66}, {"mouth", 66, 50},
                                      {"cheek", 50, 50},      {"cheekbones", 66, 50},
                                      {"eyes", 66, 50},       {"jaw", 50, 66},  {"ears", 50, 50}};
    std::string out;
    for (const auto& t : table)
        if (random(100) >= (female ? t.woman : t.man))
            out += std::string(t.region) + ",";
    return out;
}

Dna dnaFromLook(const std::string& hair, const std::string& skin, const std::string& eyes, bool female,
                const Random& random) {
    Dna d;
    d.alleles[gene::Hair] = {hair, hair};
    d.alleles[gene::Skin] = {skin, skin};
    d.alleles[gene::Eyes] = {eyes, eyes};
    d.alleles[gene::SkinBetween] = {skin, skin}; // every shipped Sim has gene 6 = its skin tone
    const std::string regions = setupDominantRegions(female, random);
    d.alleles[gene::DominantRegions] = {regions, ""};
    return d;
}

Allele pickGene(uint32_t type, const Allele& mother, const Allele& father, const GeneticValue& value,
                const Random& random, const std::vector<SkinTone>& skinTones, std::string* between) {
    const std::string a = oneOf(mother, random), b = oneOf(father, random);
    auto ordered = [&]() -> Allele { return random(2) == 0 ? Allele{a, b} : Allele{b, a}; };
    if (type == gene::Hair || type == gene::Eyes) {
        // The lower genetic value is dominant (Black/Brown 1 over Blond/Red
        // 2; dark brown, dark blue, alien eyes 1 over grey, green, light blue 2).
        const float va = value(type, a), vb = value(type, b);
        if (va < vb)
            return {a, b};
        if (vb < va)
            return {b, a};
        return ordered();
    }
    if (type == gene::Skin) {
        const float va = value(type, a), vb = value(type, b);
        if (va != 0 && vb != 0) {
            const float lo = std::min(va, vb), hi = std::max(va, vb);
            std::vector<const SkinTone*> candidates;
            for (const auto& t : skinTones)
                if (t.genetic != 0 && t.genetic >= lo - 1e-4f && t.genetic <= hi + 1e-4f)
                    candidates.push_back(&t);
            if (between && !candidates.empty())
                *between = candidates[random(uint32_t(candidates.size()))]->family;
            return {a, b};
        }
        if (a == b)
            return {a, b};
        return ordered();
    }
    return ordered();
}

Dna determineGenetics(const Dna& ma, const Dna& pa, const GeneticValue& value, const Random& random,
                      const std::vector<SkinTone>& skinTones) {
    Dna child;
    for (uint32_t t = 1; t <= 15; ++t) {
        if (t == gene::Face || t == gene::DominantRegions || t == gene::SkinBetween || t == 11)
            continue;
        const bool hasA = ma.has(t), hasB = pa.has(t);
        if (!hasA && !hasB)
            continue;
        const Allele a = hasA ? ma.alleles.at(t) : pa.alleles.at(t);
        const Allele b = hasB ? pa.alleles.at(t) : ma.alleles.at(t);
        std::string between;
        child.alleles[t] = pickGene(t, a, b, value, random, skinTones, &between);
        if (t == gene::Skin && !between.empty())
            child.alleles[gene::SkinBetween] = {between, between};
    }
    return child;
}

std::string DescendantFace::script() const {
    std::string s;
    // DetermineDescendantFace (BV 0x548404) writes "%s,%d" with 1 for the
    // first parent and 0 for the second.
    for (const auto& [r, i] : regions)
        s += r + "," + (i == 0 ? "1" : "0") + ";";
    return s;
}

DescendantFace determineDescendantFace(const Dna& a, const Dna& b, const Random& random) {
    DescendantFace f;
    const auto da = a.dominantRegions(), db = b.dominantRegions();
    for (const char* region : {"nose", "brow", "mouth", "eyes", "jaw", "ears"}) {
        const std::string r = region;
        const bool inA = contains(da, r), inB = contains(db, r);
        const uint32_t roll = random(100);
        int parent;
        if (!inA && inB)
            parent = roll <= 66 ? 1 : 0;
        else
            parent = roll < 50 ? 0 : 1;
        std::vector<std::string> group = {r};
        if (r == "eyes")
            group.push_back("cheekbones");
        if (r == "mouth")
            group.push_back("cheek");
        const bool dominant = parent == 0 ? inA : inB;
        for (const auto& g : group) {
            f.regions.emplace_back(g, parent);
            if (dominant)
                f.dominant += g + ",";
        }
    }
    return f;
}

GeneticValue baseGameGeneticValues() {
    return [](uint32_t type, const std::string& family) -> float {
        static const std::map<std::string, float> eyes = {
            {"12d4f3e1-fdbe-4fe7-ace3-46dd9ff52b51", 1}, // alien
            {"2d6839c5-0b7c-48a1-9c55-4bd9cc873b0f", 1}, // dark blue
            {"32dee745-b6ce-419f-9e86-ae93802d2682", 1}, // dark brown
            {"0758508c-7111-40f9-b33b-706464626ac9", 2}, // grey
            {"51c4a750-c9f4-4cfe-801c-898efc360cb7", 2}, // green
            {"e43f3360-3a08-4755-8b83-a0d37a6c424b", 2}, // light blue
        };
        if (type == gene::Eyes) {
            auto it = eyes.find(family);
            return it == eyes.end() ? 0 : it->second;
        }
        int n = 0;
        if (family.size() >= 8)
            try {
                n = int(std::stoul(family.substr(0, 8), nullptr, 16));
            } catch (const std::logic_error&) {
            }
        if (type == gene::Hair) // black 1, brown 1, blond 2, red 2, grey 0, other 0
            return n == 1 || n == 2 ? 1.0f : n == 3 || n == 4 ? 2.0f : 0.0f;
        if (type == gene::Skin) // Light .1, Medium .3, Medium-Dark .6, Dark .9
            return n == 1 ? 0.1f : n == 2 ? 0.3f : n == 3 ? 0.6f : n == 4 ? 0.9f : 0.0f;
        return 0;
    };
}

std::vector<SkinTone> baseGameSkinTones() {
    return {{"00000001-0000-0000-0000-000000000000", 0.1f},
            {"00000002-0000-0000-0000-000000000000", 0.3f},
            {"00000003-0000-0000-0000-000000000000", 0.6f},
            {"00000004-0000-0000-0000-000000000000", 0.9f}};
}

bool babyIsFemale(bool singleParent, bool parentFemale, const Random& random) {
    if (singleParent)
        return parentFemale;
    return random(2) == 0;
}

} // namespace os2::genetics
