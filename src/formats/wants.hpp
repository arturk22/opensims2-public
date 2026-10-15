#pragma once

// Wants and fears (docs/wants-mac.md): the want definitions, the want trees
// and the tuning of TSData/Res/Wants, the want texts of Text/Wants.package,
// and the SWAF resource (0xCD95548E) that keeps each Sim's wants, fears and
// achievement history in the neighborhood package.

#include "strings.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace os2 {
class ResourceIndex;
}

namespace os2::wants {

constexpr uint32_t kTypeWantDefinition = 0xED7D7B4D; // Wants.package, XML property sets
constexpr uint32_t kTypeWantTree = 0x6D814AFE;       // WantTrees.package, <tree> XML
constexpr uint32_t kTypeSwaf = 0xCD95548E;           // per Sim, instance = neighbor ID
// WantTuning.package: <wantSimulator> XML.
constexpr uint32_t kTuningGroup = 0xCDA53B6F, kTuningInstance = 0x2D7EE26B;
// The achievement every Sim gets when it is read ("Is Born",
// cTSWantSimulator::AddIsBornAchievement).
constexpr uint32_t kIsBornWant = 0xAD9ACD9A;
// The "Wants - Controller" object the check and feedback trees run on
// (GetWantControllerObject), and the group of "Wants - Check Trees", the
// object definition whose code holds those trees.
constexpr uint32_t kControllerGuid = 0x6D9E58F0;
constexpr uint32_t kCheckTreesGroup = 0x7F739728;

// cTSWantSubject types (the definition's objectType; SWAF instance byte).
enum class SubjectType : uint8_t { None = 0, Sim = 1, Guid = 2, Category = 3, Skill = 4, Career = 5 };
SubjectType subjectTypeFromName(const std::string& name); // "None", "Sim", "Guid", ...
const char* subjectTypeName(SubjectType t);

// Integer comparison of wants with a number (integerOperator), as the
// achievement tests use it: the achieved value against the want's.
// The names are the editor's; the order is inferred from the compares.
enum class IntegerOperator : uint8_t { None = 0, Equals, LessThan, GreaterThan, LessOrEquals, GreaterOrEquals };
IntegerOperator integerOperatorFromName(const std::string& name);

enum class Level : uint8_t { Transitory, Power, Memorable };

struct Definition {
    uint32_t guid = 0;
    std::string folder, nodeText; // editor only
    uint32_t primaryIcon = 0, secondaryIcon = 0;
    uint16_t stringSet = 0; // STR# instance in Text/Wants.package (15 bits)
    std::string checkTree, feedbackTree, simArrayTree;
    Level level = Level::Transitory;
    int32_t score = 0; // aspiration points; negative = fear
    int32_t influence = 0;
    SubjectType objectType = SubjectType::None;
    std::string integerType;
    IntegerOperator integerOperator = IntegerOperator::None;
    int32_t integerMultiplier = 1;
    bool eventRequiresSim = false, eventRequiresObject = false, deprecated = false;
    bool isFear() const { return score < 0; } // no base XML has "isFear": the score's sign
};
Definition parseDefinition(std::string_view xml);

// The factors of a node's priority (cITSWantNode::tFactorType).
enum Factor : uint8_t { kAge, kAspiration, kMood, kPersonality, kRelationship, kAspirationScore, kPersonData, kFactorCount };

// A node of a want tree (cTSWantNode, read by FUN_00e4d664 and the tree
// parser FUN_019c4c90 of the BV binary).
struct Node {
    uint32_t id = 0;     // the node's own ID
    uint32_t wantId = 0; // its definition
    uint32_t tree = 0;
    int parent = -1;     // index in Library::nodes
    std::vector<int> children;
    int depth = 0;
    bool visible = true;        // can be offered (default true)
    bool inheritSubject = true; // takes the subject of the achievement it follows (default true)
    bool regeneration = false;  // "evergreen": offered again after it is achieved
    // In want-clock units (3600 per hour: "lifetime" hours × 0xE10). A
    // regenerating node with a lifetime gets it as its frequency instead
    // (the binary reads "adFrequency", not the XML's "frequency", so the
    // XML frequencies are not used).
    int32_t lifetime = 0, frequency = 0;
    std::optional<int32_t> scoreOverride; // else the definition's score
    SubjectType subjectType = SubjectType::None; // "subjectType", else the definition's objectType
    uint32_t objectField = 0, integerField = 0;
    int32_t minScore = 0, maxScore = 100;
    std::array<int32_t, kFactorCount> weight{};
    std::array<bool, kFactorCount> superZero{}; // "exclusionary": the factor at 0 zeroes the priority
    std::vector<int> ages;           // enum values (Adult 0, Child 1, Toddler 2, Teen 3, Elder 4, Baby 13)
    std::vector<int> aspirations;    // person data 46 values (Romance 1, Family 2, Fortune 4, ...)
    std::vector<int> personality;    // 1 Nice, 2 Mean, 3 Active, 4 Lazy, 7 Playful, 8 Serious, 9 Outgoing, 10 Shy, 11 Neat, 12 Sloppy
    std::array<int32_t, 2> moodRange{}, relationshipRange{}, aspirationScoreRange{}, personDataRange{};
    int32_t personDataIndex = 0; // 1..0xCC (person data index + 1)

    bool useFactor(int f) const { return weight[size_t(f)] >= 1 || superZero[size_t(f)]; }
    static bool hasRange(const std::array<int32_t, 2>& r) { return r[0] != 0 || r[1] != 0; }
};

struct Tree {
    uint32_t id = 0;
    std::string folder;
    int version = 0;
    int root = -1;
};

struct Category {
    int32_t id = 0;
    std::string name;
    uint32_t primaryIcon = 0, secondaryIcon = 0;
};

struct Tuning {
    // Raw-score thresholds by age enum, ascending: rock bottom, severe
    // failure, neutral, bronze, silver, gold, perfect.
    std::map<int, std::array<int32_t, 7>> levels;
    std::vector<Category> categories;
    std::map<int, std::string> skills;     // person data index -> name
    std::map<uint32_t, std::string> careers;
    std::map<std::string, std::vector<uint32_t>> lifetimeGoals; // BV only in effect
};
Tuning parseTuning(std::string_view xml);

// Ages as the want simulator numbers them, from person data 58.
int ageEnum(int16_t personAge);
const char* ageName(int ageEnum);

struct Library {
    std::map<uint32_t, Definition> definitions;
    std::vector<Node> nodes;
    std::vector<Tree> trees;
    std::vector<int> roots; // node indices of the trees' roots, by increasing tree id (one per id)
    Tuning tuning;
    std::map<uint16_t, StringTable> texts; // Text/Wants.package STR# by instance
    std::vector<std::string> errors;

    // From the game's packages (any index over TSData/Res).
    static Library load(ResourceIndex& index);
    bool empty() const { return definitions.empty(); }
    // A <tree> XML (after the definitions it uses: a node takes its
    // subject type from its definition).
    void addTree(std::string_view xml);

    const Definition* definition(uint32_t guid) const {
        auto it = definitions.find(guid);
        return it == definitions.end() ? nullptr : &it->second;
    }
    // The want's text: entry `index` of its string set (0 = the name).
    std::string text(const Definition& d, size_t index = 0) const;
    const Category* category(int32_t id) const;
};

// --- SWAF ---

// cTSWantInstance as saved (Write/Read BV 0xae8f84/0xae9140). Fields
// depend on the version (8 in the base game's data, 7 in older ones).
struct Instance {
    uint32_t version = 8;
    uint16_t owner = 0; // neighbor ID
    uint32_t guid = 0;
    SubjectType subjectType = SubjectType::None;
    uint32_t subjectValue = 0; // neighbor ID, GUID, category, skill or career
    uint16_t subjectData = 0;  // the number of a want with a number
    uint32_t time = 0;         // want-clock time it was achieved (0 = not)
    int32_t score = 0;         // v >= 6
    int32_t influence = 0;     // v >= 9 (BV)
    uint8_t flags = 0;         // v >= 5: 0x01 locked (BV), 0x02 checked, 0x04 check passed, 0x10 purgeable
    bool operator==(const Instance&) const = default;
};

struct History {
    uint32_t version = 2;
    int32_t clock = 0; // the Sim's personal want clock
    // By want GUID (ascending, as the game's map), the achievements of it.
    std::vector<std::pair<uint32_t, std::vector<Instance>>> entries;
    bool operator==(const History&) const = default;
};

struct Record {
    uint32_t version = 1;
    std::optional<Instance> lifetime; // v >= 3 (BV)
    uint32_t wantSlots = 4, fearSlots = 3; // saved from v >= 2
    std::vector<Instance> wants, fears;    // the filled slots, in slot order
    uint32_t locks = 0;                    // v >= 4 (BV)
    History history;
    // Bytes after the history: the game writes a record over the old one
    // without cutting it, so a shorter record keeps the old tail (434 of
    // 1045 base records have none). Kept as read.
    std::vector<uint8_t> tail;

    static Record parse(std::span<const uint8_t> data);
    std::vector<uint8_t> write() const; // parse's inverse
};

} // namespace os2::wants
