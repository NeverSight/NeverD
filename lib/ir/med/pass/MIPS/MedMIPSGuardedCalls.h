//===- MedMIPSGuardedCalls.h - Equality-guarded native arguments -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_MED_PASS_MIPS_MEDMIPSGUARDEDCALLS_H
#define NEVERD_LIB_IR_MED_PASS_MIPS_MEDMIPSGUARDEDCALLS_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace neverd {
class MedDefSiteIndex;
namespace safety::detail {
class ExactPhiGraph;
}
namespace mips {
/// The operand whose complete bits are copied by a register move or alias.
const MedVar *copiedWord(const MedOp &Op);

using NativeCallSites =
    llvm::SmallDenseMap<const MedBlock *, llvm::SmallVector<size_t, 1>, 4>;

struct GuardedCall {
  uint32_t Registers = 0;
  llvm::SmallVector<unsigned, 2> Inputs;
};
using GuardedCalls = llvm::SmallDenseMap<const MedOp *, GuardedCall, 4>;

/// Arguments whose full machine word is equal to an independently non-frame
/// value on every path to the native call.
GuardedCalls
guardedResultArguments(const MedFunc &F, const NativeCallSites &Native,
                       const TargetRegInfo &TRI, BinaryFormat Format,
                       const safety::detail::ExactPhiGraph &Graph,
                       MedDefSiteIndex &Definitions, size_t &Remaining,
                       llvm::function_ref<bool(const MedVar &)> IsNonFrame);
} // namespace mips
} // namespace neverd
#endif
