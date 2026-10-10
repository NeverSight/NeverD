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
#include "neverd/ir/med/MedIntrinsicOutputs.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

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
    if (ExpressionObserver && S.Cond && CurOp.Addr != InvalidVA &&
        CurOp.OriginSeq >= 0)
      ExpressionObserver(CurOp, S.Cond);
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
  bool HasMemoryEffect =
      CurOp.Opcode == NdOp::LOAD ||
      CurOp.RegistrationRoot ==
          MedOp::RegistrationRootKind::CallbackStackPointer ||
      CurOp.MemoryOrdering != NdMemoryOrdering::None ||
      CurOp.MemoryAddressSpace != NdMemoryAddressSpace::Default;
  if (!MultiUse && !IsCallResult && !FeedsPhi && !HasMemoryEffect)
    return;
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Addr = CurOp.Addr;
  S.Dst = HighExpr::makeVar(CurOp.Output);
  S.Val = medOpToExpr(CurOp);
  S.KeepsName = S.Val && S.Val->Kind == ExprKind::EntryRegister;
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
  S.MemoryAddressSpace = CurOp.MemoryAddressSpace;
  if (CurOp.NumInputs >= 2) {
    S.StoreAddr = memoryAddressExpr(CurOp.Inputs[0]);

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
  std::string Callee = calleeDisplayName(Target);

  size_t CallIdx = 0;
  for (size_t K = 0; K < CurBlock.Ops.size(); ++K) {
    if (&CurBlock.Ops[K] == &CurOp) {
      CallIdx = K;
      break;
    }
  }
  auto Args = collectCallArgs(CurBlock, CallIdx);
  if (Callee == "___error")
    Args.clear();
  // An image linked at a fixed address relocates no immediate, so a string
  // argument arrives as a plain number.  A parameter the callee reads as a C
  // string makes it the address of that string.
  if (Image && Image->LoadsAtLinkAddress && !CurOp.SourceCallHint)
    for (size_t K = 0; K < Args.size(); ++K) {
      ExprPtr &Arg = Args[K];
      HighExpr *Constant = Arg.get();
      while (Constant && Constant->Operands.size() == 1 &&
             (Constant->Kind == ExprKind::Cast ||
              (Constant->Kind == ExprKind::UnaryOp &&
               Constant->Op == NdOp::INT_ZEXT)))
        Constant = Constant->Operands[0].get();
      if (!Constant || Constant->Kind != ExprKind::Const ||
          (Constant->ConstProvenance != ConstantAddressProvenance::Scalar &&
           Constant->ConstProvenance != ConstantAddressProvenance::Unknown) ||
          !libc::isCStringParameter(Callee, static_cast<unsigned>(K)))
        continue;
      const Segment *Seg = Image->getSegmentFor(Constant->ConstVal);
      if (!Seg || Seg->isExecutable())
        continue;
      Arg = HighExpr::makeConst(Constant->ConstVal,
                                Arg->Type        ? Arg->Type->Size
                                : Constant->Type ? Constant->Type->Size
                                                 : 8,
                                ConstantAddressProvenance::DataAddress);
    }
  auto CallExpr = HighExpr::makeCall(Callee, Target, std::move(Args));
  CallExpr->SourceCallHint = CurOp.SourceCallHint;
  CallExpr->DoesNotReturn = CurOp.DoesNotReturn;
  const bool X87Result =
      CurOp.Output.Kind == MedVar::Reg && CurOp.Output.Size == 10 &&
      getTargetRegInfo(TargetArch).isX87StackReg(CurOp.Output.RegOff);
  if (CurOp.SourceCallHint || X87Result)
    CallExpr->Type = sourceCallResultType(CurOp);
  if (ExpressionObserver && CurOp.Addr != InvalidVA && CurOp.OriginSeq >= 0)
    ExpressionObserver(CurOp, CallExpr);

  if (CurOp.Output.Id >= 0 && CurOp.Output.Size > 0) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Addr = CurOp.Addr;
    S.Dst = HighExpr::makeVar(CurOp.Output,
                              CallExpr->Type &&
                                      CallExpr->Type->Kind == NdTypeKind::Struct
                                  ? CallExpr->Type
                                  : nullptr);
    // All machine consumers read the x87 carrier as eighty raw bits. Keep
    // that representation at the assignment boundary, including when a
    // later pass inlines the call into an arithmetic operand or return.
    S.Val = X87Result ? HighExpr::makeBitCast(
                            CallExpr, NdType::makeInt(CurOp.Output.Size, false))
                      : CallExpr;
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
  CallExpr->MemoryOrdering = CurOp.MemoryOrdering;
  CallExpr->MemoryAddressSpace = CurOp.MemoryAddressSpace;
  // A terminating instruction has no observable return value, even when
  // the generic intrinsic carrier contains a synthetic output temporary.
  const bool HasResult =
      CurOp.Output.Size > 0 && !isUnconditionalTrapIntrinsic(IID);
  if (HasResult)
    CallExpr->Type = NdType::makeInt(CurOp.Output.Size, false);
  else if (isUnconditionalTrapIntrinsic(IID))
    CallExpr->Type = NdType::makeVoid();

  uint8_t NumOut = intrinsicOutputCount(IID);
  if (NumOut > 0) {
    std::vector<MedVar> CoOutputs;
    for (const MedIntrinsicOutputBinding &Binding :
         collectMedIntrinsicOutputBindings(CurBlock, OpIdx, NumOut)) {
      CoOutputs.push_back(Binding.Source);
      // COPY only forwards the pending value.  Sub-register normalization
      // still feeds later stores and returns and must be lowered normally.
      if (Binding.IsCopy)
        IntrinsicSkip.insert(Binding.OpIndex);
    }
    CallExpr->IntrinsicOutputs = std::move(CoOutputs);
  }

  if (CurOp.Output.Id >= 0 && HasResult) {
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
