//===- X64ContinuationNativeRegisters.h - Native banks --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit native debug and bounded CR4 accesses for continuation source.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONNATIVEREGISTERS_H
#define NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONNATIVEREGISTERS_H

#include "X64ControlTransitions.h"

#include "neverd/analysis/arch/x86_64/X64ContinuationSource.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

#include <cstdint>

namespace neverd::analysis::detail {

inline bool
nativeDebugRegisterOperation(const LowOp &Op,
                             const X64ContinuationSourceOptions &O) {
  if (!O.NativeDebugRegisterAccess || Op.Opcode != NdOp::INTRINSIC ||
      Op.NumInputs < 2 || !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      !Op.Inputs[1].isConst() || Op.Inputs[1].Size != 4 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  return ((Id == Intrinsic::ReadDr && Op.NumInputs == 2 &&
           Op.Output.Size == 8) ||
          (Id == Intrinsic::WriteDr && Op.NumInputs == 3 && !Op.Output.Size &&
           Op.Inputs[2].Size == 8)) &&
         x86SystemRegisterName(Id, Op.Inputs[1].Offset).has_value();
}

inline bool nativePageGlobalOperation(const LowOp &Op,
                                      const X64ContinuationSourceOptions &O) {
  return O.NativePageGlobalAccess &&
         x64::pageglobal::describeOperation(Op).has_value();
}

template <class Writer>
void guardNativePageGlobalWrite(Writer &W, const LowOp &Op) {
  if (x64::pageglobal::describeOperation(Op) !=
      x64::pageglobal::Operation::Write)
    return;
  const NdVar Current = W.temporary();
  W.emit(NdOp::INTRINSIC, Current,
         {NdVar::cst(uint64_t(Intrinsic::ReadCr4), 2)});
  // Reuse the authoritative PGE-only transition guard, then keep the actual
  // MOV CR4 in the caller. A scalar state update cannot replace its TLB effect.
  const auto Transition = x64::pageglobal::lowerScalarOperation(
      Op, Current, [&](uint16_t Bytes) { return W.temporary(Bytes); });
  W.append(Transition->Ops);
  W.rejectIf(*Transition->Rejected);
}

} // namespace neverd::analysis::detail

#endif // NEVERD_LIB_ANALYSIS_ARCH_X86_64_X64CONTINUATIONNATIVEREGISTERS_H
