// Genetics (vm/genetics.hpp): dominant regions, PickGene, descendant face.
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
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
