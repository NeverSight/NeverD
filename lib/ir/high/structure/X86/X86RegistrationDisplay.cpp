//===- X86RegistrationDisplay.cpp - x86 SEH in the decompile view ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X86RegistrationDisplay.h"

#include "../../pass/HighFrameAddress.h"
#include "../HighCFSimplifyDetail.h"
#include "../SEHFilterValues.h"
#include "../SEHFinallyBodiesDetail.h"
#include "X86RegistrationRegions.h"

#include "neverd/Limits.h"
#include "neverd/ir/RegistrationDisplay.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/loader/ExceptionFunction.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <functional>

namespace neverd {

namespace {

using seh_filter::callOfRuntime;
using seh_filter::callsNamed;
using seh_filter::ExceptionReads;
using seh_filter::kExceptionCode;
using seh_filter::peelViews;

constexpr llvm::StringLiteral kAbnormalTermination = "AbnormalTermination";
/// Where MSVC keeps the registration record, from the frame pointer.
constexpr int32_t kMSVCRecordFromFrame = -16;
/// How many times a function's statements are read for what its variables hold:
/// statements come in the order structuring left, which can put a use first.
constexpr unsigned kFrameAddressPasses = 2;

/// Whether \p E calls anything.
bool hasCallIn(const ExprPtr &E) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Call)
    return true;
  return llvm::any_of(E->Operands, hasCallIn);
}

/// The direct call that is all \p S does, past the views of its value.
const HighExpr *wholeCall(const HighStmt &S) {
  const ExprPtr *Value = nullptr;
  switch (S.Kind) {
  case StmtKind::Call:
    Value = &S.CallExpr;
    break;
  case StmtKind::ExprStmt:
  case StmtKind::Assign:
    Value = &S.Val;
    break;
  case StmtKind::Return:
    Value = &S.RetVal;
    break;
  default:
    return nullptr;
  }
  const HighExpr *E = peelViews(Value->get());
  return E && E->Kind == ExprKind::Call && !E->IsIndirectCall ? E : nullptr;
}

/// The names of the values computed from the frame pointer or the stack pointer
/// by a fixed distance.  SSA defines each before it is read in block order, but
/// for a loop's back edges, which frame addresses do not take.
std::set<VarKey> frameValues(const MedFunc &Med) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);
  std::set<VarKey> Values;
  auto IsFrame = [&](const MedVar &V) {
    return (V.Kind == MedVar::Reg &&
            (V.RegOff == TRI.FramePointer || V.RegOff == TRI.StackPointer)) ||
           Values.count(varKey(V));
  };
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      if (!Op.Output.Size || !Op.NumInputs || !IsFrame(Op.Inputs[0]))
        continue;
      const bool ByConstant =
          Op.NumInputs == 2 && Op.Inputs[1].Kind == MedVar::Const;
      if (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
          ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
            Op.Opcode == NdOp::SUBBYTES) &&
           ByConstant))
        Values.insert(varKey(Op.Output));
    }
  return Values;
}

/// Whether \p Address is one of \p Frame at a fixed distance.
bool addressesTheFrame(const HighExpr *Address, const std::set<VarKey> &Frame) {
  Address = peelViews(Address);
  while (Address && Address->Kind == ExprKind::BinOp &&
         Address->Operands.size() == 2 &&
         (Address->Op == NdOp::INT_ADD || Address->Op == NdOp::INT_SUB) &&
         peelViews(Address->Operands[1].get()) &&
         peelViews(Address->Operands[1].get())->Kind == ExprKind::Const)
    Address = peelViews(Address->Operands[0].get());
  if (!Address || Address->Kind != ExprKind::Var)
    return false;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);
  return (Address->Var.Kind == MedVar::Reg &&
          (Address->Var.RegOff == TRI.FramePointer ||
           Address->Var.RegOff == TRI.StackPointer)) ||
         Frame.count(varKey(Address->Var));
}

/// Whether \p E reads through nothing but the frame: it cannot fault or call.
bool readsOnlyTheFrame(const HighExpr *E, const std::set<VarKey> &Frame) {
  if (!E)
    return true;
  if (E->Kind == ExprKind::Call)
    return false;
  if (E->Kind == ExprKind::Load)
    return !E->Operands.empty() &&
           addressesTheFrame(E->Operands[0].get(), Frame);
  return llvm::all_of(E->Operands, [&](const ExprPtr &Operand) {
    return readsOnlyTheFrame(Operand.get(), Frame);
  });
}

/// Whether \p S can run at the end of a `__try` instead of between it and the
/// `call` of its finally: it runs before the body either way and cannot raise,
/// as the abnormal flag is cleared there.
bool runsQuietlyBeforeFinally(const HighStmt &S,
                              const std::set<VarKey> &Frame) {
  switch (S.Kind) {
  case StmtKind::Nop:
    return true;
  case StmtKind::Block:
    return S.Body.empty();
  case StmtKind::Assign:
    return S.Dst &&
           (S.Dst->Kind == ExprKind::Var ||
            readsOnlyTheFrame(S.Dst.get(), Frame)) &&
           readsOnlyTheFrame(S.Val.get(), Frame);
  case StmtKind::Store:
    return addressesTheFrame(S.StoreAddr.get(), Frame) &&
           readsOnlyTheFrame(S.StoreVal.get(), Frame);
  default:
    return false;
  }
}

/// What \p Body returns to its `call`, when one variable or constant does on
/// every path.
ExprPtr returnedValue(const std::vector<HighStmt> &Body,
                      const std::set<va_t> &Returns) {
  ExprPtr Value;
  bool One = true;
  walkStmts(Body, [&](const HighStmt &S) {
    if (S.Kind != StmtKind::Return || !Returns.count(S.Addr))
      return;
    const bool Same =
        S.RetVal &&
        (S.RetVal->Kind == ExprKind::Var
             ? !Value || (Value->Kind == ExprKind::Var &&
                          varKey(Value->Var) == varKey(S.RetVal->Var))
             : S.RetVal->Kind == ExprKind::Const &&
                   (!Value || (Value->Kind == ExprKind::Const &&
                               Value->ConstVal == S.RetVal->ConstVal)));
    if (Same)
      Value = S.RetVal;
    else
      One = false;
  });
  return One ? Value : nullptr;
}

template <typename F>
void forEachList(std::vector<HighStmt> &List, const F &Fn) {
  Fn(List);
  for (HighStmt &S : List) {
    forEachList(S.Body, Fn);
    forEachList(S.ElseBody, Fn);
    for (SwitchCase &Case : S.Cases)
      forEachList(Case.Body, Fn);
    forEachList(S.DefaultBody, Fn);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      forEachList(Clause, Fn);
  }
}

/// The frame pointer's value at each runtime entry, as the distance from the
/// stack pointer at the function's entry, by the name it goes by.
std::map<VarKey, int64_t> runtimeFrames(const MedFunc &Med) {
  std::map<VarKey, int64_t> Frames;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.RegistrationRoot != MedOp::RegistrationRootKind::None)
        if (const auto Offset = registrationRootEntryStackOffset(Op))
          Frames.emplace(varKey(Op.Output), *Offset);
  return Frames;
}

bool hasLabelAddress(const HighStmt &S) {
  return S.Addr && S.Addr != InvalidVA;
}

/// What a jump names to run the statements of \p List from \p From on.
std::optional<va_t> labelOfNext(const std::vector<HighStmt> &List,
                                size_t From) {
  for (size_t I = From; I < List.size(); ++I) {
    const HighStmt &S = List[I];
    if (S.Kind == StmtKind::Nop ||
        (S.Kind == StmtKind::Block && S.Body.empty() && !hasLabelAddress(S)))
      continue;
    if (hasLabelAddress(S))
      return S.Addr;
    if (S.Kind == StmtKind::Goto)
      return S.GotoTarget;
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

RegistrationDisplay::RegistrationDisplay(const MedFunc &Med,
                                         const ExceptionFunction &EH,
                                         const RegistrationDisplayFacts &Facts)
    : Med(&Med), EH(&EH), Facts(&Facts), Frames(runtimeFrames(Med)) {
  const auto &Scopes = EH.Registration->Scopes;
  for (size_t I = 0; I < Scopes.size(); ++I)
    for (const va_t Root : {Scopes[I].HandlerVA, Scopes[I].FilterVA})
      if (Root)
        ScopeOfRoot.emplace(Root, I);
}

std::optional<RegistrationDisplay>
RegistrationDisplay::build(const MedFunc &Med, const ExceptionFunction &EH) {
  if (!Med.AnalysisOnly || !EH.Registration || EH.Cxx ||
      !Med.RegistrationStates || !Med.RegistrationStates->Display)
    return std::nullopt;
  return RegistrationDisplay(Med, EH, *Med.RegistrationStates->Display);
}

std::optional<std::vector<ExceptionAddressRange>>
RegistrationDisplay::bodyAt(va_t Root) const {
  std::vector<ExceptionAddressRange> Merged;
  for (const RegistrationDisplayFacts::Stretch &S : Facts->Stretches) {
    if (S.Owner != Root)
      continue;
    if (!Merged.empty() && Merged.back().End == S.Range.Begin)
      Merged.back().End = S.Range.End;
    else
      Merged.push_back(S.Range);
  }
  if (Merged.empty())
    return std::nullopt;
  return Merged;
}

void RegistrationDisplay::dropLocalUnwinds(HighFunc &Func) const {
  if (Facts->LocalUnwinds.empty())
    return;
  std::set<VarKey> Results;
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    for (size_t I = 0; I < List.size();) {
      HighStmt &S = List[I];
      const HighExpr *Call = wholeCall(S);
      if (!Call || !Facts->LocalUnwinds.count(
                       Call->CallSiteAddr ? Call->CallSiteAddr : S.Addr)) {
        ++I;
        continue;
      }
      if (S.Kind == StmtKind::Return) {
        const uint16_t Size =
            S.RetVal && S.RetVal->Type ? S.RetVal->Type->Size : 4;
        S.RetVal = HighExpr::makeUndef(Size);
        ++I;
        continue;
      }
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
        Results.insert(varKey(S.Dst->Var));
      // It leaves the tries down to its level, as a jump out of them does, so
      // the jump stays in them.
      if (const std::optional<va_t> Next = labelOfNext(List, I + 1)) {
        const va_t At = Call->CallSiteAddr ? Call->CallSiteAddr : S.Addr;
        S = HighStmt{};
        S.Kind = StmtKind::Goto;
        S.Addr = At;
        S.GotoTarget = *Next;
        S.AbnormalExit = true;
        ++I;
        continue;
      }
      List.erase(List.begin() + static_cast<ptrdiff_t>(I));
    }
  });
  // The register held nothing the function meant.
  if (!Results.empty())
    forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
      for (HighStmt &S : List)
        if (S.Kind == StmtKind::Return && S.RetVal &&
            S.RetVal->Kind == ExprKind::Var &&
            Results.count(varKey(S.RetVal->Var)))
          S.RetVal = HighExpr::makeUndef(S.RetVal->Var.Size);
    });
}

std::optional<RegistrationDisplay::SettledFinally>
RegistrationDisplay::settleClause(HighStmt &Try,
                                  const std::set<va_t> &Leaving) const {
  if (Try.Kind != StmtKind::SEHTry || !Try.EHIsReducible ||
      Try.EHClauses.size() != 1 || Try.EHClauseBodies.size() != 1 ||
      Try.EHClauses[0].Kind != HighEHClauseKind::SEHFinally ||
      Try.EHClauses[0].FinallyKind == HighEHClause::FinallyBody::WholeBody)
    return std::nullopt;
  HighEHClause &Clause = Try.EHClauses[0];
  auto Ranges = bodyAt(Clause.FilterOrActionVA);
  if (!Ranges)
    return std::nullopt;
  std::vector<HighStmt> &Body = Try.EHClauseBodies[0];
  // The last `return` of a function prints as nothing, so what the machine does
  // says whether the body comes back.
  const bool Returns =
      llvm::any_of(*Ranges, [&](const ExceptionAddressRange &R) {
        const auto It = Facts->FinallyReturns.lower_bound(R.Begin);
        return It != Facts->FinallyReturns.end() && R.contains(*It);
      });
  ExprPtr Returned = returnedValue(Body, Facts->FinallyReturns);
  if (!seh_finally::settleHandlerReturns(Body, &Leaving))
    return std::nullopt;
  nameAbnormalTermination(Body, Clause.FilterOrActionVA);
  Clause.FinallyKind = HighEHClause::FinallyBody::WholeBody;
  BodyResults[Clause.FilterOrActionVA] = Returned;
  return SettledFinally{std::move(*Ranges), Returns, std::move(Returned)};
}

bool RegistrationDisplay::hasLeftScope(va_t Address, va_t Root) const {
  const auto Scope = ScopeOfRoot.find(Root);
  if (Scope == ScopeOfRoot.end())
    return false;
  const auto &Scopes = EH->Registration->Scopes;
  const auto &All = Facts->Stretches;
  auto After =
      std::upper_bound(All.begin(), All.end(), Address,
                       [](va_t A, const RegistrationDisplayFacts::Stretch &S) {
                         return A < S.Range.Begin;
                       });
  if (After == All.begin() || !std::prev(After)->Range.contains(Address))
    return false;
  for (int32_t Level : std::prev(After)->Levels)
    for (size_t Step = 0; Step <= Scopes.size() && Level >= 0 &&
                          static_cast<size_t>(Level) < Scopes.size();
         ++Step) {
      if (static_cast<size_t>(Level) == Scope->second)
        return false;
      Level = Scopes[Level].EnclosingLevel;
    }
  return true;
}

void RegistrationDisplay::nameAbnormalTermination(std::vector<HighStmt> &Body,
                                                  va_t Root) const {
  const auto Outlined = Facts->OutlinedBodies.find(Root);
  if (Outlined == Facts->OutlinedBodies.end())
    return;
  // The handler is the unwind's way in, and says 1 where the normal path's call
  // says 0: the clause is both, as AbnormalTermination() is.
  forEachList(Body, [&](std::vector<HighStmt> &List) {
    for (HighStmt &S : List) {
      HighExpr *Call = const_cast<HighExpr *>(wholeCall(S));
      if (!Call || Call->CallAddr != Outlined->second || Call->Operands.empty())
        continue;
      const HighExpr *First = peelViews(Call->Operands[0].get());
      if (First && First->Kind == ExprKind::Const && First->ConstVal == 1)
        Call->Operands[0] =
            callOfRuntime(kAbnormalTermination, Call->Operands[0]->Type);
    }
  });
}

void RegistrationDisplay::takeNormalPath(std::vector<HighStmt> &List,
                                         size_t Index,
                                         const SettledFinally &Finally,
                                         const std::set<va_t> &Labels,
                                         const std::set<VarKey> &Frame) const {
  // What it does before the call, then the call.
  auto InBody = [&](va_t Address) {
    return llvm::any_of(Finally.Ranges, [&](const ExceptionAddressRange &R) {
      return R.contains(Address);
    });
  };
  size_t Call = Index + 1;
  for (; Call < List.size(); ++Call) {
    const HighStmt &S = List[Call];
    if (const HighExpr *E = wholeCall(S);
        E && (InBody(E->CallAddr) || Facts->NoValueCalls.count(S.Addr)))
      break;
    // A jump to the call, past what runs nowhere.
    if (S.Kind == StmtKind::Goto) {
      size_t Target = Call + 1;
      while (Target < List.size() && List[Target].Addr != S.GotoTarget)
        ++Target;
      if (Target == List.size())
        return;
      Call = Target - 1;
      continue;
    }
    if (!runsQuietlyBeforeFinally(S, Frame))
      return;
  }
  if (Call == List.size())
    return;
  size_t After = Call + 1;
  HighStmt &Normal = List[Call];
  if (Finally.Returns &&
      (Normal.Kind == StmtKind::Return || Normal.Kind == StmtKind::Assign)) {
    // What the body left in the register the call returns.
    ExprPtr &Value =
        Normal.Kind == StmtKind::Return ? Normal.RetVal : Normal.Val;
    const uint16_t Size = Value && Value->Type ? Value->Type->Size : 4;
    Value = Finally.Returned ? Finally.Returned : HighExpr::makeUndef(Size);
    After = Call;
  }
  // A body that never returns to its `call` runs nothing after it.
  if (!Finally.Returns)
    while (After < List.size() && !Labels.count(List[After].Addr))
      ++After;
  // What ran before the call runs at the end of the `__try`.  A jump that
  // landed among these statements is a `__leave`, as C says it from the try
  // itself, and lands past the clause from anywhere further in.
  std::vector<HighStmt> Before;
  std::vector<HighStmt> Landings;
  std::set<va_t> Leaves;
  for (size_t I = Index + 1; I < After; ++I) {
    HighStmt &S = List[I];
    if (Labels.count(S.Addr)) {
      Leaves.insert(S.Addr);
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = S.Addr;
      Landings.push_back(std::move(Anchor));
    }
    if (I < Call && (S.Kind == StmtKind::Assign || S.Kind == StmtKind::Store))
      Before.push_back(std::move(S));
  }
  List.erase(List.begin() + static_cast<ptrdiff_t>(Index) + 1,
             List.begin() + static_cast<ptrdiff_t>(After));
  List.insert(List.begin() + static_cast<ptrdiff_t>(Index) + 1,
              std::make_move_iterator(Landings.begin()),
              std::make_move_iterator(Landings.end()));
  std::vector<HighStmt> &Protected = List[Index].Body;
  // `__leave` runs what follows only if the path that jumps does it itself.
  std::function<void(std::vector<HighStmt> &)> MarkLeaves =
      [&](std::vector<HighStmt> &Stmts) {
        for (HighStmt &S : Stmts) {
          if (S.Kind == StmtKind::Goto && Leaves.count(S.GotoTarget) &&
              Before.empty())
            S.LeavesTry = true;
          if (S.Kind == StmtKind::SEHTry || S.Kind == StmtKind::CxxTry)
            continue;
          MarkLeaves(S.Body);
          MarkLeaves(S.ElseBody);
          for (SwitchCase &Case : S.Cases)
            MarkLeaves(Case.Body);
          MarkLeaves(S.DefaultBody);
        }
      };
  if (!Leaves.empty())
    MarkLeaves(Protected);
  Protected.insert(Protected.end(), std::make_move_iterator(Before.begin()),
                   std::make_move_iterator(Before.end()));
}

va_t RegistrationDisplay::ownerOf(va_t Address) const {
  auto After = std::upper_bound(
      Facts->Stretches.begin(), Facts->Stretches.end(), Address,
      [](va_t A, const RegistrationDisplayFacts::Stretch &S) {
        return A < S.Range.Begin;
      });
  if (After == Facts->Stretches.begin())
    return 0;
  --After;
  return After->Range.contains(Address) ? After->Owner : 0;
}

bool RegistrationDisplay::isFilterTemporary(int64_t Slot, va_t Root,
                                            bool OrExceptArms) const {
  const int64_t FromFrame = Slot - Facts->FrameFromEntry.value_or(-4);
  if (FromFrame != static_cast<int32_t>(FromFrame) ||
      Facts->FrameAddressesTaken.count(static_cast<int32_t>(FromFrame)))
    return false;
  const auto &Scopes = EH->Registration->Scopes;
  auto IsFilter = [&](va_t Owner) {
    return llvm::any_of(Scopes, [&](const RegistrationScopeRecord &Scope) {
      return !Scope.IsFinally && Scope.FilterVA == Owner;
    });
  };
  auto IsExceptArm = [&](va_t Owner) {
    return llvm::any_of(Scopes, [&](const RegistrationScopeRecord &Scope) {
      return !Scope.IsFinally && Scope.HandlerVA == Owner;
    });
  };
  if (const auto Reads =
          Facts->FrameReads.find(static_cast<int32_t>(FromFrame));
      Reads != Facts->FrameReads.end())
    for (const va_t Read : Reads->second) {
      const va_t Owner = ownerOf(Read);
      if (Owner != Root && !(OrExceptArms && Owner && IsExceptArm(Owner)))
        return false;
    }
  if (const auto Writes =
          Facts->FrameWrites.find(static_cast<int32_t>(FromFrame));
      Writes != Facts->FrameWrites.end())
    for (const va_t Write : Writes->second)
      if (!IsFilter(ownerOf(Write)))
        return false;
  return true;
}

int64_t RegistrationDisplay::exceptionPointersSlot() const {
  return registration_display::exceptionPointersSlot(*Facts, *EH->Registration);
}

void RegistrationDisplay::settleLists(std::vector<HighStmt> &List,
                                      const std::set<va_t> &Labels,
                                      const std::set<va_t> &Leaving,
                                      const std::set<VarKey> &Frame) const {
  for (size_t I = 0; I < List.size(); ++I) {
    if (const auto Settled = settleClause(List[I], Leaving))
      takeNormalPath(List, I, *Settled, Labels, Frame);
    HighStmt &S = List[I];
    settleLists(S.Body, Labels, Leaving, Frame);
    settleLists(S.ElseBody, Labels, Leaving, Frame);
    for (SwitchCase &Case : S.Cases)
      settleLists(Case.Body, Labels, Leaving, Frame);
    settleLists(S.DefaultBody, Labels, Leaving, Frame);
    for (std::vector<HighStmt> &Clause : S.EHClauseBodies)
      settleLists(Clause, Labels, Leaving, Frame);
  }
}

void RegistrationDisplay::dropBodyCalls(HighFunc &Func) const {
  if (Facts->Stretches.empty())
    return;
  // The body of each clause that holds one, what it left and whether it
  // returns.
  std::map<va_t, std::optional<std::vector<ExceptionAddressRange>>> Filled;
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    for (const HighStmt &S : List)
      if (S.Kind == StmtKind::SEHTry && S.EHClauses.size() == 1 &&
          S.EHClauses[0].Kind == HighEHClauseKind::SEHFinally &&
          S.EHClauses[0].FinallyKind == HighEHClause::FinallyBody::WholeBody)
        Filled.try_emplace(S.EHClauses[0].FilterOrActionVA);
  });
  if (Filled.empty())
    return;
  // The display facts stay fixed while calls are removed. Resolve each
  // queried body once; an engaged empty vector also remembers a missing body.
  // A clause with no remaining call needs no range scan.
  auto Holder = [&](va_t Target) -> std::optional<va_t> {
    for (auto &[Root, Ranges] : Filled) {
      if (!Ranges)
        Ranges = bodyAt(Root).value_or(std::vector<ExceptionAddressRange>{});
      for (const ExceptionAddressRange &R : *Ranges)
        if (R.contains(Target))
          return Root;
    }
    return std::nullopt;
  };
  // A `goto` out of a `__try` runs the clause, and lands on what ran the body
  // when structuring moved that code away from the end of the try.
  const std::set<va_t> Landings = gotoTargets(Func.Body);
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    for (size_t I = 0; I < List.size();) {
      HighStmt &S = List[I];
      const HighExpr *E = wholeCall(S);
      auto Root = E ? Holder(E->CallAddr) : std::nullopt;
      // The outlined body of a clause that is there, called from past its
      // scope.
      if (E && !Root) {
        const va_t Site = E->CallSiteAddr ? E->CallSiteAddr : S.Addr;
        if (const auto Outlined = Facts->OutlinedCalls.find(Site);
            Outlined != Facts->OutlinedCalls.end() &&
            Filled.count(Outlined->second) &&
            hasLeftScope(Site, Outlined->second))
          Root = Outlined->second;
      }
      if (!Root) {
        ++I;
        continue;
      }
      if (S.Kind == StmtKind::Return || S.Kind == StmtKind::Assign) {
        // What the body left in the register the call returns.
        ExprPtr &Value = S.Kind == StmtKind::Return ? S.RetVal : S.Val;
        const uint16_t Size = Value && Value->Type ? Value->Type->Size : 4;
        const auto Result = BodyResults.find(*Root);
        Value = Result != BodyResults.end() && Result->second
                    ? Result->second
                    : HighExpr::makeUndef(Size);
        ++I;
        continue;
      }
      if (Landings.count(S.Addr)) {
        const va_t Addr = S.Addr;
        S = HighStmt{};
        S.Kind = StmtKind::Block;
        S.Addr = Addr;
        ++I;
        continue;
      }
      List.erase(List.begin() + static_cast<ptrdiff_t>(I));
    }
  });
}

std::map<va_t, ExprPtr> RegistrationDisplay::takeFilters(HighFunc &Func) const {
  // A filter's code leaves the function's listing, whether it says a value.
  std::map<va_t, std::vector<HighStmt>> Bodies;
  for (const RegistrationScopeRecord &Scope : EH->Registration->Scopes)
    if (!Scope.IsFinally && Scope.FilterVA)
      Bodies.try_emplace(Scope.FilterVA);
  std::function<void(std::vector<HighStmt> &)> Take =
      [&](std::vector<HighStmt> &List) {
        std::vector<HighStmt> Kept;
        Kept.reserve(List.size());
        std::vector<HighStmt> *Following = nullptr;
        for (HighStmt &S : List) {
          if (hasLabelAddress(S)) {
            const auto Body = Bodies.find(ownerOf(S.Addr));
            Following = Body == Bodies.end() ? nullptr : &Body->second;
          }
          if (Following) {
            Following->push_back(std::move(S));
            continue;
          }
          Take(S.Body);
          Take(S.ElseBody);
          for (SwitchCase &Case : S.Cases)
            Take(Case.Body);
          Take(S.DefaultBody);
          Kept.push_back(std::move(S));
        }
        List = std::move(Kept);
      };
  Take(Func.Body);
  std::map<va_t, ExprPtr> Values;
  for (const auto &[Root, Body] : Bodies)
    if (!Body.empty())
      Values[Root] = filterValue(Body, Root);
  return Values;
}

void RegistrationDisplay::structure(HighFunc &Func) const {
  dropLocalUnwinds(Func);
  const std::map<va_t, ExprPtr> FilterValues = takeFilters(Func);
  std::set<va_t> Leaving;
  for (const MedBlock &Block : Med->Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::RETURN && !Facts->FinallyReturns.count(Op.Addr))
        Leaving.insert(Op.Addr);
  const std::set<VarKey> Frame = frameValues(*Med);
  const auto &Scopes = EH->Registration->Scopes;
  // C has no jump into a `__try` or a clause, and a scope's code can have one
  // where a loop and the scope overlap.  Such a scope prints as a block with
  // its handlers in a comment, and the rest settle again without it.
  const std::vector<HighStmt> Unsettled = seh_finally::cloneStmts(Func.Body);
  const std::function<bool(const HighStmt &)> NeverComesBack =
      [&](const HighStmt &S) {
        const HighExpr *Call = wholeCall(S);
        if (!Call)
          return false;
        const va_t Root = ownerOf(Call->CallAddr);
        const auto Scope = ScopeOfRoot.find(Root);
        if (Scope == ScopeOfRoot.end() || !Scopes[Scope->second].IsFinally)
          return false;
        const auto Ranges = bodyAt(Root);
        return Ranges &&
               llvm::none_of(*Ranges, [&](const ExceptionAddressRange &R) {
                 const auto It = Facts->FinallyReturns.lower_bound(R.Begin);
                 return It != Facts->FinallyReturns.end() && R.contains(*It);
               });
      };
  std::set<va_t> Blocks;
  for (size_t Round = 0; Round <= Scopes.size(); ++Round) {
    const RegistrationRegionFacts In{Scopes,          *Facts,       ScopeOfRoot,
                                     EH->ParseStatus, FilterValues, Blocks,
                                     NeverComesBack};
    Func.Body = placeRegistrationRegions(std::move(Func.Body), In);
    dropNaturalJumps(Func.Body);
    settleLists(Func.Body, gotoTargets(Func.Body), Leaving, Frame);
    dropBodyCalls(Func);
    dropNaturalJumps(Func.Body);
    threadJumps(Func.Body);
    dropNaturalJumps(Func.Body);
    std::optional<va_t> Refused = registrationTryEntered(Func.Body);
    if (!Refused)
      walkStmts(Func.Body, [&](const HighStmt &S) {
        // A `return` left in a `__try` of its body: C cannot say that.
        if (S.Kind == StmtKind::SEHTry && S.EHIsReducible &&
            S.EHClauses.front().Kind == HighEHClauseKind::SEHFinally &&
            !S.EHClauseBodies.front().empty() &&
            S.EHClauses.front().FinallyKind !=
                HighEHClause::FinallyBody::WholeBody)
          Refused = S.EHClauses.front().HandlerVA;
      });
    if (!Refused)
      break;
    Blocks.insert(*Refused);
    Func.Body = seh_finally::cloneStmts(Unsettled);
  }
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::SEHTry)
      ++(S.EHIsReducible ? Func.StructuredExceptionRegions
                         : Func.UnstructuredExceptionRegions);
  });
  nameExceptionReads(Func);
}

ExprPtr RegistrationDisplay::filterValue(const std::vector<HighStmt> &Body,
                                         va_t Root) const {
  std::set<int64_t> Codes;
  seh_filter::FilterFrame Frame;
  Frame.TargetArch = Arch::X86;
  Frame.Frames = &Frames;
  Frame.PointersSlot = exceptionPointersSlot();
  Frame.IsPrivate = [&](int64_t Slot, const HighExpr &Written) {
    if (isFilterTemporary(Slot, Root, false))
      return true;
    if (!callsNamed(&Written, kExceptionCode) ||
        !isFilterTemporary(Slot, Root, true))
      return false;
    Codes.insert(Slot);
    return true;
  };
  if (Facts->StackFromEntry)
    Frame.StackFloor = *Facts->StackFromEntry;
  ExprPtr Value = seh_filter::foldFilterBody(Body, Frame);
  if (Value)
    CodeSlots.insert(Codes.begin(), Codes.end());
  return Value;
}

void RegistrationDisplay::nameExceptionReads(HighFunc &Func) const {
  if (Frames.empty())
    return;
  const int64_t Slot = exceptionPointersSlot();
  // Only an `__except` arm can call these; what no clause holds keeps its
  // reads.
  std::vector<std::vector<HighStmt> *> Arms;
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    for (HighStmt &S : List)
      if (S.Kind == StmtKind::SEHTry && S.EHIsReducible)
        for (size_t K = 0; K < S.EHClauses.size(); ++K)
          if (S.EHClauses[K].Kind == HighEHClauseKind::SEHExcept &&
              K < S.EHClauseBodies.size())
            Arms.push_back(&S.EHClauseBodies[K]);
  });
  for (std::vector<HighStmt> *Arm : Arms)
    forEachList(*Arm, [&](std::vector<HighStmt> &List) {
      ExceptionReads Reads(Frames, Slot, Arch::X86, &CodeSlots);
      for (HighStmt &S : List) {
        if (S.Kind != StmtKind::Assign && S.Kind != StmtKind::Store &&
            S.Kind != StmtKind::Return && S.Kind != StmtKind::Call &&
            S.Kind != StmtKind::ExprStmt && S.Kind != StmtKind::If)
          continue;
        forEachExpr(S, [&](ExprPtr &E) {
          if (E && &E != &S.Dst)
            E = Reads.rewrite(E);
        });
        if (S.Kind == StmtKind::Assign && S.Dst && S.Val)
          Reads.noteAssign(*S.Dst, *S.Val);
      }
    });
}

void dropUnreadRegistrationStackWrites(HighFunc &Func,
                                       const RegistrationDisplayFacts &Facts,
                                       Arch Architecture) {
  if (!Facts.StackFromEntry || !Facts.FrameFromEntry)
    return;
  const int64_t Floor = *Facts.StackFromEntry;
  const int64_t Frame = *Facts.FrameFromEntry;
  size_t Budget = ::neverd::limits::ir::
      kHighDeadStoreElimElimUnreadPrivateFrameStoresBudget;
  // A variable defined once as a frame address is that address.  A long
  // function chains thousands of pushes, so each takes what the one before it
  // worked out.
  std::map<VarKey, unsigned> Definitions;
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      ++Definitions[varKey(S.Dst->Var)];
  });
  std::map<VarKey, int64_t> Addresses;
  const auto Known = [&](const MedVar &V, unsigned) -> std::optional<int64_t> {
    const auto It = Addresses.find(varKey(V));
    return It == Addresses.end() ? std::nullopt
                                 : std::optional<int64_t>(It->second);
  };
  const auto AddressOf = [&](const ExprPtr &E) {
    return high_detail::frameAddressOffsetFrom(E, Func, Architecture, Budget, 0,
                                               Known);
  };
  // Blocks can come in any order; what a second look adds is a join's.
  for (unsigned Pass = 0; Pass < kFrameAddressPasses; ++Pass)
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst ||
          S.Dst->Kind != ExprKind::Var || !S.Val ||
          Definitions[varKey(S.Dst->Var)] != 1)
        return;
      if (const auto Address = AddressOf(S.Val))
        Addresses[varKey(S.Dst->Var)] = *Address;
    });
  // What the function reads of its stack, how low a pointer into it goes, and
  // how many stack words a call takes.
  std::vector<std::pair<int64_t, int64_t>> Read;
  std::optional<int64_t> Lowest;
  bool FrameEscapes = false;
  size_t MostArguments = 0;
  std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Call)
      MostArguments = std::max(MostArguments, E->Operands.size());
    if (E->Kind == ExprKind::Load && !E->Operands.empty()) {
      if (const auto Word = AddressOf(E->Operands[0])) {
        const int64_t Size = E->Type ? E->Type->Size : sizeof(int32_t);
        Read.emplace_back(*Word, *Word + Size);
        return;
      }
    } else if (E->Kind != ExprKind::Const) {
      if (const auto Word = AddressOf(E)) {
        // A pointer reaches up from where it points, and the frame's own
        // reaches its locals as well, which an outlined body does.
        FrameEscapes |= *Word >= Frame;
        Lowest = Lowest ? std::min(*Lowest, *Word) : *Word;
        return;
      }
    }
    for (const ExprPtr &Operand : E->Operands)
      Visit(Operand);
  };
  const auto Written = [&](const HighStmt &S) -> std::optional<int64_t> {
    if (S.Kind != StmtKind::Store || !S.StoreAddr || !S.StoreVal ||
        S.MemoryOrdering != NdMemoryOrdering::None ||
        S.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return std::nullopt;
    return AddressOf(S.StoreAddr);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    // An address a variable holds is read where the variable is.
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
        Addresses.count(varKey(S.Dst->Var)))
      return;
    if (Written(S)) {
      Visit(S.StoreVal);
      return;
    }
    forEachExpr(S, [&](const ExprPtr &E) {
      if (&E != &S.Dst || E->Kind != ExprKind::Var)
        Visit(E);
    });
    for (const HighEHClause &Clause : S.EHClauses)
      Visit(Clause.FilterValue);
  });
  if (!Budget)
    return;
  const auto IsUnread = [&](int64_t Begin, int64_t End) {
    return llvm::none_of(Read, [&](const std::pair<int64_t, int64_t> &R) {
      return Begin < R.second && R.first < End;
    });
  };
  // clang-cl reserves the words its calls take at the bottom of the frame,
  // below anything a pointer to a local reaches.
  const int64_t Reserved =
      Facts.RecordFromFrame != kMSVCRecordFromFrame
          ? Floor + int64_t(MostArguments) * int64_t(sizeof(uint32_t))
          : Floor;
  const std::set<va_t> Targets = gotoTargets(Func.Body);
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    std::set<const HighStmt *> Dropped;
    for (size_t I = 0; I < List.size(); ++I) {
      const HighStmt &S = List[I];
      const auto Word = Written(S);
      if (!Word || hasCallIn(S.StoreVal) || !S.StoreVal->Type)
        continue;
      const int64_t Size = S.StoreVal->Type->Size;
      if (!IsUnread(*Word, *Word + Size))
        continue;
      if (*Word + Size > Reserved) {
        if (!FrameEscapes && (!Lowest || *Word + Size <= *Lowest))
          Dropped.insert(&S);
        continue;
      }
      // Below the frame's stack pointer nothing but a call reads, and its
      // arguments say what, as long as it says this one: a word past those a
      // callee's text takes is all that shows it takes more.
      int64_t Called = *Word;
      const HighExpr *Call = nullptr;
      for (size_t J = I + 1; J < List.size() && !Call; ++J) {
        const HighStmt &Next = List[J];
        if (const auto Other = Written(Next))
          Called = std::min(Called, *Other);
        else if (Next.Kind == StmtKind::Assign || Next.Kind == StmtKind::Nop ||
                 (Next.Kind == StmtKind::Block && Next.Body.empty()))
          continue;
        else if (!(Call = wholeCall(Next)))
          break;
      }
      if (!Call || (*Word - Called) / int64_t(sizeof(uint32_t)) <
                       int64_t(Call->Operands.size()))
        Dropped.insert(&S);
    }
    if (!Dropped.empty())
      eraseKeepingBranchEntries(
          List, Targets, [&](const HighStmt &S) { return Dropped.count(&S); });
  });
}

void finishRegistrationDisplay(HighFunc &Func, const MedFunc &Med,
                               Arch Architecture) {
  if (!Med.RegistrationStates || !Med.RegistrationStates->Display)
    return;
  // A phi's copy nothing reads stands between a jump and where it lands.
  std::set<VarKey> Read;
  std::function<void(const ExprPtr &)> Note = [&](const ExprPtr &E) {
    if (!E)
      return;
    if (E->Kind == ExprKind::Var)
      Read.insert(varKey(E->Var));
    for (const ExprPtr &Operand : E->Operands)
      Note(Operand);
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    forEachExpr(S, [&](const ExprPtr &E) {
      if (&E != &S.Dst || E->Kind != ExprKind::Var)
        Note(E);
    });
    for (const HighEHClause &Clause : S.EHClauses)
      Note(Clause.FilterValue);
  });
  const std::set<va_t> Targets = gotoTargets(Func.Body);
  forEachList(Func.Body, [&](std::vector<HighStmt> &List) {
    eraseKeepingBranchEntries(List, Targets, [&](const HighStmt &S) {
      return S.IsPhiCopy && S.Kind == StmtKind::Assign && S.Dst &&
             S.Dst->Kind == ExprKind::Var && !Read.count(varKey(S.Dst->Var)) &&
             !hasCallIn(S.Val);
    });
  });
  threadJumps(Func.Body);
  dropNaturalJumps(Func.Body);
  dropUnreadRegistrationStackWrites(Func, *Med.RegistrationStates->Display,
                                    Architecture);
  keepRegistrationLabelsInReach(Func.Body);
}

} // namespace neverd
