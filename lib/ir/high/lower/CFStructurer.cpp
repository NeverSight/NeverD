//===- CFStructurer.cpp - Control-flow structuring ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Control-flow structuring for HighIR: block-level dispatch loop and PHI
/// copy insertion.  Individual NdOp lowering helpers live in:
///   NdOpLowering.cpp         — STORE, CALL, INTRINSIC, COND_BR, BRANCH,
///                               generic-assign
///   NdOpCallIndLowering.cpp  — INDIR_CALL target resolution and lowering
///   NdOpReturnLowering.cpp   — RETURN value recovery
///   NdOpSwitchRecovery.cpp   — INDIR_BR / jump-table switch recovery
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <set>

namespace neverd {

//===----------------------------------------------------------------------===//
// insertPhiCopies
//===----------------------------------------------------------------------===//

void MedToHighConverter::insertPhiCopies(
    HighFunc &Func, const MedBlock &CurBlock, int BlkIdx, size_t BlkBodyStart,
    const std::map<std::pair<int, int>, std::vector<std::pair<MedVar, MedVar>>>
        &PhiCopies) {
  auto CopiesFor = [&](int Successor) {
    std::vector<HighStmt> Copies;
    auto It = PhiCopies.find({BlkIdx, Successor});
    if (It == PhiCopies.end())
      return Copies;
    VarKeySet Destinations;
    for (const auto &[Output, Argument] : It->second)
      if (Output.Kind != MedVar::Flag)
        Destinations.insert(varKey(Output));
    std::vector<HighStmt> Writes;
    for (const auto &[Output, Argument] : It->second) {
      if (Output.Kind == MedVar::Flag || Output == Argument)
        continue;
      HighStmt Copy;
      Copy.Kind = StmtKind::Assign;
      Copy.Addr =
          CurBlock.Ops.empty() ? CurBlock.StartAddr : CurBlock.Ops.back().Addr;
      Copy.IsPhiCopy = true;
      Copy.Dst = HighExpr::makeVar(Output);
      Copy.Val = medvarToExpr(Argument);
      bool ReadsDestination = false;
      std::set<const HighExpr *> Seen;
      std::function<void(const ExprPtr &)> Visit =
          [&](const ExprPtr &Expression) {
            if (!Expression || !Seen.insert(Expression.get()).second)
              return;
            if (Expression->Kind == ExprKind::Var &&
                Destinations.count(varKey(Expression->Var)))
              ReadsDestination = true;
            for (const auto &Operand : Expression->Operands)
              Visit(Operand);
          };
      Visit(Copy.Val);
      if (ReadsDestination) {
        // PHIs read their predecessor values simultaneously. Capture any
        // expression using an overwritten PHI before publishing edge writes;
        // this also handles cycles such as a <- b, b <- a.
        MedVar Snapshot;
        Snapshot.Kind = MedVar::Temp;
        Snapshot.Id = NextHighTempId++;
        Snapshot.Size = Output.Size;
        HighStmt Capture = Copy;
        Capture.Dst = HighExpr::makeVar(Snapshot, Copy.Val->Type);
        Copies.push_back(Capture);
        Copy.Val = Capture.Dst;
      }
      Writes.push_back(std::move(Copy));
    }
    Copies.insert(Copies.end(), Writes.begin(), Writes.end());
    return Copies;
  };

  size_t BranchIndex = Func.Body.size();
  for (size_t I = Func.Body.size(); I > BlkBodyStart; --I)
    if (Func.Body[I - 1].Kind == StmtKind::Goto ||
        Func.Body[I - 1].Kind == StmtKind::If) {
      BranchIndex = I - 1;
      break;
    }
  for (int Successor : CurBlock.Succs) {
    if (!CurMed || Successor < 0 || Successor >= int(CurMed->Blocks.size()))
      continue;
    auto Copies = CopiesFor(Successor);
    if (Copies.empty())
      continue;
    const auto &TargetBlock = CurMed->Blocks[Successor];
    va_t Target = TargetBlock.StartAddr;
    if (!Target && !TargetBlock.Ops.empty())
      Target = TargetBlock.Ops.front().Addr;
    if (BranchIndex < Func.Body.size()) {
      auto &Branch = Func.Body[BranchIndex];
      if (Branch.Kind == StmtKind::If && !Branch.Body.empty() &&
          Branch.Body.back().Kind == StmtKind::Goto &&
          Branch.Body.back().GotoTarget == Target) {
        Branch.Body.insert(Branch.Body.end() - 1, Copies.begin(), Copies.end());
        continue;
      }
      if (Branch.Kind == StmtKind::Goto && Branch.GotoTarget == Target) {
        Func.Body.insert(Func.Body.begin() + BranchIndex, Copies.begin(),
                         Copies.end());
        BranchIndex += Copies.size();
        continue;
      }
    }
    // The untaken conditional edge (or the sole fallthrough edge) executes
    // its copies after the branch. No loop-backedge write occurs on an exit.
    Func.Body.insert(Func.Body.end(), Copies.begin(), Copies.end());
  }
}

//===----------------------------------------------------------------------===//
// structureControlFlow — block-level dispatch
//===----------------------------------------------------------------------===//

void MedToHighConverter::structureControlFlow(HighFunc &Func,
                                              const MedFunc &Med) {
  std::map<std::pair<int, int>, std::vector<std::pair<MedVar, MedVar>>>
      PhiCopies;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        PhiCopies[{PredId, Block.Id}].push_back({Phi.Output, Arg});

  JtConsumedBlocks.clear();

  VarKeySet PhiArgVars;
  for (auto &Block : Med.Blocks)
    for (auto &Phi : Block.Phis)
      for (auto &[PredId, Arg] : Phi.Args)
        if (Arg.Id >= 0)
          PhiArgVars.insert(varKey(Arg));

  for (int BlkIdx = 0; BlkIdx < static_cast<int>(Med.Blocks.size()); ++BlkIdx) {
    if (JtConsumedBlocks.count(BlkIdx))
      continue;
    auto &CurBlock = Med.Blocks[BlkIdx];
    size_t BlkBodyStart = Func.Body.size();
    std::set<size_t> IntrinsicSkip;
    for (size_t OpIdx = 0; OpIdx < CurBlock.Ops.size(); ++OpIdx) {
      if (IntrinsicSkip.count(OpIdx))
        continue;
      auto &CurOp = CurBlock.Ops[OpIdx];
      switch (CurOp.Opcode) {
      case NdOp::STORE:
        lowerStore(Func, CurOp);
        break;
      case NdOp::CALL:
        lowerCall(Func, CurBlock, CurOp);
        break;
      case NdOp::INTRINSIC:
        lowerIntrinsic(Func, CurBlock, CurOp, OpIdx, IntrinsicSkip);
        break;
      case NdOp::INDIR_CALL:
        lowerCallInd(Func, CurBlock, CurOp);
        break;
      case NdOp::RETURN:
        lowerReturn(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::COND_BR:
        lowerCBranch(Func, CurOp);
        break;
      case NdOp::BRANCH:
        lowerBranch(Func, CurOp);
        break;
      case NdOp::INDIR_BR:
        lowerBranchInd(Func, CurBlock, CurOp, Med);
        break;
      case NdOp::NOP:
        break;
      default:
        lowerGenericAssign(Func, CurOp, PhiArgVars);
        break;
      }
    }

    insertPhiCopies(Func, CurBlock, BlkIdx, BlkBodyStart, PhiCopies);
  }
}

} // namespace neverd
