// Genetics (vm/genetics.hpp): dominant regions, PickGene, descendant face.
#include "formats/cas_catalog.hpp"
#include "formats/cas_personality.hpp"
#include "formats/lxnr.hpp"
#include "vm/genetics.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>

using namespace os2::genetics;

static int failures = 0;
static void check(bool cond, const char* what) {
    std::printf("%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        ++failures;
}

// A random source that returns the given values in turn (mod n).
static Random scripted(std::deque<uint32_t> values) {
    auto q = std::make_shared<std::deque<uint32_t>>(std::move(values));
    return [q](uint32_t n) {
        uint32_t v = q->empty() ? 0 : q->front();
        if (!q->empty())
            q->pop_front();
        return n ? v % n : 0;
    };
}

static const std::string kBlack = "00000001-0000-0000-0000-000000000000", kBrown = "00000002-0000-0000-0000-000000000000",
                         kBlond = "00000003-0000-0000-0000-000000000000", kRed = "00000004-0000-0000-0000-000000000000";

int main() {
    // A woman whose rolls are all 60: nose 50 brow 50 mouth 66 cheek 50 cheekbones 66 eyes 66 jaw 50 ears 50.
    check(setupDominantRegions(true, scripted(std::deque<uint32_t>(8, 60))) == "nose,brow,cheek,jaw,ears,",
          "dominant regions: a woman's roll of 60 passes the 50s, not the 66s");
    check(setupDominantRegions(false, scripted(std::deque<uint32_t>(8, 60))) == "mouth,cheek,cheekbones,eyes,ears,",
          "dominant regions: a man's roll of 60 passes the 50s, not the 66s");

    const GeneticValue value = baseGameGeneticValues();
    // Each parent gives one allele (rolls 0 = dominant); hair: blond (2) vs black (1): black dominant.
    Allele h = pickGene(gene::Hair, {kBlond, kBlond}, {kBlack, kBrown}, value, scripted({0, 0}));
    check(h.first == kBlack && h.second == kBlond, "PickGene: the hair with the lower genetic value is dominant");
    h = pickGene(gene::Hair, {kBlond, kBlond}, {kBlack, kRed}, value, scripted({0, 1, 1}));
    check(h.first == kRed && h.second == kBlond, "PickGene: equal genetic values (blond, red) in a drawn order");
    std::string between;
    const Allele s = pickGene(gene::Skin, {kBlack, kBlack}, {kRed, kRed}, value, scripted({0, 0, 1}), baseGameSkinTones(), &between);
    check(s.first == kBlack && s.second == kRed && between == kBrown,
          "PickGene: skin alleles stay (mother, father) and gene 6 is a tone between theirs");

    Dna a, b;
    a.alleles[gene::DominantRegions] = {"nose,", ""};
    b.alleles[gene::DominantRegions] = {"brow,eyes,", ""};
    // Rolls: nose 10 (50/50 -> a), brow 60 (only in b: <= 66 -> b), mouth 70 (-> b), eyes 80 (only in b, > 66 -> a), jaw 0, ears 99.
    const DescendantFace f = determineDescendantFace(a, b, scripted({10, 60, 70, 80, 0, 99}));
    check(f.script() == "nose,0;brow,1;mouth,1;cheek,1;eyes,0;cheekbones,0;jaw,0;ears,1;" && f.dominant == "nose,brow,",
          "descendant face: a parent per region (eyes bring cheekbones, mouth brings cheek) and the dominant ones");

    Dna d = Dna::fromProperties({{"1", kBlack}, {"268435457", kBlond}, {"5", "nose,"}});
    check(d.dominant(gene::Hair) == kBlack && d.alleles[gene::Hair].second == kBlond && d.dominantRegions().size() == 1 &&
              d.toProperties().at("268435457") == kBlond,
          "DNA: SDNA keys t and 0x10000000 + t");
    check(babyIsFemale(true, true, scripted({1})) && !babyIsFemale(true, false, scripted({0})),
          "a lone parent's baby has its gender");
    // Zodiac (docs/cas-mac.md §6): a sign's own points give it back; 5 everywhere is Cancer.
    bool signs = true;
    for (int z = 1; z <= 12; ++z)
        signs = signs && os2::cas::zodiacSign(os2::cas::zodiacPersonality(z)) == z;
    check(signs, "every sign's points compute to that sign");
    check(os2::cas::zodiacSign(os2::cas::Personality{}) == 4, "5 points in each trait is Cancer");
    check(os2::cas::compatibleSigns(1) == std::array<int, 2>{3, 2} && os2::cas::incompatibleSigns(1) == std::array<int, 2>{4, 7},
          "Aries attracts Gemini and Taurus, repels Cancer and Libra");
    os2::cas::Personality p{10, 10, 5, 0, 0};
    os2::cas::setTrait(p, 3, 4);
    check(p.neat == 0 && p.total() == 25, "a trait cannot pass the 25 points");
    // The LxNR face structure: keys, the shared vector pool, round trip.
    {
        using os2::cas::FaceStructure;
        check(FaceStructure::key(os2::cas::age::Adult, os2::cas::gender::Female) == 0x003, "LxNR key: adult female 0x003");
        check(FaceStructure::key(os2::cas::age::Baby, os2::cas::gender::Male, true) == 0xF0105,
              "LxNR key: baby male low detail 0xF0105");
        FaceStructure fs;
        fs.set(0x003, {{{0.001f, -0.002f, 0.0f}, 0.25f, 0.5f}, {{0.001f, -0.002f, 0.0f}, 0.75f, 0.5f}});
        fs.set(0x103, {{{0.0f, 0.0f, 0.0f}, 0.25f, 0.5f}});
        const auto bytes = fs.write();
        // 8 + 2 keys + 'keye' + u16 + 4 distinct vectors + 'mstr' + (2 + 8) + (2 + 4) + 'data'
        check(bytes.size() == 8 + 8 + 4 + 2 + 4 * 12 + 4 + 10 + 6 + 4, "LxNR: equal vectors are pooled once");
        const FaceStructure back = FaceStructure::parse(bytes);
        check(back.keys == fs.keys && back.data == fs.data, "LxNR: write then parse gives the same data");
        check(back.find(os2::cas::age::Adult, os2::cas::gender::Male) && back.find(os2::cas::age::Adult, os2::cas::gender::Male)->size() == 1,
              "LxNR: find by age and gender");
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
