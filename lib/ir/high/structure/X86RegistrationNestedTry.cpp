//===- X86RegistrationNestedTry.cpp - Nested PE32 callback ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CxxCleanupScope.h"
#include "HighCFSimplifyDetail.h"
#include "X86RegistrationTry.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

#include "llvm/ADT/STLExtras.h"

namespace neverd {

bool checkNestedRegistrationTry(const HighStmt &Stmt, const MedFunc &Med,
                                int32_t TryLow, int32_t TryHigh, size_t &Work) {
  if (Stmt.Kind != StmtKind::CxxTry || !Stmt.EHIsReducible ||
      Stmt.Body.empty() || Stmt.EHClauses.empty() ||
      Stmt.EHClauses.size() != Stmt.EHClauseBodies.size() ||
      !Med.ExceptionMetadata || !Med.ExceptionMetadata->Cxx ||
      !Med.RegistrationStates)
    return false;
  const auto &Tries = Med.ExceptionMetadata->Cxx->TryBlocks;
  const CxxTryBlock *Inner = nullptr;
  uint32_t TryIndex = 0;
  for (uint32_t I = 0; I < Tries.size(); ++I) {
    if (++Work > limits::kMaxRegistrationEHStateWork)
      return false;
    const auto &Try = Tries[I];
    if (Try.Handlers.empty() ||
        Try.Handlers.front().HandlerVA != Stmt.EHClauses.front().HandlerVA)
      continue;
    if (Inner || Try.TryLow <= TryLow || Try.CatchHigh > TryHigh ||
        Try.Handlers.size() > Stmt.EHClauses.size())
      return false;
    Inner = &Try;
    TryIndex = I;
  }
  if (!Inner)
    return false;
  const auto &Cxx = *Med.ExceptionMetadata->Cxx;
  size_t ClauseIndex = Inner->Handlers.size();
  for (int32_t State = Inner->TryLow; State <= Inner->TryHigh; ++State) {
    Work += Cxx.TryBlocks.size() + 1;
    if (Work > limits::kMaxRegistrationEHStateWork)
      return false;
    if (!cxxTryOwnsCleanup(Cxx, TryIndex, State))
      continue;
    if (ClauseIndex >= Stmt.EHClauses.size())
      return false;
    const auto &Clause = Stmt.EHClauses[ClauseIndex];
    const auto &Action = Cxx.UnwindMap[State];
    if (Clause.Kind != HighEHClauseKind::CxxCleanup || Clause.State != State ||
        Clause.FilterOrActionVA != Action.ActionVA ||
        Clause.UnwindActionKind != Action.Kind ||
        Clause.UnwindObjectOffset != Action.ObjectOffset ||
        !Stmt.EHClauseBodies[ClauseIndex].empty())
      return false;
    ++ClauseIndex;
  }
  if (ClauseIndex != Stmt.EHClauses.size())
    return false;
  size_t Operations = 0;
  for (const auto &Block : Med.Blocks) {
    if (Block.Ops.size() > limits::kMaxRegistrationEHStateWork - Operations)
      return false;
    Operations += Block.Ops.size();
  }
  for (uint32_t I = 0; I < Inner->Handlers.size(); ++I) {
    const auto &Clause = Stmt.EHClauses[I];
    const auto &Handler = Inner->Handlers[I];
    const auto &Body = Stmt.EHClauseBodies[I];
    if (Clause.Kind != HighEHClauseKind::CxxCatch ||
        Clause.ParseStatus != ExceptionParseStatus::Complete ||
        Clause.HandlerVA != Handler.HandlerVA ||
        Clause.TypeDescriptorVA != Handler.TypeDescriptorVA ||
        Clause.Adjectives != Handler.Adjectives ||
        Clause.CatchObjectOffset != Handler.CatchObjectOffset ||
        Clause.ParentFrameOffset != Handler.ParentFrameOffset || Body.empty() ||
        Body.front().Addr != Handler.HandlerVA ||
        !highStmtEndsItsBlock(Body.back()))
      return false;
    Work +=
        Operations + Med.Blocks.size() + Med.RegistrationStates->Blocks.size();
    if (Work > limits::kMaxRegistrationEHStateWork)
      return false;
    const auto Region = registrationCallbackRegion(Med, Handler.HandlerVA);
    if (!Region)
      return false;
    auto Owns = [&](va_t Address) {
      return llvm::any_of(Region->Ranges,
                          [&](const auto &R) { return R.contains(Address); });
    };
    for (const auto &Block : Med.Blocks)
      if (Owns(Block.StartAddr))
        for (const auto &Op : Block.Ops) {
          if (++Work > limits::kMaxRegistrationEHStateWork ||
              (!Op.Dead && Op.Opcode == NdOp::INDIR_CALL))
            return false;
          if (Op.Dead || Op.Opcode != NdOp::CALL)
            continue;
          const auto &States = *Med.RegistrationStates;
          const auto *Call = States.callFrameEffect(Op.Addr, Op.OriginSeq);
          const auto State = llvm::find_if(States.Blocks, [&](const auto &S) {
            return S.Range.Begin == Block.StartAddr &&
                   S.Range.End == Block.EndAddr;
          });
          if (!States.CallFrameEffectsComplete || !Call ||
              Call->DoesNotReturn != Op.DoesNotReturn || Op.NumInputs != 1 ||
              !Op.Inputs[0].isConst() ||
              Op.Inputs[0].ConstVal != Call->Target ||
              State == States.Blocks.end() || State->Unknown ||
              State->Levels.size() != 1 ||
              !llvm::any_of(State->CxxSearches, [&](const auto &Search) {
                return Search.Level == State->Levels.front() &&
                       Search.ExitedCatches == 1 &&
                       Search.TryIndex < Tries.size() &&
                       Tries[Search.TryIndex].TryLow == TryLow &&
                       Tries[Search.TryIndex].TryHigh == TryHigh;
              }))
            return false;
        }
    std::vector<const HighStmt *> Pending;
    for (const auto &Child : Body)
      Pending.push_back(&Child);
    bool Resumes = false;
    while (!Pending.empty()) {
      const auto &Child = *Pending.back();
      Pending.pop_back();
      const va_t Address = Child.Addr && Child.Addr != InvalidVA ? Child.Addr
                           : Child.Kind == StmtKind::Goto ? Child.GotoTarget
                                                          : 0;
      const bool Nested =
          !Child.EHClauses.empty() || !Child.EHClauseBodies.empty();
      if (++Work > limits::kMaxRegistrationEHStateWork || !Owns(Address) ||
          (Nested && !checkNestedRegistrationTry(Child, Med, Inner->TryHigh + 1,
                                                 Inner->CatchHigh, Work)) ||
          Child.Kind == StmtKind::Return || Child.Kind == StmtKind::Break ||
          Child.Kind == StmtKind::Continue)
        return false;
      if (Child.Kind == StmtKind::Goto && !Owns(Child.GotoTarget)) {
        Work += Med.RegistrationStates->CxxContinuations.size();
        if (Work > limits::kMaxRegistrationEHStateWork ||
            !llvm::is_contained(Clause.ContinuationVAs, Child.GotoTarget) ||
            !llvm::any_of(Med.RegistrationStates->CxxContinuations,
                          [&](const auto &Resume) {
                            return Resume.TryIndex == TryIndex &&
                                   Resume.CatchIndex == I &&
                                   Resume.Address == Child.Addr &&
                                   Resume.TargetVA == Child.GotoTarget;
                          }))
          return false;
        Resumes = true;
      }
      auto Add = [&](const std::vector<HighStmt> &Children) {
        for (const auto &Next : Children)
          Pending.push_back(&Next);
      };
      Add(Child.Body);
      Add(Child.ElseBody);
      Add(Child.DefaultBody);
      for (const auto &Case : Child.Cases)
        Add(Case.Body);
    }
    if (!Resumes)
      return false;
  }
  return true;
}

} // namespace neverd
