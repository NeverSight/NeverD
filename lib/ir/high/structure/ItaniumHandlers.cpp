//===- ItaniumHandlers.cpp - Catch bodies from Itanium landing pads ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A landing pad is ordinary code the unwinder enters with the exception
/// object and a selector, the type filter of the clause the personality
/// matched.  The pad runs the destructors of the region's objects, tests the
/// selector and enters the handler of that clause.  Each clause body here is
/// the pad's code as it runs for one selector: tests of the selector are
/// decided, code shared by several clauses is copied into each, and code the
/// function's own flow also runs is left by a jump, as a handler ends by
/// leaving its try.  Anything this cannot follow exactly — a loop or a
/// further region inside the pad, a selector test it cannot decide, a
/// handler that does not begin by taking the exception — leaves the region
/// as it was: its pad in place and its clauses without bodies.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/ItaniumHandlers.h"

#include "../pass/HighFlowGraph.h"

#include "neverd/Common.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/ADT/STLExtras.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

bool isItaniumRuntimeCall(const HighExpr &Call, llvm::StringRef Routine) {
  return Call.Kind == ExprKind::Call && !Call.IsIndirectCall &&
         !Call.CallTarget.empty() &&
         stripLeadingUnderscores(Call.CallTarget) ==
             stripLeadingUnderscores(Routine);
}

namespace {

/// NEVERD_DEBUG_ITANIUM_HANDLERS names why a region keeps its pad.
void note(const char *Why, va_t Where) {
  if (std::getenv("NEVERD_DEBUG_ITANIUM_HANDLERS"))
    std::fprintf(stderr, "itanium-handlers 0x%llx: %s\n",
                 static_cast<unsigned long long>(Where), Why);
}

/// Statements a pad may run before this gives up on it: a pad is a few
/// blocks, and copying it once per clause stays well within this.
constexpr unsigned kMaxPadStatements = 4096;

using Position = std::pair<const std::vector<HighStmt> *, size_t>;

void indexPositions(const std::vector<HighStmt> &L,
                    std::map<const HighStmt *, Position> &Out) {
  for (size_t I = 0; I < L.size(); ++I) {
    const HighStmt &S = L[I];
    Out[&S] = {&L, I};
    indexPositions(S.Body, Out);
    indexPositions(S.ElseBody, Out);
    for (const SwitchCase &C : S.Cases)
      indexPositions(C.Body, Out);
    indexPositions(S.DefaultBody, Out);
    for (const auto &Clause : S.EHClauseBodies)
      indexPositions(Clause, Out);
  }
}

const ExprPtr *callOf(const HighStmt &S) {
  const ExprPtr *E = nullptr;
  if (S.Kind == StmtKind::Call)
    E = &S.CallExpr;
  else if (S.Kind == StmtKind::Assign || S.Kind == StmtKind::ExprStmt)
    E = &S.Val;
  return E && *E && (*E)->Kind == ExprKind::Call ? E : nullptr;
}

bool endsPath(const HighStmt &S) {
  const ExprPtr *Call = callOf(S);
  if (!Call)
    return false;
  const std::string &Callee = (*Call)->CallTarget;
  return isTerminatingHighCall(*Call) || (*Call)->DoesNotReturn ||
         (!Callee.empty() && (*Call)->IntrinsicId == Intrinsic::None &&
          libc::isNoReturnFunction(Callee));
}

/// \p E with integer views peeled: the selector is compared through them.
const HighExpr *peelViews(const HighExpr *E) {
  for (unsigned Depth = 0; E && Depth < 8; ++Depth) {
    const bool View = !E->Operands.empty() && E->Operands[0] &&
                      (E->Kind == ExprKind::Cast ||
                       (E->Kind == ExprKind::UnaryOp &&
                        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) ||
                       (E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
                        E->Operands.size() == 2 && E->Operands[1] &&
                        E->Operands[1]->Kind == ExprKind::Const &&
                        E->Operands[1]->ConstVal == 0));
    if (!View)
      return E;
    E = E->Operands[0].get();
  }
  return E;
}

bool isVariable(const HighExpr *E) {
  return E && (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi);
}

/// \p E as a base and a constant displacement from it.
std::pair<const HighExpr *, int64_t> baseAndOffset(const HighExpr *E) {
  int64_t Offset = 0;
  for (unsigned Depth = 0; E && Depth < 8; ++Depth) {
    E = peelViews(E);
    if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
        (E->Op != NdOp::INT_ADD && E->Op != NdOp::INT_SUB))
      break;
    const HighExpr *Right = peelViews(E->Operands[1].get());
    if (!Right || Right->Kind != ExprKind::Const)
      break;
    // A narrow displacement is its own width's two's complement.
    const uint16_t Bytes = Right->Type ? Right->Type->Size : 8;
    int64_t Delta = static_cast<int64_t>(Right->ConstVal);
    if (Bytes == 4)
      Delta = static_cast<int32_t>(Right->ConstVal);
    else if (Bytes == 2)
      Delta = static_cast<int16_t>(Right->ConstVal);
    else if (Bytes == 1)
      Delta = static_cast<int8_t>(Right->ConstVal);
    Offset += E->Op == NdOp::INT_SUB ? -Delta : Delta;
    E = E->Operands[0].get();
  }
  return {E, Offset};
}

/// Whether bytes [\p A, +\p ASize) and [\p B, +\p BSize) are certainly
/// apart: the same base at displacements that do not overlap.
bool apart(const HighExpr &A, uint16_t ASize, const HighExpr &B,
           uint16_t BSize) {
  const auto [BaseA, OffA] = baseAndOffset(&A);
  const auto [BaseB, OffB] = baseAndOffset(&B);
  if (!BaseA || !BaseB || !BaseA->structuralEq(*BaseB))
    return false;
  return OffA + static_cast<int64_t>(ASize) <= OffB ||
         OffB + static_cast<int64_t>(BSize) <= OffA;
}

/// The address and value of the memory \p S writes, if it writes memory.
std::optional<std::pair<const HighExpr *, const HighExpr *>>
memoryWrite(const HighStmt &S) {
  if (S.Kind == StmtKind::Store && S.StoreAddr && S.StoreVal)
    return std::make_pair(S.StoreAddr.get(), S.StoreVal.get());
  if (S.Kind == StmtKind::Assign && S.Dst && S.Val &&
      S.Dst->Kind == ExprKind::Load && !S.Dst->Operands.empty() &&
      S.Dst->Operands[0])
    return std::make_pair(S.Dst->Operands[0].get(), S.Val.get());
  return std::nullopt;
}

uint16_t bytesOf(const HighExpr *E) { return E && E->Type ? E->Type->Size : 0; }

/// Frame slots a run stored one value in, as long as they certainly hold it.
struct SlotCopies {
  std::vector<std::pair<const HighExpr *, uint16_t>> Slots;

  bool holds(const HighExpr *E) const {
    E = peelViews(E);
    if (!E || E->Kind != ExprKind::Load || E->Operands.empty() ||
        !E->Operands[0])
      return false;
    return llvm::any_of(Slots, [&](const auto &Slot) {
      return Slot.second == bytesOf(E) &&
             Slot.first->structuralEq(*E->Operands[0]);
    });
  }
  /// \p S writes memory at \p Address: keep a slot it certainly misses.
  void write(const HighExpr &Address, uint16_t Bytes) {
    llvm::erase_if(Slots, [&](const auto &Slot) {
      return !apart(*Slot.first, Slot.second, Address, Bytes);
    });
  }
};

/// Whether \p S is a region around nothing but `__cxa_end_catch`.
bool endsHandlerOnly(const HighStmt &S) {
  if (S.Kind != StmtKind::ItaniumTry)
    return false;
  bool Ends = false;
  for (const HighStmt &Inner : S.Body) {
    if (Inner.Kind == StmtKind::Nop ||
        (Inner.Kind == StmtKind::Block && Inner.Body.empty()))
      continue;
    const ExprPtr *Call = callOf(Inner);
    if (!Call || !isItaniumRuntimeCall(**Call, "__cxa_end_catch") ||
        (Inner.Kind == StmtKind::Assign && Inner.Dst &&
         !isVariable(Inner.Dst.get())))
      return false;
    Ends = true;
  }
  return Ends;
}

/// One clause's run through a landing pad.
class PadRun {
public:
  PadRun(const HighFlowGraph &G, const std::set<size_t> &Normal,
         const std::map<const HighStmt *, Position> &Positions,
         const std::vector<HighStmt> &TryBody, bool VoidFunction,
         int64_t Selector)
      : G(G), Normal(Normal), Positions(Positions), TryBody(TryBody),
        VoidFunction(VoidFunction), Selector(Selector) {}

  /// The statements the pad entered at \p Pad runs for this selector.
  std::optional<std::vector<HighStmt>> run(size_t Pad) {
    std::vector<HighStmt> Out;
    Top = &Out;
    follow(Pad, Out);
    Top = nullptr;
    if (Failed)
      return std::nullopt;
    return Out;
  }

  /// Where the run decided its first selector test: the statements of the
  /// run before that index are the pad's own, whatever the selector, and the
  /// test is \p Decision.
  size_t DispatchAt = std::numeric_limits<size_t>::max();
  const HighStmt *Decision = nullptr;
  /// The run's statements at top level that only copy the selector: its
  /// tests are decided, so nothing reads them.
  std::set<size_t> SelectorWrites;

  /// The pad statements the run copied.
  std::set<const HighStmt *> Copied;
  /// The frame slots the pad stored the selector in.
  std::vector<const HighExpr *> SelectorSpills;
  /// The pads of the regions around `__cxa_end_catch` the run dropped.
  std::set<va_t> DroppedPads;

private:
  const HighFlowGraph &G;
  const std::set<size_t> &Normal;
  const std::map<const HighStmt *, Position> &Positions;
  const std::vector<HighStmt> &TryBody;
  const bool VoidFunction;
  const int64_t Selector;
  /// Variables and frame slots holding the selector on this run.
  std::vector<MedVar> SelectorCopies;
  SlotCopies SelectorSlots;
  std::set<size_t> Entered;
  std::vector<HighStmt> *Top = nullptr;
  unsigned Steps = 0;
  bool Failed = false;

public:
  /// Why the run failed.
  const char *Reason = nullptr;

private:
  void fail(const char *Why) {
    if (!Failed)
      Reason = Why;
    Failed = true;
  }

  bool isSelector(const HighExpr *E) const {
    if (SelectorSlots.holds(E))
      return true;
    E = peelViews(E);
    if (!isVariable(E))
      return false;
    return E->Var.Kind == MedVar::EHSelector ||
           llvm::is_contained(SelectorCopies, E->Var);
  }

  /// Whether \p S stores the selector to memory.
  bool SpillsSelector(const HighStmt &S) const {
    const auto Write = memoryWrite(S);
    return Write && isSelector(Write->second);
  }

  /// Track what \p S does to the selector's copies, before it is copied.
  void track(const HighStmt &S) {
    if (const auto Write = memoryWrite(S)) {
      const uint16_t Bytes = bytesOf(Write->second);
      const bool Selector = isSelector(Write->second);
      SelectorSlots.write(*Write->first, Bytes);
      if (Selector) {
        SelectorSlots.Slots.push_back({Write->first, Bytes});
        SelectorSpills.push_back(Write->first);
      }
    }
    // A slot the pad spilled the selector to is the compiler's own
    // temporary: no callee is handed its address, and C++ gives none a way
    // to reach it from another object's, so only the pad's stores move it.
  }

  /// The value of \p E on this run when it depends on nothing but constants
  /// and the selector.
  std::optional<int64_t> evaluate(const HighExpr *E, unsigned Depth = 0) {
    if (!E || Depth > 16)
      return std::nullopt;
    if (isSelector(E))
      return Selector;
    E = peelViews(E);
    if (E->Kind == ExprKind::Const) {
      const uint16_t Bytes = E->Type ? E->Type->Size : 8;
      if (Bytes >= 8)
        return static_cast<int64_t>(E->ConstVal);
      const uint64_t Mask = (uint64_t{1} << (Bytes * 8)) - 1;
      const uint64_t Sign = uint64_t{1} << (Bytes * 8 - 1);
      const uint64_t V = E->ConstVal & Mask;
      return static_cast<int64_t>((V ^ Sign) - Sign);
    }
    if (E->Kind == ExprKind::UnaryOp && E->Operands.size() == 1 &&
        E->Op == NdOp::BOOL_NOT) {
      const auto V = evaluate(E->Operands[0].get(), Depth + 1);
      return V ? std::optional<int64_t>(*V == 0) : std::nullopt;
    }
    if (E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
      return std::nullopt;
    const auto L = evaluate(E->Operands[0].get(), Depth + 1);
    const auto R = evaluate(E->Operands[1].get(), Depth + 1);
    if (!L || !R)
      return std::nullopt;
    // The selector is a small positive filter, so the signed and unsigned
    // orders agree on every pair this meets.
    switch (E->Op) {
    case NdOp::INT_EQUAL:
      return *L == *R;
    case NdOp::INT_NOTEQUAL:
      return *L != *R;
    case NdOp::INT_LESS:
      return static_cast<uint64_t>(*L) < static_cast<uint64_t>(*R);
    case NdOp::INT_SLESS:
      return *L < *R;
    case NdOp::INT_LESSEQUAL:
      return static_cast<uint64_t>(*L) <= static_cast<uint64_t>(*R);
    case NdOp::INT_SLESSEQUAL:
      return *L <= *R;
    case NdOp::BOOL_AND:
      return (*L != 0) && (*R != 0);
    case NdOp::BOOL_OR:
      return (*L != 0) || (*R != 0);
    case NdOp::BOOL_XOR:
      return (*L != 0) != (*R != 0);
    default:
      return std::nullopt;
    }
  }

  /// \p E with the selector's copies spelled as the selector's value.
  ExprPtr rewrite(const ExprPtr &E, unsigned Depth = 0) {
    if (!E || Depth > 64)
      return E;
    if ((isVariable(E.get()) || E->Kind == ExprKind::Load) &&
        isSelector(E.get())) {
      const uint16_t Bytes = E->Type ? E->Type->Size : 4;
      auto Value = HighExpr::makeConst(static_cast<uint64_t>(Selector), Bytes,
                                       ConstantAddressProvenance::Scalar);
      Value->Type = E->Type;
      return Value;
    }
    bool Changed = false;
    std::vector<ExprPtr> Operands;
    Operands.reserve(E->Operands.size());
    for (const ExprPtr &Operand : E->Operands) {
      Operands.push_back(rewrite(Operand, Depth + 1));
      Changed |= Operands.back() != Operand;
    }
    if (!Changed)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    Copy->Operands = std::move(Operands);
    // A test the run decided reads as its truth value.
    if (Copy->Kind == ExprKind::BinOp || Copy->Kind == ExprKind::UnaryOp)
      if (const auto Decided = evaluate(Copy.get());
          Decided && Copy->Op != NdOp::INT_ADD && Copy->Op != NdOp::INT_SUB) {
        auto Value = HighExpr::makeConst(static_cast<uint64_t>(*Decided),
                                         Copy->Type ? Copy->Type->Size : 1,
                                         ConstantAddressProvenance::Scalar);
        Value->Type = Copy->Type;
        return Value;
      }
    return Copy;
  }

  bool insideTry(const HighStmt &Target) const {
    bool Inside = false;
    walkStmts(TryBody, [&](const HighStmt &S) { Inside |= &S == &Target; });
    return Inside;
  }

  /// Leave the handler for \p Target, code the function's own flow runs: a
  /// jump to its label.  C++ allows a jump out of a handler but not into a
  /// try, so a target in the region's own body cannot be reached this way.
  bool leaveTo(size_t Node, std::vector<HighStmt> &Out) {
    // A loop that never ends its own test runs its body when entered: the
    // jump lands on the body's first statement.
    if (const HighStmt *Loop = G.Nodes[Node].Statement;
        Loop && Loop->Kind == StmtKind::While &&
        G.Nodes[Node].Successors.size() == 1)
      Node = G.Nodes[Node].Successors.front();
    const HighStmt *Target = G.Nodes[Node].Statement;
    if (!Target || !Target->Addr || Target->Addr == InvalidVA)
      return false;
    auto Label = G.LabelAt.find(Target->Addr);
    if (Label == G.LabelAt.end() || Label->second != Node || insideTry(*Target))
      return false;
    HighStmt Jump;
    Jump.Kind = StmtKind::Goto;
    Jump.GotoTarget = Target->Addr;
    Out.push_back(std::move(Jump));
    return true;
  }

  /// Run on from \p Node; true once the run has left.
  bool follow(size_t Node, std::vector<HighStmt> &Out) {
    if (Node == 0) {
      // Running off the function's end returns from a void function.
      if (!VoidFunction) {
        fail("pad runs off a function that returns a value");
        return true;
      }
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Out.push_back(std::move(Return));
      return true;
    }
    if (Normal.count(Node)) {
      if (!leaveTo(Node, Out))
        fail("pad joins ordinary code no label or try allows a jump to");
      return true;
    }
    // A pad that comes back to code it ran already loops, which a handler
    // copied clause by clause cannot keep.
    if (!Entered.insert(Node).second) {
      fail("pad loops");
      return true;
    }
    auto At = Positions.find(G.Nodes[Node].Statement);
    if (At == Positions.end()) {
      fail("pad statement has no position");
      return true;
    }
    return copyFrom(*At->second.first, At->second.second, Out,
                    /*Structured=*/false);
  }

  /// Copy the statements of \p L from \p I on into \p Out.  True once the
  /// run has left; false when it runs off the end of a list whose owner
  /// continues it (\p Structured).
  bool copyFrom(const std::vector<HighStmt> &L, size_t I,
                std::vector<HighStmt> &Out, bool Structured) {
    for (size_t J = I; J < L.size(); ++J) {
      const HighStmt &S = L[J];
      if (++Steps > kMaxPadStatements) {
        fail("pad too long");
        return true;
      }
      auto NodeIt = G.NodeOf.find(&S);
      if (NodeIt == G.NodeOf.end()) {
        fail("statement outside the flow graph");
        return true;
      }
      const bool Shared = Normal.count(NodeIt->second) != 0;
      if (J != I && Shared) {
        if (!leaveTo(NodeIt->second, Out))
          fail("pad runs into ordinary code no label or try allows a jump to");
        return true;
      }
      // Only code the unwinder alone enters leaves its place.
      if (!Shared)
        Copied.insert(&S);
      switch (S.Kind) {
      case StmtKind::Nop:
        continue;
      case StmtKind::Block:
        if (S.Body.empty())
          continue;
        if (copyFrom(S.Body, 0, Out, /*Structured=*/true))
          return true;
        continue;
      case StmtKind::Assign: {
        HighStmt C = S;
        C.Addr = 0;
        // The value is spelled before the write changes what it reads.
        C.Val = rewrite(S.Val);
        if (S.Dst && !isVariable(S.Dst.get()))
          C.Dst = rewrite(S.Dst);
        const bool ToVariable = S.Dst && isVariable(S.Dst.get());
        const bool CopiesSelector = ToVariable && isSelector(S.Val.get());
        const bool Spill = SpillsSelector(S);
        track(S);
        if (ToVariable)
          llvm::erase(SelectorCopies, S.Dst->Var);
        // The pad's copy of the selector holds this clause's filter.
        if (CopiesSelector)
          SelectorCopies.push_back(S.Dst->Var);
        if (&Out == Top && (CopiesSelector || Spill))
          SelectorWrites.insert(Out.size());
        Out.push_back(std::move(C));
        if (endsPath(S))
          return true;
        continue;
      }
      case StmtKind::Store:
      case StmtKind::Call:
      case StmtKind::ExprStmt: {
        HighStmt C = S;
        C.Addr = 0;
        C.StoreAddr = rewrite(S.StoreAddr);
        C.StoreVal = rewrite(S.StoreVal);
        C.CallExpr = rewrite(S.CallExpr);
        C.Val = rewrite(S.Val);
        const bool Spill = SpillsSelector(S);
        track(S);
        if (&Out == Top && Spill)
          SelectorWrites.insert(Out.size());
        Out.push_back(std::move(C));
        if (endsPath(S))
          return true;
        continue;
      }
      case StmtKind::Return: {
        HighStmt C = S;
        C.Addr = 0;
        C.RetVal = rewrite(S.RetVal);
        Out.push_back(std::move(C));
        return true;
      }
      case StmtKind::If:
      case StmtKind::IfElse: {
        if (const auto Taken = evaluate(S.Cond.get())) {
          if (!Decision) {
            Decision = &S;
            DispatchAt =
                &Out == Top ? Out.size() : std::numeric_limits<size_t>::max();
          }
          const std::vector<HighStmt> &Arm = *Taken ? S.Body : S.ElseBody;
          if (copyFrom(Arm, 0, Out, /*Structured=*/true))
            return true;
          continue;
        }
        HighStmt C = S;
        C.Addr = 0;
        C.Cond = rewrite(S.Cond);
        C.Body.clear();
        C.ElseBody.clear();
        const auto Saved = SelectorCopies;
        const auto SavedSlots = SelectorSlots;
        const bool ThenLeaves = copyFrom(S.Body, 0, C.Body, true);
        const auto ThenCopies = SelectorCopies;
        const auto ThenSlots = SelectorSlots;
        SelectorCopies = Saved;
        SelectorSlots = SavedSlots;
        const bool ElseLeaves = S.Kind == StmtKind::IfElse &&
                                copyFrom(S.ElseBody, 0, C.ElseBody, true);
        // After the arms a copy holds the selector only if both left it so.
        llvm::erase_if(SelectorCopies, [&](const MedVar &V) {
          return !llvm::is_contained(ThenCopies, V);
        });
        llvm::erase_if(SelectorSlots.Slots, [&](const auto &Slot) {
          return llvm::none_of(ThenSlots.Slots, [&](const auto &Other) {
            return Other.first == Slot.first;
          });
        });
        Out.push_back(std::move(C));
        if (Failed || (ThenLeaves && ElseLeaves))
          return true;
        continue;
      }
      case StmtKind::Goto: {
        auto Target = G.LabelAt.find(S.GotoTarget);
        if (Target == G.LabelAt.end()) {
          fail("jump to an unknown label");
          return true;
        }
        return follow(Target->second, Out);
      }
      case StmtKind::ItaniumTry:
        // The region a compiler wraps around `__cxa_end_catch` alone, whose
        // handler terminates or reaches an enclosing try, is the implicit end
        // of a C++ handler: the call stands for it.
        if (endsHandlerOnly(S)) {
          for (const HighStmt &Inner : S.Body)
            if (Inner.Kind != StmtKind::Nop &&
                !(Inner.Kind == StmtKind::Block && Inner.Body.empty())) {
              HighStmt C = Inner;
              C.Addr = 0;
              Out.push_back(std::move(C));
            }
          for (const HighEHClause &Clause : S.EHClauses)
            DroppedPads.insert(Clause.LandingPadVAs.begin(),
                               Clause.LandingPadVAs.end());
          continue;
        }
        [[fallthrough]];
      default:
        // Loops, switches, jumps out of loops and further regions are not
        // followed: their structure is not one a copy may duplicate.
        fail(S.Kind == StmtKind::ItaniumTry ? "pad holds a region"
             : S.Kind == StmtKind::While || S.Kind == StmtKind::DoWhile ||
                     S.Kind == StmtKind::For
                 ? "pad holds a loop"
             : S.Kind == StmtKind::Switch ? "pad holds a switch"
                                          : "pad holds a break or continue");
        return true;
      }
    }
    if (Structured)
      return false;
    auto Next = G.ListNext.find(&L);
    if (Next == G.ListNext.end()) {
      fail("list without a continuation");
      return true;
    }
    return follow(Next->second, Out);
  }
};

/// The call \p S makes to the runtime routine \p Routine, if any.
const HighExpr *runtimeCall(const HighStmt &S, llvm::StringRef Routine) {
  const ExprPtr *Call = callOf(S);
  return Call && isItaniumRuntimeCall(**Call, Routine) ? Call->get() : nullptr;
}

bool readsVariable(const HighExpr *E, const MedVar &V, unsigned Depth = 0) {
  if (!E || Depth > 64)
    return false;
  if (isVariable(E) && E->Var == V)
    return true;
  bool Reads = false;
  E->forEachChildExpr([&](const ExprPtr &Child) {
    Reads = Reads || readsVariable(Child.get(), V, Depth + 1);
  });
  return Reads;
}

bool readsExceptionState(const HighExpr *E, unsigned Depth = 0) {
  if (!E || Depth > 64)
    return false;
  if (isVariable(E) &&
      (E->Var.Kind == MedVar::EHException || E->Var.Kind == MedVar::EHSelector))
    return true;
  bool Reads = false;
  E->forEachChildExpr([&](const ExprPtr &Child) {
    Reads = Reads || readsExceptionState(Child.get(), Depth + 1);
  });
  return Reads;
}

void forEachStatementExpr(const HighStmt &S,
                          llvm::function_ref<void(const HighExpr *)> Fn) {
  for (const ExprPtr *E : {&S.Cond, &S.Dst, &S.Val, &S.CallExpr, &S.RetVal,
                           &S.StoreAddr, &S.StoreVal, &S.SwitchExpr})
    if (*E)
      Fn(E->get());
}

/// The pad's copies of the exception object in a run's statements: the
/// variables and frame slots it passes through on the way to the call that
/// takes it or resumes the unwind.
struct ExceptionTrack {
  std::vector<MedVar> Copies;
  SlotCopies Slots;
  std::vector<const HighExpr *> SpilledTo;
  /// Variables holding the address of a slot the object was spilled to, as
  /// clang computes one in a frame too large for an immediate offset.
  std::vector<std::pair<MedVar, const HighExpr *>> SlotAddresses;

  bool isException(const HighExpr *E) const {
    if (Slots.holds(E))
      return true;
    E = peelViews(E);
    if (E && E->Kind == ExprKind::Load && !E->Operands.empty() &&
        isVariable(E->Operands[0].get()))
      for (const auto &[Var, Address] : SlotAddresses)
        if (Var == E->Operands[0]->Var &&
            llvm::any_of(Slots.Slots, [&](const auto &Slot) {
              return Slot.second == bytesOf(E) &&
                     Slot.first->structuralEq(*Address);
            }))
          return true;
    return isVariable(E) && (E->Var.Kind == MedVar::EHException ||
                             llvm::is_contained(Copies, E->Var));
  }

  /// Follow \p Body's statements before \p End; which only copy the object.
  std::vector<bool> scan(const std::vector<HighStmt> &Body, size_t End) {
    std::vector<bool> Copying(Body.size(), false);
    for (size_t I = 0; I < End && I < Body.size(); ++I) {
      const HighStmt &S = Body[I];
      if (const auto Write = memoryWrite(S)) {
        const bool Spill = isException(Write->second);
        Slots.write(*Write->first, bytesOf(Write->second));
        if (Spill) {
          Slots.Slots.push_back({Write->first, bytesOf(Write->second)});
          SpilledTo.push_back(Write->first);
          Copying[I] = true;
        }
        continue;
      }
      // The spill slots are the compiler's temporaries, as for the selector.
      if (S.Kind == StmtKind::Assign && S.Dst && isVariable(S.Dst.get())) {
        const MedVar Var = S.Dst->Var;
        llvm::erase(Copies, Var);
        llvm::erase_if(SlotAddresses,
                       [&](const auto &Known) { return Known.first == Var; });
        if (isException(S.Val.get())) {
          Copies.push_back(Var);
          Copying[I] = true;
        } else if (S.Val) {
          for (const auto &Slot : Slots.Slots)
            if (Slot.first->structuralEq(*S.Val)) {
              SlotAddresses.push_back({Var, Slot.first});
              Copying[I] = true;
              break;
            }
        }
      }
    }
    return Copying;
  }

  /// Whether \p Stmts still read the unwinder's state, the object's copies
  /// or the slots it was spilled to.  The compiler spills to a slot of its
  /// own frame, which it names by its frame base only: a read at another
  /// base is the code's own data.
  bool readBy(const std::vector<HighStmt> &Stmts) const {
    std::function<bool(const HighExpr *, unsigned)> ReadsSpill =
        [&](const HighExpr *E, unsigned Depth) {
          if (!E || Depth > 64)
            return false;
          if (E->Kind == ExprKind::Load && !E->Operands.empty() &&
              E->Operands[0] &&
              llvm::any_of(SpilledTo, [&](const HighExpr *Slot) {
                const HighExpr *SlotBase = baseAndOffset(Slot).first;
                const HighExpr *ReadBase =
                    baseAndOffset(E->Operands[0].get()).first;
                return SlotBase && ReadBase &&
                       SlotBase->structuralEq(*ReadBase) &&
                       !apart(*Slot, 8, *E->Operands[0], bytesOf(E));
              }))
            return true;
          bool Found = false;
          E->forEachChildExpr([&](const ExprPtr &C) {
            Found = Found || ReadsSpill(C.get(), Depth + 1);
          });
          return Found;
        };
    bool Reads = false;
    walkStmts(Stmts, [&](const HighStmt &S) {
      forEachStatementExpr(S, [&](const HighExpr *E) {
        if (Reads)
          return;
        Reads = readsExceptionState(E) ||
                llvm::any_of(Copies,
                             [&](const MedVar &Copy) {
                               return readsVariable(E, Copy);
                             }) ||
                llvm::any_of(SlotAddresses,
                             [&](const auto &Known) {
                               return readsVariable(E, Known.first);
                             }) ||
                (E != S.Dst.get() && ReadsSpill(E, 0));
      });
    });
    return Reads;
  }
};

/// Drop the statements of \p Body at \p Indices; \p End, an index into it,
/// moves with the statements before it.
void dropAt(std::vector<HighStmt> &Body, const std::set<size_t> &Indices,
            size_t &End) {
  std::vector<HighStmt> Kept;
  size_t NewEnd = 0;
  for (size_t I = 0; I < Body.size(); ++I) {
    if (Indices.count(I))
      continue;
    if (I < End)
      ++NewEnd;
    Kept.push_back(std::move(Body[I]));
  }
  if (End <= Body.size())
    End = NewEnd;
  Body = std::move(Kept);
}

/// Spell \p Body, one clause's run, as a handler: the exception it takes is
/// the clause's parameter and the C++ catch ends it.  The pad's copies of
/// the exception object go with the call that took it, and its copies of
/// the selector are dead once its tests are decided.  With \p Prefix, the
/// statements before \p PrefixEnd, which the pad runs whatever the selector,
/// move there instead of heading the handler.
const char *makeHandler(std::vector<HighStmt> &Body, bool CatchAll,
                        const std::set<size_t> &SelectorWrites,
                        size_t PrefixEnd = 0,
                        std::vector<HighStmt> *Prefix = nullptr) {
  dropAt(Body, SelectorWrites, PrefixEnd);
  if (!Prefix)
    PrefixEnd = 0;
  // The handler begins where the run takes the exception.  What runs before
  // that, the destructors of the region's objects, stays at its head.
  size_t Begin = Body.size();
  for (size_t I = 0; I < Body.size(); ++I) {
    if (runtimeCall(Body[I], "__cxa_begin_catch")) {
      Begin = I;
      break;
    }
    // clang's `__clang_call_terminate(exception)` takes the exception and
    // calls std::terminate: a catch-all whose handler terminates, which is
    // what `noexcept` compiles to.
    if (CatchAll && runtimeCall(Body[I], "__clang_call_terminate") &&
        runtimeCall(Body[I], "__clang_call_terminate")->Operands.size() == 1) {
      if (I < PrefixEnd)
        return "the pad terminates before it tests the selector";
      std::vector<HighStmt> Handler;
      for (size_t K = 0; K < I; ++K) {
        const HighStmt &S = Body[K];
        // The pad's copies of the exception went to the call.
        if (S.Kind == StmtKind::Assign && S.Dst && isVariable(S.Dst.get()) &&
            readsExceptionState(S.Val.get()))
          continue;
        if (const auto Write = memoryWrite(S);
            Write && readsExceptionState(Write->second))
          continue;
        (K < PrefixEnd ? *Prefix : Handler).push_back(S);
      }
      HighStmt Terminate;
      Terminate.Kind = StmtKind::Call;
      Terminate.CallExpr = HighExpr::makeCall("_ZSt9terminatev", 0, {});
      Terminate.CallExpr->Type = NdType::makeVoid();
      Terminate.CallExpr->DoesNotReturn = true;
      Handler.push_back(std::move(Terminate));
      bool Reads = false;
      walkStmts(Handler, [&](const HighStmt &S) {
        forEachStatementExpr(
            S, [&](const HighExpr *E) { Reads |= readsExceptionState(E); });
      });
      if (Reads)
        return "the terminating handler reads the unwinder's state";
      Body = std::move(Handler);
      return nullptr;
    }
    // Control flow before the exception is taken would leave a path that
    // never takes it.
    if (Body[I].Kind != StmtKind::Assign && Body[I].Kind != StmtKind::Store &&
        Body[I].Kind != StmtKind::Call && Body[I].Kind != StmtKind::ExprStmt)
      return "control flow before the handler takes the exception";
  }
  if (Begin == Body.size())
    return "the run never takes the exception";
  if (Begin < PrefixEnd)
    return "the pad takes the exception before it tests the selector";
  // The exception object the run took, and the pad's variables and frame
  // slots that copied it on the way.
  const HighExpr *Taken = runtimeCall(Body[Begin], "__cxa_begin_catch");
  if (Taken->Operands.size() != 1)
    return "__cxa_begin_catch without one argument";
  ExceptionTrack Track;
  const std::vector<bool> CopiesException = Track.scan(Body, Begin);
  if (!Track.isException(Taken->Operands[0].get()))
    return "the handler takes something other than the pad's exception";
  std::vector<HighStmt> Handler;
  for (size_t I = 0; I < Body.size(); ++I) {
    HighStmt &S = Body[I];
    // The copies of the exception object served the call that took it.
    if (I < Begin && CopiesException[I])
      continue;
    if (I == Begin) {
      const ExprPtr *Call = callOf(S);
      auto Took = std::make_shared<HighExpr>(**Call);
      Took->Operands.clear();
      // A catch-all has no parameter: nothing it took may be read.
      if (CatchAll) {
        if (S.Kind == StmtKind::Assign && S.Dst && isVariable(S.Dst.get())) {
          const MedVar Result = S.Dst->Var;
          bool Read = false;
          for (size_t K = I + 1; K < Body.size(); ++K)
            walkStmts(std::vector<HighStmt>{Body[K]}, [&](const HighStmt &N) {
              forEachStatementExpr(N, [&](const HighExpr *E) {
                Read |= readsVariable(E, Result);
              });
            });
          if (Read)
            return "a catch-all reads what it caught";
        }
        continue;
      }
      // The handler reads the caught object through what the call returned;
      // with nothing reading it the call has nothing left to say.
      if (S.Kind != StmtKind::Assign || !S.Dst || !isVariable(S.Dst.get()))
        continue;
      bool Read = false;
      for (size_t K = I + 1; K < Body.size(); ++K)
        walkStmts(std::vector<HighStmt>{Body[K]}, [&](const HighStmt &N) {
          forEachStatementExpr(N, [&](const HighExpr *E) {
            Read |= readsVariable(E, S.Dst->Var);
          });
        });
      if (!Read)
        continue;
      HighStmt C = S;
      C.Val = Took;
      Handler.push_back(std::move(C));
      continue;
    }
    (I < PrefixEnd ? *Prefix : Handler).push_back(std::move(S));
  }
  // C++ leaves the handler at the end of its catch: the calls that end it
  // are the catch's own.  A run that takes the exception again is not one
  // handler.
  unsigned Ends = 0;
  unsigned Takes = 0;
  std::function<void(std::vector<HighStmt> &)> DropEnds =
      [&](std::vector<HighStmt> &Stmts) {
        std::vector<HighStmt> Kept;
        for (HighStmt &S : Stmts) {
          if (runtimeCall(S, "__cxa_end_catch")) {
            ++Ends;
            continue;
          }
          Takes += runtimeCall(S, "__cxa_begin_catch") != nullptr;
          DropEnds(S.Body);
          DropEnds(S.ElseBody);
          Kept.push_back(std::move(S));
        }
        Stmts = std::move(Kept);
      };
  DropEnds(Handler);
  // A handler that rethrows leaves through the rethrow, whose own landing
  // pad ends the handler: C++'s `throw;` does both.
  const bool Rethrows =
      !Handler.empty() && runtimeCall(Handler.back(), "__cxa_rethrow");
  if (Ends == 0 && !Rethrows)
    return "the handler never ends with __cxa_end_catch";
  if (Takes > (CatchAll ? 0u : 1u))
    return "the handler takes the exception twice";
  // Nothing of the unwinder's state may outlive the rewrite: C++ has no name
  // for it.
  if (Track.readBy(Handler) || (Prefix && Track.readBy(*Prefix)))
    return "the handler reads the unwinder's state";
  Body = std::move(Handler);
  return nullptr;
}

/// Spell \p Body, the run of a pad that no clause matched, as cleanups: the
/// statements before \p PrefixEnd, which the pad runs whatever the selector,
/// into \p Prefix and the rest into \p Rest.  The `_Unwind_Resume` that
/// ends the run, or ARM EHABI's `__cxa_end_cleanup`, which resumes the
/// exception the runtime is propagating, is the rethrow a C++ cleanup ends
/// with; the pad's copies of the exception and the selector go.
const char *makeCleanup(std::vector<HighStmt> &Body,
                        const std::set<size_t> &SelectorWrites,
                        size_t PrefixEnd, std::vector<HighStmt> &Prefix,
                        std::vector<HighStmt> &Rest) {
  dropAt(Body, SelectorWrites, PrefixEnd);
  if (Body.empty())
    return "the cleanup run is empty";
  const HighExpr *Resume = runtimeCall(Body.back(), "_Unwind_Resume");
  const HighExpr *EndCleanup = runtimeCall(Body.back(), "__cxa_end_cleanup");
  if ((!Resume || Resume->Operands.size() != 1) &&
      (!EndCleanup || !EndCleanup->Operands.empty()))
    return "the cleanup run does not end resuming the unwind";
  ExceptionTrack Track;
  const std::vector<bool> Copying = Track.scan(Body, Body.size() - 1);
  if (Resume && !Track.isException(Resume->Operands[0].get()))
    return "the cleanup resumes something other than the pad's exception";
  for (size_t I = 0; I + 1 < Body.size(); ++I) {
    if (Copying[I])
      continue;
    if (runtimeCall(Body[I], "__cxa_begin_catch") ||
        runtimeCall(Body[I], "__cxa_end_catch"))
      return "the cleanup run takes the exception";
    (I < PrefixEnd ? Prefix : Rest).push_back(std::move(Body[I]));
  }
  if (Track.readBy(Prefix) || Track.readBy(Rest))
    return "the cleanup reads the unwinder's state";
  return nullptr;
}

/// The run's stores of the selector to frame slots nothing reads any more:
/// the tests that read them were decided.
void dropSelectorSpills(std::vector<HighStmt> &Body,
                        const std::vector<const HighExpr *> &Spills) {
  if (Spills.empty())
    return;
  auto IsSpill = [&](const HighExpr &Address) {
    return llvm::any_of(Spills, [&](const HighExpr *Spill) {
      return Spill->structuralEq(Address);
    });
  };
  bool Read = false;
  std::function<void(const HighExpr *, unsigned)> Visit = [&](const HighExpr *E,
                                                              unsigned Depth) {
    if (!E || Read || Depth > 64)
      return;
    if (E->Kind == ExprKind::Load && !E->Operands.empty() && E->Operands[0]) {
      const HighExpr *Base = baseAndOffset(E->Operands[0].get()).first;
      Read = llvm::any_of(Spills, [&](const HighExpr *Spill) {
        const HighExpr *SpillBase = baseAndOffset(Spill).first;
        return Base && SpillBase && Base->structuralEq(*SpillBase) &&
               !apart(*Spill, 4, *E->Operands[0], bytesOf(E));
      });
    }
    E->forEachChildExpr([&](const ExprPtr &C) { Visit(C.get(), Depth + 1); });
  };
  walkStmts(Body, [&](const HighStmt &S) {
    for (const ExprPtr *E :
         {&S.Cond, &S.Val, &S.CallExpr, &S.RetVal, &S.StoreVal, &S.SwitchExpr})
      Visit(E->get(), 0);
  });
  if (Read)
    return;
  llvm::erase_if(Body, [&](const HighStmt &S) {
    const auto Write = memoryWrite(S);
    return Write && IsSpill(*Write->first);
  });
}

/// The landing pad of the call site that covers \p Addr, 0 for none.
va_t padAt(const ItaniumEHInfo &LSDA, va_t Addr) {
  for (const ItaniumCallSite &Site : LSDA.CallSites)
    if (Site.GuardedRange.isValid() && Site.GuardedRange.contains(Addr))
      return Site.LandingPadVA;
  return 0;
}

/// Split each region whose call sites reach different landing pads into
/// consecutive regions with one pad each.  The pads run different code (one
/// frees an exception a constructor was building), and one C++ handler
/// cannot tell which call it was entered from.  A region keeps its shape
/// when a statement's calls reach more than one pad.
void splitRegionsByPad(std::vector<HighStmt> &Stmts,
                       const ItaniumEHInfo &LSDA) {
  for (size_t I = 0; I < Stmts.size(); ++I) {
    HighStmt &S = Stmts[I];
    splitRegionsByPad(S.Body, LSDA);
    splitRegionsByPad(S.ElseBody, LSDA);
    for (SwitchCase &C : S.Cases)
      splitRegionsByPad(C.Body, LSDA);
    splitRegionsByPad(S.DefaultBody, LSDA);
    if (S.Kind != StmtKind::ItaniumTry || S.EHClauses.empty())
      continue;
    std::set<va_t> Pads;
    for (const HighEHClause &Clause : S.EHClauses)
      Pads.insert(Clause.LandingPadVAs.begin(), Clause.LandingPadVAs.end());
    if (Pads.size() < 2)
      continue;
    // The pad each top-level statement's calls reach.
    std::vector<va_t> PadOf(S.Body.size(), 0);
    bool Mixed = false;
    for (size_t K = 0; K < S.Body.size() && !Mixed; ++K) {
      std::set<va_t> Reached;
      walkStmts(std::vector<HighStmt>{S.Body[K]}, [&](const HighStmt &N) {
        if (N.Addr && N.Addr != InvalidVA)
          if (const va_t Pad = padAt(LSDA, N.Addr); Pad && Pads.count(Pad))
            Reached.insert(Pad);
      });
      Mixed = Reached.size() > 1;
      PadOf[K] = Reached.empty() ? 0 : *Reached.begin();
    }
    if (Mixed)
      continue;
    // A statement that reaches no pad cannot throw: it joins its neighbour.
    for (size_t K = 1; K < PadOf.size(); ++K)
      if (!PadOf[K])
        PadOf[K] = PadOf[K - 1];
    for (size_t K = PadOf.size(); K-- > 1;)
      if (!PadOf[K - 1])
        PadOf[K - 1] = PadOf[K];
    std::vector<HighStmt> Parts;
    for (size_t K = 0; K < S.Body.size();) {
      size_t End = K + 1;
      while (End < S.Body.size() && PadOf[End] == PadOf[K])
        ++End;
      HighStmt Part = S;
      Part.Body.assign(std::make_move_iterator(S.Body.begin() + K),
                       std::make_move_iterator(S.Body.begin() + End));
      Part.Addr = K == 0 ? S.Addr : 0;
      for (HighEHClause &Clause : Part.EHClauses)
        Clause.LandingPadVAs = {PadOf[K]};
      Parts.push_back(std::move(Part));
      K = End;
    }
    if (Parts.size() < 2 || llvm::any_of(PadOf, [](va_t P) { return !P; }))
      continue;
    Stmts.erase(Stmts.begin() + I);
    Stmts.insert(Stmts.begin() + I, std::make_move_iterator(Parts.begin()),
                 std::make_move_iterator(Parts.end()));
    I += Parts.size() - 1;
  }
}

/// The type filters the personality may enter \p Pad with: those of the
/// action chain of every call site that lands there, 0 for a cleanup.
std::set<int64_t> padSelectors(const ItaniumEHInfo &LSDA, va_t Pad) {
  std::set<int64_t> Selectors;
  for (const ItaniumCallSite &Site : LSDA.CallSites) {
    if (Site.LandingPadVA != Pad)
      continue;
    if (!Site.FirstActionOffset) {
      Selectors.insert(0);
      continue;
    }
    std::optional<uint64_t> Offset = Site.FirstActionOffset;
    for (size_t Step = 0; Offset && Step <= LSDA.Actions.size(); ++Step) {
      const ItaniumAction *Action = nullptr;
      for (const ItaniumAction &A : LSDA.Actions)
        if (A.TableOffset == *Offset)
          Action = &A;
      if (!Action) {
        // A broken chain names nothing this can account for.
        Selectors.insert(std::numeric_limits<int64_t>::min());
        break;
      }
      Selectors.insert(Action->TypeFilter);
      Offset = Action->NextActionOffset;
    }
  }
  return Selectors;
}

bool hasItaniumCxxRegions(const HighFunc &Func) {
  return Func.ExceptionMetadata &&
         getPersonalityRuntime(Func.ExceptionMetadata->Personality) ==
             SourceLanguageRuntime::CxxItanium &&
         Func.ExceptionMetadata->Itanium &&
         Func.ExceptionMetadata->Itanium->IsCallSiteAddressForm;
}

/// The landing pads that only clean up: every call site that reaches one
/// enters it with selector 0, so it runs the destructors of the objects the
/// throw leaves behind and resumes the unwind, whatever was thrown.
std::vector<va_t> cleanupOnlyPads(const ItaniumEHInfo &LSDA) {
  std::set<va_t> Pads;
  for (const ItaniumCallSite &Site : LSDA.CallSites)
    if (Site.LandingPadVA && Site.GuardedRange.isValid())
      Pads.insert(Site.LandingPadVA);
  std::vector<va_t> Cleanups;
  for (va_t Pad : Pads)
    if (padSelectors(LSDA, Pad) == std::set<int64_t>{0})
      Cleanups.push_back(Pad);
  return Cleanups;
}

/// Call \p Fn with the call instruction of each call \p E makes, intrinsics
/// and the compiler builtins this spells operations as (`__builtin_alloca`)
/// aside: none raises a C++ exception.  A call no instruction makes, or one
/// too deep to reach, passes 0.
void forEachCallSite(const HighExpr *E, llvm::function_ref<void(va_t)> Fn,
                     unsigned Depth = 0) {
  if (!E)
    return;
  if (Depth > 64) {
    Fn(0);
    return;
  }
  if (E->Kind == ExprKind::Call && E->IntrinsicId == Intrinsic::None &&
      !llvm::StringRef(E->CallTarget).starts_with("__builtin_"))
    Fn(E->CallSiteAddr);
  E->forEachChildExpr([&](const ExprPtr &Child) {
    forEachCallSite(Child.get(), Fn, Depth + 1);
  });
}

/// The runs of statements the scopes of one cleanup-only landing pad wrap.
/// Together they hold every call of the function's ordinary flow that the
/// pad's call sites cover, and no call they do not cover: the C++ handler of
/// the scope would run the cleanups for that call too.  Statements that make
/// no call may go either way, as they cannot raise.  Code only the unwinder
/// runs is no part of a scope; a call there that the merged call-site ranges
/// happen to cover runs while another exception is unwinding.  A handler, and
/// the code it goes on to once it caught the exception, run as ordinary code:
/// a call there that the pad covers keeps the pad in place.
class CleanupScopes {
public:
  struct Run {
    const HighStmt *First = nullptr;
    const HighStmt *Last = nullptr;
  };
  std::vector<Run> Runs;
  /// Why no runs were found.
  const char *Reason = nullptr;

  /// \p Handlers holds the nodes of the code catch pads run.
  CleanupScopes(const HighFlowGraph &G, const std::set<size_t> &Normal,
                const std::set<size_t> &Handlers, const ItaniumEHInfo &LSDA,
                va_t Pad)
      : G(G), Normal(Normal), Handlers(Handlers), LSDA(LSDA), Pad(Pad) {}

  /// Find the runs of \p Body.
  bool find(const std::vector<HighStmt> &Body) {
    walkStmts(Body, [&](const HighStmt &S) {
      if (!Handlers.count(nodeOf(S)))
        return;
      forEachStatementExpr(S, [&](const HighExpr *E) {
        forEachCallSite(E, [&](va_t Site) {
          if (!Site || padAt(LSDA, Site) == Pad)
            fail("a handler makes a call the pad may cover");
        });
      });
    });
    if (!Failed)
      cover(Body);
    if (!Failed && Runs.empty())
      fail("no call of ordinary flow reaches the pad");
    // Every covered call of ordinary flow sits in a run.  The walk passes
    // over a statement only the unwinder enters, which may still hold one a
    // jump reaches.
    if (!Failed) {
      std::set<const HighStmt *> Wrapped;
      std::function<void(const HighStmt &)> Mark = [&](const HighStmt &S) {
        Wrapped.insert(&S);
        for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
          for (const HighStmt &Child : *List)
            Mark(Child);
        for (const SwitchCase &Case : S.Cases)
          for (const HighStmt &Child : Case.Body)
            Mark(Child);
        for (const auto &Clause : S.EHClauseBodies)
          for (const HighStmt &Child : Clause)
            Mark(Child);
      };
      // A run's statements are neighbours in one list.
      for (const Run &R : Runs)
        for (const HighStmt *S = R.First; S <= R.Last; ++S)
          Mark(*S);
      walkStmts(Body, [&](const HighStmt &S) {
        if (Failed || !Normal.count(nodeOf(S)) || Wrapped.count(&S))
          return;
        forEachStatementExpr(S, [&](const HighExpr *E) {
          forEachCallSite(E, [&](va_t Site) {
            if (Site && padAt(LSDA, Site) == Pad)
              fail("a covered call lies outside every scope");
          });
        });
      });
    }
    return !Failed;
  }

private:
  struct Calls {
    /// A call the pad's call sites cover.
    bool Covered = false;
    /// A call they do not cover, or code a scope may not hold.
    bool Other = false;
  };

  const HighFlowGraph &G;
  const std::set<size_t> &Normal;
  const std::set<size_t> &Handlers;
  const ItaniumEHInfo &LSDA;
  const va_t Pad;
  std::map<const HighStmt *, Calls> Memo;
  bool Failed = false;

  void fail(const char *Why) {
    if (!Failed)
      Reason = Why;
    Failed = true;
  }

  size_t nodeOf(const HighStmt &S) const {
    const auto It = G.NodeOf.find(&S);
    return It == G.NodeOf.end() ? 0 : It->second;
  }

  Calls classify(const HighStmt &S) {
    if (const auto It = Memo.find(&S); It != Memo.end())
      return It->second;
    Calls C;
    if (!Normal.count(nodeOf(S))) {
      C.Other = true;
      return Memo[&S] = C;
    }
    // A copy of a call keeps its instruction, whatever address its statement
    // took; a call that stands for two instructions keeps none
    // (withMergedCallSites).
    forEachStatementExpr(S, [&](const HighExpr *E) {
      forEachCallSite(E, [&](va_t Site) {
        if (!Site)
          fail("a call has no call instruction of its own");
        else if (padAt(LSDA, Site) == Pad)
          C.Covered = true;
        else
          C.Other = true;
      });
    });
    const bool OwnCovered = C.Covered;
    auto Add = [&](const std::vector<HighStmt> &List) {
      for (const HighStmt &Child : List) {
        const Calls Inner = classify(Child);
        C.Covered |= Inner.Covered;
        C.Other |= Inner.Other;
      }
    };
    Add(S.Body);
    Add(S.ElseBody);
    for (const SwitchCase &Case : S.Cases)
      Add(Case.Body);
    Add(S.DefaultBody);
    for (const auto &Clause : S.EHClauseBodies)
      Add(Clause);
    // A covered call of the statement's own, a condition's say, goes into a
    // scope only with all of the statement.
    if (OwnCovered && C.Other)
      fail("a statement makes calls of the pad and calls of another");
    return Memo[&S] = C;
  }

  /// Cover the calls of \p L: a run for each stretch of statements whose
  /// calls the pad's call sites all cover, and the statements that make both
  /// kinds of call looked into.
  void cover(const std::vector<HighStmt> &L) {
    std::optional<size_t> Open;
    size_t Last = 0;
    auto Close = [&] {
      if (Open)
        close(L, *Open, Last);
      Open.reset();
    };
    for (size_t I = 0; I < L.size() && !Failed; ++I) {
      const HighStmt &S = L[I];
      const Calls C = classify(S);
      if (C.Covered && !C.Other) {
        if (!Open)
          Open = I;
        Last = I;
        continue;
      }
      if (!C.Covered && !C.Other)
        continue;
      Close();
      if (!C.Covered)
        continue;
      cover(S.Body);
      cover(S.ElseBody);
      for (const SwitchCase &Case : S.Cases)
        cover(Case.Body);
      cover(S.DefaultBody);
      for (const auto &Clause : S.EHClauseBodies)
        cover(Clause);
    }
    if (!Failed)
      Close();
  }

  /// Make \p L's statements from \p Begin to \p End a run, unless a jump
  /// enters them below the first: C++ jumps into no try block.
  void close(const std::vector<HighStmt> &L, size_t Begin, size_t End) {
    std::set<size_t> Inside;
    std::function<void(const HighStmt &)> Add = [&](const HighStmt &S) {
      Inside.insert(nodeOf(S));
      for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
        for (const HighStmt &Child : *List)
          Add(Child);
      for (const SwitchCase &Case : S.Cases)
        for (const HighStmt &Child : Case.Body)
          Add(Child);
      for (const auto &Clause : S.EHClauseBodies)
        for (const HighStmt &Child : Clause)
          Add(Child);
    };
    for (size_t I = Begin; I <= End; ++I)
      Add(L[I]);
    // A do-while is entered at its body, as its label is.
    const HighStmt *Top = &L[Begin];
    while (Top->Kind == StmtKind::DoWhile && !Top->Body.empty())
      Top = &Top->Body.front();
    const size_t Entry = nodeOf(*Top);
    for (size_t N = 0; N < G.Nodes.size(); ++N) {
      if (Inside.count(N))
        continue;
      for (size_t Next : G.Nodes[N].Successors)
        if (Next != Entry && Inside.count(Next)) {
          fail("a jump enters the scope below its first statement");
          return;
        }
    }
    Runs.push_back({&L[Begin], &L[End]});
  }
};

/// The cleanup scopes of one landing pad that only cleans up.
struct CleanupPlan {
  va_t Pad = 0;
  ExceptionAddressRange Range;
  std::vector<HighStmt> Actions;
  std::vector<CleanupScopes::Run> Runs;
};

/// The cleanup scope that opens at a statement: the last statement it holds
/// and the plan it belongs to.
struct ScopeOpening {
  const HighStmt *Last = nullptr;
  const CleanupPlan *Plan = nullptr;
};

void removeStatements(std::vector<HighStmt> &Stmts,
                      const std::set<const HighStmt *> &Gone,
                      const std::map<const HighStmt *, ScopeOpening> &Opens);

void removeNestedStatements(
    HighStmt &S, const std::set<const HighStmt *> &Gone,
    const std::map<const HighStmt *, ScopeOpening> &Opens) {
  removeStatements(S.Body, Gone, Opens);
  removeStatements(S.ElseBody, Gone, Opens);
  for (SwitchCase &C : S.Cases)
    removeStatements(C.Body, Gone, Opens);
  removeStatements(S.DefaultBody, Gone, Opens);
  for (auto &Clause : S.EHClauseBodies)
    removeStatements(Clause, Gone, Opens);
}

/// Remove the statements of \p Stmts in \p Gone, children with their parent,
/// and wrap each run of \p Opens in its cleanup scope: a C++ try whose
/// catch-all handler runs the pad's cleanups and rethrows.
void removeStatements(std::vector<HighStmt> &Stmts,
                      const std::set<const HighStmt *> &Gone,
                      const std::map<const HighStmt *, ScopeOpening> &Opens) {
  std::vector<HighStmt> Kept;
  Kept.reserve(Stmts.size());
  for (size_t I = 0; I < Stmts.size(); ++I) {
    HighStmt &S = Stmts[I];
    if (Gone.count(&S))
      continue;
    const auto Open = Opens.find(&S);
    if (Open == Opens.end()) {
      removeNestedStatements(S, Gone, Opens);
      Kept.push_back(std::move(S));
      continue;
    }
    const CleanupPlan &Plan = *Open->second.Plan;
    HighStmt Scope;
    Scope.Kind = StmtKind::ItaniumTry;
    // The scope opens where its first statement is, so a label there is the
    // scope's.
    Scope.Addr = S.Addr;
    Scope.EHRange = Plan.Range;
    Scope.EHIsReducible = true;
    HighEHClause Clause;
    Clause.Kind = HighEHClauseKind::CxxCleanup;
    Clause.HandlerVA = Plan.Pad;
    Clause.LandingPadVAs = {Plan.Pad};
    Scope.EHClauses = {std::move(Clause)};
    Scope.EHClauseBodies = {Plan.Actions};
    for (; I < Stmts.size(); ++I) {
      HighStmt &Held = Stmts[I];
      const bool IsLast = &Held == Open->second.Last;
      if (!Gone.count(&Held)) {
        removeNestedStatements(Held, Gone, Opens);
        Scope.Body.push_back(std::move(Held));
      }
      if (IsLast)
        break;
    }
    Kept.push_back(std::move(Scope));
  }
  Stmts = std::move(Kept);
}

} // namespace

void attachItaniumHandlerBodies(HighFunc &Func) {
  if (!hasItaniumCxxRegions(Func))
    return;
  const ItaniumEHInfo &LSDA = *Func.ExceptionMetadata->Itanium;
  splitRegionsByPad(Func.Body, LSDA);
  std::vector<HighStmt *> Regions;
  walkStmts(Func.Body, [&](HighStmt &S) {
    if (S.Kind == StmtKind::ItaniumTry && S.EHIsReducible &&
        !S.EHClauses.empty())
      Regions.push_back(&S);
  });
  const std::vector<va_t> CleanupPads = cleanupOnlyPads(LSDA);
  if (Regions.empty() && CleanupPads.empty())
    return;
  const HighFlowGraph G(Func.Body, endsPath);
  if (!G.Complete)
    return;
  std::set<size_t> Normal;
  {
    std::vector<size_t> Work{G.Entry};
    while (!Work.empty()) {
      const size_t N = Work.back();
      Work.pop_back();
      if (!Normal.insert(N).second)
        continue;
      Work.insert(Work.end(), G.Nodes[N].Successors.begin(),
                  G.Nodes[N].Successors.end());
    }
  }
  std::map<const HighStmt *, Position> Positions;
  indexPositions(Func.Body, Positions);
  const bool VoidFunction =
      !Func.ReturnType || Func.ReturnType->Kind == NdTypeKind::Void;

  // Every pad statement each region's clauses run, and the pads whose
  // regions this left alone: a statement one of those reaches stays.
  struct Plan {
    HighStmt *Region = nullptr;
    va_t Pad = 0;
    std::vector<std::vector<HighStmt>> Bodies;
    /// What the pad runs before it tests the selector, and what it runs when
    /// no clause matched: the cleanups of the region's objects and of those
    /// that outlive it.
    std::vector<HighStmt> Inner;
    std::vector<HighStmt> Outer;
  };
  std::set<const HighStmt *> Consumed;
  std::vector<Plan> Done;
  std::set<va_t> KeptPads;
  std::set<va_t> DroppedPads;
  for (HighStmt *Region : Regions) {
    std::set<va_t> Pads;
    for (const HighEHClause &Clause : Region->EHClauses)
      Pads.insert(Clause.LandingPadVAs.begin(), Clause.LandingPadVAs.end());
    const char *Why = nullptr;
    auto Pad = G.LabelAt.end();
    if (Pads.size() != 1)
      Why = "clauses reach several landing pads";
    else if (Pad = G.LabelAt.find(*Pads.begin());
             Pad == G.LabelAt.end() || Normal.count(Pad->second))
      Why = "landing pad is no label of its own";
    for (const HighEHClause &Clause : Region->EHClauses)
      if (!Why && (Clause.Kind != HighEHClauseKind::ItaniumCatch ||
                   Clause.ParseStatus != ExceptionParseStatus::Complete ||
                   Clause.TypeFilter <= 0))
        Why = "clause is no complete catch";
    // Every selector the pad may be entered with must be spelled: a clause
    // here, or 0 for the cleanups it runs when nothing matched.  A clause of
    // an enclosing try that the same pad dispatches is not one of these
    // handlers.
    bool Cleanup = false;
    if (!Why) {
      std::set<int64_t> Filters;
      for (const HighEHClause &Clause : Region->EHClauses)
        Filters.insert(Clause.TypeFilter);
      for (int64_t Selector :
           padSelectors(*Func.ExceptionMetadata->Itanium, *Pads.begin())) {
        if (Selector == 0)
          Cleanup = true;
        else if (!Filters.count(Selector))
          Why = "pad also dispatches another region's clause";
      }
    }
    Plan P;
    P.Region = Region;
    std::set<const HighStmt *> Copied;
    std::set<va_t> Dropped;
    // With a cleanup the pad's work before its tests is a scope of its own
    // around the region's body: it runs for every exception, and a handler
    // runs after it, but an exception a handler throws must not run it
    // again.
    size_t DispatchAt = 0;
    const HighStmt *Decision = nullptr;
    if (!Why && Cleanup) {
      PadRun Run(G, Normal, Positions, Region->Body, VoidFunction, 0);
      auto Body = Run.run(Pad->second);
      if (!Body)
        Why = Run.Reason;
      else if (Run.DispatchAt == std::numeric_limits<size_t>::max())
        Why = "the pad tests the selector in a branch";
      else
        Why = makeCleanup(*Body, Run.SelectorWrites, Run.DispatchAt, P.Inner,
                          P.Outer);
      if (!Why) {
        DispatchAt = Run.DispatchAt;
        Decision = Run.Decision;
        Copied.insert(Run.Copied.begin(), Run.Copied.end());
        Dropped.insert(Run.DroppedPads.begin(), Run.DroppedPads.end());
      }
    }
    for (const HighEHClause &Clause : Region->EHClauses) {
      if (Why)
        break;
      PadRun Run(G, Normal, Positions, Region->Body, VoidFunction,
                 Clause.TypeFilter);
      auto Body = Run.run(Pad->second);
      const bool CatchAll =
          Clause.TypeName.empty() && Clause.TypeDescriptorVA == 0;
      std::vector<HighStmt> Shared;
      if (!Body)
        Why = Run.Reason;
      else if (Cleanup &&
               (Run.Decision != Decision || Run.DispatchAt != DispatchAt))
        Why = "the clauses part before the pad tests the selector";
      else
        Why =
            makeHandler(*Body, CatchAll, Run.SelectorWrites,
                        Cleanup ? DispatchAt : 0, Cleanup ? &Shared : nullptr);
      if (Why)
        break;
      dropSelectorSpills(*Body, Run.SelectorSpills);
      Copied.insert(Run.Copied.begin(), Run.Copied.end());
      Dropped.insert(Run.DroppedPads.begin(), Run.DroppedPads.end());
      P.Bodies.push_back(std::move(*Body));
    }
    if (Why) {
      note(Why, Region->EHRange.Begin);
      KeptPads.insert(Pads.begin(), Pads.end());
      continue;
    }
    P.Pad = *Pads.begin();
    Consumed.insert(Copied.begin(), Copied.end());
    DroppedPads.insert(Dropped.begin(), Dropped.end());
    Done.push_back(std::move(P));
  }
  // A pad that only cleans up is a scope of its own around the calls that
  // reach it: `try { … } catch (...) { cleanups; throw; }`, as an MSVC
  // object's unwind action reads.
  std::vector<CleanupPlan> Cleanups;
  Cleanups.reserve(CleanupPads.size());
  std::vector<va_t> CleanupPadsKept;
  // The code the other pads run: handlers, and what they continue to.
  std::set<size_t> Handlers;
  if (!CleanupPads.empty())
    for (const ItaniumCallSite &Site : LSDA.CallSites) {
      if (!Site.LandingPadVA ||
          llvm::is_contained(CleanupPads, Site.LandingPadVA))
        continue;
      const auto Entry = G.LabelAt.find(Site.LandingPadVA);
      if (Entry == G.LabelAt.end())
        continue;
      std::vector<size_t> Work{Entry->second};
      while (!Work.empty()) {
        const size_t N = Work.back();
        Work.pop_back();
        if (N == 0 || Normal.count(N) || !Handlers.insert(N).second)
          continue;
        Work.insert(Work.end(), G.Nodes[N].Successors.begin(),
                    G.Nodes[N].Successors.end());
      }
    }
  for (va_t Pad : CleanupPads) {
    const char *Why = nullptr;
    const auto Entry = G.LabelAt.find(Pad);
    if (Entry == G.LabelAt.end() || Normal.count(Entry->second))
      Why = "landing pad is no label of its own";
    const std::vector<HighStmt> NoTry;
    PadRun Run(G, Normal, Positions, NoTry, VoidFunction, 0);
    std::optional<std::vector<HighStmt>> Body;
    if (!Why && !(Body = Run.run(Entry->second)))
      Why = Run.Reason;
    CleanupPlan Plan;
    Plan.Pad = Pad;
    std::vector<HighStmt> Prefix;
    if (!Why)
      Why = makeCleanup(*Body, Run.SelectorWrites, 0, Prefix, Plan.Actions);
    // A pad that runs nothing but the resume is no cleanup at all.
    CleanupScopes Scopes(G, Normal, Handlers, LSDA, Pad);
    if (!Why && !Plan.Actions.empty() && !Scopes.find(Func.Body))
      Why = Scopes.Reason;
    if (Why) {
      note(Why, Pad);
      CleanupPadsKept.push_back(Pad);
      continue;
    }
    for (const ItaniumCallSite &Site : LSDA.CallSites)
      if (Site.LandingPadVA == Pad && Site.GuardedRange.isValid())
        Plan.Range =
            Plan.Range.isValid()
                ? ExceptionAddressRange{std::min(Plan.Range.Begin,
                                                 Site.GuardedRange.Begin),
                                        std::max(Plan.Range.End,
                                                 Site.GuardedRange.End)}
                : Site.GuardedRange;
    if (!Plan.Actions.empty())
      Plan.Runs = std::move(Scopes.Runs);
    Consumed.insert(Run.Copied.begin(), Run.Copied.end());
    DroppedPads.insert(Run.DroppedPads.begin(), Run.DroppedPads.end());
    Cleanups.push_back(std::move(Plan));
  }
  if (Done.empty() && Cleanups.empty())
    return;
  // The pads of the regions the handlers no longer spell run nothing else.
  auto PadCode = [&](va_t Pad, llvm::function_ref<void(const HighStmt *)> Fn) {
    auto Entry = G.LabelAt.find(Pad);
    if (Entry == G.LabelAt.end())
      return;
    std::vector<size_t> Work{Entry->second};
    std::set<size_t> Seen;
    while (!Work.empty()) {
      const size_t N = Work.back();
      Work.pop_back();
      if (N == 0 || Normal.count(N) || !Seen.insert(N).second)
        continue;
      Fn(G.Nodes[N].Statement);
      Work.insert(Work.end(), G.Nodes[N].Successors.begin(),
                  G.Nodes[N].Successors.end());
    }
  };
  for (va_t Pad : DroppedPads)
    PadCode(Pad, [&](const HighStmt *S) { Consumed.insert(S); });
  // A cleanup pad left in place keeps the code it reaches, unless the run of
  // a pad spelled above took its entry along: its code then goes on as that
  // pad's tail.
  for (va_t Pad : CleanupPadsKept)
    if (const auto Entry = G.LabelAt.find(Pad);
        Entry != G.LabelAt.end() &&
        !Consumed.count(G.Nodes[Entry->second].Statement))
      KeptPads.insert(Pad);
  // Pad code a region left alone still reaches stays where it is.
  for (va_t Kept : KeptPads)
    PadCode(Kept, [&](const HighStmt *S) { Consumed.erase(S); });
  // A statement goes with its parent only when nothing of it stays.
  std::set<const HighStmt *> Gone;
  std::function<bool(const HighStmt &)> AllGone = [&](const HighStmt &S) {
    if (!Consumed.count(&S))
      return false;
    for (const auto *List : {&S.Body, &S.ElseBody, &S.DefaultBody})
      for (const HighStmt &C : *List)
        if (!AllGone(C))
          return false;
    for (const SwitchCase &Case : S.Cases)
      for (const HighStmt &C : Case.Body)
        if (!AllGone(C))
          return false;
    return true;
  };
  walkStmts(Func.Body, [&](const HighStmt &S) {
    if (AllGone(S))
      Gone.insert(&S);
  });
  // The bodies are copies.  Attaching them and wrapping a region in its
  // cleanup scopes moves no other statement, so the pad code they replace is
  // still where Gone names it until it is removed last.
  for (Plan &P : Done)
    P.Region->EHClauseBodies = std::move(P.Bodies);
  auto Cleanup = [](std::vector<HighStmt> Body, std::vector<HighStmt> Actions,
                    va_t Pad, const HighStmt &Region) {
    HighStmt Scope;
    Scope.Kind = StmtKind::ItaniumTry;
    // The scope opens where the region does, so a label there is the scope's.
    Scope.Addr = Region.Addr;
    Scope.EHRange = Region.EHRange;
    Scope.EHIsReducible = true;
    Scope.Body = std::move(Body);
    HighEHClause Clause;
    Clause.Kind = HighEHClauseKind::CxxCleanup;
    Clause.HandlerVA = Pad;
    Clause.LandingPadVAs = {Pad};
    Scope.EHClauses = {std::move(Clause)};
    Scope.EHClauseBodies = {std::move(Actions)};
    return Scope;
  };
  for (Plan &P : Done) {
    HighStmt Region = std::move(*P.Region);
    if (!P.Inner.empty()) {
      HighStmt Inner =
          Cleanup(std::move(Region.Body), std::move(P.Inner), P.Pad, Region);
      Region.Body.clear();
      Region.Body.push_back(std::move(Inner));
    }
    if (!P.Outer.empty()) {
      HighStmt Outer = Cleanup({}, std::move(P.Outer), P.Pad, Region);
      Outer.Body.push_back(std::move(Region));
      *P.Region = std::move(Outer);
    } else {
      *P.Region = std::move(Region);
    }
  }
  // The runs of the cleanup scopes hold no region's statements, which keep
  // their places above, and no pad code.
  std::map<const HighStmt *, ScopeOpening> Opens;
  for (const CleanupPlan &Plan : Cleanups)
    for (const CleanupScopes::Run &Run : Plan.Runs)
      Opens[Run.First] = {Run.Last, &Plan};
  removeStatements(Func.Body, Gone, Opens);
}

} // namespace neverd
