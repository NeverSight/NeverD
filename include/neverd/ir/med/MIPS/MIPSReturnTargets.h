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
/// Accept scalar identity transports and exact private frame spills. Complete
/// fixed FP calls preserve a spill only when its slot is outside the outgoing
/// arguments and no argument can expose the frame. Opaque effects fail closed.
bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H
