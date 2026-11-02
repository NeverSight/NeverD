//===- HighCFSimplifyDetail.h - CF simplification internals ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal types and function declarations shared between the
/// control-flow simplification files: HighCFSimplify.cpp,
/// HighCFSimplifyIfElse.cpp, HighLoopRecovery.cpp, and
/// HighIfChainToSwitch.cpp.
///
/// This header is an implementation detail of the high/ library and
/// should NOT be included by code outside lib/ir/high/structure/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_STRUCTURE_HIGHCFSIMPLIFYDETAIL_H
#define NEVERD_IR_HIGH_STRUCTURE_HIGHCFSIMPLIFYDETAIL_H

#include "neverd/ir/high/HighIR.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace neverd {

/// Address-to-statement-index map used by multiple CF simplification passes.
struct AddrMap {
  std::unordered_map<va_t, size_t> Idx;
  std::vector<std::pair<va_t, size_t>> Sorted;
  bool SortedValid = false;

  /// A synthetic always-true loop executes its first instruction immediately.
  /// It can share that continuation without acquiring another emitted label.
  /// A conditional loop or any prefix before the header is not equivalent.
  static va_t entryAddress(const HighStmt &S) {
    if (S.Addr)
      return S.Addr;
    if (S.Kind == StmtKind::While && S.LoopHeaderAddr &&
        S.LoopHeaderAddr != InvalidVA && S.Cond &&
        S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal &&
        S.Cond->Operands.empty() &&
        S.Cond->MemoryOrdering == NdMemoryOrdering::None &&
        S.Cond->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        !S.Body.empty() && !S.Body.front().IsPhiCopy &&
        S.Body.front().Addr == S.LoopHeaderAddr)
      return S.LoopHeaderAddr;
    return 0;
  }

  void rebuild(const std::vector<HighStmt> &Stmts) {
    Idx.clear();
    Idx.reserve(Stmts.size());
    for (size_t I = 0; I < Stmts.size(); ++I)
      if (const va_t Address = entryAddress(Stmts[I]))
        Idx.try_emplace(Address, I);
    SortedValid = false;
  }

  void ensureSorted() {
    if (SortedValid)
      return;
    Sorted.clear();
    Sorted.reserve(Idx.size());
    for (auto &[Addr, I] : Idx)
      Sorted.push_back({Addr, I});
    std::sort(Sorted.begin(), Sorted.end());
    SortedValid = true;
  }
};

struct MedFunc;

/// Convert layout-backward gotos only after proving their native loop region.
/// Defined in HighLoopRecovery.cpp.
void detectAndConvertLoops(HighFunc &Func,
                           const std::unordered_map<va_t, int> &AddrToBlock,
                           const MedFunc &Med, bool IsMega);

/// Recover switch statements from if-chains comparing the same variable.
/// Defined in HighIfChainToSwitch.cpp.
void recoverSwitchStatements(HighFunc &Func);
/// True when control never reaches the statement after \p S: a return,
/// jump, noreturn call or endless loop.
bool highStmtEndsItsBlock(const HighStmt &S);
/// Whether running \p S itself, not the statements nested in it, may raise
/// an exception: only integer and boolean work on registers cannot.
bool highStmtMayFault(const HighStmt &S);
void cleanupGuardBeforeSwitch(HighFunc &Func);

/// Fold if(cond){goto} patterns into if/else trees, up to \p MaxPasses
/// iterations.  Defined in HighCFSimplifyIfElse.cpp.
void structureIfElse(HighFunc &Func, int MaxPasses,
                     const MedFunc *Med = nullptr);

/// Negate one condition without reevaluating its operands. Equality trees
/// retain readable comparisons; other expressions keep an explicit Boolean NOT.
ExprPtr invertHighCond(ExprPtr Condition);

/// True when exceptions enter or leave \p Func in ways its statement tree does
/// not spell: a language exception table, an exception statement or clause,
/// or a MedIR block with exceptional edges. The unwinder enters catch
/// continuations and landing pads without a goto, so a fold that deletes or
/// moves statements because two copies compare equal leaves such a function
/// unchanged. Defined in HighCFSimplifyIfElse.cpp.
bool hasExceptionFlow(const HighFunc &Func, const MedFunc *Med);

} // namespace neverd

#endif // NEVERD_IR_HIGH_STRUCTURE_HIGHCFSIMPLIFYDETAIL_H
