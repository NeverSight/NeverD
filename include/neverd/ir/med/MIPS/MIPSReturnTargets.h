//===- MIPSReturnTargets.h - Ordinary source return qualification --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove that ordinary source returns retain the captured MIPS entry link.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H
#define NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd {
struct LowFunc;
struct MedFunc;

/// Every original return must use the exact incoming RA, including its mode
/// tag. Qualify the captured operand, not the register after the delay slot.
/// Accept scalar identity transports and exact private frame spills. Ordinary
/// native ABI calls preserve allocated private spills outside their outgoing
/// area. Fixed descriptors own that extent; unbound calls check every possible
/// argument definition. Frame escapes (even before the save), partial links,
/// unknown SP and opaque effects fail closed. A bound source entry carries
/// its result operands before the captured jump target until this check.
bool hasMIPSSourceReturnTargets(
    const LowFunc &Low, const MedFunc &Function, Arch Target,
    BinaryFormat Format, unsigned SourceResultOperands = 0,
    llvm::ArrayRef<uint32_t> StaticAllocatorCalls = {},
    bool *UsedAllocator = nullptr, bool *PrivateStores = nullptr);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H
