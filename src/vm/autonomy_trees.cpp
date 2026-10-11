// What a check tree can do to an autonomy search (autonomy.hpp,
// checkTreeAdFlags), as the Mac binary's tree scan (cTSBehaviorTree vtable
// +0x88, FUN_0048cc58) works it out for cTSPerson::FBA_ScoreStaticInteractions.

#include "autonomy.hpp"

#include "formats/bhav.hpp"

#include <map>
#include <tuple>

namespace os2::vm {

namespace {

// Data owners of the check tree ads (Expression lhs): 0x0C range, 0x1C
// personality, 0x1D min. The binary's mask 0x30001000 over the owner byte.
bool isAdOwner(uint8_t owner) {
    return owner < 0x1E && ((1u << owner) & 0x30001000u);
}

// Expression operators that write the lhs: the binary's mask 0x3E3EF8 over
// the operator byte (3-7 :=, +=, -=, *=, /=; 9-13 set flag, clear flag, ++,
// modulo, &=; 17-21 --, |=, ^=, >>=, <<=, as the editor's operator list).
bool isAssignment(uint8_t op) {
    return op < 0x16 && ((1u << op) & 0x3E3EF8u);
}

struct Key {
    const CodeLibrary* code;
    uint32_t group, semiGroup;
    uint16_t tree;
    auto operator<=>(const Key&) const = default;
};
struct Entry {
    std::string fileName; // checked on a hit: a later code library may reuse the address
    size_t size = 0;
    uint8_t flags = 0;
};

uint8_t scan(VM& vm, const Object& owner, uint16_t treeId, std::map<Key, Entry>& cache) {
    const auto [group, tree] = vm.resolveTree(owner, treeId);
    if (!tree)
        return CheckTreeScanned;
    const Key key{&vm.code(), owner.group, owner.semiGroup, treeId};
    if (auto it = cache.find(key); it != cache.end() && it->second.fileName == tree->fileName &&
                                   it->second.size == tree->instructions.size())
        return it->second.flags;
    // Marked as scanned first, as the binary does, so a tree that calls
    // itself (or a caller) stops there.
    Entry& e = cache[key];
    e = {tree->fileName, tree->instructions.size(), CheckTreeScanned};
    uint8_t flags = CheckTreeScanned;
    for (const auto& in : tree->instructions) {
        switch (in.opcode) {
        case 0x02: // Expression writing a check tree ad
            if (isAdOwner(in.operands[6]) && isAssignment(in.operands[5]))
                flags |= CheckTreeChangesAds;
            break;
        case 0x7E: flags |= CheckTreeChangesAds; break;    // Run Script (Bon Voyage)
        case 0x32: flags |= CheckTreeActionStrings; break; // Add/Change the Action String
        case 0x14:                                         // Run Functional Tree
        case 0x1C:                                         // Run Tree by Name: anything
            flags |= CheckTreeChangesAds | CheckTreeActionStrings;
            break;
        default:
            if (in.opcode >= 0x100)
                flags |= scan(vm, owner, in.opcode, cache) & (CheckTreeChangesAds | CheckTreeActionStrings);
            break;
        }
        if ((flags & (CheckTreeChangesAds | CheckTreeActionStrings)) == (CheckTreeChangesAds | CheckTreeActionStrings))
            break;
    }
    e.flags = flags; // std::map nodes stay put while the calls above add entries
    return flags;
}

} // namespace

uint8_t checkTreeAdFlags(VM& vm, const Object& codeOwner, uint16_t treeId) {
    static std::map<Key, Entry> cache;
    return scan(vm, codeOwner, treeId, cache);
}

} // namespace os2::vm
