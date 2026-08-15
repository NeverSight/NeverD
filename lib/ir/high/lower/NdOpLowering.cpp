//===- NdOpLowering.cpp - NdOp lowering to HighIR statements -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// NdOp-specific lowering logic: STORE, CALL, INTRINSIC, COND_BR, BRANCH,
/// and generic-assign lowering.
///
/// See also:
///   CallArgCollection.cpp    — call-argument collection and regToArgIdx
///   NdOpReturnLowering.cpp   — RETURN value recovery
///   NdOpCallIndLowering.cpp  — INDIR_CALL target resolution and lowering
///   NdOpSwitchRecovery.cpp   — INDIR_BR / jump-table switch recovery
///   CFStructurer.cpp         — control-flow structuring and PHI copy insertion
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"

#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// lowerCBranch / lowerBranch / lowerGenericAssign
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerCBranch(HighFunc &Func, const MedOp &CurOp) {
  HighStmt S;
  S.Kind = StmtKind::If;
  S.Addr = CurOp.Addr;
  if (CurOp.NumInputs >= 2) {
    S.Cond = medvarToExpr(CurOp.Inputs[1]);
    HighStmt GotoStmt;
    GotoStmt.Kind = StmtKind::Goto;
    if (CurOp.Inputs[0].isConst())
      GotoStmt.GotoTarget = CurOp.Inputs[0].ConstVal;
    S.Body.push_back(GotoStmt);
  }
  Func.Body.push_back(std::move(S));
}

void MedToHighConverter::lowerBranch(HighFunc &Func, const MedOp &CurOp) {
  HighStmt S;
  S.Kind = StmtKind::Goto;
  S.Addr = CurOp.Addr;
  if (CurOp.NumInputs >= 1 && CurOp.Inputs[0].isConst())
    S.GotoTarget = CurOp.Inputs[0].ConstVal;
  Func.Body.push_back(std::move(S));
}

void MedToHighConverter::lowerGenericAssign(HighFunc &Func, const MedOp &CurOp,
                                            const VarKeySet &PhiArgVars) {
  if (CurOp.Output.Id < 0 || CurOp.Output.Size == 0)
    return;
  auto Key = varKey(CurOp.Output);
  auto UIt = UseCount.find(Key);
  bool MultiUse = UIt != UseCount.end() && UIt->second > 1;
  bool IsCallResult = CallOutputs.count(Key) > 0;
  bool FeedsPhi = PhiArgVars.count(Key) > 0;
  bool HasMemoryEffect = CurOp.MemoryOrdering != NdMemoryOrdering::None;
  if (!MultiUse && !IsCallResult && !FeedsPhi && !HasMemoryEffect)
    return;
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = CurOp.Addr;
  S.Dst = HighExpr::makeVar(CurOp.Output);
  S.Val = medOpToExpr(CurOp);
  Func.Body.push_back(std::move(S));
}

//===----------------------------------------------------------------------===//
// lowerStore
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerStore(HighFunc &Func, const MedOp &CurOp) {
  HighStmt S;
  S.Kind = StmtKind::Store;
  S.Addr = CurOp.Addr;
  S.MemoryOrdering = CurOp.MemoryOrdering;
  if (CurOp.NumInputs >= 2) {
    auto &AddrVar = CurOp.Inputs[0];
    ExprPtr AddrExpr;
    if (AddrVar.Id >= 0) {
      auto AKey = std::make_pair(AddrVar.Id, AddrVar.SSAVer);
      auto DIt = DefExpr.find(AKey);
      if (DIt != DefExpr.end() && DIt->second->Kind != ExprKind::Call)
        AddrExpr = DIt->second;
    }
    S.StoreAddr = AddrExpr ? AddrExpr : medvarToExpr(AddrVar);

    auto ValKey = std::make_pair(CurOp.Inputs[1].Id, CurOp.Inputs[1].SSAVer);
    if (CallOutputs.count(ValKey))
      S.StoreVal = HighExpr::makeVar(CurOp.Inputs[1]);
    else
      S.StoreVal = medvarToExpr(CurOp.Inputs[1]);
  }
  Func.Body.push_back(std::move(S));
}

//===----------------------------------------------------------------------===//
// lowerCall
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerCall(HighFunc &Func, const MedBlock &CurBlock,
                                   const MedOp &CurOp) {
  va_t Target = 0;
  if (CurOp.NumInputs >= 1 && CurOp.Inputs[0].isConst())
    Target = CurOp.Inputs[0].ConstVal;
  std::string Callee;
  if (FuncNames) {
    auto FnameIt = FuncNames->find(Target);
    Callee = FnameIt != FuncNames->end()
                 ? FnameIt->second
                 : (kAutoFuncPrefix + llvm::utohexstr(Target)).str();
  } else {
    Callee = (kAutoFuncPrefix + llvm::utohexstr(Target)).str();
  }

  size_t CallIdx = 0;
  for (size_t K = 0; K < CurBlock.Ops.size(); ++K) {
    if (&CurBlock.Ops[K] == &CurOp) {
      CallIdx = K;
      break;
    }
  }
  auto Args = collectCallArgs(CurBlock, CallIdx);
  auto CallExpr = HighExpr::makeCall(Callee, Target, std::move(Args));

  if (CurOp.Output.Id >= 0 && CurOp.Output.Size > 0) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = CurOp.Addr;
    S.Dst = HighExpr::makeVar(CurOp.Output);
    S.Val = CallExpr;
    Func.Body.push_back(std::move(S));
  } else {
    HighStmt S;
    S.Kind = StmtKind::Call;
    S.Addr = CurOp.Addr;
    S.CallExpr = CallExpr;
    Func.Body.push_back(std::move(S));
  }
}

//===----------------------------------------------------------------------===//
// lowerIntrinsic
//===----------------------------------------------------------------------===//

void MedToHighConverter::lowerIntrinsic(HighFunc &Func,
                                        const MedBlock &CurBlock,
                                        const MedOp &CurOp, size_t OpIdx,
                                        std::set<size_t> &IntrinsicSkip) {
  Intrinsic IID = Intrinsic::None;
  if (CurOp.NumInputs >= 1 && CurOp.Inputs[0].isConst())
    IID = static_cast<Intrinsic>(CurOp.Inputs[0].ConstVal);
  const char *CName = intrinsicCName(IID);
  std::string Name = CName ? CName : intrinsicName(CurOp);
  if (Name.empty())
    Name = "unknown_intrinsic";
  std::vector<ExprPtr> CoArgs;
  for (uint8_t AI = 1; AI < CurOp.NumInputs; ++AI)
    CoArgs.push_back(medvarToExpr(CurOp.Inputs[AI]));
  auto CallExpr = HighExpr::makeCall(Name, 0, std::move(CoArgs));
  CallExpr->IntrinsicId = IID;
  if (CurOp.Output.Size > 0)
    CallExpr->Type = NdType::makeInt(CurOp.Output.Size, false);

  uint8_t NumOut = intrinsicOutputCount(IID);
  if (NumOut > 0) {
    std::vector<MedVar> CoOutputs;
    for (size_t CI = OpIdx + 1;
         CI < CurBlock.Ops.size() && CoOutputs.size() < NumOut; ++CI) {
      auto &NextOp = CurBlock.Ops[CI];
      if (NextOp.Opcode == NdOp::COPY && NextOp.NumInputs >= 1) {
        CoOutputs.push_back(NextOp.Inputs[0]);
        // The output copy is folded into IntrinsicOutputs; skip lowering it
        // as a standalone assignment.
        IntrinsicSkip.insert(CI);
        continue;
      }
      // LowToMed interleaves sub-register normalization (zero/sign extension
      // of the value just copied, sub-piece extraction) between the output
      // copies on targets with sub-register writes (e.g. an x86-64 EAX write
      // zero-extends into RAX).  These feed later uses (stores, returns) and
      // must still be lowered, so tolerate but do not skip them; otherwise a
      // multi-output intrinsic such as RDTSC would drop its EDX half.
      if (NextOp.Opcode == NdOp::INT_ZEXT || NextOp.Opcode == NdOp::INT_SEXT ||
          NextOp.Opcode == NdOp::SUBBYTES)
        continue;
      break;
    }
    CallExpr->IntrinsicOutputs = std::move(CoOutputs);
  }

  if (CurOp.Output.Id >= 0 && CurOp.Output.Size > 0) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = CurOp.Addr;
    S.Dst = HighExpr::makeVar(CurOp.Output);
    S.Val = CallExpr;
    Func.Body.push_back(std::move(S));
  } else {
    HighStmt S;
    S.Kind = StmtKind::Call;
    S.Addr = CurOp.Addr;
    S.CallExpr = CallExpr;
    Func.Body.push_back(std::move(S));
  }
}

} // namespace neverd
