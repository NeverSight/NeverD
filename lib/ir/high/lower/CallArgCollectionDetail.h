//===- CallArgCollectionDetail.h - Per-arch call-argument ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal declarations shared between CallArgCollection.cpp and the
/// architecture-specific ABI refinements (CallArgCollectionX86.cpp,
/// CallArgCollectionARM.cpp, CallArgCollectionAArch64.cpp).  Not public.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H
#define NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H

#include "neverd/Common.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <vector>

namespace neverd {

struct BinaryImage;
class TargetRegInfo;

namespace call_args_detail {

struct CallArgScan {
  const std::vector<MedOp> *Ops = nullptr;
  size_t CallIdx = 0;
  uint64_t SpRegOff = 0;
  const TargetRegInfo *TRI = nullptr;
  const BinaryImage *Image = nullptr;
  Arch TheArch = Arch::Unknown;
  int MaxArgs = 0;
  int FirstStackSlot = 0;
  int StoreScanWindow = 0;
  llvm::function_ref<ExprPtr(const MedVar &)> ToExpr;
  llvm::function_ref<bool(const MedVar &)> IsCalleeSave;
};

/// True only for a same-SSA no-op (`COPY rcx = rcx`).  `COPY rcx.3 = rcx`
/// restores the entry value into a new SSA version and is a real call-arg
/// write — MSVC `__GSHandlerCheck_EH` does this after copy-prop replaces
/// `mov rcx, rbp` with the saved ExceptionRecord.
inline bool isNoopRegisterCopy(const MedOp &Op) {
  return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
         Op.Output.Kind == MedVar::Reg && Op.Inputs[0].Kind == MedVar::Reg &&
         Op.Output.RegOff == Op.Inputs[0].RegOff &&
         Op.Output.Size == Op.Inputs[0].Size &&
         Op.Output.Id == Op.Inputs[0].Id &&
         Op.Output.SSAVer == Op.Inputs[0].SSAVer;
}

void collectSpilledStackArgs(const CallArgScan &Scan,
                             std::vector<ExprPtr> &Found);
void collectCallArgsX86(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args);
void collectCallArgsARM(const CallArgScan &Scan, std::vector<ExprPtr> &Found,
                        std::vector<ExprPtr> &Args);
void collectCallArgsAArch64(const CallArgScan &Scan,
                            std::vector<ExprPtr> &Found,
                            std::vector<ExprPtr> &Args);

} // namespace call_args_detail
} // namespace neverd

#endif // NEVERD_IR_HIGH_CALLARGCOLLECTIONDETAIL_H
