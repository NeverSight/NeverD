//===- HighLoopRecovery.cpp - While-loop recovery from backward gotos -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Proves natural-loop regions in the MedIR CFG before converting backward
/// gotos into while-loop constructs. Handles latch-conditional loops,
/// header-conditional loops, phi-copy ordering inside loop bodies, and
/// break/continue conversion for nested gotos.
///
/// See also:
///   HighCFSimplify.cpp         — main simplifyControlFlow entry point
///   HighIfChainToSwitch.cpp    — if-chain to switch recovery
///   HighCFSimplifyDetail.h     — shared AddrMap helper
///
//===----------------------------------------------------------------------===//

#include "HighCFSimplifyDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/high/MedToHigh.h"

#include <functional>
#include <map>
#include <optional>
#include <set>

namespace neverd {

namespace {

// Statement order is a layout property. A natural loop additionally needs an
// actual CFG edge to a header that dominates the latch. Compute its reverse
// predecessor closure, stopping at the header: reaching another entry would
// disprove that dominance. This also gives the exact blocks a rewrite may move.
class NaturalLoopEvidence {
  const MedFunc &Function;
  std::map<va_t, int> Entries, Owners;
  std::vector<std::vector<int>> Predecessors;
  std::vector<bool> Reachable;
  int Entry = -1;
  size_t Budget = ::neverd::limits::ir::kHighLoopRecoveryBudget;
  bool Valid = true;

  bool consume() {
    if (!Budget)
      return false;
    --Budget;
    return true;
  }

  /// The blocks that reach \p Latch without passing \p Header, plus the
  /// header; nullopt when that reaches the entry, so the header does not
  /// dominate the latch.
  std::optional<std::set<int>> closure(int Header, int Latch) {
    std::set<int> Members{Header};
    std::vector<int> Pending{Latch};
    while (!Pending.empty()) {
      const int Block = Pending.back();
      Pending.pop_back();
      if (!consume())
        return std::nullopt;
      if (!Members.insert(Block).second)
        continue;
      if (Block == Entry || !Reachable[Block] || Predecessors[Block].empty())
        return std::nullopt;
      for (int Predecessor : Predecessors[Block]) {
        if (!consume())
          return std::nullopt;
        Pending.push_back(Predecessor);
      }
    }
    return Members;
  }

public:
  explicit NaturalLoopEvidence(const MedFunc &Function) : Function(Function) {
    const size_t Count = Function.Blocks.size();
    Predecessors.resize(Count);
    Reachable.resize(Count);
    auto Record = [&](auto &Map, va_t Address, int Block) {
      if (!Address || Address == InvalidVA)
        return;
      auto [It, Added] = Map.emplace(Address, Block);
      if (!Added && It->second != Block)
        Valid = false;
    };
    for (size_t I = 0; I < Count; ++I) {
      const auto &Block = Function.Blocks[I];
      if (Block.Id != static_cast<int>(I) || !Block.ExceptionalSuccs.empty()) {
        Valid = false;
        return;
      }
      Record(Entries, Block.StartAddr, I);
      Record(Owners, Block.StartAddr, I);
      if (!Block.Ops.empty())
        Record(Entries, Block.Ops.front().Addr, I);
      for (const auto &Op : Block.Ops) {
        if (!consume()) {
          Valid = false;
          return;
        }
        Record(Owners, Op.Addr, I);
      }
      for (int Successor : Block.Succs) {
        if (!consume() || Successor < 0 ||
            Successor >= static_cast<int>(Count)) {
          Valid = false;
          return;
        }
        Predecessors[Successor].push_back(I);
      }
    }
    const auto Found = Entries.find(Function.Entry);
    if (!Valid || Found == Entries.end()) {
      Valid = false;
      return;
    }
    Entry = Found->second;
    std::vector<int> Pending{Entry};
    Reachable[Entry] = true;
    while (!Pending.empty()) {
      int Block = Pending.back();
      Pending.pop_back();
      for (int Successor : Function.Blocks[Block].Succs) {
        if (!consume()) {
          Valid = false;
          return;
        }
        if (!Reachable[Successor]) {
          Reachable[Successor] = true;
          Pending.push_back(Successor);
        }
      }
    }
  }

  /// The blocks of the loop that the back edge from \p Branch to \p Target
  /// closes, together with those of every other back edge to the same
  /// header: the natural loop of that header.
  std::optional<std::set<int>> body(va_t Branch, va_t Target) {
    if (!Valid)
      return std::nullopt;
    const auto Latch = Owners.find(Branch), Header = Entries.find(Target);
    if (Latch == Owners.end() || Header == Entries.end() ||
        !Reachable[Latch->second])
      return std::nullopt;
    const auto &Successors = Function.Blocks[Latch->second].Succs;
    if (std::find(Successors.begin(), Successors.end(), Header->second) ==
        Successors.end())
      return std::nullopt;
    std::optional<std::set<int>> Members =
        closure(Header->second, Latch->second);
    if (!Members)
      return std::nullopt;
    // A predecessor the header does not dominate enters the loop instead.
    for (int Other : Predecessors[Header->second])
      if (Other != Latch->second && Reachable[Other] && !Members->count(Other))
        if (std::optional<std::set<int>> More = closure(Header->second, Other))
          Members->insert(More->begin(), More->end());
    return Members;
  }

  bool contains(const std::set<int> &Members, va_t Address) const {
    const auto Found = Owners.find(Address);
    return Found != Owners.end() && Members.count(Found->second);
  }

  bool startsAtHeader(va_t Target, va_t Address) const {
    const auto Header = Entries.find(Target), Owner = Owners.find(Address);
    return Header != Entries.end() && Owner != Owners.end() &&
           Header->second == Owner->second;
  }
};

} // namespace

void detectAndConvertLoops(HighFunc &Func,
                           const std::unordered_map<va_t, int> &,
                           const MedFunc &Med, bool IsMega) {
  NaturalLoopEvidence Evidence(Med);
  AddrMap AM;
  AM.rebuild(Func.Body);

  auto FindStmtAt = [&](va_t Target) -> std::optional<size_t> {
    auto It = AM.Idx.find(Target);
    if (It != AM.Idx.end() && It->second < Func.Body.size() &&
        Func.Body[It->second].Kind != StmtKind::Nop)
      return It->second;
    AM.ensureSorted();
    auto LowerBound = std::lower_bound(AM.Sorted.begin(), AM.Sorted.end(),
                                       std::make_pair(Target, size_t(0)));
    if (LowerBound != AM.Sorted.end() &&
        LowerBound->second < Func.Body.size() &&
        Func.Body[LowerBound->second].Kind != StmtKind::Nop)
      return LowerBound->second;
    return std::nullopt;
  };

  const int MaxLoopPasses =
      IsMega ? ::neverd::limits::ir::
                   kHighLoopRecoveryDetectAndConvertLoopsMaxLoopPassesSelected
             : ::neverd::limits::ir::
                   kHighLoopRecoveryDetectAndConvertLoopsMaxLoopPassesDefault;
  for (int LoopPass = 0; LoopPass < MaxLoopPasses; ++LoopPass) {
    bool FoundLoop = false;
    AM.rebuild(Func.Body);

    for (int I = static_cast<int>(Func.Body.size()) - 1; I >= 0; --I) {
      auto &GotoStmt = Func.Body[I];
      if (GotoStmt.Kind == StmtKind::Nop)
        continue;

      va_t Target = 0;
      ExprPtr LatchCond;
      std::vector<HighStmt> LatchCopies;
      if (GotoStmt.Kind == StmtKind::Goto) {
        Target = GotoStmt.GotoTarget;
      } else if (GotoStmt.Kind == StmtKind::If && !GotoStmt.Body.empty() &&
                 GotoStmt.Body.back().Kind == StmtKind::Goto &&
                 std::all_of(GotoStmt.Body.begin(), GotoStmt.Body.end() - 1,
                             [](const HighStmt &S) { return S.IsPhiCopy; })) {
        Target = GotoStmt.Body.back().GotoTarget;
        LatchCond = GotoStmt.Cond;
        LatchCopies.assign(GotoStmt.Body.begin(), GotoStmt.Body.end() - 1);
      }
      if (Target == 0 || Target == InvalidVA)
        continue;

      auto OptIdx = FindStmtAt(Target);
      if (!OptIdx)
        continue;
      size_t HeaderIdx = *OptIdx;
      if (HeaderIdx > static_cast<size_t>(I))
        continue;

      const auto Members = Evidence.body(GotoStmt.Addr, Target);
      if (!Members ||
          !Evidence.startsAtHeader(Target, Func.Body[HeaderIdx].Addr))
        continue;
      const va_t LatchAddr = LatchCond ? 0 : GotoStmt.Addr;
      bool OwnsRegion = true;
      bool HasLatchPrefix = false;
      for (size_t K = HeaderIdx; K <= static_cast<size_t>(I); ++K) {
        const auto &S = Func.Body[K];
        if (S.Kind == StmtKind::Nop)
          continue;
        if (!Evidence.contains(*Members, S.Addr)) {
          OwnsRegion = false;
          break;
        }
        if (K != static_cast<size_t>(I) && S.Addr == LatchAddr)
          HasLatchPrefix = true;
      }
      if (!OwnsRegion)
        continue;

      // A jump directly to the terminal goto starts the next iteration.
      // If its address labels an earlier statement, entering there must
      // still execute the latch's delay-slot effects and edge copies.
      bool BareLatchEntered = false;
      if (!HasLatchPrefix && LatchAddr != 0 && LatchAddr != InvalidVA &&
          HeaderIdx != static_cast<size_t>(I))
        walkStmts(Func.Body, [&](const HighStmt &S) {
          BareLatchEntered |=
              S.Kind == StmtKind::Goto && S.GotoTarget == LatchAddr;
        });

      std::vector<HighStmt> LoopBody;
      LoopBody.reserve(static_cast<size_t>(I) - HeaderIdx);
      for (size_t K = HeaderIdx; K < static_cast<size_t>(I); ++K) {
        if (Func.Body[K].Kind == StmtKind::Nop)
          continue;
        LoopBody.push_back(std::move(Func.Body[K]));
      }

      ExprPtr WhileCond;
      va_t LoopExitTarget = 0;

      if (LatchCond) {
        WhileCond = HighExpr::makeConst(1, 1);

        HighStmt ExitCheck;
        ExitCheck.Kind = StmtKind::If;
        ExitCheck.Addr = Func.Body[I].Addr;
        ExitCheck.Cond = HighExpr::makeUnary(NdOp::BOOL_NOT, LatchCond);
        HighStmt Exit;
        Exit.Kind = StmtKind::Break;
        // The untaken latch edge continues after this loop, including any
        // edge copies or explicit transfer. A generated statement there may
        // have no native address; it must not become a fabricated goto label.
        ExitCheck.Body.push_back(std::move(Exit));
        LoopBody.push_back(ExitCheck);
        LoopBody.insert(LoopBody.end(), LatchCopies.begin(), LatchCopies.end());
      } else if (!LoopBody.empty() && LoopBody[0].Kind == StmtKind::If &&
                 LoopBody[0].Body.size() == 1 &&
                 LoopBody[0].Body[0].Kind == StmtKind::Goto) {
        va_t ExitTarget = LoopBody[0].Body[0].GotoTarget;
        if (ExitTarget != 0) {
          // A header test becomes the loop condition only when its exit is
          // what follows the loop: a failed `while` falls through there. A
          // test that exits elsewhere (a search whose miss path differs from
          // its hit path) stays a jump inside the loop.
          const size_t After = static_cast<size_t>(I) + 1;
          const bool ExitsAfterLoop =
              After < Func.Body.size() && Func.Body[After].Addr == ExitTarget;
          if (ExitsAfterLoop && LoopBody[0].Cond) {
            WhileCond = HighExpr::makeUnary(NdOp::BOOL_NOT, LoopBody[0].Cond);
            LoopExitTarget = ExitTarget;
            LoopBody.erase(LoopBody.begin());
          }
        }
      }

      if (!WhileCond)
        WhileCond = HighExpr::makeConst(1, 1);

      HighStmt WhileStmt;
      WhileStmt.Kind = StmtKind::While;
      // Only a hoisted header test transfers its native entry to the loop.
      // An always-true wrapper is synthetic: the first body statement keeps
      // its exact label, including for another backedge outside this region.
      // A self-loop (`jmp $`) leaves no body statement to carry the label.
      WhileStmt.Addr =
          LoopExitTarget || LoopBody.empty() ? Func.Body[HeaderIdx].Addr : 0;
      WhileStmt.LoopHeaderAddr = Target;
      WhileStmt.Cond = WhileCond;

      // Edge copies already have predecessor snapshots and exact branch
      // ownership. Preserve that order; moving every copy to the latch would
      // execute exit-edge values on backedges and break parallel PHI updates.

      WhileStmt.Body = std::move(LoopBody);

      va_t LoopExitAddr = 0;
      if (static_cast<size_t>(I) + 1 < Func.Body.size() &&
          !Func.Body[static_cast<size_t>(I) + 1].IsPhiCopy)
        LoopExitAddr = Func.Body[static_cast<size_t>(I) + 1].Addr;

      auto IsExit = [&](va_t GT) -> bool {
        if (GT == 0 || GT == InvalidVA)
          return false;
        if (LoopExitAddr != 0 && GT == LoopExitAddr)
          return true;
        return false;
      };

      auto ToLatch = [&](va_t GT) {
        return BareLatchEntered && GT == LatchAddr;
      };

      std::function<void(std::vector<HighStmt> &, va_t)> ConvertLoopGotos;
      ConvertLoopGotos = [&](std::vector<HighStmt> &Stmts, va_t Hdr) {
        for (auto &S : Stmts) {
          if (S.Kind == StmtKind::Goto && S.GotoTarget != 0 &&
              S.GotoTarget != InvalidVA) {
            if (IsExit(S.GotoTarget))
              S.Kind = StmtKind::Break;
            else if (S.GotoTarget == Hdr || ToLatch(S.GotoTarget))
              S.Kind = StmtKind::Continue;
          }
          if (S.Kind == StmtKind::If && S.Body.size() == 1 &&
              S.Body[0].Kind == StmtKind::Goto) {
            va_t GotoDest = S.Body[0].GotoTarget;
            if (IsExit(GotoDest))
              S.Body[0].Kind = StmtKind::Break;
            else if (GotoDest == Hdr || ToLatch(GotoDest))
              S.Body[0].Kind = StmtKind::Continue;
          }
          if (S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse ||
              S.Kind == StmtKind::Block) {
            ConvertLoopGotos(S.Body, Hdr);
            ConvertLoopGotos(S.ElseBody, Hdr);
          }
        }
      };
      ConvertLoopGotos(WhileStmt.Body, Target);
      if (BareLatchEntered) {
        HighStmt Anchor;
        Anchor.Kind = StmtKind::Block;
        Anchor.Addr = LatchAddr;
        WhileStmt.Body.push_back(std::move(Anchor));
      }

      Func.Body[HeaderIdx] = std::move(WhileStmt);
      for (size_t K = HeaderIdx + 1; K <= static_cast<size_t>(I); ++K) {
        Func.Body[K].Kind = StmtKind::Nop;
        Func.Body[K].Addr = 0;
      }
      FoundLoop = true;
      AM.SortedValid = false;
      I = static_cast<int>(HeaderIdx);
    }
    if (!FoundLoop)
      break;
    Func.Body.erase(std::remove_if(Func.Body.begin(), Func.Body.end(),
                                   [](const HighStmt &S) {
                                     return S.Kind == StmtKind::Nop;
                                   }),
                    Func.Body.end());
  }
}

} // namespace neverd
