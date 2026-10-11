#include "vm.hpp"

#include "formats/binreader.hpp"

#include <algorithm>
#include <cstdlib>

namespace os2::vm {

namespace {

// Expression (0x02): operands lhs data u16, rhs data u16, flags u8,
// operator u8, lhs owner u8, rhs owner u8. Operators 0-17 are The Sims 1's;
// 18-20 are inferred from how the game's trees use them (|= to accumulate
// flags, ^= 1 to toggle a hand, := abs() on differences).
Exit expression(Context& c) {
    const auto& o = c.instruction.operands;
    const auto lhsData = int16_t(le16(&o[0]));
    const auto rhsData = int16_t(le16(&o[2]));
    const uint8_t op = o[5], lhsOwner = o[6], rhsOwner = o[7];

    const int32_t rhs = c.get(rhsOwner, rhsData);
    auto lhs = [&] { return int32_t(c.get(lhsOwner, lhsData)); };
    auto assign = [&](int32_t v) {
        c.set(lhsOwner, lhsData, int16_t(v));
        return Exit::True;
    };
    auto test = [](bool b) { return b ? Exit::True : Exit::False; };
    auto bit = [&] {
        if (rhs < 1 || rhs > 16)
            throw VMError("Expression: flag " + std::to_string(rhs) + " out of range");
        return int32_t(1) << (rhs - 1); // flags are numbered from 1
    };

    switch (op) {
    case 0: return test(lhs() > rhs);
    case 1: return test(lhs() < rhs);
    case 2: return test(lhs() == rhs);
    case 3: return assign(lhs() + rhs);
    case 4: return assign(lhs() - rhs);
    case 5: return assign(rhs);
    case 6: return assign(lhs() * rhs);
    case 7:
        if (rhs == 0)
            throw VMError("Expression: division by zero");
        return assign(lhs() / rhs);
    case 8: return test(lhs() & bit());
    case 9: return assign(lhs() | bit());
    case 10: return assign(lhs() & ~bit());
    case 11: {
        const int16_t v = int16_t(lhs() + 1);
        assign(v);
        return test(v < rhs);
    }
    case 12:
        if (rhs == 0)
            throw VMError("Expression: modulo by zero");
        return assign(lhs() % rhs);
    case 13: return assign(lhs() & rhs);
    case 14: return test(lhs() >= rhs);
    case 15: return test(lhs() <= rhs);
    case 16: return test(lhs() != rhs);
    case 17: {
        const int16_t v = int16_t(lhs() - 1);
        assign(v);
        return test(v > rhs);
    }
    case 18: return assign(lhs() | rhs);
    case 19: return assign(lhs() ^ rhs);
    case 20: return assign(std::abs(rhs));
    default: throw VMError("Expression: operator " + std::to_string(op) + " not implemented");
    }
}

// Operand layouts below follow The Sims 1's primitives where the game's own
// trees agree with them; enumerations are named after the editor's labels
// (STR# 0x7FE59FD0/0xNN, noted where used).

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerStackObjectId = 0x0A;

// Sleep (0x00) and Idle for Input (0x11): u16 index of the parameter holding
// the ticks left, counted down once per tick (one call to VM::run). Idle for
// Input can also be cut short by a pushed interaction (u16 at 4 seems to allow
// that); there are no interaction queues yet, so it only times out.
Exit countDownParam(Context& c) {
    const auto param = int16_t(le16(&c.instruction.operands[0]));
    const int16_t ticks = c.get(kOwnerParam, param);
    if (ticks <= 0)
        return Exit::True;
    c.set(kOwnerParam, param, int16_t(ticks - 1));
    return Exit::Yield;
}

// Random Number (0x08): destination (data u16, owner u16), range (data u16,
// owner u16); stores a value in [0, range). A range of 0 is an error (the
// game's message, Sims2.exe); a negative one stores 0 (inferred).
// Bytes 8+ are 1 in most uses and look like editor leftovers elsewhere.
Exit randomNumber(Context& c) {
    const auto& o = c.instruction.operands;
    const int16_t range = c.get(o[6], int16_t(le16(&o[4])));
    if (range == 0)
        throw VMError("Random number range evaluated to zero.");
    int16_t value = 0;
    if (range > 0)
        value = int16_t(std::uniform_int_distribution<int>(0, range - 1)(c.vm.random));
    c.set(o[2], int16_t(le16(&o[0])), value);
    return Exit::True;
}

// Break Point (0x0F): stops in the editor's debugger when (data u16, owner
// u16) is non-zero (all 0xFF in about half the uses); the game goes on through
// the true branch. Some trees point that branch back at the Break Point (472
// of 2,637) to halt the thread for good, which the game only ends by
// resetting the object; here that fails the thread.
Exit breakPoint(Context& c) {
    const auto& t = c.instruction.onTrue;
    if (t.kind == Behavior::TargetKind::Instruction && t.index == c.frame.pc)
        throw VMError("Break Point: halts here (its true branch loops to itself)");
    return Exit::True;
}

// Remove Object Instance (0x12): u16 target, 0 = me, 1 = stack object (STR#
// 0x9C), then flags u8 at 2: 0x02 removes every part of a multi-tile object
// ("clean up all"), 0x01 "return immediately" (nothing to wait for here). A
// thread whose object is removed is killed.
Exit removeObjectInstance(Context& c) {
    const uint16_t target = le16(&c.instruction.operands[0]);
    if (target > 1)
        throw VMError("Remove Object Instance: target " + std::to_string(target) + " not implemented");
    const Object& x = target == 0 ? c.me() : c.stackObject();
    if (c.instruction.operands[2] & 0x02) {
        const ObjectId group = multiTileGroup(x);
        std::vector<ObjectId> parts;
        for (const auto& [id, o] : c.vm.objects())
            if (multiTileGroup(o) == group)
                parts.push_back(id);
        for (ObjectId id : parts)
            c.vm.removeObject(id);
    } else {
        c.vm.removeObject(x.id);
    }
    return Exit::True;
}

// Person data of a neighbor: its Sim's while it is on the lot, else the
// neighborhood's copy (as data owner 0x20 sees it).
std::array<int16_t, kPersonDataCount>& neighborData(VM& vm, int16_t id, Neighbor& n) {
    if (Object* sim = vm.neighborSim(id))
        return sim->person->data;
    return n.personData;
}

// Set to Next (0x1F): GUID u32, flags u8 (low 7 bits: search type, STR# 0xA4;
// 0x80: the target is (owner u8 at 5, data u16 at 7) instead of the stack
// object ID), local u8 at 6 (for the searches relative to an object in a
// local). Iterating searches move the target to the next matching object (or
// neighbor) by ID and return false when there is none; the verify ones only
// test it. The categories (6, 12) are object data "category", with the value
// in parameter 0 ("sp 0", the trees set it right before) or temp 0; the GUID
// of 18 is temp 0 (low word) and temp 1. Vehicles (16) are objects of type
// vehicle; most uses also carry a GUID that no OBJD has, so it is ignored.
// Parts of a multi-tile object (3) cycle through the target's group (the
// trees go around until they are back at the object they started from); a
// single-tile object has none. The family searches (21 parent, 22 sibling,
// 23 child, 31 spouse of the neighbor ID in temp 0) go through VM::familyTree
// in neighbor ID order; siblings share at least one parent (inferred).
// Family members (17) are the neighbors whose person data "family number"
// is temp 0; 28 the neighbors whose relationship variable 3 to the neighbor
// in temp 0 is not zero (that direction is inferred).
Exit setToNext(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    const uint8_t type = o[4] & 0x7F;
    const bool explicitTarget = o[4] & 0x80;
    const uint8_t owner = explicitTarget ? o[5] : kOwnerStackObjectId;
    const auto data = explicitTarget ? int16_t(le16(&o[7])) : int16_t(0);
    const ObjectId current = c.get(owner, data);
    auto found = [&](int16_t id) {
        c.set(owner, data, id);
        return Exit::True;
    };

    // Neighbor searches go through neighbor IDs instead of objects.
    auto nextNeighbor = [&](auto matches) {
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it)
            if (matches(it->second))
                return found(it->first);
        return Exit::False;
    };
    switch (type) {
    case 5: return nextNeighbor([](const Neighbor&) { return true; });                  // neighbor id
    case 7: return nextNeighbor([guid](const Neighbor& n) { return n.guid == guid; }); // neighbor of type
    case 17: {                                                                         // neighbor of family in temp 0
        const int16_t family = c.thread.temps[0];
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it)
            if (neighborData(c.vm, it->first, it->second)[persondata::FamilyNumber] == family)
                return found(it->first);
        return Exit::False;
    }
    case 28: {                    // neighbor with a non-zero relationship variable 3 to the neighbor in temp 0
        const VM::RelationshipKey to{true, c.thread.temps[0]};
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it) {
            auto rel = c.vm.relationships.find({VM::RelationshipKey{true, it->first}, to});
            if (rel != c.vm.relationships.end() && rel->second.size() > 3 && rel->second[3] != 0)
                return found(it->first);
        }
        return Exit::False;
    }
    case 29: return c.vm.neighbor(current) ? Exit::True : Exit::False;                 // verify neighbor id
    case 21:                                                                           // parent of temp 0
    case 22:                                                                           // sibling of temp 0
    case 23:                                                                           // child of temp 0
    case 31: {                                                                         // spouse of temp 0
        const int16_t of = c.thread.temps[0];
        std::set<int16_t> ids;
        if (auto it = c.vm.familyTree.find(of); it != c.vm.familyTree.end()) {
            const auto& ties = it->second;
            if (type == 22) {
                for (int16_t parent : ties.parents)
                    if (auto p = c.vm.familyTree.find(parent); p != c.vm.familyTree.end())
                        ids.insert(p->second.children.begin(), p->second.children.end());
                ids.erase(of);
            } else {
                ids = type == 21 ? ties.parents : type == 23 ? ties.children : ties.spouses;
            }
        }
        auto next = ids.upper_bound(current);
        return next == ids.end() ? Exit::False : found(*next);
    }
    case 8: {                                                                          // object on the same tile
        // Objects placed on the target's tile, by ID, round and round: the
        // trees ("Tile unobstructed", "Repair - Repair Failed?") go on until
        // they are back at the object they started from. False when the
        // target is not on a tile itself (as in FreeSO).
        const Object* x = c.vm.object(current);
        if (!x || !x->position.inWorld() || x->data[objdata::ContainerId] != 0)
            return Exit::False;
        const auto ids = c.vm.objectsOnTile(VM::tileOf(*x));
        auto it = std::upper_bound(ids.begin(), ids.end(), current);
        return found(it == ids.end() ? ids.front() : *it);
    }
    case 9:                                                                            // adjacent to object in local
    case 34: {                                                                         // within one tile of object in local
        // 9: the objects on the four tiles next to the anchor, a direction
        // at a time (0, 2, 4, 6), by ID within a tile; 34: the objects on
        // the anchor's tile and the eight around it, by ID. The anchor's ID
        // is in the local of byte 6. Objects in slots are not on tiles.
        const Object* anchor = c.vm.object(c.get(0x19, o[6]));
        if (!anchor || !anchor->position.inWorld())
            return Exit::False;
        const TilePos at = VM::tileOf(*anchor);
        std::vector<ObjectId> ids;
        if (type == 9) {
            for (int dir : {0, 2, 4, 6}) {
                const auto [dx, dy] = directionStep(dir);
                for (ObjectId id : c.vm.objectsOnTile({at.x + dx, at.y + dy, at.level}))
                    ids.push_back(id);
            }
            auto it = std::find(ids.begin(), ids.end(), current);
            it = it == ids.end() ? ids.begin() : it + 1;
            return it == ids.end() ? Exit::False : found(*it);
        }
        for (const auto& [id, x] : c.vm.objects()) {
            const TilePos t = VM::tileOf(x);
            if (id != anchor->id && x.position.inWorld() && x.data[objdata::ContainerId] == 0 && t.level == at.level &&
                std::abs(t.x - at.x) <= 1 && std::abs(t.y - at.y) <= 1)
                ids.push_back(id);
        }
        auto it = std::upper_bound(ids.begin(), ids.end(), current);
        return it == ids.end() ? Exit::False : found(*it);
    }
    case 3: {                                                                          // part of a multi-tile object
        const Object* x = c.vm.object(current);
        if (!x)
            return Exit::False;
        const ObjectId group = multiTileGroup(*x);
        const auto& objects = c.vm.objects();
        auto inGroup = [&](const auto& entry) { return entry.first != current && multiTileGroup(entry.second) == group; };
        auto it = std::find_if(objects.upper_bound(current), objects.end(), inGroup);
        if (it == objects.end())
            it = std::find_if(objects.begin(), objects.end(), inGroup);
        return it == objects.end() ? Exit::False : found(it->first);
    }
    }

    std::function<bool(const Object&)> matches;
    switch (type) {
    case 0: matches = [](const Object&) { return true; }; break;               // object
    case 1: matches = [](const Object& x) { return x.person.has_value(); }; break;  // person
    case 2: matches = [](const Object& x) { return !x.person; }; break;            // non-person
    case 4: matches = [guid](const Object& x) { return x.guid() == guid; }; break; // object of type
    case 6:                                                                        // category == param 0
    case 12: {                                                                     // category == temp 0
        const int16_t category = type == 6 ? c.get(kOwnerParam, 0) : c.thread.temps[0];
        matches = [category](const Object& x) { return x.data[objdata::Category] == category; };
        break;
    }
    case 16:                                                                       // vehicle
        matches = [](const Object& x) { return x.definition && x.definition->objectType() == kObjectTypeVehicle; };
        break;
    case 18: {                                                                     // object of type GUID in temp 0/1
        const uint32_t g = uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
        matches = [g](const Object& x) { return x.guid() == g; };
        break;
    }
    case 26: return c.vm.object(current) ? Exit::True : Exit::False;               // verify stack object
    case 30: {                                                                     // verify sim in stack object
        const Object* x = c.vm.object(current);
        return x && x->person ? Exit::True : Exit::False;
    }
    default: throw VMError("Set to Next: search type " + std::to_string(type) + " not implemented");
    }
    const auto& objects = c.vm.objects();
    for (auto it = objects.upper_bound(current); it != objects.end(); ++it)
        if (matches(it->second))
            return found(it->first);
    return Exit::False;
}

// Test Object Type (0x20): GUID u32, object ID (data u16, owner u8); true if
// that object has the GUID, error if it does not exist. Byte 7 (0-4) changes
// the test: 1 is used with one GUID (0x2C9BDEB4) on Sims and 4 with memory and
// want GUIDs; neither is understood yet.
Exit testObjectType(Context& c) {
    const auto& o = c.instruction.operands;
    if (o[7] != 0)
        throw VMError("Test Object Type: mode " + std::to_string(o[7]) + " not implemented");
    const ObjectId id = c.get(o[6], int16_t(le16(&o[4])));
    const Object* x = c.vm.object(id);
    if (!x)
        throw VMError("Test Object Type: object " + std::to_string(id) + " does not exist");
    return x->guid() == le32(&o[0]) ? Exit::True : Exit::False;
}

// Create New Object Instance (0x2A): GUID u32, where u8 (STR# 0xA7), flags u8
// (STR# 0xA8 lists 0-2, but the trees also use bits 0x04/0x10/0x80; ignored).
// Only "out of world" (6, 72% of the uses) is implemented: the new object's ID
// goes to the stack object. With VM::initCreatedObjects, every part runs its
// Init right away and gets its Main thread (as FreeSO's VMEntity.Init); an
// Init that fails does not fail the creator (it is kept in createdInits).
Exit createObjectInstance(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    if (o[4] != 6)
        throw VMError("Create New Object Instance: placement " + std::to_string(o[4]) + " not implemented");
    std::vector<ObjectId> before;
    for (const auto& [id, _] : c.vm.objects())
        before.push_back(id);
    Object* x = c.vm.instantiateGuid(guid);
    if (!x)
        throw VMError("Create New Object Instance: no OBJD with GUID " + hexString(guid));
    const ObjectId created = x->id;
    if (c.vm.initCreatedObjects)
        c.vm.initialize(created, before);
    c.frame.stackObject = created;
    return Exit::True;
}

// Run Tree by Name (0x1C): table u16 (0x012F: the global STR# 0x12F; 0: the
// running code's), u16 at 2 (not understood), name index u8 at 4 (1-based),
// how u8 at 5 (STR# 0xDE: 0 run in my stack, 1 run in stack object's stack,
// 2 push onto my stack), then up to three arguments as (owner u8, data u16)
// at 6, 9 and 12. The tree is looked up by name in the stack object's private,
// semi-global and global trees, in that order, and runs as a subroutine in my
// thread with the stack object's code (CT trees read the object through the
// stack object); the instruction branches on its result, and on false when
// the object has no tree of that name (Standard Exit asks every object for
// "CT - Update Dirty State"). Inferred from the data: when the running code's
// table is missing or shorter than the index, the name comes from the code
// owner's semi-global table and then the global one (the memory objects ask
// for entry 85 of the global table, "CT - Add Memory", and have none); modes
// 0 and 2 are run the same way. Mode 1 needs the stack object's own thread
// and is not implemented. With zeroes at 6-7 and 2 at 8 (77 uses) the current
// tree's parameters are passed on.
Exit runTreeByName(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t mode = o[5];
    if (mode == 1)
        throw VMError("Run Tree by Name: run in stack object's stack not implemented");
    if (mode > 2)
        throw VMError("Run Tree by Name: mode " + std::to_string(mode) + " not implemented");
    // Zeros at 6-7 and 2 at 8: the arguments are the current tree's parameters
    // (inferred from the editor string ", Passing in params from the current
    // tree"; bytes 9+ of that form, e.g. 00 09 FD, are not understood).
    const bool passParams = o[6] == 0 && o[7] == 0 && o[8] != 0;
    if (passParams && o[8] != 2)
        throw VMError("Run Tree by Name: argument form " + std::to_string(o[8]) + " not implemented");

    auto& code = c.vm.code();
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("code owner does not exist");
    const size_t index = size_t(o[4]) - 1;
    std::string name;
    std::vector<uint32_t> tables;
    if (le16(&o[0]) == kNamedTreeStrings)
        tables = {kGlobalGroup};
    else
        tables = {c.frame.group, c.frame.group == owner->group ? owner->semiGroup : 0, kGlobalGroup};
    for (uint32_t group : tables)
        if (const StringTable* t = group ? code.strings(group, kNamedTreeStrings) : nullptr)
            if (index < t->count(lang::EnglishUS)) {
                name = t->get(index, lang::EnglishUS);
                break;
            }
    if (name.empty())
        throw VMError("Run Tree by Name: no name at index " + std::to_string(index + 1));

    const Object& target = c.stackObject();
    std::optional<uint16_t> tree;
    for (uint32_t group : {target.group, target.semiGroup, kGlobalGroup}) {
        if (!group)
            continue;
        // Only trees a call opcode reaches (not the private trees of a library).
        tree = code.behaviorByName(group, name);
        if (tree && c.vm.resolveTree(target, *tree).first == group)
            break;
        tree.reset();
    }
    if (!tree)
        return Exit::False;

    std::array<int16_t, 4> args{};
    if (passParams)
        std::copy_n(c.frame.params.begin(), std::min<size_t>(c.frame.params.size(), 4), args.begin());
    else
        for (size_t k = 0; k < 3; ++k)
            args[k] = c.get(o[6 + 3 * k], int16_t(le16(&o[7 + 3 * k])));
    c.vm.call(c.thread, target.id, *tree, args);
    return Exit::Call;
}

// Relationship (0x1A): variable u8, flags u8, from (owner u8, data u16 at
// 2), to (owner u8 at 5, data u16), value (owner u8 at 8, data u16). Flags,
// from the branches the trees take: 0x01 fails (false) when there is no
// relationship yet, otherwise it is created (the false branch of the others
// is always Error; STR# 0xAB "create new relationship"/"fail"); 0x04 sets
// the variable from the value, otherwise gets it into the value (only these
// write literals and constants; STR# 0xA9 get/set); 0x02 names the two by
// neighbor ID instead of object ID (inferred: "Setup Parents", "Meet All" and
// the trees that iterate neighbors with Set to Next set it). 0x08 (10 uses)
// is not understood, nor bytes 11+ (non-zero in 6% of the uses).
Exit relationship(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t var = o[0], flags = o[1];
    if (flags & ~0x07)
        throw VMError("Relationship: flags " + hexString(flags) + " not implemented");
    if (var >= kMaxRelationshipVars)
        throw VMError("Relationship: variable " + std::to_string(var) + " out of range");
    auto key = [&](uint8_t owner, const uint8_t* data) {
        const int16_t id = c.get(owner, int16_t(le16(data)));
        return flags & 0x02 ? VM::RelationshipKey{true, id} : c.vm.relationshipKey(id);
    };
    const auto pair = std::pair{key(o[2], &o[3]), key(o[5], &o[6])};
    auto it = c.vm.relationships.find(pair);
    if (it == c.vm.relationships.end()) {
        if (flags & 0x01)
            return Exit::False;
        it = c.vm.relationships.emplace(pair, std::vector<int16_t>{}).first;
    }
    auto& vars = it->second;
    if (var >= vars.size())
        vars.resize(var + 1);
    if (flags & 0x04)
        vars[var] = c.get(o[8], int16_t(le16(&o[9])));
    else
        c.set(o[8], int16_t(le16(&o[9])), vars[var]);
    return Exit::True;
}

// Array Operation (0x76): byte 0 always 0, operation u8, whose u8 (0 me, 1
// stack object), array u8, then two operands (owner u8, data u16 at 5 and 8):
// a value or size in the first and an index (iterator) in the second. The
// numbering comes from the editor's disassembly strings in Sims2.exe; which
// operand is which, from how the trees use them:
//   0 clear                         1 size into the first
//   2 resize to the first (new elements 0; "Temp Array - Clear" resizes to 0)
//   3 set every element to the first
//   4 insert the first at the front, 5 at the end, 6 at index (second); the
//     index it went to into the second (parallel arrays in "CT - Create STFX"
//     share that index)
//   7 remove the first element, 8 the last, into the first (error if empty)
//   9 remove the element at index (second) into the first
//  10 find the first from index (second) on and put its index in the second;
//     false if absent (the only operation whose false branch is used;
//     "CT - Add Duration Walk" finds a value, then removes that index from
//     four parallel arrays with 9)
//  11 swap the elements at the indices in the first and the second
//  12 sort, largest first
// 13 is not understood. Arrays are created on demand (the object data
// "number of object arrays" is not checked).
Exit arrayOperation(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t op = o[1];
    if (o[2] > 1)
        throw VMError("Array Operation: owner " + std::to_string(o[2]) + " not implemented");
    auto& values = (o[2] == 0 ? c.me() : c.stackObject()).array(o[3]).values;
    auto get1 = [&] { return c.get(o[5], int16_t(le16(&o[6]))); };
    auto set1 = [&](int16_t v) { c.set(o[5], int16_t(le16(&o[6])), v); };
    auto get2 = [&] { return c.get(o[8], int16_t(le16(&o[9]))); };
    auto set2 = [&](int16_t v) { c.set(o[8], int16_t(le16(&o[9])), v); };
    auto index = [&](int16_t i, size_t end, const char* what) {
        if (i < 0 || size_t(i) >= end)
            throw VMError(std::string("Array Operation: ") + what + " at " + std::to_string(i) + " out of range (" +
                          std::to_string(values.size()) + ")");
        return size_t(i);
    };
    auto insert = [&](size_t at) {
        if (values.size() >= kMaxArrayLength)
            throw VMError("Array Operation: array full");
        values.insert(values.begin() + ptrdiff_t(at), get1());
        set2(int16_t(at));
        return Exit::True;
    };
    auto remove = [&](size_t at) {
        const int16_t v = values[at];
        values.erase(values.begin() + ptrdiff_t(at));
        set1(v);
        return Exit::True;
    };
    switch (op) {
    case 0: values.clear(); return Exit::True;
    case 1: set1(int16_t(values.size())); return Exit::True;
    case 2: {
        const int16_t n = get1();
        if (n < 0 || size_t(n) > kMaxArrayLength)
            throw VMError("Array Operation: size " + std::to_string(n) + " out of range");
        values.resize(size_t(n));
        return Exit::True;
    }
    case 3: std::fill(values.begin(), values.end(), get1()); return Exit::True;
    case 4: return insert(0);
    case 5: return insert(values.size());
    case 6: return insert(index(get2(), values.size() + 1, "insert"));
    case 7:
    case 8:
        if (values.empty())
            throw VMError("Array Operation: remove from an empty array");
        return remove(op == 7 ? 0 : values.size() - 1);
    case 9: return remove(index(get2(), values.size(), "remove"));
    case 10: {
        const int16_t from = std::max<int16_t>(get2(), 0);
        if (size_t(from) >= values.size())
            return Exit::False;
        auto it = std::find(values.begin() + from, values.end(), get1());
        if (it == values.end())
            return Exit::False;
        set2(int16_t(it - values.begin()));
        return Exit::True;
    }
    case 11: std::swap(values[index(get1(), values.size(), "swap")], values[index(get2(), values.size(), "swap")]);
        return Exit::True;
    case 12: std::sort(values.begin(), values.end(), std::greater<>()); return Exit::True;
    default: throw VMError("Array Operation: operation " + std::to_string(op) + " not implemented");
    }
}

// Change Material (0x6D): material u8 (index into the code owner's STR#
// 0x88 "Material Names"), u8 always 0, flags u8 (not understood), mesh group
// u16 at 3 (index into STR# 0x87 "Mesh Groups", 0xFFFF = all of them), target
// object (owner u8 at 5, data u16). The change is recorded on the target for
// the renderer. Bytes 8+ (an owner at 8 that is 0x0A or 0, a u16 at 11 that is
// 0xFFFF or small) are not understood.
Exit changeMaterial(Context& c) {
    const auto& o = c.instruction.operands;
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("code owner does not exist");
    auto name = [&](uint16_t table, size_t index) {
        const StringTable* t = c.vm.code().strings(owner->group, table);
        if (!t || index >= t->count(lang::EnglishUS))
            throw VMError("Change Material: no entry " + std::to_string(index) + " in STR# " + hexString(table) +
                          " of " + hexString(owner->group));
        return t->get(index, lang::EnglishUS);
    };
    const uint16_t group = le16(&o[3]);
    const std::string material = name(kMaterialStrings, o[0]);
    const std::string meshGroup = group == 0xFFFF ? "" : name(kMeshGroupStrings, group);
    const ObjectId target = c.get(o[5], int16_t(le16(&o[6])));
    Object* x = c.vm.object(target);
    if (!x)
        throw VMError("Change Material: object " + std::to_string(target) + " does not exist");
    if (meshGroup.empty())
        x->materials.clear();
    x->materials[meshGroup] = material;
    return Exit::True;
}

// Manage Inventory (0x33): u8 at 0 (bits 0-1 the inventory kind, "G = 0, H,
// F, N" in STR# 0xFC; 0x04 counted instead of singular tokens), inventory ID
// (owner u8 at 1, data u16; not read for the global inventory), operation u8
// at 4, GUID u32 at 5, category u8 at 9 (counted tokens), then two operands
// (owner u8, data u16 at 10 and 13). The operation names come from the
// editor's disassembly strings in Sims2.exe ("Add token", "Find the token",
// "Push property on token at index from", "Set To Next token", "Save My Temp
// Token back to the location it was loaded from", ...); which number is which,
// and the rest, from how the trees use them:
//   0 add a token of the GUID (0: the stack object's, as in "CT - Add Memory"
//     and "CT - Add to Inventory"); counted: add the second operand to its count
//   1 remove the token at index (first operand)
//   2 remove every token of the GUID
//   3 find the first token of the GUID, index into the first operand; false if none
//   4 push the second operand onto the properties of the token at index (first)
//   5 pop the last property of the token at index (first) into the second; false if none
//   6 read the token at index (first) into my temporary token; with a GUID, the
//     next token of that GUID from there on (the index is written back); false
//     past the end (loops stop on it)
//   7 property (first) of my temporary token into the second; false if out of range
//   8 set property (first) of my temporary token to the second; false if out of range
//   9 save my temporary token back where it was read; false if it is gone
//  10 token count into the second operand
//  11 add a copy of my temporary token ("Copy Marker (no dupe)", "Transport
//     Tokens - Copy Global to Local"; "Move Invitees from Local to Global" walks
//     the source by index while doing it, so the source keeps its token)
// Token indices start at 1: the loops over an inventory run from 1 to the count,
// or from the count down to 1. Not understood: bits 0x08 and 0x10 of byte 0
// (0x08 with counted adds, 0x18 on memory and aspiration finds), and counted
// tokens beyond add and count.
Exit manageInventory(Context& c) {
    const auto& o = c.instruction.operands;
    const auto kind = InventoryKind(o[0] & 0x03);
    const bool counted = o[0] & 0x04;
    const int16_t id = kind == InventoryKind::Global ? 0 : c.get(o[1], int16_t(le16(&o[2])));
    const uint8_t op = o[4];
    const uint32_t guid = le32(&o[5]);
    auto get1 = [&] { return c.get(o[10], int16_t(le16(&o[11]))); };
    auto set1 = [&](int16_t v) { c.set(o[10], int16_t(le16(&o[11])), v); };
    auto get2 = [&] { return c.get(o[13], int16_t(le16(&o[14]))); };
    auto set2 = [&](int16_t v) { c.set(o[13], int16_t(le16(&o[14])), v); };
    Inventory& inventory = c.vm.inventory(kind, id);
    auto& tokens = counted ? inventory.counted : inventory.singular;
    auto guidOrStackObject = [&] { return guid ? guid : c.stackObject().guid(); };
    auto checkRoom = [&] {
        if (tokens.size() >= kMaxInventoryTokens)
            throw VMError("Manage Inventory: inventory full");
    };
    auto at = [&](int16_t index) -> InventoryToken& {
        if (index < 1 || size_t(index) > tokens.size())
            throw VMError("Manage Inventory: token index " + std::to_string(index) + " out of range (" +
                          std::to_string(tokens.size()) + ")");
        return tokens[size_t(index) - 1];
    };
    TempToken& temp = c.me().tempToken;

    if (counted) {
        switch (op) {
        case 0: {
            const uint32_t g = guidOrStackObject();
            const int16_t n = get2();
            auto it = std::find_if(tokens.begin(), tokens.end(), [&](const auto& t) { return t.guid == g; });
            if (it == tokens.end()) {
                checkRoom();
                tokens.push_back({.guid = g, .category = o[9]});
                it = tokens.end() - 1;
            }
            it->count = int16_t(it->count + n);
            return Exit::True;
        }
        case 10: set2(int16_t(tokens.size())); return Exit::True;
        default:
            throw VMError("Manage Inventory: counted operation " + std::to_string(op) + " not implemented");
        }
    }

    switch (op) {
    case 0:
        checkRoom();
        tokens.push_back({.guid = guidOrStackObject()});
        return Exit::True;
    case 1: {
        const int16_t index = get1();
        at(index);
        tokens.erase(tokens.begin() + (index - 1));
        return Exit::True;
    }
    case 2:
        std::erase_if(tokens, [&](const auto& t) { return t.guid == guid; });
        return Exit::True;
    case 3: {
        auto it = std::find_if(tokens.begin(), tokens.end(), [&](const auto& t) { return t.guid == guid; });
        if (it == tokens.end())
            return Exit::False;
        set1(int16_t(it - tokens.begin() + 1));
        return Exit::True;
    }
    case 4: {
        auto& props = at(get1()).properties;
        if (props.size() >= kMaxTokenProperties)
            throw VMError("Manage Inventory: too many token properties");
        props.push_back(get2());
        return Exit::True;
    }
    case 5: {
        auto& props = at(get1()).properties;
        if (props.empty())
            return Exit::False;
        const int16_t v = props.back();
        props.pop_back();
        set2(v);
        return Exit::True;
    }
    case 6: {
        const int16_t start = get1();
        int16_t index = start;
        if (guid) {
            while (index >= 1 && size_t(index) <= tokens.size() && tokens[size_t(index) - 1].guid != guid)
                ++index;
        }
        if (index < 1 || size_t(index) > tokens.size())
            return Exit::False;
        if (index != start)
            set1(index);
        temp = {.token = tokens[size_t(index) - 1], .loaded = true, .kind = kind, .inventoryId = id, .index = index};
        return Exit::True;
    }
    case 7:
    case 8: {
        const int16_t p = get1();
        if (!temp.loaded || p < 0 || size_t(p) >= temp.token.properties.size())
            return Exit::False;
        if (op == 7)
            set2(temp.token.properties[size_t(p)]);
        else
            temp.token.properties[size_t(p)] = get2();
        return Exit::True;
    }
    case 9: {
        if (!temp.loaded)
            return Exit::False;
        auto& source = c.vm.inventory(temp.kind, temp.inventoryId);
        auto& list = temp.counted ? source.counted : source.singular;
        if (temp.index < 1 || size_t(temp.index) > list.size())
            return Exit::False;
        list[size_t(temp.index) - 1] = temp.token;
        return Exit::True;
    }
    case 10: set2(int16_t(tokens.size())); return Exit::True;
    case 11:
        if (!temp.loaded)
            return Exit::False;
        checkRoom();
        tokens.push_back(temp.token);
        return Exit::True;
    default: throw VMError("Manage Inventory: operation " + std::to_string(op) + " not implemented");
    }
}

// Push Interaction (0x0D): interaction u16 at 0, priority u8 at 2 (STR#
// 0xE0: inherited, maximum, autonomous, user driven), flags u8 at 3, u8 at 4
// (0-8), then the object whose interaction it is (owner u8 at 5, data u16)
// and two more variables (owner u8, data u16 at 8 and 11). It goes onto the
// stack object's queue ("... onto the stack object's queue" in the editor's
// disassembly strings, Sims2.exe). The same strings name what the rest may
// hold: an icon ("use icon from", "Icon Index is %d", "Getting Icon Index from
// Temp6", "use Icon from selector GUID in temp4/5"), "Returing ID in" and
// "Linking to interaction with ID in" variables, "Passing first 4 params in",
// "Force run check tree", "continue as current" and "use name"; which byte
// and flag is which is not known, so nothing is written back and the operand
// is kept whole in the record. Inferred: the index is not checked against the
// object's interactions (Sims push socials with indices up to 0x1E6), and an
// object that does not exist fails the push (false). There is no queue
// processing yet.
Exit pushInteraction(Context& c) {
    const auto& o = c.instruction.operands;
    const ObjectId source = c.get(o[5], int16_t(le16(&o[6])));
    if (!c.vm.object(source))
        return Exit::False;
    Object& target = c.stackObject();
    if (target.queue.size() >= kMaxQueuedInteractions)
        throw VMError("Push Interaction: queue of object " + std::to_string(target.id) + " is full");
    if (o[2] > 3)
        throw VMError("Push Interaction: priority " + std::to_string(o[2]) + " not implemented");
    target.queue.push_back({source, le16(&o[0]), o[2], o[3], c.thread.me, o});
    return Exit::True;
}

// Generic Sims Call (0x01): call index u8 at 0 (the other bytes are 0); the
// arguments and results are in the temps, as the editor's labels describe each
// call (STR# 0xDC, 54 calls). Calls that only ask the engine for something
// outside the simulation (camera, UI, preloading, lot loading and transitions,
// tutorial state, the Sim's 3D look, the wants recalculation) are recorded in
// VM::engineRequests and return true. Implemented in the VM: 4 add to family
// (neighbor in the stack object; family in temp 0, 0 = the current family,
// negative = a new one; false without the neighbor or when the family has 8
// Sims), 6 remove from family, 10/11 build and buy off/on, 13 abort the
// interactions whose object is the stack object from every queue, 14 house radio station, 18
// simulator speed (0 pause, 1 unpause, 2-4 into the Speed global as is), 25
// selected Sim, 21-23 the family tree (child of, spouse of, remove the ties
// between two Sims; VM::familyTree), 40 game state transitions off/on, 48 remapped neighbor ID
// (false: there are no imported lots). 7 is deprecated ("tree error" in the
// labels). The rest need the neighborhood, outfits or the world.
Exit genericSimsCall(Context& c) {
    const uint8_t call = c.instruction.operands[0];
    auto& temps = c.thread.temps;
    auto& vm = c.vm;
    auto request = [&] {
        vm.request({call, c.thread.me, c.frame.stackObject, temps});
        return Exit::True;
    };
    switch (call) {
    case 0:  // exit lot
    case 1:  // center view on stack object
    case 2:  // set action icon to stack object
    case 3:  // uncenter view
    case 8:  // family tutorial complete
    case 9:  // architecture tutorial complete
    case 15: // my routing footprint := temp 0
    case 16: // change normal outfit
    case 17: // swap lot (temp 0)
    case 19: // swap lot to the lot of the family of neighbor in temp 0
    case 20: // swap lot via family in temp 0
    case 24: // preload new Sim age
    case 26: // start lot transition
    case 29: // preload visitor
    case 30: // preload clothing
    case 31: // preload object
    case 33: // update footprint
    case 37: // lot transition done
    case 38: // unlink character
    case 39: // force recalc of wants
    case 41: // facial overlay zits toggle
    case 42: // Sim fitness update
    case 44: // hide/unhide puck
    case 45: // copy last name
    case 46: // set Sim hair override
    case 47: // clear Sim hair override
    case 51: // show info on lot loading screen
    case 53: // set facial overlay state
        return request();
    case 4: { // add to family
        Neighbor* n = vm.neighbor(c.frame.stackObject);
        if (!n)
            return Exit::False;
        int16_t family = temps[0] ? temps[0] : vm.globals[global::CurrentFamily];
        if (family <= 0) {
            family = std::max<int16_t>(vm.globals[global::CurrentFamily], 0);
            for (auto& [id, other] : vm.neighbors)
                family = std::max(family, neighborData(vm, id, other)[persondata::FamilyNumber]);
            ++family;
        }
        size_t members = 0;
        for (auto& [id, other] : vm.neighbors)
            members += id != c.frame.stackObject && neighborData(vm, id, other)[persondata::FamilyNumber] == family;
        if (members >= kMaxFamilySize)
            return Exit::False;
        neighborData(vm, c.frame.stackObject, *n)[persondata::FamilyNumber] = family;
        return Exit::True;
    }
    case 6: { // remove from family
        Neighbor* n = vm.neighbor(c.frame.stackObject);
        if (!n)
            return Exit::False;
        neighborData(vm, c.frame.stackObject, *n)[persondata::FamilyNumber] = 0;
        return Exit::True;
    }
    case 7: throw VMError("Generic Sims Call: 7 (make new neighbor) is deprecated");
    case 10:
    case 11: vm.buildBuyEnabled = call == 11; return Exit::True;
    case 14: vm.globals[global::HouseRadioStation] = temps[0]; return Exit::True;
    case 18:
        if (temps[0] < 0 || temps[0] > 4)
            throw VMError("Generic Sims Call: simulator speed " + std::to_string(temps[0]) + " out of range");
        if (temps[0] <= 1)
            vm.globals[global::Paused] = temps[0] == 0;
        else
            vm.globals[global::Speed] = temps[0], vm.globals[global::Paused] = 0;
        return Exit::True;
    case 21: // add child (temp 0) of parent (temp 1) to the family tree
        vm.familyTree[temps[0]].parents.insert(temps[1]);
        vm.familyTree[temps[1]].children.insert(temps[0]);
        return Exit::True;
    case 22: // add spouse (temp 0) of the initial spouse (temp 1)
        vm.familyTree[temps[0]].spouses.insert(temps[1]);
        vm.familyTree[temps[1]].spouses.insert(temps[0]);
        return Exit::True;
    case 23: // remove the ties between temp 0 and its relative in temp 1
        for (auto [a, b] : {std::pair{temps[0], temps[1]}, std::pair{temps[1], temps[0]}}) {
            auto& t = vm.familyTree[a];
            t.parents.erase(b), t.children.erase(b), t.spouses.erase(b);
        }
        return Exit::True;
    case 25: vm.globals[global::SelectedPersonId] = temps[0]; return Exit::True;
    case 40: vm.gameStateTransitionsEnabled = temps[0] != 0; return Exit::True;
    case 13: // abort the interactions of the stack object (only queued ones exist)
        for (const auto& [id, _] : vm.objects())
            std::erase_if(vm.object(id)->queue,
                          [&](const QueuedInteraction& q) { return q.object == c.frame.stackObject; });
        return Exit::True;
    case 48: return Exit::False;
    default: throw VMError("Generic Sims Call: " + std::to_string(call) + " not implemented");
    }
}

} // namespace

void registerPrimitives(VM& vm) {
    vm.setPrimitive(0x00, countDownParam);
    vm.setPrimitive(0x01, genericSimsCall);
    vm.setPrimitive(0x02, expression);
    vm.setPrimitive(0x08, randomNumber);
    vm.setPrimitive(0x0D, pushInteraction);
    vm.setPrimitive(0x0F, breakPoint);
    vm.setPrimitive(0x11, countDownParam);
    vm.setPrimitive(0x12, removeObjectInstance);
    vm.setPrimitive(0x1A, relationship);
    vm.setPrimitive(0x1C, runTreeByName);
    vm.setPrimitive(0x1F, setToNext);
    vm.setPrimitive(0x20, testObjectType);
    vm.setPrimitive(0x2A, createObjectInstance);
    vm.setPrimitive(0x33, manageInventory);
    vm.setPrimitive(0x6D, changeMaterial);
    vm.setPrimitive(0x76, arrayOperation);
}

} // namespace os2::vm
