// Object ownership (ownership.hpp).

#include "ownership.hpp"

#include "formats/neighborhood.hpp"

#include <algorithm>
#include <map>

namespace os2::vm {

void updateObjectOwnership(VM& vm, ObjectId simId, ObjectId objectId) {
    const Object* sim = vm.object(simId);
    const Object* object = vm.object(objectId);
    if (!sim || !sim->person || !object || object->person)
        return;
    const int16_t neighbor = sim->person->data[persondata::NeighborId];
    if (neighbor == 0 || sim->person->data[persondata::FamilyNumber] != vm.globals[global::CurrentFamily])
        return;
    const ObjectId leaderId = multiTileGroup(*object);
    const Object* leader = vm.object(leaderId);
    if (!leader)
        leader = object;
    if (!leader->definition || !(leader->definition->field(kObjdOwnershipFlags) & 1))
        return;
    auto& values = vm.relationships[VM::objectRelationship(leader->id, neighbor)];
    if (values.size() < 2)
        values.resize(2, 0);
    if (values[1] < 100)
        ++values[1];
}

int16_t nextObjectOwner(const VM& vm, ObjectId object, int16_t after, int16_t threshold) {
    // The table's keys in the order it walks them (the insertion order is
    // not kept: ascending, inferred; it only matters for keys sharing a
    // bucket).
    const VM::RelationshipKey from{false, object};
    std::vector<uint32_t> keys;
    std::map<uint32_t, const std::vector<int16_t>*> entries;
    for (auto it = vm.relationships.lower_bound(VM::objectRelationship(object, int16_t(-32768)));
         it != vm.relationships.end() && it->first.first == from; ++it) {
        keys.push_back(uint32_t(uint16_t(it->first.second.id)));
        entries[keys.back()] = &it->second;
    }
    keys = hood::relationshipTableOrder(keys);
    size_t i = 0;
    if (after > 0) { // GetNextKey: the key after `after`, none if it is not a key
        const auto at = std::find(keys.begin(), keys.end(), uint32_t(uint16_t(after)));
        if (at == keys.end())
            return -1;
        i = size_t(at - keys.begin()) + 1;
    }
    for (; i < keys.size(); ++i) {
        const auto& values = *entries.at(keys[i]);
        if (values.size() >= 2 && uint32_t(uint16_t(threshold)) <= uint32_t(int32_t(values[1])))
            return int16_t(keys[i]);
    }
    return -1;
}

} // namespace os2::vm
