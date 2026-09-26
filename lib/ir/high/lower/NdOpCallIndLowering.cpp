//===- NdOpCallIndLowering.cpp - Indirect call lowering -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect call target resolution and INDIR_CALL lowering to HighIR.
/// Extracts the indirect call target from four sources (in priority order):
///   1. Direct constant in the INDIR_CALL input nd-var
///   2. LOAD source tracing (including RIP-relative patterns)
///   3. Register-to-argument-index mapping
///   4. STORE→LOAD stack-slot offset matching
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Resolution strategies (static helpers)
//===----------------------------------------------------------------------===//

/// Strategy 1: the lifter already resolved the IAT slot address, so the
/// INDIR_CALL input is a constant that maps directly to a known function name.
static bool tryResolveNamedAddress(va_t Addr,
                                   const std::map<va_t, std::string> *FuncNames,
                                   const BinaryImage *Image,
                                   MedToHighConverter::CallIndTarget &Out) {
  if (FuncNames) {
    auto It = FuncNames->find(Addr);
    if (It != FuncNames->end()) {
      Out.Name = It->second;
      Out.Addr = Addr;
      Out.IsIndirect = false;
      return true;
    }
  }
  if (Image) {
    if (const Import *Imp = Image->findImportAt(Addr);
        Imp && !Imp->Name.empty()) {
      Out.Name = Imp->Name;
      Out.Addr = Imp->IATAddr ? Imp->IATAddr : Addr;
      Out.IsIndirect = false;
      return true;
    }
    std::string FnName = Image->getFunctionNameAt(Addr);
    if (!FnName.empty() && FnName.find(kAutoFuncPrefix) != 0) {
      Out.Name = FnName;
      Out.Addr = Addr;
      Out.IsIndirect = false;
      return true;
    }
  }
  return false;
}

static bool tryResolveConstTarget(const MedOp &CurOp,
                                  const std::map<va_t, std::string> *FuncNames,
                                  const BinaryImage *Image,
                                  MedToHighConverter::CallIndTarget &Out) {
  if (CurOp.NumInputs < 1 || !CurOp.Inputs[0].isConst())
    return false;
  return tryResolveNamedAddress(CurOp.Inputs[0].ConstVal, FuncNames, Image,
                                Out);
}

/// Strategy 2: trace through LOAD (and optional INT_ADD for RIP-relative
/// addressing) to find a known import address.
static bool tryResolveLoadTarget(const MedOp &CurOp, const MedBlock &CurBlock,
                                 const std::map<va_t, std::string> *FuncNames,
                                 const BinaryImage *Image,
                                 MedToHighConverter::CallIndTarget &Out) {
  if (CurOp.NumInputs < 1)
    return false;

  auto &TVar = CurOp.Inputs[0];
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode != NdOp::LOAD || BOp.Output.Id != TVar.Id ||
        BOp.Output.SSAVer != TVar.SSAVer || BOp.NumInputs < 1 ||
        BOp.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;

    // Direct constant load address.
    auto &Addr = BOp.Inputs[0];
    if (Addr.isConst() &&
        tryResolveNamedAddress(Addr.ConstVal, FuncNames, Image, Out))
      return true;

    // RIP-relative pattern: INT_ADD rip, disp -> LOAD -> INDIR_CALL.
    for (auto &AOp : CurBlock.Ops) {
      if (AOp.Opcode != NdOp::INT_ADD || AOp.Output.Id != Addr.Id ||
          AOp.Output.SSAVer != Addr.SSAVer || AOp.NumInputs < 2)
        continue;
      for (uint8_t KI = 0; KI < AOp.NumInputs; ++KI) {
        if (!AOp.Inputs[KI].isConst())
          continue;
        if (tryResolveNamedAddress(AOp.Inputs[KI].ConstVal, FuncNames, Image,
                                   Out))
          return true;
      }
    }
    break;
  }
  return false;
}

/// Strategy 3: if the target expression is a register that maps to a known
/// calling-convention argument index, use that.
static bool tryResolveRegTarget(const ExprPtr &TargetExpr, Arch TargetArch,
                                MedToHighConverter::CallIndTarget &Out) {
  if (TargetExpr->Kind != ExprKind::Var || TargetExpr->Var.Kind != MedVar::Reg)
    return false;
  int PIdx = getTargetRegInfo(TargetArch).regToArgIdx(TargetExpr->Var.RegOff);
  if (PIdx < 0)
    return false;
  Out.IndirectParam = PIdx;
  Out.Name = "arg" + std::to_string(PIdx);
  return true;
}

/// Strategy 4: match STORE→LOAD via stack-slot offset to recover the
/// indirect parameter index.
static bool tryResolveStackSlotTarget(const MedOp &CurOp,
                                      const MedBlock &CurBlock, Arch TargetArch,
                                      MedToHighConverter::CallIndTarget &Out) {
  if (CurOp.NumInputs < 1)
    return false;

  auto &TVar = CurOp.Inputs[0];
  int64_t LoadSpOff = INT64_MIN;
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode == NdOp::LOAD && BOp.Output.Id == TVar.Id &&
        BOp.Output.SSAVer == TVar.SSAVer && BOp.NumInputs >= 1 &&
        BOp.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      auto &LA = BOp.Inputs[0];
      for (auto &ROp : CurBlock.Ops) {
        if (ROp.Opcode == NdOp::INT_ADD && ROp.Output.Id == LA.Id &&
            ROp.Output.SSAVer == LA.SSAVer && ROp.NumInputs >= 2 &&
            ROp.Inputs[1].isConst()) {
          LoadSpOff = static_cast<int64_t>(ROp.Inputs[1].ConstVal);
          break;
        }
      }
      break;
    }
  }
  if (LoadSpOff == INT64_MIN)
    return false;

  const auto &TRI = getTargetRegInfo(TargetArch);
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode != NdOp::STORE || BOp.NumInputs < 2 ||
        BOp.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      continue;
    if (BOp.Inputs[1].Kind != MedVar::Reg)
      continue;
    int PIdx = TRI.regToArgIdx(BOp.Inputs[1].RegOff);
    if (PIdx < 0)
      continue;
    auto &SA = BOp.Inputs[0];
    for (auto &ROp : CurBlock.Ops) {
      if (ROp.Opcode == NdOp::INT_ADD && ROp.Output.Id == SA.Id &&
          ROp.Output.SSAVer == SA.SSAVer && ROp.NumInputs >= 2 &&
          ROp.Inputs[1].isConst() &&
          static_cast<int64_t>(ROp.Inputs[1].ConstVal) == LoadSpOff) {
        Out.IndirectParam = PIdx;
        Out.Name = "arg" + std::to_string(PIdx);
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// resolveCallIndTarget
//===----------------------------------------------------------------------===//

MedToHighConverter::CallIndTarget MedToHighConverter::resolveCallIndTarget(
    const MedBlock &CurBlock, const MedOp &CurOp, const ExprPtr &TargetExpr) {
  CallIndTarget Result;

  if (tryResolveConstTarget(CurOp, FuncNames, Image, Result))
    return Result;
  if (tryResolveLoadTarget(CurOp, CurBlock, FuncNames, Image, Result))
    return Result;
  if (tryResolveRegTarget(TargetExpr, TargetArch, Result))
    return Result;
  if (tryResolveStackSlotTarget(CurOp, CurBlock, TargetArch, Result))
    return Result;

  return Result;
}

//===----------------------------------------------------------------------===//
// lowerCallInd
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerCallInd(HighFunc &Func, const MedBlock &CurBlock,
                                      const MedOp &CurOp) {
  HighStmt S;
  S.Kind = StmtKind::Call;
  S.Addr = CurOp.Addr;

  ExprPtr TargetExpr = (CurOp.NumInputs >= 1) ? medvarToExpr(CurOp.Inputs[0])
                                              : HighExpr::makeConst(0, 8);

  auto Target = resolveCallIndTarget(CurBlock, CurOp, TargetExpr);

  size_t CI = 0;
  for (size_t K = 0; K < CurBlock.Ops.size(); ++K)
    if (&CurBlock.Ops[K] == &CurOp) {
      CI = K;
      break;
    }
  auto Args = collectCallArgs(CurBlock, CI);

  auto Call = HighExpr::makeCall(
      Target.Name, Target.Addr ? Target.Addr : CurOp.Addr, std::move(Args));
  Call->IsIndirectCall = Target.IsIndirect;
  Call->IndirectParamIdx = Target.IndirectParam;
  if (Target.IsIndirect) {
    ExprPtr Callee = TargetExpr;
    if (Callee && Callee->Kind == ExprKind::Var && Callee->Var.Id >= 0) {
      auto It = DefExpr.find(varKey(Callee->Var));
      if (It != DefExpr.end() && It->second &&
          It->second->Kind == ExprKind::Load)
        Callee = It->second;
    }
    Call->IndirectTarget = forceInlineCallTarget(Callee);
  }
  Call->SourceCallHint = CurOp.SourceCallHint;
  if (CurOp.SourceCallHint)
    Call->CallAddr = CurOp.SourceCallHint->TargetAddress;
  if (CurOp.Output.Id >= 0 && CurOp.Output.Size > 0) {
    Call->Type = sourceCallResultType(CurOp);
    S.Kind = StmtKind::Assign;
    S.Dst = HighExpr::makeVar(CurOp.Output, Call->Type && Call->Type->Kind ==
                                                              NdTypeKind::Struct
                                                ? Call->Type
                                                : nullptr);
    S.Val = Call;
  }
  CallOutputs.insert({CurOp.Output.Id, CurOp.Output.SSAVer});
  if (S.Kind == StmtKind::Call)
    S.CallExpr = Call;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
