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

#include "llvm/ADT/StringExtras.h"

namespace neverd {

//===----------------------------------------------------------------------===//
// Resolution strategies (static helpers)
//===----------------------------------------------------------------------===//

/// Strategy 1: the lifter already resolved the IAT slot address, so the
/// INDIR_CALL input is a constant that maps directly to a known function name.
static bool tryResolveConstTarget(const MedOp &CurOp,
                                  const std::map<va_t, std::string> *FuncNames,
                                  MedToHighConverter::CallIndTarget &Out) {
  if (!FuncNames || CurOp.NumInputs < 1 || !CurOp.Inputs[0].isConst())
    return false;
  auto It = FuncNames->find(CurOp.Inputs[0].ConstVal);
  if (It == FuncNames->end())
    return false;
  Out.Name = It->second;
  Out.IsIndirect = false;
  return true;
}

/// Strategy 2: trace through LOAD (and optional INT_ADD for RIP-relative
/// addressing) to find a known import address.
static bool tryResolveLoadTarget(const MedOp &CurOp, const MedBlock &CurBlock,
                                 const std::map<va_t, std::string> *FuncNames,
                                 MedToHighConverter::CallIndTarget &Out) {
  if (!FuncNames || CurOp.NumInputs < 1)
    return false;

  auto &TVar = CurOp.Inputs[0];
  for (auto &BOp : CurBlock.Ops) {
    if (BOp.Opcode != NdOp::LOAD || BOp.Output.Id != TVar.Id ||
        BOp.Output.SSAVer != TVar.SSAVer || BOp.NumInputs < 1)
      continue;

    // Direct constant load address.
    auto &Addr = BOp.Inputs[0];
    if (Addr.isConst()) {
      auto It = FuncNames->find(Addr.ConstVal);
      if (It != FuncNames->end()) {
        Out.Name = It->second;
        Out.IsIndirect = false;
        return true;
      }
    }

    // RIP-relative pattern: INT_ADD rip, disp -> LOAD -> INDIR_CALL.
    for (auto &AOp : CurBlock.Ops) {
      if (AOp.Opcode != NdOp::INT_ADD || AOp.Output.Id != Addr.Id ||
          AOp.Output.SSAVer != Addr.SSAVer || AOp.NumInputs < 2)
        continue;
      for (uint8_t KI = 0; KI < AOp.NumInputs; ++KI) {
        if (!AOp.Inputs[KI].isConst())
          continue;
        auto It = FuncNames->find(AOp.Inputs[KI].ConstVal);
        if (It != FuncNames->end()) {
          Out.Name = It->second;
          Out.IsIndirect = false;
          return true;
        }
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
        BOp.Output.SSAVer == TVar.SSAVer && BOp.NumInputs >= 1) {
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
    if (BOp.Opcode != NdOp::STORE || BOp.NumInputs < 2)
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

  if (tryResolveConstTarget(CurOp, FuncNames, Result))
    return Result;
  if (tryResolveLoadTarget(CurOp, CurBlock, FuncNames, Result))
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

  auto Call = HighExpr::makeCall(Target.Name, CurOp.Addr, std::move(Args));
  Call->IsIndirectCall = Target.IsIndirect;
  Call->IndirectParamIdx = Target.IndirectParam;
  CallOutputs.insert({CurOp.Output.Id, CurOp.Output.SSAVer});
  S.CallExpr = Call;
  Func.Body.push_back(std::move(S));
}

} // namespace neverd
