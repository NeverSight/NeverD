//===- MedDynamicStack.h - Dynamic stack allocations ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack allocated at run time: a VLA or alloca lowers to `sp - size` with a
/// runtime size, whose result becomes the stack pointer, and code may compute
/// the allocation's base again as `old_sp - size`.  The LLVM and C routes
/// consume the same classification; each supplies how it looks up a value's
/// definition, its loop PHI, and what a frame-slot load reads back.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDDYNAMICSTACK_H
#define NEVERD_IR_MED_MEDDYNAMICSTACK_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace neverd {

/// What the classification reads of the function a route lowers.
struct MedDynamicStackLookups {
  const MedFunc *Func = nullptr;
  /// The target's stack pointer register offset.
  uint64_t StackPointer = 0;
  std::function<const MedOp *(const MedVar &)> Definition;
  std::function<const PhiNode *(const MedVar &)> Phi;
  /// The values a frame-slot load reads back; false where the route cannot
  /// tell, and null where it never can.
  std::function<bool(const MedOp &Load, std::vector<MedVar> &Sources)>
      ReloadSources;
};

/// Outgoing call storage below a dynamic object's usable base. Win64 keeps
/// its register home area at the new SP and places the object above it; a
/// separately allocated source object must include both areas. This is not
/// an adjustment to the machine's allocation-size value or stack coordinate.
uint64_t dynamicStackCallArea(Arch Architecture, BinaryFormat Format);

/// Whether \p V derives from the entry stack pointer through copies, width
/// changes, masks, additions and subtractions, and the PHIs of a loop that
/// allocates in each iteration.
bool medStackPointerDerived(const MedVar &V, const MedDynamicStackLookups &L,
                            int Depth = 0);

/// Whether \p V is \p Target through copies and width changes.
bool medValueReaches(const MedVar &V, const MedVar &Target,
                     const MedDynamicStackLookups &L, int Depth = 0);

/// The value a runtime size copies or changes the width of: the key under
/// which an allocation and the recomputations of its base meet.
std::pair<int, int>
medSizeRootKey(const MedVar &V, const MedDynamicStackLookups &L, int Depth = 0);

/// Whether \p V is a stack-pointer-derived value spilled to a frame slot and
/// loaded back.
bool medReloadedStackPointer(const MedVar &V, const MedDynamicStackLookups &L,
                             int Depth = 0);

/// The runtime size \p Op allocates when it is `sp - size` whose result
/// becomes the stack pointer, written to it or copied there through width
/// changes; null for anything else (a constant size is the fixed frame).
const MedVar *medDynamicStackAllocationSize(const MedOp &Op,
                                            const MedDynamicStackLookups &L);

/// Offers \p Try each runtime size of an allocation whose base \p Op may
/// compute again, `sp - size` or `sp + (-size)` with the stack pointer on
/// either side over a derived or reloaded stack pointer, until \p Try takes
/// one.  A candidate is examined only once the one before it is refused.
bool medTryDynamicStackAddressSizes(
    const MedOp &Op, const MedDynamicStackLookups &L,
    llvm::function_ref<bool(const MedVar &Size)> Try);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDDYNAMICSTACK_H
