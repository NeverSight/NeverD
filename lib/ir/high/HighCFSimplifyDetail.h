//===- HighCFSimplifyDetail.h - CF simplification internals ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal types and function declarations shared between the
/// control-flow simplification files: HighCFSimplify.cpp,
/// HighLoopRecovery.cpp, and HighIfChainToSwitch.cpp.
///
/// This header is an implementation detail of the high/ library and
/// should NOT be included by code outside lib/ir/high/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHCFSIMPLIFYDETAIL_H
#define NEVERD_IR_HIGH_HIGHCFSIMPLIFYDETAIL_H

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

  void rebuild(const std::vector<HighStmt> &Stmts) {
    Idx.clear();
    Idx.reserve(Stmts.size());
    for (size_t I = 0; I < Stmts.size(); ++I)
      if (Stmts[I].Addr != 0)
        Idx.try_emplace(Stmts[I].Addr, I);
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

/// Detect backward gotos and convert them into while loops.
/// Defined in HighLoopRecovery.cpp.
void detectAndConvertLoops(HighFunc &Func,
                           const std::unordered_map<va_t, int> &AddrToBlock,
                           bool IsMega);

/// Recover switch statements from if-chains comparing the same variable.
/// Defined in HighIfChainToSwitch.cpp.
void recoverSwitchStatements(HighFunc &Func);

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHCFSIMPLIFYDETAIL_H
