#pragma once
// The nodes a thread may run in one turn, for every s2pkg command that runs
// the VM: vm::kNodeLimitPerTurn, the binary's limit (cTSTreeSim::DoNodeAction,
// BV 0xe1b18c, throws 0x44D "Too many iterations" past DAT_0277ea94 = 100000
// nodes; 300000 with a debug flag; docs/routing-mac.md §7.1), the same one the
// app (LotVM) and the VM's own default use. S2PKG_BUDGET=n changes it.
#include "vm/vm.hpp"

#include <cstddef>
#include <cstdlib>

inline size_t s2pkgBudget() {
    const char* b = std::getenv("S2PKG_BUDGET");
    return b ? size_t(std::strtoul(b, nullptr, 10)) : os2::vm::kNodeLimitPerTurn;
}
