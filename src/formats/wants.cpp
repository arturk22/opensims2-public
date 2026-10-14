// Want definitions, trees, tuning and the SWAF resource (wants.hpp).

#include "wants.hpp"
#include "formats/log_once.hpp"

#include "binreader.hpp"
#include "binwriter.hpp"
#include "dbpf.hpp"
#include "property_set.hpp"
#include "resindex.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>

namespace os2::wants {

namespace {

std::string lower(std::string s) {
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return std::string(s.substr(a, b - a));
}

int64_t number(const std::string& s, int64_t fallback = 0) {
    const std::string t = trim(s);
    if (t.empty())
        return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 0);
    if (end == t.c_str())
        return fallback;
    // Hex GUIDs above 0x7FFFFFFF still fit (strtoll reads them whole).
    return v;
}

bool boolean(const std::string& s) {
    const std::string t = lower(trim(s));
    return t == "true" || t == "1";
}

// The property sets' keys are matched without case ("wantId" in the XML,
// "wantID" in the binary's reader).
std::map<std::string, std::string> lowerKeys(const std::map<std::string, std::string>& in) {
    std::map<std::string, std::string> out;
    for (const auto& [k, v] : in)
        out[lower(k)] = v;
    return out;
}

const std::string* find(const std::map<std::string, std::string>& m, const char* key) {
    auto it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
}

// A small XML reader for the tree and tuning files: elements with
// attributes, text and children (no DTD, CDATA or processing beyond <?...?>
// and comments, which the files do not need).
struct Xml {
    std::string name;
    std::map<std::string, std::string> attrs;
    std::string text;
    std::vector<std::unique_ptr<Xml>> children;
    const Xml* child(std::string_view n) const {
        for (const auto& c : children)
            if (c->name == n)
                return c.get();
        return nullptr;
    }
    std::string attr(const char* k) const {
        auto it = attrs.find(k);
        return it == attrs.end() ? std::string() : it->second;
    }
};

std::string decode(std::string_view s) {
    if (s.find('&') == std::string_view::npos)
        return std::string(s); // nothing to decode (nearly always)
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            static const std::pair<std::string_view, char> ents[] = {
                {"&quot;", '"'}, {"&apos;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}};
            bool done = false;
            for (const auto& [e, c] : ents)
                if (s.substr(i, e.size()) == e) {
                    out += c;
                    i += e.size();
                    done = true;
                    break;
                }
            if (done)
                continue;
        }
        out += s[i++];
    }
    return out;
}

std::unique_ptr<Xml> parseXml(std::string_view s) {
    auto root = std::make_unique<Xml>();
    std::vector<Xml*> stack{root.get()};
    size_t i = 0;
    while (i < s.size()) {
        const size_t lt = s.find('<', i);
        if (lt == std::string_view::npos)
            break;
        if (lt > i)
            stack.back()->text += decode(s.substr(i, lt - i));
        if (s.substr(lt, 4) == "<!--") {
            const size_t e = s.find("-->", lt);
            i = e == std::string_view::npos ? s.size() : e + 3;
            continue;
        }
        const size_t gt = s.find('>', lt);
        if (gt == std::string_view::npos)
            break;
        std::string_view tag = s.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!')
            continue;
        if (tag[0] == '/') {
            if (stack.size() > 1)
                stack.pop_back();
            continue;
        }
        const bool selfClosing = tag.back() == '/';
        if (selfClosing)
            tag.remove_suffix(1);
        auto node = std::make_unique<Xml>();
        size_t p = 0;
        while (p < tag.size() && !std::isspace(static_cast<unsigned char>(tag[p])))
            ++p;
        node->name = std::string(tag.substr(0, p));
        while (p < tag.size()) {
            while (p < tag.size() && std::isspace(static_cast<unsigned char>(tag[p])))
                ++p;
            const size_t eq = tag.find('=', p);
            if (eq == std::string_view::npos)
                break;
            const std::string key = trim(tag.substr(p, eq - p));
            size_t q = eq + 1;
            while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q])))
                ++q;
            if (q >= tag.size())
                break;
            const char quote = tag[q];
            const size_t end = tag.find(quote, q + 1);
            if (end == std::string_view::npos)
                break;
            node->attrs[key] = decode(tag.substr(q + 1, end - q - 1));
            p = end + 1;
        }
        Xml* raw = node.get();
        stack.back()->children.push_back(std::move(node));
        if (!selfClosing)
            stack.push_back(raw);
    }
    return root;
}

// The <cGZPropertySetString> element's keys and values (text of each <AnyX key=...>).
std::map<std::string, std::string> propertySet(const Xml& ps) {
    std::map<std::string, std::string> out;
    for (const auto& c : ps.children)
        if (auto k = c->attrs.find("key"); k != c->attrs.end())
            out[lower(k->second)] = c->text;
    return out;
}

int ageKey(const std::string& k) {
    if (k == "Baby") return 13;
    if (k == "Toddler") return 2;
    if (k == "Child") return 1;
    if (k == "Teen") return 3;
    if (k == "Adult") return 0;
    if (k == "Elder") return 4;
    return -1;
}

int aspirationKey(const std::string& k) {
    if (k == "Romance") return 1;
    if (k == "Family") return 2;
    if (k == "Wealth" || k == "Fortune") return 4;
    if (k == "Reputation" || k == "Popularity") return 0x10;
    if (k == "Knowledge") return 0x20;
    if (k == "GrowUp") return 0x40;
    if (k == "PleasureSeeker") return 0x80;
    if (k == "GrilledCheese") return 0x100;
    return -1;
}

int personalityKey(const std::string& k) {
    if (k == "Nice") return 1;
    if (k == "Mean") return 2;
    if (k == "Active") return 3;
    if (k == "Lazy") return 4;
    if (k == "Playful") return 7;
    if (k == "Serious") return 8;
    if (k == "Outgoing") return 9;
    if (k == "Shy") return 10;
    if (k == "Neat") return 11;
    if (k == "Sloppy") return 12;
    return -1;
}

int factorKey(const std::string& k) {
    if (k == "age") return kAge;
    if (k == "aspiration") return kAspiration;
    if (k == "mood") return kMood;
    if (k == "personality") return kPersonality;
    if (k == "relationship") return kRelationship;
    if (k == "aspirationScore") return kAspirationScore;
    if (k == "personData") return kPersonData;
    return -1;
}

// One <wantNode>: its property set, its ads and its children.
int readNode(Library& lib, const Xml& x, uint32_t tree, int parent, int depth) {
    Node n;
    n.tree = tree;
    n.parent = parent;
    n.depth = depth;
    if (const Xml* ps = x.child("cGZPropertySetString")) {
        const auto p = propertySet(*ps);
        if (auto v = find(p, "id")) n.id = uint32_t(number(*v));
        if (auto v = find(p, "wantid")) n.wantId = uint32_t(number(*v));
        const Definition* def = lib.definition(n.wantId);
        n.subjectType = def ? def->objectType : SubjectType::None;
        if (auto v = find(p, "subjecttype"); v && number(*v) < 6) n.subjectType = SubjectType(number(*v));
        if (auto v = find(p, "visible")) n.visible = boolean(*v);
        if (auto v = find(p, "inheritsubject")) n.inheritSubject = boolean(*v);
        if (auto v = find(p, "regeneration")) n.regeneration = boolean(*v);
        if (auto v = find(p, "lifetime")) n.lifetime = int32_t(number(*v)) * 3600;
        if (auto v = find(p, "adfrequency"))
            n.frequency = int32_t(number(*v)) * 3600;
        else if (n.regeneration && n.lifetime > 0)
            n.frequency = n.lifetime, n.lifetime = 0;
        if (auto v = find(p, "scoreoverride")) n.scoreOverride = int32_t(number(*v));
        if (auto v = find(p, "objectfield")) n.objectField = uint32_t(number(*v));
        if (auto v = find(p, "integerfield")) n.integerField = uint32_t(number(*v));
        if (auto v = find(p, "minscore")) n.minScore = int32_t(number(*v));
        if (auto v = find(p, "maxscore")) n.maxScore = int32_t(number(*v));
        if (auto v = find(p, "relationshipmin")) n.relationshipRange[0] = int32_t(number(*v));
        if (auto v = find(p, "relationshipmax")) n.relationshipRange[1] = int32_t(number(*v));
    }
    for (const auto& c : x.children) {
        if (c->name != "ads")
            continue;
        const int f = factorKey(c->attr("key"));
        if (f < 0)
            continue;
        n.weight[size_t(f)] = int32_t(number(c->attr("weight")));
        n.superZero[size_t(f)] = boolean(c->attr("superZero"));
        for (const auto& b : c->children) {
            if (b->name == "bool" && boolean(b->attr("value"))) {
                const std::string k = b->attr("key");
                if (f == kAge && ageKey(k) >= 0)
                    n.ages.push_back(ageKey(k));
                else if (f == kAspiration && aspirationKey(k) >= 0)
                    n.aspirations.push_back(aspirationKey(k));
                else if (f == kPersonality && personalityKey(k) >= 0)
                    n.personality.push_back(personalityKey(k));
            } else if (b->name == "range") {
                const std::array<int32_t, 2> r{int32_t(number(b->attr("min"))), int32_t(number(b->attr("max")))};
                switch (f) {
                case kMood: n.moodRange = r; break;
                case kRelationship: n.relationshipRange = r; break;
                case kAspirationScore: n.aspirationScoreRange = r; break;
                case kPersonData:
                    n.personDataRange = r;
                    n.personDataIndex = int32_t(number(b->attr("index")));
                    break;
                default: break;
                }
            }
        }
    }
    const int index = int(lib.nodes.size());
    lib.nodes.push_back(std::move(n));
    if (const Xml* kids = x.child("children"))
        for (const auto& c : kids->children)
            if (c->name == "wantNode") {
                const int k = readNode(lib, *c, tree, index, depth + 1);
                lib.nodes[size_t(index)].children.push_back(k);
            }
    return index;
}

void readTree(Library& lib, std::string_view xml) {
    const auto doc = parseXml(xml);
    const Xml* t = doc->child("tree");
    if (!t)
        throw FormatError("want tree: no <tree>");
    Tree tree;
    tree.version = int(number(t->attr("version")));
    if (const Xml* ps = t->child("cGZPropertySetString")) {
        const auto p = propertySet(*ps);
        if (auto v = find(p, "id")) tree.id = uint32_t(number(*v));
        if (auto v = find(p, "folder")) tree.folder = trim(*v);
    }
    // Versions 3..8 are read (the parser reports others, once).
    if (tree.version < 3 || tree.version > 8)
        lib.errors.push_back("want tree " + std::to_string(tree.id) + ": version " + std::to_string(tree.version));
    if (const Xml* root = t->child("wantNode")) {
        tree.root = readNode(lib, *root, tree.id, -1, 0);
        lib.roots.push_back(tree.root);
    }
    lib.trees.push_back(std::move(tree));
}

Instance readInstance(BinReader& r) {
    Instance i;
    i.version = r.u32();
    if (i.version > 10)
        throw FormatError("SWAF: want instance version " + std::to_string(i.version));
    i.owner = r.u16();
    i.guid = r.u32();
    const uint8_t t = r.u8();
    if (t > 6)
        throw FormatError("SWAF: subject type " + std::to_string(t));
    i.subjectType = SubjectType(t);
    switch (t) {
    case 1:
    case 4: i.subjectValue = r.u16(); break;
    case 2:
    case 5: i.subjectValue = r.u32(); break;
    case 3:
        if (i.version > 1)
            i.subjectValue = r.u32();
        break;
    case 6: i.subjectValue = i.version < 10 ? r.u16() : r.u32(); break;
    default: break;
    }
    if ((t == 0 && i.version > 6) || (t == 1 && i.version > 7) || (t > 1 && i.version > 2))
        i.subjectData = r.u16();
    i.time = r.u32();
    if (i.version < 4)
        i.time *= 2;
    if (i.version >= 6)
        i.score = r.i32();
    if (i.version >= 9)
        i.influence = r.i32();
    if (i.version >= 5)
        i.flags = r.u8();
    return i;
}

void writeInstance(BinWriter& w, const Instance& i) {
    w.u32(i.version);
    w.u16(i.owner);
    w.u32(i.guid);
    const auto t = uint8_t(i.subjectType);
    w.u8(t);
    switch (t) {
    case 1:
    case 4: w.u16(uint16_t(i.subjectValue)); break;
    case 2:
    case 5: w.u32(i.subjectValue); break;
    case 3:
        if (i.version > 1)
            w.u32(i.subjectValue);
        break;
    case 6:
        if (i.version < 10)
            w.u16(uint16_t(i.subjectValue));
        else
            w.u32(i.subjectValue);
        break;
    default: break;
    }
    if ((t == 0 && i.version > 6) || (t == 1 && i.version > 7) || (t > 1 && i.version > 2))
        w.u16(i.subjectData);
    w.u32(i.version < 4 ? i.time / 2 : i.time);
    if (i.version >= 6)
        w.i32(i.score);
    if (i.version >= 9)
        w.i32(i.influence);
    if (i.version >= 5)
        w.u8(i.flags);
}

} // namespace

SubjectType subjectTypeFromName(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "sim") return SubjectType::Sim;
    if (n == "guid") return SubjectType::Guid;
    if (n == "category") return SubjectType::Category;
    if (n == "skill") return SubjectType::Skill;
    if (n == "career") return SubjectType::Career;
    return SubjectType::None;
}

const char* subjectTypeName(SubjectType t) {
    switch (t) {
    case SubjectType::None: return "none";
    case SubjectType::Sim: return "Sim";
    case SubjectType::Guid: return "object";
    case SubjectType::Category: return "category";
    case SubjectType::Skill: return "skill";
    case SubjectType::Career: return "career";
    }
    return "?";
}

IntegerOperator integerOperatorFromName(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "equals") return IntegerOperator::Equals;
    if (n == "less_then") return IntegerOperator::LessThan;
    if (n == "greater_then") return IntegerOperator::GreaterThan;
    if (n == "less_then_or_equals") return IntegerOperator::LessOrEquals;
    if (n == "greater_then_or_equals") return IntegerOperator::GreaterOrEquals;
    return IntegerOperator::None;
}

Definition parseDefinition(std::string_view xml) {
    const auto p = lowerKeys(parsePropertySet(xml));
    Definition d;
    auto s = [&](const char* k) { auto v = find(p, k); return v ? trim(*v) : std::string(); };
    d.guid = uint32_t(number(s("id")));
    d.folder = s("folder");
    d.nodeText = s("nodetext");
    d.primaryIcon = uint32_t(number(s("primaryicon")));
    d.secondaryIcon = uint32_t(number(s("secondaryicon")));
    d.stringSet = uint16_t(number(s("stringset")) & 0x7FFF);
    d.checkTree = s("checktree");
    d.feedbackTree = s("feedbacktree");
    d.simArrayTree = s("simarraytree");
    const std::string level = lower(s("level"));
    d.level = level == "power" ? Level::Power : level == "memorable" ? Level::Memorable : Level::Transitory;
    d.score = int32_t(number(s("score")));
    d.influence = int32_t(number(s("influence")));
    d.objectType = subjectTypeFromName(s("objecttype"));
    d.integerType = s("integertype");
    d.integerOperator = integerOperatorFromName(s("integeroperator"));
    d.integerMultiplier = int32_t(number(s("integermultiplier"), 1));
    d.eventRequiresSim = boolean(s("eventrequiressim"));
    d.eventRequiresObject = boolean(s("eventrequiresobject"));
    d.deprecated = boolean(s("deprecated"));
    return d;
}

Tuning parseTuning(std::string_view xml) {
    const auto doc = parseXml(xml);
    const Xml* ws = doc->child("wantSimulator");
    if (!ws)
        throw FormatError("want tuning: no <wantSimulator>");
    Tuning t;
    if (const Xml* levels = ws->child("levels"))
        for (const auto& age : levels->children) {
            if (age->name != "age")
                continue;
            static const char* const order[] = {"rock bottom", "severe failure", "neutral", "bronze",
                                                "silver",      "gold",           "perfect"};
            std::array<int32_t, 7> v{};
            bool any = false;
            for (const auto& th : age->children) {
                const std::string n = lower(th->attr("name"));
                for (size_t i = 0; i < 7; ++i)
                    if (n == order[i]) {
                        v[i] = int32_t(number(th->attr("score")));
                        any = true;
                    }
            }
            if (any)
                t.levels[ageKey(age->attr("name"))] = v;
        }
    if (const Xml* cats = ws->child("categories"))
        for (const auto& c : cats->children)
            if (c->name == "category")
                t.categories.push_back({int32_t(number(c->attr("id"))), c->attr("name"),
                                        uint32_t(number(c->attr("primaryIcon"))),
                                        uint32_t(number(c->attr("secondaryIcon")))});
    if (const Xml* skills = ws->child("skills"))
        for (const auto& c : skills->children)
            if (c->name == "skill")
                t.skills[int(number(c->attr("id")))] = c->attr("name");
    if (const Xml* careers = ws->child("careers"))
        for (const auto& c : careers->children)
            if (c->name == "career")
                t.careers[uint32_t(number(c->attr("id")))] = c->attr("name");
    if (const Xml* goals = ws->child("lifetimeGoals"))
        for (const auto& a : goals->children)
            for (const auto& g : a->children)
                if (g->name == "want")
                    t.lifetimeGoals[a->attr("name")].push_back(uint32_t(number(g->attr("guid"))));
    return t;
}

int ageEnum(int16_t personAge) {
    // Person data 58 holds the life-stage flags (animation.hpp lifestage).
    switch (personAge) {
    case 0x01: return 13; // baby
    case 0x02: return 2;  // toddler
    case 0x03: return 1;  // child
    case 0x10: return 3;  // teen
    case 0x13: return 0;  // adult
    case 0x33: return 4;  // elder
    default: return personAge >= 0x33 ? 4 : personAge >= 0x13 ? 0 : personAge >= 0x10 ? 3 : personAge >= 3 ? 1 : 2;
    }
}

const char* ageName(int e) {
    switch (e) {
    case 13: return "baby";
    case 2: return "toddler";
    case 1: return "child";
    case 3: return "teen";
    case 0: return "adult";
    case 4: return "elder";
    }
    return "?";
}

Library Library::load(ResourceIndex& index) {
    Library lib;
    for (const auto& loc : index.ofType(kTypeWantDefinition)) {
        try {
            const auto bytes = index.read(loc);
            Definition d = parseDefinition({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            if (d.guid)
                lib.definitions[d.guid] = std::move(d);
        } catch (const std::exception& e) {
            lib.errors.push_back(std::string("want definition: ") + e.what());
            os2::logOnce("want definition", std::to_string(loc.entry->tgi.instance), e); // (the app does not print lib.errors)
        }
    }
    for (const auto& loc : index.ofType(kTypeWantTree)) {
        try {
            const auto bytes = index.read(loc);
            readTree(lib, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        } catch (const std::exception& e) {
            lib.errors.push_back(std::string("want tree: ") + e.what());
            os2::logOnce("want tree", std::to_string(loc.entry->tgi.instance), e); // (the app does not print lib.errors)
        }
    }
    if (auto bytes = index.read(Tgi{0, kTuningGroup, kTuningInstance, 0})) {
        try {
            lib.tuning = parseTuning({reinterpret_cast<const char*>(bytes->data()), bytes->size()});
        } catch (const std::exception& e) {
            lib.errors.push_back(std::string("want tuning: ") + e.what());
            os2::logOnce("want tuning", "", e); // (the app does not print lib.errors)
        }
    }
    // The texts: the STR# of Text/Wants.package (their TGIs, group
    // 0xFFFFFFFF, repeat in other packages, so the package is picked by name).
    for (const auto& loc : index.ofType(0x53545223)) {
        const auto& path = loc.package->path();
        if (path.filename() != "Wants.package" || path.parent_path().filename() != "Text")
            continue;
        try {
            lib.texts[uint16_t(loc.entry->tgi.instance)] = StringTable::parse(index.read(loc));
        } catch (const std::exception& err) {
            os2::logOnce("want texts", std::to_string(loc.entry->tgi.instance), err);
        }
    }
    return lib;
}

void Library::addTree(std::string_view xml) { readTree(*this, xml); }

std::string Library::text(const Definition& d, size_t index) const {
    auto it = texts.find(d.stringSet);
    if (it == texts.end())
        return {};
    return it->second.get(index);
}

const Category* Library::category(int32_t id) const {
    for (const auto& c : tuning.categories)
        if (c.id == id)
            return &c;
    return nullptr;
}

Record Record::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Record rec;
    rec.version = r.u32();
    if (rec.version > 6)
        throw FormatError("SWAF: version " + std::to_string(rec.version));
    if (rec.version >= 3 && r.u32() != 0)
        rec.lifetime = readInstance(r);
    if (rec.version >= 2)
        rec.wantSlots = r.u32();
    for (uint32_t n = r.u32(), i = 0; i < n; ++i)
        rec.wants.push_back(readInstance(r));
    if (rec.version >= 2)
        rec.fearSlots = r.u32();
    for (uint32_t n = r.u32(), i = 0; i < n; ++i)
        rec.fears.push_back(readInstance(r));
    if (rec.version >= 4)
        rec.locks = r.u32();
    // The achievement history (cTSWantAchievementHistory, written right
    // after by WritePerson, FUN_00aee07c): u32 version 2, i32 personal
    // clock, u32 count, then per want GUID: u32 GUID, u32 count, instances.
    if (r.remaining() >= 12) {
        rec.history.version = r.u32();
        rec.history.clock = r.i32();
        for (uint32_t n = r.u32(), i = 0; i < n; ++i) {
            const uint32_t guid = r.u32();
            std::vector<Instance> list;
            for (uint32_t m = r.u32(), j = 0; j < m; ++j)
                list.push_back(readInstance(r));
            rec.history.entries.emplace_back(guid, std::move(list));
        }
    }
    const auto rest = r.bytes(r.remaining());
    rec.tail.assign(rest.begin(), rest.end());
    return rec;
}

std::vector<uint8_t> Record::write() const {
    BinWriter w;
    w.u32(version);
    if (version >= 3) {
        w.u32(lifetime ? 1 : 0);
        if (lifetime)
            writeInstance(w, *lifetime);
    }
    if (version >= 2)
        w.u32(wantSlots);
    w.u32(uint32_t(wants.size()));
    for (const auto& i : wants)
        writeInstance(w, i);
    if (version >= 2)
        w.u32(fearSlots);
    w.u32(uint32_t(fears.size()));
    for (const auto& i : fears)
        writeInstance(w, i);
    if (version >= 4)
        w.u32(locks);
    w.u32(history.version);
    w.i32(history.clock);
    w.u32(uint32_t(history.entries.size()));
    for (const auto& [guid, list] : history.entries) {
        w.u32(guid);
        w.u32(uint32_t(list.size()));
        for (const auto& i : list)
            writeInstance(w, i);
    }
    w.raw(tail);
    return std::move(w.bytes());
}

} // namespace os2::wants
