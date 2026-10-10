//===- X86RegistrationTry.cpp - Synchronous registration regions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86RegistrationTry.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCall.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>

namespace neverd {

bool extractTerminalRegistrationTry(HighFunc &Func, const MedFunc &Med,
                                    std::vector<HighStmt> &Body,
                                    size_t &InsertAt) {
  if (!Med.ExceptionMetadata || !Med.RegistrationStates || Med.Blocks.empty() ||
      Med.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return false;
  const auto &EH = *Med.ExceptionMetadata;
  const auto &State = *Med.RegistrationStates;
  if (!EH.Registration || EH.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Cxx ||
      !EH.Cxx->IsSynchronous || EH.Cxx->TryBlocks.size() != 1 ||
      !State.Complete || !State.CallFrameEffectsComplete ||
      !State.CxxContinuationsComplete || !State.RegistrationLifetimeComplete ||
      State.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return false;
  const auto &Try = EH.Cxx->TryBlocks.front();
  std::map<int, const MedBlock *> Blocks;
  std::map<va_t, std::pair<va_t, int>> Ranges;
  int Entry = -1;
  size_t OperationCount = 0;
  for (const auto &Block : Med.Blocks) {
    OperationCount += Block.Ops.size();
    if (OperationCount > limits::kMaxRegistrationEHStateWork)
      return false;
    if (Block.StartAddr >= Block.EndAddr ||
        !Blocks.emplace(Block.Id, &Block).second ||
        !Ranges
             .emplace(Block.StartAddr, std::make_pair(Block.EndAddr, Block.Id))
             .second)
      return false;
    if (Block.StartAddr == Med.Entry)
      Entry = Block.Id;
  }
  va_t End = 0;
  for (const auto &[Start, Range] : Ranges) {
    if (Start < End)
      return false;
    End = Range.first;
  }
  std::set<int> Ordinary;
  std::set<va_t> Calls;
  std::vector<int> Pending{Entry};
  size_t Work = 0;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork || !Blocks.count(Id))
      return false;
    if (!Ordinary.insert(Id).second)
      continue;
    const auto &Block = *Blocks.at(Id);
    if (!Block.ExceptionalPreds.empty())
      return false;
    bool Terminal = false;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      if (Op.Dead)
        continue;
      if (Terminal || Op.Opcode == NdOp::RETURN ||
          Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR)
        return false;
      if (Op.Opcode != NdOp::CALL)
        continue;
      Work += OperationCount + State.Blocks.size();
      if (Work > limits::kMaxRegistrationEHStateWork)
        return false;
      // Extending a synchronous language scope over its branch and frame
      // setup is valid only if every ordinary call already has that scope.
      // In particular, never bring an unprotected call or an EHa memory fault
      // under a new handler merely because the addresses surround the try.
      if (!registrationCallABI(Med, Block, Op) || !Op.DoesNotReturn ||
          !llvm::any_of(State.Blocks,
                        [&](const auto &S) {
                          return S.Reached && !S.CallbackOnly && !S.Unknown &&
                                 S.Range.contains(Op.Addr) &&
                                 !S.Levels.empty() &&
                                 llvm::all_of(S.Levels, [&](int32_t Level) {
                                   return Level >= Try.TryLow &&
                                          Level <= Try.TryHigh;
                                 });
                        }) ||
          !Calls.insert(Op.Addr).second)
        return false;
      Terminal = true;
    }
    if (Terminal != Block.Succs.empty())
      return false;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  if (Calls.empty())
    return false;
  for (int Id : Ordinary)
    for (int Pred : Blocks.at(Id)->Preds)
      if (!Ordinary.count(Pred))
        return false;

  auto Owner = [&](va_t Address) -> std::optional<bool> {
    auto It = Ranges.upper_bound(Address);
    if (It == Ranges.begin() || (--It)->second.first <= Address)
      return std::nullopt;
    return Ordinary.count(It->second.second) != 0;
  };
  std::vector<bool> Selected;
  std::set<va_t> SeenCalls;
  for (const auto &Top : Func.Body) {
    bool Inside = false, Outside = false, Invalid = false;
    std::vector<const HighStmt *> Statements{&Top};
    while (!Statements.empty()) {
      const auto &Stmt = *Statements.back();
      Statements.pop_back();
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return false;
      const va_t Address = Stmt.Addr && Stmt.Addr != InvalidVA ? Stmt.Addr
                           : Stmt.Kind == StmtKind::Goto       ? Stmt.GotoTarget
                                                               : 0;
      const auto Part = Owner(Address);
      if (!Part) {
        Invalid = true;
        break;
      }
      Inside |= *Part;
      Outside |= !*Part;
      if (Calls.count(Stmt.Addr))
        SeenCalls.insert(Stmt.Addr);
      auto Add = [&](const std::vector<HighStmt> &Children) {
        for (const auto &Child : Children)
          Statements.push_back(&Child);
      };
      Add(Stmt.Body);
      Add(Stmt.ElseBody);
      Add(Stmt.DefaultBody);
      for (const auto &Case : Stmt.Cases)
        Add(Case.Body);
      // Existing language regions need their own scope ownership; this
      // terminal-component projection does not flatten nested handlers.
      if (!Stmt.EHClauses.empty() || !Stmt.EHClauseBodies.empty())
        return false;
    }
    if (Invalid || Inside == Outside)
      return false;
    Selected.push_back(Inside);
  }
  if (SeenCalls != Calls)
    return false;
  // Every gap must follow an explicit terminal source call. Relocating an
  // independently entered resume body cannot manufacture a fallthrough edge.
  for (size_t I = 1; I < Selected.size(); ++I)
    if (Selected[I - 1] && !Selected[I]) {
      const auto &Previous = Func.Body[I - 1];
      const auto &Call =
          Previous.Kind == StmtKind::Call ? Previous.CallExpr : Previous.Val;
      if (!Calls.count(Previous.Addr) || !Call ||
          Call->Kind != ExprKind::Call || !Call->DoesNotReturn)
        return false;
    }
  std::vector<HighStmt> Protected, Remaining;
  InsertAt = 0;
  bool Found = false;
  for (size_t I = 0; I < Selected.size(); ++I) {
    if (Selected[I]) {
      if (!Found)
        InsertAt = Remaining.size();
      Found = true;
      Protected.push_back(std::move(Func.Body[I]));
    } else
      Remaining.push_back(std::move(Func.Body[I]));
  }
  if (!Found)
    return false;
  Func.Body = std::move(Remaining);
  Body = std::move(Protected);
  return true;
}

} // namespace neverd
