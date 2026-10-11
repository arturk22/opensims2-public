// Object ownership (ownership.hpp).

#include "ownership.hpp"

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
    auto& values = vm.relationships[{VM::RelationshipKey{false, leader->id}, VM::RelationshipKey{true, neighbor}}];
    if (values.size() < 2)
        values.resize(2, 0);
    if (values[1] < 100)
        ++values[1];
}

int16_t nextObjectOwner(const VM& vm, ObjectId object, int16_t after, int16_t threshold) {
    const VM::RelationshipKey from{false, object};
    const auto& rels = vm.relationships;
    const auto first = std::pair{from, VM::RelationshipKey{true, after > 0 ? after : int16_t(-32768)}};
    for (auto it = after > 0 ? rels.upper_bound(first) : rels.lower_bound(first); it != rels.end() && it->first.first == from;
         ++it) {
        const auto& [key, values] = *it;
        if (!key.second.neighbor)
            continue;
        if (values.size() >= 2 && uint32_t(uint16_t(threshold)) <= uint32_t(int32_t(values[1])))
            return key.second.id;
    }
    return -1;
}

} // namespace os2::vm
