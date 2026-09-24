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

#include <optional>
#include <set>
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
  llvm::function_ref<ExprPtr(const MedVar &)> ToExpr;
  llvm::function_ref<bool(const MedVar &)> IsCalleeSave;
  /// The value register argument \p Index holds at the call, for filling a
  /// register slot the call did not visibly write (nullptr when unknown).
  llvm::function_ref<ExprPtr(int)> ReachingRegArg;
  /// Offset of an address from the stack pointer at function entry, when it
  /// resolves through copies and constant adjustments (an `r11 = rsp`
  /// frame); nullopt otherwise.
  llvm::function_ref<std::optional<int64_t>(const MedVar &)> EntryOffsetOf;
  /// Bytes the prologue moved the stack pointer below its entry value.
  int64_t FrameSize = 0;
  /// Entry-relative stack slots the function loads; see MedToHigh.h.
  const std::set<int64_t> *LoadedEntrySlots = nullptr;
};

/// Win64 passes arguments 0-3 in RCX, RDX, R8, R9 above a 32-byte home area;
/// argument 4 + K is the 8-byte slot at [rsp + 32 + 8K] at the call.
bool isWin64(const CallArgScan &Scan);

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
