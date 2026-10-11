// objt records from object definitions (save_types.hpp).

#include "save_types.hpp"

#include "formats/simantics.hpp"
#include "formats/strings.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <vector>

namespace os2::vm {

namespace {

const Behavior* tree(CodeLibrary& code, uint32_t group, uint16_t id) {
    uint32_t g = 0;
    if (id >= 0x100 && id < 0x1000)
        g = kGlobalGroup;
    else if (id >= 0x1000 && id < 0x2000)
        g = group;
    else if (id >= 0x2000)
        g = code.semiGlobalFor(group);
    return g ? code.behavior(g, id) : nullptr;
}

// What the objt calls a tree's version (cTSBehavior vtable 0x74, BV
// 0x48d9f0): the root's tree version in the high 16 bits plus the sum of
// the tree versions of the root and of every tree it reaches through calls
// (opcodes >= 0x100), each counted once.
uint32_t treeVersion(CodeLibrary& code, uint32_t group, uint16_t root) {
    std::vector<uint16_t> pending{root}, seen;
    while (!pending.empty()) {
        const uint16_t id = pending.back();
        pending.pop_back();
        if (std::find(seen.begin(), seen.end(), id) != seen.end())
            continue;
        seen.push_back(id);
        if (const Behavior* b = tree(code, group, id))
            for (const auto& in : b->instructions)
                if (in.opcode > 0xFF)
                    pending.push_back(in.opcode);
    }
    const Behavior* r = tree(code, group, root);
    uint32_t v = r ? (r->treeVersion & 0xFFFF) << 16 : 0;
    for (uint16_t id : seen)
        if (const Behavior* b = tree(code, group, id))
            v += b->treeVersion;
    return v;
}

} // namespace

std::optional<LotObjects::SaveType> saveTypeRecord(CodeLibrary& code, uint32_t guid) {
    const auto where = code.definitionByGuid(guid);
    if (!where)
        return std::nullopt;
    const auto [group, instance] = *where;
    const ObjectDefinition* d = code.definition(group, instance);
    if (!d)
        return std::nullopt;
    LotObjects::SaveType t;
    t.guid = guid;
    // The binary's OBJD field i is at file offset 0x40 + 2i.
    auto field = [&](size_t i) { return d->field(0x40 + 2 * i); };
    uint16_t init = 0, main = 0;
    if (const ObjectFunctions* f = code.functions(group, instance)) {
        if (f->functions.size() > 0)
            init = f->functions[0].action;
        if (f->functions.size() > 1)
            main = f->functions[1].action;
    }
    t.initTreeVersion = treeVersion(code, group, init);
    t.mainTreeVersion = main ? treeVersion(code, group, main) : 0;
    t.field3A = field(0x3A);
    t.field3B = field(0x3B);
    // The semi-global group's STR# 0x100: the semi-attribute labels.
    if (const uint32_t semi = code.semiGlobalFor(group))
        t.stringCount = uint32_t(code.attributeLabelCount(semi));
    t.objectType = int16_t(field(9));
    // Parts of a multi-tile object (a master ID and a sub index) have no
    // catalog entry: type 1, no name, saved as XMTO.
    const bool part = d->masterId() != 0 && d->subIndex() != 0xFFFF;
    t.flags = part ? 1 : 0;
    if (!part) {
        // The catalog name (CTSS of the instance in OBJD field 0x29, string
        // 0), "*" and the file name without one.
        const uint16_t ctss = field(0x29);
        std::string name;
        if (ctss && code.index())
            if (auto data = code.index()->read(Tgi{0x43545353, group, ctss, 0}))
                name = StringTable::parse(*data).get(0, lang::EnglishUS);
        // A Sim's character file has its names in its own CTSS: the first
        // name ends its OBJD's file name ("N001_User00000 - Mortimer").
        if (name.empty() && t.objectType == 2)
            if (const size_t at = d->fileName.find(" - "); at != std::string::npos)
                name = d->fileName.substr(at + 3);
        t.catalogName = name.empty() ? "*" + d->fileName : name;
    }
    // OBJD field 0xD0 (past the fields of the base game's OBJDs): the GUID
    // itself in 24,000 of the 25,600 records of the lots, a Sim's template
    // (its OBJD's original GUID, 0x7FD96B54) for the Sims; the rest are
    // records of older definitions.
    t.guid2 = t.objectType == 2 && d->originalGuid() ? d->originalGuid() : guid;
    return t;
}

} // namespace os2::vm
