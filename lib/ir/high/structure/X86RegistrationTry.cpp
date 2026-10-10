//===- X86RegistrationTry.cpp - Synchronous registration regions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86RegistrationTry.h"

#include "neverd/Limits.h"
#include "neverd/ir/med/X86RegistrationCall.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

#include "llvm/ADT/STLExtras.h"

#include <map>
#include <set>

namespace neverd {

namespace {
struct TerminalRegistrationRegion {
  std::map<va_t, std::pair<va_t, int>> Ranges;
  std::set<int> Ordinary;
  std::set<va_t> Calls;
  size_t Work = 0;
};

std::optional<TerminalRegistrationRegion>
terminalRegistrationRegion(const MedFunc &Med, int32_t TryLow,
                           int32_t TryHigh) {
  if (!Med.ExceptionMetadata || !Med.RegistrationStates || Med.Blocks.empty() ||
      Med.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto &EH = *Med.ExceptionMetadata;
  const auto &State = *Med.RegistrationStates;
  if (!EH.Registration || EH.Encoding != ExceptionEncoding::X86CxxFuncInfo ||
      EH.ParseStatus != ExceptionParseStatus::Complete || !EH.Cxx ||
      EH.Cxx->TryBlocks.size() > limits::kMaxRegistrationEHRecords ||
      EH.Cxx->UnwindMap.size() > limits::kMaxRegistrationEHRecords ||
      !EH.Cxx->IsSynchronous || !EH.Cxx->hasValidStateGraph() ||
      !State.Complete || !State.CallFrameEffectsComplete ||
      !State.CxxContinuationsComplete || !State.RegistrationLifetimeComplete ||
      State.Blocks.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const auto NativeTry = llvm::find_if(EH.Cxx->TryBlocks, [&](const auto &Try) {
    return Try.TryLow == TryLow && Try.TryHigh == TryHigh;
  });
  if (NativeTry == EH.Cxx->TryBlocks.end())
    return std::nullopt;
  const auto &Try = *NativeTry;
  const auto Parents = registrationCatchParents(Med);
  if (!Parents)
    return std::nullopt;
  const auto Parent = (*Parents)[NativeTry - EH.Cxx->TryBlocks.begin()];
  const va_t EntryVA =
      Parent
          ? EH.Cxx->TryBlocks[Parent->first].Handlers[Parent->second].HandlerVA
          : Med.Entry;
  std::map<int, const MedBlock *> Blocks;
  TerminalRegistrationRegion Result;
  auto &Ranges = Result.Ranges;
  int Entry = -1;
  size_t OperationCount = 0;
  for (const auto &Block : Med.Blocks) {
    OperationCount += Block.Ops.size();
    if (OperationCount > limits::kMaxRegistrationEHStateWork)
      return std::nullopt;
    if (Block.StartAddr >= Block.EndAddr ||
        !Blocks.emplace(Block.Id, &Block).second ||
        !Ranges
             .emplace(Block.StartAddr, std::make_pair(Block.EndAddr, Block.Id))
             .second)
      return std::nullopt;
    if (Block.StartAddr == EntryVA)
      Entry = Block.Id;
  }
  va_t End = 0;
  for (const auto &[Start, Range] : Ranges) {
    if (Start < End)
      return std::nullopt;
    End = Range.first;
  }
  auto &Ordinary = Result.Ordinary;
  auto &Calls = Result.Calls;
  std::vector<int> Pending{Entry};
  auto &Work = Result.Work;
  while (!Pending.empty()) {
    const int Id = Pending.back();
    Pending.pop_back();
    if (++Work > limits::kMaxRegistrationEHStateWork || !Blocks.count(Id))
      return std::nullopt;
    if (!Ordinary.insert(Id).second)
      continue;
    const auto &Block = *Blocks.at(Id);
    if (!Block.ExceptionalPreds.empty() && (!Parent || Id != Entry))
      return std::nullopt;
    bool Terminal = false;
    for (const auto &Op : Block.Ops) {
      if (++Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      if (Op.Dead)
        continue;
      if (Terminal || Op.Opcode == NdOp::RETURN ||
          Op.Opcode == NdOp::INDIR_CALL || Op.Opcode == NdOp::INDIR_BR)
        return std::nullopt;
      if (Op.Opcode != NdOp::CALL)
        continue;
      Work += OperationCount + State.Blocks.size();
      if (Work > limits::kMaxRegistrationEHStateWork)
        return std::nullopt;
      // Extending a synchronous language scope over its branch and frame
      // setup is valid when each throw already has that scope. A checked
      // returning leaf has no calls or C++ throws, so synchronous EH permits
      // its placement before the compiler's first state store. Unknown calls
      // and asynchronous memory faults cannot use this exception.
      if (!registrationCallABI(Med, Block, Op) ||
          !llvm::any_of(
              State.Blocks,
              [&](const auto &S) {
                const bool Context =
                    Parent ? S.CallbackOnly && S.CxxCatchStacks.size() == 1 &&
                                 !S.CxxCatchStacks[0].empty() &&
                                 S.CxxCatchStacks[0].back() == *Parent
                           : !S.CallbackOnly;
                return S.Reached && Context && !S.Unknown &&
                       S.Range.contains(Op.Addr) && !S.Levels.empty() &&
                       llvm::all_of(S.Levels, [&](int32_t Level) {
                         return !Op.DoesNotReturn ||
                                (Level >= Try.TryLow && Level <= Try.TryHigh);
                       });
              }) ||
          !Calls.insert(Op.Addr).second)
        return std::nullopt;
      Terminal = Op.DoesNotReturn;
    }
    if (Terminal != Block.Succs.empty())
      return std::nullopt;
    Pending.insert(Pending.end(), Block.Succs.begin(), Block.Succs.end());
  }
  if (Calls.empty())
    return std::nullopt;
  for (int Id : Ordinary)
    for (int Pred : Blocks.at(Id)->Preds)
      if (!Ordinary.count(Pred))
        return std::nullopt;

  return Result;
}
} // namespace

std::vector<ExceptionAddressRange>
terminalRegistrationTryRanges(const MedFunc &Med, int32_t TryLow,
                              int32_t TryHigh) {
  const auto Region = terminalRegistrationRegion(Med, TryLow, TryHigh);
  if (!Region)
    return {};
  std::vector<ExceptionAddressRange> Ranges;
  for (const auto &[Begin, Range] : Region->Ranges)
    if (Region->Ordinary.count(Range.second))
      Ranges.push_back({Begin, Range.first});
  return Ranges;
}

bool extractTerminalRegistrationTry(HighFunc &Func, const MedFunc &Med,
                                    int32_t TryLow, int32_t TryHigh,
                                    std::vector<HighStmt> &Body,
                                    size_t &InsertAt) {
  auto Region = terminalRegistrationRegion(Med, TryLow, TryHigh);
  if (!Region)
    return false;
  const auto &Ranges = Region->Ranges;
  const auto &Ordinary = Region->Ordinary;
  const auto &Calls = Region->Calls;
  auto &Work = Region->Work;

  auto Owner = [&](va_t Address) -> std::optional<bool> {
    auto It = Ranges.upper_bound(Address);
    if (It == Ranges.begin() || (--It)->second.first <= Address)
      return std::nullopt;
    return Ordinary.count(It->second.second) != 0;
  };
  std::vector<bool> Selected;
  std::set<va_t> SeenCalls;
  std::set<const HighStmt *> Nested;
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
      // Inner callback code is an independent runtime entry, not part of
      // the ordinary component. Keep it attached to its checked inner try.
      if (!Stmt.EHClauses.empty() || !Stmt.EHClauseBodies.empty()) {
        if (!checkNestedRegistrationTry(Stmt, Med, TryLow, TryHigh, Work))
          return false;
        Nested.insert(&Stmt);
      }
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
      const HighStmt *Previous = &Func.Body[I - 1];
      while (Nested.count(Previous))
        Previous = &Previous->Body.back();
      const auto &Call =
          Previous->Kind == StmtKind::Call ? Previous->CallExpr : Previous->Val;
      if (!Calls.count(Previous->Addr) || !Call ||
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
