#include "vm.hpp"

#include "hood_bridge.hpp"
#include "interactions.hpp"
#include "errors.hpp"
#include "sound.hpp"

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

// Sleep (0x00): u16 index of the parameter holding the ticks left, counted
// down once per tick (one call to VM::run). Idle for Input (0x11), which
// also looks at the interaction queue, is in queue.cpp.
// The Mac binary (cTSObject::TryIdle): a parameter out of range is error
// 0x08; 0 (or less) is true at once, else it counts down and waits.
Exit countDownParam(Context& c) {
    const auto param = int16_t(le16(&c.instruction.operands[0]));
    if (param < 0 || size_t(param) >= c.frame.params.size())
        throw gameError(err::StackNumberOutOfRange, "Sleep parameter " + std::to_string(param));
    const int16_t ticks = c.get(kOwnerParam, param);
    if (ticks <= 0)
        return Exit::True;
    c.set(kOwnerParam, param, int16_t(ticks - 1));
    return Exit::Yield;
}

// Random Number (0x08): destination (data u16 at 0, owner u16 at 2), limit
// (data u16 at 4, owner u16 at 6), as in The Sims 1 (the owners are u16:
// SwizzleParams in the Mac binary). Stores a value in [0, limit); the Mac
// binary (base and Bon Voyage) stores 0 for a limit of 0 and returns true
// (the "Random number range evaluated to zero." error is not thrown here).
// A negative limit stores 0 too (inferred). Bytes 8+ are not read.
Exit randomNumber(Context& c) {
    const auto& o = c.instruction.operands;
    const int16_t range = c.get(uint8_t(le16(&o[6])), int16_t(le16(&o[4])));
    int16_t value = 0;
    if (range > 0)
        value = int16_t(std::uniform_int_distribution<int>(0, range - 1)(c.vm.random));
    c.set(uint8_t(le16(&o[2])), int16_t(le16(&o[0])), value);
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

// Remove Object Instance (0x12): target u16 at 0 (STR# 0x9C: 0 me, 1 stack
// object, 2 "target - UNUSED", which the Mac binary takes as object ID 0;
// anything else is the game's error 0x09), flags u8 at 2: 0x01 "return
// immediately" (nothing to wait for here); without 0x02 the binary passes
// "cleanup all" to the removal. What that cleans up is not known: here the
// whole multi-tile object goes either way (inferred; leaving some parts of
// it behind would make no sense). A thread whose object is removed is
// killed. The binary also refuses to remove 7 fixed GUIDs (which ones was
// not checked).
Exit removeObjectInstance(Context& c) {
    const uint16_t target = le16(&c.instruction.operands[0]);
    if (target > 2)
        throw gameError(err::IllegalOwner, "Remove Object Instance target " + std::to_string(target));
    if (target == 2)
        return Exit::True; // object 0: nothing to remove (inferred)
    const Object& x = target == 0 ? c.me() : c.stackObject();
    const ObjectId group = multiTileGroup(x);
    std::vector<ObjectId> parts;
    for (const auto& [id, o] : c.vm.objects())
        if (id == x.id || multiTileGroup(o) == group)
            parts.push_back(id);
    for (ObjectId id : parts)
        c.vm.removeObject(id);
    return Exit::True;
}

// Person data of a neighbor: its Sim's while it is on the lot, else the
// neighborhood's copy (as data owner 0x20 sees it).
std::array<int16_t, kPersonDataCount>& neighborData(VM& vm, int16_t id, Neighbor& n) {
    if (Object* sim = vm.neighborSim(id))
        return sim->person->data;
    return n.personData;
}

// Set to Next (0x1F), checked against the Mac binary (TrySetToNext, base
// 1.0 and Bon Voyage; docs/primitives-mac.md §4): GUID u32 at 0, search type
// = byte 4 & 0x7F (STR# 0xA4); byte 4 bit 7 names an explicit target (owner
// u8 at 5, data u8 at 7), else the target is the stack object ID; local u8
// at 6 (type 9). Byte 8 (flags: including disabled objects, a "where" object
// data filter in u16 9 / u16 11) is Bon Voyage only: base trees are node
// version 0, whose flags the game zeroes, so it is not read. The iterating
// searches start after the target's current value (0 = from the start) and
// write what they find into the target; when nothing is found they write 0
// and return false (the binary does so for the list searches; for the
// neighbor and family ones it is inferred). The verify searches (26, 29, 30)
// only test the stack object. Search types not in the binary's list are the
// game's error 0x1E. Implemented:
//   0 object, 1 person, 2 non-person, 4 object of the GUID, 18 object of the
//   GUID in temps 0 (low) / 1 (high), 6/12 category (object data 59) =
//   parameter 0 / temp 0, 16 vehicle (OBJD type; the GUID is not used), 25
//   object in the room in temp 0, 27 "Utility Using Object" (object data 76
//   or 77 non-zero): objects by ID;
//   3 the next part of the target's multi-tile object, round its group;
//   8 object on the target's tile and level, by ID after it (target 0: false);
//   9 object on the tiles +X, -X, +Y, -Y of the object in the local, in that
//   order, in the anchor's room (the binary keeps "the last valid one" of
//   those; here they are iterated, inferred);
//   14/15 the instance of the "job object" whose GUID is person data 89
//   (low) / 90 (high) of the Sim in the stack object / of the neighbor
//   whose ID is in the stack object (missing: error 0x37);
//   5 neighbor ID, 7 neighbor of the GUID's type, 17 neighbor of the family
//   in temp 0, 21/22/23/31 parent/sibling/child/spouse of the neighbor in
//   temp 0 (VM::familyTree; siblings share a parent, inferred), 28/35/36
//   neighbors whose relationship from the neighbor in temp 0 has variable 3
//   non-zero / variable 0 > temp 1 / variable 1 & temp 1 non-zero, 11 lot
//   ID; 24 visible object within temp 0 tiles (view.cpp); 10 career: false.
// Not done: 13 (nearest person), 33 (hauntable), 37 (want category) and the
// rest of the base game's types up to 37.
Exit setToNext(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    const uint8_t type = o[4] & 0x7F;
    const bool explicitTarget = o[4] & 0x80;
    const uint8_t owner = explicitTarget ? o[5] : kOwnerStackObjectId;
    const auto data = explicitTarget ? int16_t(o[7]) : int16_t(0);
    const ObjectId current = c.get(owner, data);
    auto found = [&](int16_t id) {
        c.set(owner, data, id);
        return Exit::True;
    };
    auto none = [&] {
        c.set(owner, data, 0);
        return Exit::False;
    };
    auto result = [&](std::optional<int16_t> id) { return id ? found(*id) : none(); };

    // Neighbor searches go through neighbor IDs instead of objects.
    auto nextNeighbor = [&](auto matches) {
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it)
            if (matches(it->first, it->second))
                return found(it->first);
        return none();
    };
    // Relationship from the neighbor in temp 0 to each candidate.
    auto relationFromTemp0 = [&](int16_t to) -> const std::vector<int16_t>* {
        auto rel = c.vm.relationships.find({VM::RelationshipKey{true, c.thread.temps[0]}, VM::RelationshipKey{true, to}});
        return rel == c.vm.relationships.end() ? nullptr : &rel->second;
    };
    switch (type) {
    case 5: return nextNeighbor([](int16_t, const Neighbor&) { return true; });                       // neighbor id
    case 7: return nextNeighbor([guid](int16_t, const Neighbor& n) { return n.guid == guid; });      // neighbor of type
    case 17: {                                                                                        // neighbor of family in temp 0
        const int16_t family = c.thread.temps[0];
        return nextNeighbor([&](int16_t id, Neighbor& n) { return neighborData(c.vm, id, n)[persondata::FamilyNumber] == family; });
    }
    case 28:   // relationship variable 3 non-zero
    case 35: { // relationship variable 0 (daily) > temp 1
        if (!c.vm.neighbor(c.thread.temps[0]))
            return none();
        const int16_t limit = c.thread.temps[1];
        return nextNeighbor([&](int16_t id, const Neighbor&) {
            const auto* r = relationFromTemp0(id);
            if (!r)
                return false;
            return type == 28 ? r->size() > 3 && (*r)[3] != 0 : !r->empty() && (*r)[0] > limit;
        });
    }
    case 26: return c.vm.object(c.frame.stackObject) ? Exit::True : Exit::False;                     // verify stack object
    case 29: return c.vm.neighbor(c.frame.stackObject) ? Exit::True : Exit::False;                   // verify neighbor id in stack object
    case 30: {                                                                                        // verify sim in stack object
        const Object* x = c.vm.object(c.frame.stackObject);
        return x && x->person ? Exit::True : Exit::False;
    }
    case 10: return Exit::False;                                                                      // career
    case 24: return result(nextVisibleObject(c.vm, c.me(), current, c.thread.temps[0]));             // visible object within temp 0 (view.cpp)
    case 11: // lot ID
    case 36: // neighbor with relationship variable 1 & temp 1
        if (type == 36 && !c.vm.neighbor(c.thread.temps[0]))
            return none();
        return result(nextInNeighborhood(c, type, current));
    case 21:   // parent of temp 0
    case 22:   // sibling of temp 0
    case 23:   // child of temp 0
    case 31: { // spouse of temp 0
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
        return next == ids.end() ? none() : found(*next);
    }
    case 8: { // object on the same tile, after the target
        if (current == 0)
            return none();
        const Object* x = c.vm.object(current);
        if (!x || !x->position.inWorld() || x->data[objdata::ContainerId] != 0)
            return none();
        const auto ids = c.vm.objectsOnTile(VM::tileOf(*x));
        auto it = std::upper_bound(ids.begin(), ids.end(), current);
        return it == ids.end() ? none() : found(*it);
    }
    case 9: { // adjacent to the object in the local: tiles +X, -X, +Y, -Y, in its room
        const int16_t anchorId = c.get(0x19, o[6]);
        const Object* anchor = c.vm.object(anchorId);
        if (!anchor)
            throw gameError(err::NoTreeParameter, "Set to Next 9: object " + std::to_string(anchorId) + " does not exist");
        if (!anchor->position.inWorld())
            return none();
        const TilePos at = VM::tileOf(*anchor);
        const int16_t room = c.vm.world ? c.vm.world->room(at) : 0;
        std::vector<ObjectId> ids;
        for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const TilePos t{at.x + dx, at.y + dy, at.level};
            if (c.vm.world && (!c.vm.world->contains(t) || c.vm.world->room(t) != room))
                continue;
            for (ObjectId id : c.vm.objectsOnTile(t))
                ids.push_back(id);
        }
        auto it = std::find(ids.begin(), ids.end(), current);
        it = it == ids.end() ? ids.begin() : it + 1;
        return it == ids.end() ? none() : found(*it);
    }
    case 34: { // within one tile of the object in the local (the anchor's tile and the eight around it), by ID
        const Object* anchor = c.vm.object(c.get(0x19, o[6]));
        if (!anchor || !anchor->position.inWorld())
            return none();
        const TilePos at = VM::tileOf(*anchor);
        std::vector<ObjectId> ids;
        for (const auto& [id, x] : c.vm.objects()) {
            const TilePos t = VM::tileOf(x);
            if (id != anchor->id && x.position.inWorld() && x.data[objdata::ContainerId] == 0 && t.level == at.level &&
                std::abs(t.x - at.x) <= 1 && std::abs(t.y - at.y) <= 1)
                ids.push_back(id);
        }
        auto it = std::upper_bound(ids.begin(), ids.end(), current);
        return it == ids.end() ? none() : found(*it);
    }
    case 3: { // part of a multi-tile object
        const Object* x = c.vm.object(current);
        if (!x)
            return none();
        const ObjectId group = multiTileGroup(*x);
        const auto& objects = c.vm.objects();
        auto inGroup = [&](const auto& entry) { return entry.first != current && multiTileGroup(entry.second) == group; };
        auto it = std::find_if(objects.upper_bound(current), objects.end(), inGroup);
        if (it == objects.end())
            it = std::find_if(objects.begin(), objects.end(), inGroup);
        return it == objects.end() ? none() : found(it->first);
    }
    case 14:   // job object of the Sim in the stack object
    case 15: { // job object of the neighbor in the stack object
        const std::array<int16_t, kPersonDataCount>* pd = nullptr;
        if (type == 14) {
            const Object& sim = c.stackObject();
            if (!sim.person)
                throw gameError(err::NotAPerson, "object " + std::to_string(sim.id));
            pd = &sim.person->data;
        } else {
            Neighbor* n = c.vm.neighbor(c.frame.stackObject);
            if (!n)
                throw gameError(err::MissingNeighbor, "neighbor " + std::to_string(c.frame.stackObject));
            pd = &neighborData(c.vm, c.frame.stackObject, *n);
        }
        constexpr int16_t kJobGuidLow = 89, kJobGuidHigh = 90;
        const uint32_t g = uint16_t((*pd)[kJobGuidLow]) | uint32_t(uint16_t((*pd)[kJobGuidHigh])) << 16;
        if (!g || !c.vm.code().definitionByGuid(g))
            return none();
        for (const auto& [id, x] : c.vm.objects())
            if (x.guid() == g)
                return found(id);
        return none();
    }
    }

    std::function<bool(const Object&)> matches;
    switch (type) {
    case 0: matches = [](const Object&) { return true; }; break;                   // object
    case 1: matches = [](const Object& x) { return x.person.has_value(); }; break;  // person
    case 2: matches = [](const Object& x) { return !x.person; }; break;             // non-person
    case 4:                                                                         // object of type
    case 18: {                                                                      // object of type GUID in temp 0/1
        const uint32_t g = type == 4 ? guid
                                     : uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
        if (!c.vm.code().definitionByGuid(g))
            return none();
        matches = [g](const Object& x) { return x.guid() == g; };
        break;
    }
    case 6:                                                                         // category == param 0
    case 12: {                                                                      // category == temp 0
        const int16_t category = type == 6 ? c.get(kOwnerParam, 0) : c.thread.temps[0];
        matches = [category](const Object& x) { return x.data[objdata::Category] == category; };
        break;
    }
    case 16:                                                                        // vehicle
        matches = [](const Object& x) { return x.definition && x.definition->objectType() == kObjectTypeVehicle; };
        break;
    case 25: {                                                                      // object in the room in temp 0
        const int16_t room = c.thread.temps[0];
        matches = [room](const Object& x) { return x.data[objdata::Room] == room; };
        break;
    }
    case 27:                                                                        // utility using object
        matches = [](const Object& x) { return x.data[76] != 0 || x.data[77] != 0; };
        break;
    default:
        if (type <= 37)
            throw VMError("Set to Next: search type " + std::to_string(type) + " not implemented");
        throw gameError(err::BadSearchType, "Set to Next type " + std::to_string(type));
    }
    const auto& objects = c.vm.objects();
    for (auto it = objects.upper_bound(current); it != objects.end(); ++it)
        if (matches(it->second))
            return found(it->first);
    return none();
}

// Test Object Type (0x20), as the Mac binary has it (TryTestObjectType,
// base 1.0 and Bon Voyage): GUID u32 at 0, object (data u16 at 4, owner u8
// at 6), flags u8 at 7. GUID 0x4C7CAB2B stands for the GUID of my temporary
// inventory token ("the GUID of the temporary inventory token"). Flag 0x04
// tests no object: if the GUID has a definition, its high word goes to temp 1
// and the low word to temp 0 and the result is true, else false. Otherwise
// true when the GUID is the object's definition's (0x01: the definition's
// original GUID, OBJD 0xCC) or, failing that, its multi-tile master's; 0x02:
// the ID is a neighbor ID (the neighbor's definition). An object (or
// neighbor) that does not exist is false, not an error.
constexpr uint32_t kTempTokenGuid = 0x4C7CAB2B;

Exit testObjectType(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[7];
    uint32_t guid = le32(&o[0]);
    if (guid == kTempTokenGuid)
        guid = c.me().tempToken.loaded ? c.me().tempToken.token.guid : 0;
    auto& code = c.vm.code();
    if (flags & 0x04) {
        if (!guid || !code.definitionByGuid(guid))
            return Exit::False;
        c.thread.temps[1] = int16_t(guid >> 16);
        c.thread.temps[0] = int16_t(guid & 0xFFFF);
        return Exit::True;
    }
    const int16_t id = c.get(o[6], int16_t(le16(&o[4])));
    const ObjectDefinition* def = nullptr;
    uint32_t group = 0;
    if (flags & 0x02) {
        const Neighbor* n = c.vm.neighbor(id);
        if (!n)
            return Exit::False;
        if (const auto d = code.definitionByGuid(n->guid)) {
            def = code.definition(d->first, d->second);
            group = d->first;
        }
        if (!def)
            return n->guid == guid && !(flags & 0x01) ? Exit::True : Exit::False;
    } else {
        const Object* x = c.vm.object(id);
        if (!x || !x->definition)
            return Exit::False;
        def = x->definition;
        group = x->group;
    }
    auto matches = [&](const ObjectDefinition& d) { return (flags & 0x01 ? d.originalGuid() : d.guid()) == guid; };
    if (matches(*def))
        return Exit::True;
    if (def->masterId() != 0 && int16_t(def->subIndex()) != -1)
        if (const auto m = code.masterDefinition(group, def->masterId()))
            if (const ObjectDefinition* md = code.definition(group, *m); md && matches(*md))
                return Exit::True;
    return Exit::False;
}

// Create New Object Instance (0x2A), as the Mac binary has it
// (TryCreateObject, base 1.0 and Bon Voyage): GUID u32 at 0, where s8 at 4
// (STR# 0xA7), flags u8 at 5, local u8 at 6 (where 8 and 9), slot u8 at 9
// (where 4 and 10), flags u8 at 10; bytes 7-8 are not read here. Where: 0 in
// front of me (my tile + 1 the way I face), 1 "on top of me" and 2 "in my
// hand" (both: my slot 0), 3 in front of the stack object, 4 in slot (byte 9)
// of the stack object, 5 underneath me (my tile), 6 out of world, 7 below
// the object whose ID is parameter 0 (no parameters: error 0x08), 8 below
// the object in the local, 9 next to me in the direction in the local plus
// mine (N/E/S/W only), facing me, 10 in slot (byte 9) of the object in temp
// 0. Flags (byte 5): 0x01 do not duplicate (false when an object of that
// GUID is already on the tile), 0x02 the new object's Main gets my ID in
// parameter 0 and my stack object, 0x04 a Sim of the neighbor in the stack
// object (false without one), 0x08 false when the tile is not empty, 0x10
// temp 0 goes to the Main's parameter 0 (0x02 with 0x10: error 0x36), 0x20
// the new object goes on the stack object's level, 0x40 the GUID is that of
// my temporary inventory token, 0x80 the GUID is in temps 0 (low) / 1
// (high). Byte 10: 0x01 "moving in a new Sim" (nothing here), 0x02 copies
// the design materials of the object in temp 5 (missing: error 0x0A). False,
// with no error: no definition for the GUID, a dead definition (OBJD type
// 0x11), nowhere to go, a full slot or one the container does not have;
// a template definition (type 0x10) is error 0x90. Otherwise the stack
// object becomes the new object and the result is true. Not done: the
// binary's TSObjectCanSetPosition test (an object that does not fit on its
// tile is deleted and the result is false). With VM::initCreatedObjects,
// every part runs its Init right away and gets its Main thread (as FreeSO's
// VMEntity.Init); an Init that fails does not fail the creator (it is kept
// in createdInits). Only the lead part of a new multi-tile object is placed.
Exit createObjectInstance(Context& c) {
    const auto& o = c.instruction.operands;
    const int8_t where = int8_t(o[4]);
    const uint8_t flags = o[5], flags2 = o[10];
    if (where < 0 || where > 10)
        throw VMError("Create New Object Instance: placement " + std::to_string(where) + " not implemented");
    if ((flags & 0x12) == 0x12)
        throw gameError(err::MoreThanOnePassingValue);
    uint32_t guid = le32(&o[0]);
    if (flags & 0x80)
        guid = uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
    if (flags & 0x40)
        guid = c.me().tempToken.loaded ? c.me().tempToken.token.guid : 0;
    const Neighbor* neighbor = nullptr;
    if (flags & 0x04) {
        neighbor = c.vm.neighbor(c.frame.stackObject);
        if (!neighbor)
            return Exit::False;
        guid = neighbor->guid;
    }
    const auto defAt = c.vm.code().definitionByGuid(guid);
    const ObjectDefinition* def = defAt ? c.vm.code().definition(defAt->first, defAt->second) : nullptr;
    if (!def)
        return Exit::False;
    if (def->objectType() == 0x10)
        throw gameError(err::TemplateObject, hexString(guid));
    if (def->objectType() == 0x11)
        return Exit::False;
    const Object* materialSource = nullptr;
    if (flags2 & 0x02) {
        materialSource = c.vm.object(c.thread.temps[5]);
        if (!materialSource)
            throw gameError(err::NoTreeParameter, "object " + std::to_string(c.thread.temps[5]) + " in temp 5");
    }

    // Where it goes: a tile and direction, or a containment slot.
    Object& me = c.me();
    std::optional<Position> at;
    int dir = 0;
    const Object* container = nullptr;
    int slot = o[9];
    auto anchor = [&](const Object* x) {
        if (!x)
            throw gameError(err::InvalidObjectId);
        dir = x->data[objdata::Direction];
        if (x->position.inWorld())
            at = Position{std::floor(x->position.x) + 0.5f, std::floor(x->position.y) + 0.5f, x->position.level};
    };
    auto step = [&](int d) {
        if (!at)
            return;
        const auto [dx, dy] = directionStep(((d % 8) + 8) % 8);
        at->x += float(dx), at->y += float(dy);
    };
    switch (where) {
    case 0: anchor(&me); step(dir); break;
    case 1:
    case 2: container = &me, slot = 0; break;
    case 5: anchor(&me); break;
    case 3: anchor(&c.stackObject()); step(dir); break;
    case 4: container = &c.stackObject(); break;
    case 6: break;
    case 7:
        if (c.frame.params.empty())
            throw gameError(err::StackNumberOutOfRange, "no parameter 0");
        anchor(c.vm.object(c.frame.params[0]));
        break;
    case 8:
        if (o[6] >= c.frame.locals.size())
            throw gameError(err::StackNumberOutOfRange, "local " + std::to_string(o[6]));
        anchor(c.vm.object(c.get(0x19, o[6])));
        break;
    case 9: {
        anchor(&me);
        const int d = (c.get(0x19, o[6]) + (me.data[objdata::Direction] / 2) * 2) % 8;
        step(d);
        dir = d + 4; // facing me
        break;
    }
    case 10:
        container = c.vm.object(c.thread.temps[0]);
        if (!container)
            throw gameError(err::InvalidObjectId);
        break;
    }
    if (at && (flags & 0x20))
        at->level = c.stackObject().position.level;
    if (container) {
        size_t slots = 0;
        if (const Slots* t = container->definition ? c.vm.code().slots(container->group, container->definition->slotId())
                                                   : nullptr)
            for (const auto& sl : t->slots)
                slots += sl.type == 0;
        if (size_t(slot) >= slots && !(container->person && slot < 2))
            return Exit::False;
        if (c.vm.containedIn(container->id, int16_t(slot)))
            return Exit::False;
    }
    if (at && (flags & 0x09)) {
        const TilePos t = VM::tileOf(Object{.position = *at});
        for (ObjectId id : c.vm.objectsOnTile(t)) {
            if (id == me.id)
                continue; // "non-empty" means another object (inferred)
            const Object* x = c.vm.object(id);
            if ((flags & 0x08) || (x && x->guid() == guid))
                return Exit::False;
        }
    }

    std::vector<ObjectId> before;
    for (const auto& [id, _] : c.vm.objects())
        before.push_back(id);
    Object* x = &c.vm.instantiate(defAt->first, defAt->second);
    const ObjectId created = x->id;
    if (where != 6 && !container && !at) {
        // Nowhere in the world to put it.
        const ObjectId group = multiTileGroup(*x);
        std::vector<ObjectId> parts;
        for (const auto& [id, y] : c.vm.objects())
            if (multiTileGroup(y) == group)
                parts.push_back(id);
        for (ObjectId id : parts)
            c.vm.removeObject(id);
        return Exit::False;
    }
    if (container) {
        x->data[objdata::ContainerId] = container->id;
        x->data[objdata::SlotNumber] = int16_t(slot);
        x->position = container->position;
        x->data[objdata::Room] = container->data[objdata::Room];
    } else if (at) {
        x->position = *at;
        x->data[objdata::Direction] = int16_t(((dir % 8) + 8) % 8);
        if (c.vm.world)
            x->data[objdata::Room] = c.vm.world->room(VM::tileOf(*x));
    }
    if (materialSource)
        x->materials = materialSource->materials;
    if (neighbor && x->person) {
        x->person->data = neighbor->personData;
        x->person->data[persondata::NeighborId] = c.frame.stackObject;
    }
    const ObjectId stackObject = c.frame.stackObject, meId = me.id;
    const int16_t temp0 = c.thread.temps[0];
    if (c.vm.initCreatedObjects) {
        c.vm.initialize(created, before);
        if (flags & 0x12)
            if (auto it = c.vm.threads.find(created); it != c.vm.threads.end() && !it->second.stack.empty()) {
                Frame& f = it->second.stack.front();
                if (flags & 0x02)
                    f.stackObject = stackObject;
                if (!f.params.empty())
                    f.params[0] = flags & 0x10 ? temp0 : meId;
            }
    }
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

// Relationship (0x1A), as the Mac binary has it (TryNewRelationship, base
// 1.0 and Bon Voyage; node version 1): variable u8 at 0 (STR# 0x102), flags
// u8 at 1, from (owner u8 at 2, data u16), to (owner u8 at 5, data u16),
// value (owner u8 at 8, data u16). Flags: 0x01 "Fail if too small", 0x02
// "Use Neighbor IDs", 0x04 "Get value from" the value (sets the variable;
// without it the variable is "put into" the value), 0x08 (no editor text)
// an object's own relationship table: "from" is an object (error 0x17 if it
// does not exist) and "to" is used as is, a neighbor ID (the ownership
// trees; the lots keep these in their SRELs, vm::loadLot). With 0x02 both are
// neighbor IDs (a missing one: error 0x1C). With neither, both are objects
// (error 0x17 if one is missing): two Sims are keyed by their neighbor IDs
// (the same relationship 0x02 reaches), otherwise the table is the "from"
// object's with the "to" object's ID as the key. A relationship with no more
// entries than the variable number fails (false) with 0x01 and otherwise
// grows to variable + 1 entries.
//
// Node version 0 (7 instructions in the game; NewRelationshipVersion0Param,
// converted on load by the game): variable u8 at 0, mode u8 at 1 (0 me to
// the stack object, 1 the stack object to me, 2 the stack object to the
// object in the local, 3 that object to the stack object), flags u8 at 2,
// local u8 at 3, value (owner u8 at 4, data u16 at 6). With flag 0x02 the
// two are named by their person data 31 (neighbor ID) instead of object data
// 11 (object ID); the local's value is taken as is.
Exit relationship(Context& c) {
    const auto& o = c.instruction.operands;
    uint8_t var = o[0], flags = o[1];
    int16_t from = 0, to = 0;
    uint8_t valueOwner = o[8];
    int16_t valueData = int16_t(le16(&o[9]));
    if (c.instruction.nodeVersion == 0) {
        flags = o[2];
        valueOwner = o[4], valueData = int16_t(le16(&o[6]));
        auto idOf = [&](ObjectId id) -> int16_t {
            if (!(flags & 0x02))
                return id;
            const Object* x = c.vm.object(id);
            if (!x || !x->person)
                throw gameError(err::NotAPerson, "object " + std::to_string(id));
            return x->person->data[persondata::NeighborId];
        };
        const int16_t me = idOf(c.thread.me), stack = idOf(c.frame.stackObject);
        const int16_t local = c.get(0x19, o[3]);
        switch (o[1]) {
        case 0: from = me, to = stack; break;
        case 1: from = stack, to = me; break;
        case 2: from = stack, to = local; break;
        case 3: from = local, to = stack; break;
        default: throw VMError("Relationship: version 0 mode " + std::to_string(o[1]) + " not implemented");
        }
        flags &= 0x07;
    }
    if (flags & ~0x0F)
        throw VMError("Relationship: flags " + hexString(flags) + " not implemented");
    if (var >= kMaxRelationshipVars)
        throw VMError("Relationship: variable " + std::to_string(var) + " out of range");
    if (c.instruction.nodeVersion != 0) {
        from = c.get(o[2], int16_t(le16(&o[3])));
        to = c.get(o[5], int16_t(le16(&o[6])));
    }
    std::pair<VM::RelationshipKey, VM::RelationshipKey> pair;
    if (flags & 0x02) {
        for (int16_t id : {from, to})
            if (!c.vm.neighbor(id))
                throw gameError(err::NotAPerson, "neighbor " + std::to_string(id));
        pair = {{true, from}, {true, to}};
    } else if (flags & 0x08) {
        if (!c.vm.object(from))
            throw gameError(err::NoObject, "object " + std::to_string(from));
        pair = {{false, from}, {true, to}};
    } else {
        const Object* a = c.vm.object(from);
        const Object* b = c.vm.object(to);
        if (!a || !b)
            throw gameError(err::NoObject, "object " + std::to_string(a ? to : from));
        if (a->person && b->person)
            pair = {c.vm.relationshipKey(from), c.vm.relationshipKey(to)};
        else
            pair = {{false, from}, {false, to}};
    }
    auto it = c.vm.relationships.find(pair);
    if (it == c.vm.relationships.end()) {
        if (flags & 0x01)
            return Exit::False;
        it = c.vm.relationships.emplace(pair, std::vector<int16_t>{}).first;
    }
    auto& vars = it->second;
    if (var >= vars.size()) {
        if (flags & 0x01)
            return Exit::False;
        vars.resize(var + 1);
    }
    if (flags & 0x04)
        vars[var] = c.get(valueOwner, valueData);
    else
        c.set(valueOwner, valueData, vars[var]);
    return Exit::True;
}

// Array Operation (0x76): byte 0 always 0, operation u8, whose u8 (0 me,
// else the stack object), array u16 at 3, then two operands (owner u8, data u16 at 5 and 8):
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
    auto& values = (o[2] == 0 ? c.me() : c.stackObject()).array(le16(&o[3])).values; // array u16 at 3; byte 2: 0 mine, else the stack object's (Mac binary)
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

// Change Material (0x6D), by the Mac binary's disassembler and
// SwizzleParams (docs/primitives-mac.md §5): material u16 at 0 (an index into
// STR# 0x88 "Material Names"), flags u8 at 2, mesh group u16 at 3 (into STR#
// 0x87 "Mesh Groups"; 0xFFFF / negative = the whole model), the object it
// affects (owner u8 at 5, data u16), an object (owner u8 at 8, data u16 at
// 9) whose tables are used with flags 0x08 (material names) and 0x01 (mesh
// group names) instead of the running code's, flags u8 at 13. Flags: 0x10
// the material index is in temp 0, 0x20 the mesh group index in temp 1;
// 0x02/0x04 and 0x40/0x80 pick other name tables (which ones is not known:
// STR# 0x87/0x88 are used), byte 13 0x01 a "Moving Texture" index and 0x02
// the snapshot material (not done). The change is recorded on the target
// for the renderer.
Exit changeMaterial(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[2];
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("code owner does not exist");
    const Object* other = nullptr;
    if (flags & 0x09) {
        const int16_t id = c.get(o[8], int16_t(le16(&o[9])));
        other = c.vm.object(id);
        if (!other)
            throw gameError(err::NoObject, "Change Material: object " + std::to_string(id));
    }
    auto name = [&](const Object& from, uint16_t table, int index) {
        const StringTable* t = c.vm.code().strings(from.group, table);
        if (!t || index < 0 || size_t(index) >= t->count(lang::EnglishUS))
            throw VMError("Change Material: no entry " + std::to_string(index) + " in STR# " + hexString(table) +
                          " of " + hexString(from.group));
        return t->get(size_t(index), lang::EnglishUS);
    };
    const int materialIndex = flags & 0x10 ? c.thread.temps[0] : int(le16(&o[0]));
    const int group = flags & 0x20 ? c.thread.temps[1] : int(int16_t(le16(&o[3])));
    const std::string material = name(flags & 0x08 ? *other : *owner, kMaterialStrings, materialIndex);
    const std::string meshGroup = group < 0 ? "" : name(flags & 0x01 ? *other : *owner, kMeshGroupStrings, group);
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
// (false: there are no imported lots), 49 can change footprint (true: the
// footprints here are whole tiles, inferred). 7 is deprecated ("tree error" in the
// labels). Those that need the neighborhood are in hood_bridge.cpp; the rest
// need the camera or the world.
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
    case 13: // abort the interactions of the stack object: waiting ones are removed, running ones cancelled
        cancelInteractionsWith(vm, c.frame.stackObject);
        return Exit::True;
    case 48: return Exit::False;
    case 49: // can change footprint (to the footprint index in temp 0) without intersecting another object's?
        return Exit::True; // footprints are whole tiles here, which a footprint change does not alter (inferred)
    default:
        if (auto r = neighborhoodSimsCall(c, call))
            return *r;
        throw VMError("Generic Sims Call: " + std::to_string(call) + " not implemented");
    }
}

} // namespace

void registerPrimitives(VM& vm) {
    vm.setPrimitive(0x00, countDownParam);
    vm.setPrimitive(0x01, genericSimsCall);
    vm.setPrimitive(0x02, expression);
    vm.setPrimitive(0x08, randomNumber);
    vm.setPrimitive(0x0F, breakPoint);
    vm.setPrimitive(0x12, removeObjectInstance);
    vm.setPrimitive(0x1A, relationship);
    vm.setPrimitive(0x1C, runTreeByName);
    vm.setPrimitive(0x1F, setToNext);
    vm.setPrimitive(0x20, testObjectType);
    vm.setPrimitive(0x2A, createObjectInstance);
    vm.setPrimitive(0x33, manageInventory);
    vm.setPrimitive(0x6D, changeMaterial);
    vm.setPrimitive(0x76, arrayOperation);
    registerRoutingPrimitives(vm);
    registerNeighborhoodPrimitives(vm);
    registerAnimationPrimitives(vm);
    registerMeasurePrimitives(vm);
    registerTimerPrimitives(vm);
    registerViewPrimitives(vm);
    registerReachPrimitives(vm); // Run Functional Tree, Reach/Put (reach.cpp)
    registerInteractionPrimitives(vm); // Push Interaction, Idle for Input (queue.cpp)
    registerSoundPrimitives(vm);       // Play / Stop Sound Event, Stop ALL Sounds (sound.cpp)
}

} // namespace os2::vm
