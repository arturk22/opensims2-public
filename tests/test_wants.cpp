// Wants and fears: the SWAF format, the tuning and the aspiration meter,
// a want tree's candidates and slots, and Want Satisfy (src/formats/wants,
// src/vm/wants).

#include "formats/wants.hpp"
#include "vm/gnu_sort.hpp"
#include "vm/wants.hpp"

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static const char* kTuning = R"(<wantSimulator>
   <levels>
      <age name="Adult">
         <threshold name="perfect" score="12000"/>
         <threshold name="gold" score="7800"/>
         <threshold name="silver" score="4800"/>
         <threshold name="bronze" score="2400"/>
         <threshold name="neutral" score="1200"/>
         <threshold name="severe failure" score="600"/>
         <threshold name="rock bottom" score="0"/>
      </age>
      <age name="Elder">
         <threshold name="perfect" score="15000"/>
         <threshold name="gold" score="9750"/>
         <threshold name="silver" score="6000"/>
         <threshold name="bronze" score="3000"/>
         <threshold name="neutral" score="1500"/>
         <threshold name="severe failure" score="750"/>
         <threshold name="rock bottom" score="0"/>
      </age>
   </levels>
   <categories><category id="10000" name="dining chair" primaryIcon="0xc0001420" secondaryIcon="0x00000000"/></categories>
   <skills><skill id="10" name="Cooking"/></skills>
</wantSimulator>)";

static std::string definitionXml(uint32_t guid, int score, const char* type) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, R"(<?xml version="1.0"?>
<cGZPropertySetString version="6">
  <AnyUint32 key="id" type="0xeb61e4f7">0x%08x</AnyUint32>
  <AnyString key="nodeText" type="0x0b8bea18">test</AnyString>
  <AnyUint32 key="primaryIcon" type="0xeb61e4f7">0xc0000158</AnyUint32>
  <AnyUint32 key="stringSet" type="0xeb61e4f7">161</AnyUint32>
  <AnyString key="checkTree" type="0x0b8bea18" />
  <AnyString key="level" type="0x0b8bea18">Transitory</AnyString>
  <AnySint32 key="score" type="0x0c264712">%d</AnySint32>
  <AnyString key="objectType" type="0x0b8bea18">%s</AnyString>
  <AnyString key="integerOperator" type="0x0b8bea18">None</AnyString>
</cGZPropertySetString>)",
                  guid, score, type);
    return buf;
}

// A tree: an invisible root (achieved) with a visible child "B" that
// inherits the root's Sim subject and lasts 24 hours, and a fear root.
static const char* kTree = R"(<?xml version="1.0"?>
<tree version="7">
  <cGZPropertySetString><AnyUint32 key="id" type="0xeb61e4f7">0x1</AnyUint32></cGZPropertySetString>
  <wantNode key="root">
    <cGZPropertySetString>
      <AnyUint32 key="wantId" type="0xeb61e4f7">0x00000a00</AnyUint32>
      <AnyUint32 key="id" type="0xeb61e4f7">1</AnyUint32>
      <AnyBoolean key="visible" type="0xcba908e1">False</AnyBoolean>
      <AnySint32 key="lifetime" type="0x0c264712">0</AnySint32>
      <AnySint32 key="minScore" type="0x0c264712">0</AnySint32>
      <AnySint32 key="maxScore" type="0x0c264712">100</AnySint32>
    </cGZPropertySetString>
    <ads key="age" weight="17" superZero="False" locked="False"><bool key="Adult" value="True" /></ads>
    <children>
      <wantNode key="node">
        <cGZPropertySetString>
          <AnyUint32 key="wantId" type="0xeb61e4f7">0x00000b00</AnyUint32>
          <AnyUint32 key="id" type="0xeb61e4f7">2</AnyUint32>
          <AnyBoolean key="visible" type="0xcba908e1">True</AnyBoolean>
          <AnySint32 key="lifetime" type="0x0c264712">24</AnySint32>
          <AnyBoolean key="inheritSubject" type="0xcba908e1">True</AnyBoolean>
          <AnySint32 key="minScore" type="0x0c264712">10</AnySint32>
          <AnySint32 key="maxScore" type="0x0c264712">90</AnySint32>
        </cGZPropertySetString>
        <ads key="age" weight="10" superZero="True" locked="False"><bool key="Adult" value="True" /></ads>
        <ads key="aspiration" weight="10" superZero="False" locked="False"><bool key="Romance" value="True" /></ads>
      </wantNode>
    </children>
  </wantNode>
</tree>)";

static const char* kFearTree = R"(<tree version="7">
  <cGZPropertySetString><AnyUint32 key="id" type="0xeb61e4f7">0x2</AnyUint32></cGZPropertySetString>
  <wantNode key="root">
    <cGZPropertySetString>
      <AnyUint32 key="wantId" type="0xeb61e4f7">0x00000c00</AnyUint32>
      <AnyUint32 key="id" type="0xeb61e4f7">3</AnyUint32>
      <AnySint32 key="lifetime" type="0x0c264712">48</AnySint32>
      <AnyBoolean key="regeneration" type="0xcba908e1">True</AnyBoolean>
    </cGZPropertySetString>
    <ads key="age" weight="17" superZero="False" locked="False"><bool key="Adult" value="True" /></ads>
  </wantNode>
</tree>)";

static void testRecord() {
    wants::Record r;
    wants::Instance w;
    w.owner = 1;
    w.guid = 0x2D9ACDEF;
    w.subjectType = wants::SubjectType::Sim;
    w.subjectValue = 0x19;
    w.score = 750;
    w.flags = 6;
    r.wants.push_back(w);
    wants::Instance f;
    f.version = 7;
    f.owner = 1;
    f.guid = 0x6DB90E8F;
    f.subjectType = wants::SubjectType::Category;
    f.subjectValue = 10045;
    f.subjectData = 2000;
    f.score = -500;
    f.flags = 6;
    r.fears.push_back(f);
    r.history.clock = 9086535;
    r.history.entries.push_back({0x0D9ACDBC, {w}});
    r.history.entries.push_back({0x0D9ACDD3, {}});
    r.tail = {0, 0, 0, 0, 0x1E, 0x9D};
    const auto bytes = r.write();
    // u32 1, u32 1, a v8 Sim instance of 24 bytes, ...
    check(bytes.size() > 8 && bytes[0] == 1 && bytes[4] == 1, "SWAF header");
    check(bytes[8] == 8 && bytes[14] == 0xEF && bytes[18] == 1 && bytes[19] == 0x19, "v8 instance layout");
    const auto back = wants::Record::parse(bytes);
    check(back.wants == r.wants && back.fears == r.fears && back.history == r.history && back.tail == r.tail,
          "SWAF round trip");
    check(back.write() == bytes, "SWAF written back the same");
    // A v7 Sim instance has no subject number.
    wants::Record r7;
    wants::Instance s7 = w;
    s7.version = 7;
    r7.wants.push_back(s7);
    const auto b7 = r7.write();
    check(b7.size() == 4 + 4 + 22 + 4 + 12, "v7 Sim instance: 22 bytes");
    check(wants::Record::parse(b7).wants[0] == s7, "v7 instance round trip");
}

static void testTuningAndMeter() {
    wants::Library lib;
    lib.tuning = wants::parseTuning(kTuning);
    check(lib.tuning.levels.size() == 2, "two ages in the tuning");
    check(lib.tuning.levels[0][2] == 1200 && lib.tuning.levels[0][6] == 12000, "adult thresholds ascending");
    check(lib.category(10000) && lib.category(10000)->name == "dining chair", "categories");
    // Examples from the docs and from N001's saved Sims (person data 160 vs 162).
    check(normalizedAspirationScore(lib, 0, 1200) == 0, "adult neutral -> 0");
    check(normalizedAspirationScore(lib, 0, 0) == -100, "adult 0 -> -100");
    check(normalizedAspirationScore(lib, 0, 12000) == 100, "adult perfect -> 100");
    check(normalizedAspirationScore(lib, 0, 2000) == 17, "Sim 7 of N001: 2000 -> 17");
    check(normalizedAspirationScore(lib, 0, 1050) == -12, "Sim 17 of N001: 1050 -> -12 (ties to even)");
    check(normalizedAspirationScore(lib, 4, 11500) == 83, "Vladmir: elder 11500 -> 83");
    check(aspirationLevel(lib, 0, 1200) == 2 && aspirationLevel(lib, 0, 500) == 0 && aspirationLevel(lib, 4, 11500) == 5,
          "aspiration levels");
    // RawAspirationScore (BV 0x506efc): the inverse, rounded to nearest.
    check(rawAspirationScore(lib, 0, 0) == 1200 && rawAspirationScore(lib, 0, -100) == 0 &&
              rawAspirationScore(lib, 0, 100) == 12000 && rawAspirationScore(lib, 0, 17) == 2016 &&
              rawAspirationScore(lib, 0, 500) == 12000 && rawAspirationScore(lib, 9, 0) == 0,
          "raw score of a meter value (clamped; an age without thresholds gives 0)");
}

static void testSimulator() {
    auto lib = std::make_shared<wants::Library>();
    lib->tuning = wants::parseTuning(kTuning);
    for (const auto& [guid, score, type] : {std::tuple{0xA00u, 0, "Sim"}, std::tuple{0xB00u, 1000, "Sim"},
                                            std::tuple{0xC00u, -500, "None"}}) {
        auto d = wants::parseDefinition(definitionXml(guid, score, type));
        lib->definitions[d.guid] = d;
    }
    check(lib->definitions.at(0xC00).isFear() && !lib->definitions.at(0xB00).isFear(), "fear by the score's sign");
    lib->addTree(kTree);
    lib->addTree(kFearTree);
    check(lib->nodes.size() == 3 && lib->roots.size() == 2, "tree nodes");
    check(lib->nodes[1].lifetime == 24 * 3600 && lib->nodes[1].parent == 0, "lifetime in want-clock units");
    check(lib->nodes[2].frequency == 48 * 3600 && lib->nodes[2].lifetime == 0, "regeneration: lifetime -> frequency");

    CodeLibrary code;
    VM vm(code);
    for (int16_t id : {1, 2}) {
        Neighbor n;
        n.personData[persondata::NeighborId] = id;
        n.personData[persondata::PersonAge] = 0x13; // adult
        n.personData[46] = 1;                       // Romance
        n.personData[162] = 120;                    // raw 1200: neutral
        n.firstName = id == 1 ? "Don" : "Nina";
        vm.neighbors[id] = n;
    }
    WantSystem& ws = installWants(vm, lib);
    // Sim 1 achieved the root about Sim 2 an hour ago.
    SimWants& s = ws.sims[1];
    s.personal = 10 * 3600;
    auto a = std::make_shared<WantInstance>();
    a->data.owner = 1;
    a->data.guid = 0xA00;
    a->data.subjectType = wants::SubjectType::Sim;
    a->data.subjectValue = 2;
    a->data.time = 9 * 3600;
    s.history[0xA00].push_back(a);

    requestWantsUpdate(vm, 1, 3);
    for (int i = 0; i < 5; ++i)
        tickWants(vm);
    check(s.wants[0] && s.wants[0]->data.guid == 0xB00, "the child want is rolled");
    check(s.wants[0] && s.wants[0]->data.subjectValue == 2, "it inherits the root's subject");
    check(s.fears[0] && s.fears[0]->data.guid == 0xC00, "the fear root is rolled");
    // Priority: (10·1 + 10·1)/20 = 1 → 90, × the time left (23 of 24 hours).
    check(s.wants[0] && std::abs(s.wants[0]->priority - 90.0f * 23.0f / 24.0f) < 0.5f, "priority with the time factor");
    check(s.wants[0] && s.wants[0]->data.score == 1000, "slot score from the definition");

    // Fulfilling it: an achievement, the slot empties, +1000 points.
    WantSubject subj{wants::SubjectType::Sim, 2, 0};
    check(markWantAchieved(vm, 1, 0xB00, subj), "Want Satisfy fulfils the slot");
    check(!s.wants[0], "the slot is emptied");
    check(vm.neighbors[1].personData[162] == 220, "raw score += 1000 (/10)");
    check(vm.neighbors[1].personData[160] == 21, "2200: between neutral (0) and bronze (25) -> 21");
    check(vm.neighbors[1].personData[161] == 100, "reward points += 100");
    check(s.history.count(0xB00) && s.history[0xB00].size() == 1, "the achievement is in the history");
    // A want not in a slot only goes to the history.
    check(!markWantAchieved(vm, 2, 0xB00, subj), "no slot for Sim 2");
    check(vm.neighbors[2].personData[162] == 120, "Sim 2's score unchanged");
    // A fear coming true takes points.
    check(markWantAchieved(vm, 1, 0xC00, {}), "the fear comes true");
    check(vm.neighbors[1].personData[162] == 170, "fear: -500");
    check(vm.neighbors[1].personData[161] == 50, "reward points -50");

    // The record from the state, and back.
    const auto rec = wantRecord(vm, 1);
    check(rec.history.entries.size() == 3, "history entries by GUID");
    const auto back = wants::Record::parse(rec.write());
    check(back.history == rec.history && back.wants == rec.wants, "state record round trip");
    check(wantRecordChanged(vm, 1), "a Sim without a SWAF whose state changed gets one");
}

// The game's std::sort (vm/gnu_sort.hpp): a sort, whatever the ties; with
// 16 elements or fewer it is an insertion sort, so ties keep their order.
static void testGnuSort() {
    std::mt19937 rng(7);
    bool sorted = true, permutation = true, stableSmall = true;
    for (int n : {0, 1, 2, 5, 16, 17, 33, 100, 1000}) {
        for (int range : {3, 1000}) {
            std::vector<std::pair<int, int>> v;
            for (int i = 0; i < n; ++i)
                v.push_back({int(rng() % unsigned(range)), i});
            auto w = v;
            gnu::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return b.first < a.first; });
            sorted &= std::is_sorted(w.begin(), w.end(), [](const auto& a, const auto& b) { return b.first < a.first; });
            auto x = w;
            std::sort(x.begin(), x.end());
            std::sort(v.begin(), v.end());
            permutation &= x == v;
            if (n <= 16)
                for (size_t i = 1; i < w.size(); ++i)
                    stableSmall &= w[i - 1].first != w[i].first || w[i - 1].second < w[i].second;
        }
    }
    check(sorted && permutation, "gnu::sort sorts");
    check(stableSmall, "gnu::sort keeps ties in order up to 16 elements");

    // The roots go by tree id, one per id (cTSWantTreeManager's map).
    wants::Library lib;
    lib.addTree(kFearTree); // tree 2
    lib.addTree(kTree);     // tree 1
    lib.addTree(kTree);
    check(lib.roots.size() == 2 && lib.nodes[size_t(lib.roots[0])].tree == 1 && lib.nodes[size_t(lib.roots[1])].tree == 2,
          "roots in increasing tree id, a repeated id left out");
}

int main() {
    testRecord();
    testTuningAndMeter();
    testSimulator();
    testGnuSort();
    if (failures)
        std::fprintf(stderr, "%d failures\n", failures);
    return failures ? 1 : 0;
}
