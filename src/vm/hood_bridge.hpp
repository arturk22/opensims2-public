#pragma once

// The neighborhood in the VM: what hood::Neighborhood decodes (neighbors,
// relationships, family tree, inventories, families, lots) becomes the VM's
// data owners, and the primitives that need it read it from there.
//
// Mapping (see TODO.md, M6 "Ligar o bairro à VM"):
//   hood::Neighbor     -> VM::neighbors[id]: GUID of its SDSC, house = its
//                         family's lot, person data from the SDSC with 31 =
//                         neighbor ID and 61 = family number, outfit keys of
//                         its character file
//   hood::Relationship -> VM::relationships (neighbor ID pair, values cut to i16)
//   hood::FamilyTreeEdge -> VM::familyTree (parent/other parent: parents of
//                         `from`; child: children; spouse: spouses; siblings
//                         follow from the parents)
//   NGBH inventories   -> VM::inventories (global, lot, family, neighbor);
//                         singular tokens keep flags and data as properties,
//                         counted tokens are (count, category) in their data
//   hood::Family       -> VM::families (funds, house, flags, friends)
//   hood::Lot          -> VM::lots (zoning = lot info type, position)
// Entering a lot (enterLot) sets the globals of the lot and of the family
// living there (current house, current family, budget, zoning, neighborhood
// ID); bindLotSims gives the lot's Sims their neighbor's person data.

#include "vm.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace os2::hood {
struct Neighborhood;
}

namespace os2::vm {

struct NeighborhoodLoad {
    size_t neighbors = 0, relationships = 0, familyTies = 0, inventories = 0, families = 0, lots = 0;
};

// Copies the neighborhood into the VM (replacing what it had of it).
NeighborhoodLoad loadNeighborhood(VM& vm, const hood::Neighborhood& hood);

// The neighborhood package of a lot package
// (N001/Lots/N001_Lot11.package -> N001/N001_Neighborhood.package), empty if
// there is none.
std::filesystem::path neighborhoodPackageOf(const std::filesystem::path& lotPackage);
// Lot ID (house number) of a lot package ("N001_Lot11" -> 11), 0 if the name
// does not say.
int16_t lotIdOf(const std::filesystem::path& lotPackage);

// Sets the globals of being on lot `lot`: Current House, Current Family (the
// family living there, 0 if none), Budget Mod/Div 10000 (its funds), Lot
// Zoning and Current Neighborhood ID (`neighborhoodId`).
void enterLot(VM& vm, int16_t lot, int16_t neighborhoodId);

// Gives every Sim in the VM the person data of the neighbor with its GUID
// (the GUID of its character file's OBJD), with person data 31 = neighbor ID.
// Returns how many Sims were matched.
size_t bindLotSims(VM& vm);

// A family's funds; those of the current family are the budget globals.
int32_t familyFunds(VM& vm, int16_t family);
void setFamilyFunds(VM& vm, int16_t family, int32_t funds);

// --- Primitive helpers (called from primitives.cpp / vm.cpp) ---------------

// Generic Sims Calls that need the neighborhood: 5 combine assets, 27 number
// of community lots, 28 money effect (an engine request), 32 outfits
// information, 34 distance between lots, 35 zoning type, 36 family in lot,
// 50 extract money from another lot. Nothing for the other calls.
std::optional<Exit> neighborhoodSimsCall(Context& c, uint8_t call);
// Set to Next searches over the neighborhood: 11 next lot ID, 36 next
// neighbor with relationship variable 1 & temp 1. The next ID, nothing when
// the search is over (false); throws for other types.
std::optional<int16_t> nextInNeighborhood(Context& c, uint8_t type, int16_t current);
// Neighbor data (data owner 0x18) fields that need the neighborhood: 1
// belongs in house, 5 friend count, 7 has telephone, 8 has baby, 9 family
// friend count, 10 has computer. Throws for the others.
int16_t neighborhoodNeighborField(VM& vm, int16_t id, const Neighbor& n, int16_t field);

// Outfit category bit of the character files' outfit keys (`lk<N>` >> 16)
// for an outfit type as the trees number them (STR# 0xFA "Person Outfit
// Categories"), 0 for none.
uint32_t outfitCategoryBit(int16_t outfitType);

} // namespace os2::vm
