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

namespace neverd {
struct LowFunc;
struct MedFunc;

/// Every original return must use the exact incoming RA, including its mode
/// tag. Qualify the captured operand, not the register after the delay slot.
/// Accept scalar identity transports and exact private frame spills. Ordinary
/// native ABI calls preserve allocated private spills outside their outgoing
/// area. Fixed descriptors own that extent; unbound calls check every possible
/// argument definition. Frame escapes (even before the save), partial links,
/// unknown SP and opaque effects fail closed.
bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H
