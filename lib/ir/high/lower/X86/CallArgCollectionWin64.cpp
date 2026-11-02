//===- CallArgCollectionWin64.cpp - Microsoft x64 call arguments ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Microsoft x64 steps of call-argument recovery.  MSVC sets the four
/// register arguments as late as it can, often in a block of their own that
/// an EH state boundary or a `cmp/jnz` split from the call, and passes the
/// caller's own parameters through without touching them.  Each step below
/// recovers one of those shapes; CallArgCollection.cpp runs them through
/// Win64CallArgPolicy.
///
//===----------------------------------------------------------------------===//

#include "../CallArgCollectionDetail.h"

#include "neverd/ir/Limits.h"

#include <algorithm>
#include <functional>

namespace neverd {
namespace call_args_detail {

namespace {

bool preferScannedCallArg(const ExprPtr &Scanned, const ExprPtr &Hinted,
                          size_t Slot, const TypeRef &Declared) {
  if (!Scanned || Scanned->Kind == ExprKind::Undef)
    return false;
  // The declaration fixes the carrier width. The hinted value may itself
  // be stale (a 32-bit flag left in R8 before a pointer was restored there),
  // so its type cannot decide whether the scan repaired that argument.
  if (Scanned->Type && Declared && Scanned->Type->Size != Declared->Size)
    return false;
  if (!Hinted || Hinted->Kind == ExprKind::Undef)
    return true;
  if (Hinted->Kind == ExprKind::Record)
    return false;
  const bool ScannedParam =
      Scanned->Kind == ExprKind::Var && Scanned->Var.Kind == MedVar::Param;
  const bool HintedParam =
      Hinted->Kind == ExprKind::Var && Hinted->Var.Kind == MedVar::Param;
  const bool ScannedSlot =
      ScannedParam && Scanned->Var.Id == static_cast<int>(Slot);
  const bool HintedSlot =
      HintedParam && Hinted->Var.Id == static_cast<int>(Slot);
  if (ScannedSlot)
    return true;
  // `mov rcx, item` leaves a different param in slot 0.  The CALL input is
  // still the incoming sret; the rewrite is the argument.
  if (HintedSlot && ScannedParam && Scanned->Var.Id != static_cast<int>(Slot))
    return true;
  if (HintedSlot)
    return false;
  return true;
}

/// IP-map / EH splits isolate the CALL.  Trailing `mov r9, rdi` /
/// `mov [rsp+20h], esi` then sit in the predecessor after the last helper
/// CALL (GetLength/cstr) and must be collected here; a leftover `Find` nKey
/// in r9 is before those helpers, so it stays out.
void takePredecessorRegisters(CallArgContext &C, const MedBlock &Pred) {
  std::vector<ExprPtr> &Found = C.Found;
  for (int J = static_cast<int>(Pred.Ops.size()) - 1; J >= 0; --J) {
    const MedOp &Prev = Pred.Ops[static_cast<size_t>(J)];
    if (endsCallSetup(Prev))
      break;
    if (isNoopRegisterCopy(Prev))
      continue;
    if (Prev.Output.Kind != MedVar::Reg || Prev.Output.Size == 0)
      continue;
    const int ArgIdx = C.IntegerSlot(Prev.Output.RegOff);
    if (ArgIdx < 0 || ArgIdx >= C.MaxArgs || Found[ArgIdx])
      continue;
    if (Prev.Opcode == NdOp::COPY && Prev.NumInputs >= 1)
      Found[ArgIdx] = C.FromWindowValue(Prev.Inputs[0], Pred.Ops, J - 1);
    else
      Found[ArgIdx] = C.OpToExpr(Prev);
    if (!Found[ArgIdx] || Found[ArgIdx]->Kind == ExprKind::Undef)
      Found[ArgIdx] = C.FromWindowValue(Prev.Output, Pred.Ops, J - 1);
  }
}

/// Call-only block: recover consecutive live-ins that a predecessor wrote as
/// call setup (`lea r8, name` before a callee).  Do not use the caller's
/// parameter count when there is such setup: that drops a 3rd callee arg
/// when the caller is a 2-param sret method.  A tail call with no setup at
/// all still passes live-in arguments up to the caller's own parameter
/// count (`jmp __report_gsfailure`).
void recoverCallOnlySetup(CallArgContext &C, int &FillLast) {
  const MedFunc *CurMed = C.CurMed;
  const llvm::ArrayRef<uint64_t> ParamRegs = C.ParamRegs;
  int SetupLast = -1;
  for (int K = 0; K < static_cast<int>(ParamRegs.size()); ++K) {
    if (C.TakePrecedingSetup(K))
      SetupLast = K;
  }
  if (SetupLast >= 0)
    FillLast = SetupLast;
  else if (CurMed && !CurMed->Params.empty())
    FillLast = std::min(static_cast<int>(CurMed->Params.size()),
                        static_cast<int>(ParamRegs.size())) -
               1;
}

/// Same-block writes of rcx/rdx/r8 must not hide a join PHI in r9
/// (`CStringTable::Find` nKey).  Unused function-entry r9 is not a PHI.
void extendWrittenArgs(CallArgContext &C, int MaxRegArg, int &FillLast) {
  std::vector<ExprPtr> &Found = C.Found;
  const llvm::ArrayRef<uint64_t> ParamRegs = C.ParamRegs;
  int Next = MaxRegArg + 1;
  while (Next < static_cast<int>(ParamRegs.size()) && Next < C.MaxArgs) {
    MedVar LiveIn;
    if (!C.ReachingAtEntry(ParamRegs[Next], LiveIn))
      break;
    bool IsJoinPhi = false;
    for (const auto &Phi : C.CurBlock.Phis) {
      if (Phi.Output.Id == LiveIn.Id && Phi.Output.SSAVer == LiveIn.SSAVer) {
        IsJoinPhi = true;
        break;
      }
    }
    if (!IsJoinPhi)
      break;
    Found[Next] = C.ToExpr(LiveIn);
    FillLast = Next;
    ++Next;
  }
  // `lea r8, name` can sit in the predecessor when `cmp/jnz` splits it from
  // `mov edx; call`. Recover that setup; leftover r9 whose pred did not
  // write it stays out.
  int SetupNext = FillLast + 1;
  while (SetupNext < static_cast<int>(ParamRegs.size()) &&
         SetupNext < C.MaxArgs && C.TakePrecedingSetup(SetupNext)) {
    FillLast = SetupNext;
    ++SetupNext;
  }
}

/// Last-COPY-by-address across blocks is CFG-unsound: cookie `ror rcx` would
/// steal rcx from the mismatch path's incoming argument.  Reaching defs and
/// same-block writes already recovered the live value; name the parameter
/// it passes through.
void resolvePassThroughParams(CallArgContext &C) {
  std::vector<ExprPtr> &Found = C.Found;
  const MedFunc *CurMed = C.CurMed;
  const int RegisterPositions = static_cast<int>(C.ParamRegs.size());
  auto UniqueDef = [&](const MedVar &V) -> const MedOp * {
    const MedOp *Def = nullptr;
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Op : Blk.Ops)
        if (Op.Output.Id == V.Id && Op.Output.SSAVer == V.SSAVer) {
          if (Def)
            return nullptr;
          Def = &Op;
        }
    return Def;
  };
  std::function<bool(const MedVar &, int)> IsConstLike =
      [&](const MedVar &V, int Depth) -> bool {
    if (Depth >
        ::neverd::limits::ir::
            kCallArgCollectionWin64ResolvePassThroughParamsIsConstLikeDepthLimit)
      return false;
    if (V.isConst())
      return true;
    const MedOp *Def = UniqueDef(V);
    if (!Def || Def->NumInputs < 1)
      return false;
    if (Def->Opcode == NdOp::COPY)
      return IsConstLike(Def->Inputs[0], Depth + 1);
    return false;
  };
  std::function<bool(const MedVar &, int, int)> IsSlot =
      [&](const MedVar &V, int Slot, int Depth) -> bool {
    if (Depth >
            ::neverd::limits::ir::
                kCallArgCollectionWin64ResolvePassThroughParamsIsSlotDepthLimit ||
        Slot < 0)
      return false;
    if (V.Kind == MedVar::Param)
      return C.AbiParamIndex(V) == Slot;
    if (V.Kind == MedVar::Reg && V.SSAVer == 0)
      return C.RegToArgIdx(V.RegOff) == Slot;
    for (const auto &Blk : CurMed->Blocks)
      for (const auto &Phi : Blk.Phis)
        if (Phi.Output.Id == V.Id && Phi.Output.SSAVer == V.SSAVer) {
          if (Phi.Args.empty())
            return false;
          for (const auto &Arg : Phi.Args)
            if (!IsSlot(Arg.second, Slot, Depth + 1))
              return false;
          return true;
        }
    const MedOp *Def = UniqueDef(V);
    if (!Def)
      return false;
    if (Def->Opcode == NdOp::COPY && Def->NumInputs >= 1)
      return IsSlot(Def->Inputs[0], Slot, Depth + 1);
    if ((Def->Opcode == NdOp::INT_LEFT || Def->Opcode == NdOp::INT_RIGHT ||
         Def->Opcode == NdOp::INT_OR || Def->Opcode == NdOp::INT_AND) &&
        Def->NumInputs >= 1) {
      bool Any = false;
      for (uint8_t I = 0; I < Def->NumInputs; ++I) {
        if (IsConstLike(Def->Inputs[I], 0))
          continue;
        if (!IsSlot(Def->Inputs[I], Slot, Depth + 1))
          return false;
        Any = true;
      }
      return Any;
    }
    return false;
  };
  const int Limit =
      std::min(RegisterPositions, static_cast<int>(CurMed->Params.size()));
  for (int I = 0; I < Limit; ++I) {
    if (!Found[I] || Found[I]->Kind != ExprKind::Var)
      continue;
    int SrcSlot = -1;
    for (int J = 0; J < Limit; ++J) {
      if (!IsSlot(Found[I]->Var, J, 0))
        continue;
      SrcSlot = J;
      break;
    }
    if (SrcSlot < 0)
      continue;
    if (Found[I]->Var.Kind == MedVar::Param &&
        C.AbiParamIndex(Found[I]->Var) == SrcSlot)
      continue;
    MedVar Param = CurMed->Params[static_cast<size_t>(SrcSlot)];
    Param.Kind = MedVar::Param;
    Param.Id = SrcSlot;
    Found[I] = HighExpr::makeVar(Param, TypeRef{});
  }
}

/// MSVC leaves a register argument it passes straight through untouched, so
/// an unwritten position below the last written one, or below a source
/// binding's or the function's own arity, is the function's own parameter.
void fillUnwrittenParams(CallArgContext &C, size_t HintedCount, bool SelfCall,
                         size_t OwnParamCount) {
  std::vector<ExprPtr> &Found = C.Found;
  const MedFunc *CurMed = C.CurMed;
  const size_t RegisterPositions = C.ParamRegs.size();
  size_t FillTo = 0;
  if (HintedCount != 0)
    FillTo = std::min(HintedCount, RegisterPositions);
  else if (SelfCall)
    FillTo = std::min(OwnParamCount, RegisterPositions);
  else {
    for (int K = static_cast<int>(RegisterPositions) - 1; K >= 0; --K)
      if (Found[K] && Found[K]->Kind != ExprKind::Undef) {
        FillTo = static_cast<size_t>(K + 1);
        break;
      }
    // Tail-call with no param-reg writes (`jmp __report_gsfailure`): rcx is
    // still the incoming cookie. Do not extend a 3-arg call with live-in r9.
    if (FillTo == 0 && !CurMed->Params.empty())
      FillTo = 1;
  }
  for (size_t I = 0; I < FillTo && I < CurMed->Params.size(); ++I) {
    if ((Found[I] && Found[I]->Kind != ExprKind::Undef) ||
        CurMed->Params[I].RegOff == kNoParamReg)
      continue;
    MedVar Param = CurMed->Params[I];
    Param.Kind = MedVar::Param;
    Param.Id = static_cast<int>(I);
    Found[I] = HighExpr::makeVar(Param, TypeRef{});
  }
}

/// A scanned parameter copy can still be the argument where the source
/// binding's CALL input was clobbered.  An IAT binding can also be short
/// (`Concatenate` with only dest+a+na): keep recovered r9 / [rsp+20h] that
/// sit past that prefix.
void mergeScannedArgs(CallArgContext &C, std::vector<ExprPtr> &Hinted,
                      const SourceFunctionTypeHint &Signature) {
  const std::vector<ExprPtr> &Found = C.Found;
  for (size_t I = 0; I < Hinted.size(); ++I) {
    const ExprPtr Scanned = I < Found.size() ? Found[I] : ExprPtr{};
    const TypeRef Declared = I < Signature.Parameters.size()
                                 ? Signature.Parameters[I].Type
                                 : TypeRef{};
    if (preferScannedCallArg(Scanned, Hinted[I], I, Declared))
      Hinted[I] = Scanned;
  }
  size_t End = Hinted.size();
  while (End < static_cast<size_t>(C.MaxArgs) && Found[End] &&
         Found[End]->Kind != ExprKind::Undef)
    ++End;
  for (size_t I = Hinted.size(); I < End; ++I)
    Hinted.push_back(Found[I]);
}

} // namespace

extern const CallArgPolicy Win64CallArgPolicy;
const CallArgPolicy Win64CallArgPolicy = {
    .TheArch = Arch::X64,
    .Format = BinaryFormat::COFF,
    .ReadsPredecessorWindow = true,
    .TakePredecessorRegisters = takePredecessorRegisters,
    .RecoverCallOnlySetup = recoverCallOnlySetup,
    .ExtendWrittenArgs = extendWrittenArgs,
    .ResolvePassThroughParams = resolvePassThroughParams,
    .FillUnwrittenParams = fillUnwrittenParams,
    .RecursiveCallsPassOwnSignature = true,
    .MergeScannedArgs = mergeScannedArgs,
};

} // namespace call_args_detail
} // namespace neverd
