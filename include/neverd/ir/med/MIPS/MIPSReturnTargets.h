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
/// This initial proof accepts scalar identity transports and exact leaf-frame
/// spills. Calls and opaque memory effects do not authorize stack restoration.
bool hasMIPSSourceReturnTargets(const LowFunc &Low, const MedFunc &Function,
                                Arch Target, BinaryFormat Format);

} // namespace neverd

#endif // NEVERD_IR_MED_MIPS_MIPSRETURNTARGETS_H
