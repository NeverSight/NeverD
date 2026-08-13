//===- HighDCEDetail.h - Shared DCE utilities -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal types and utilities shared between the dead-code elimination
/// framework (HighDCE.cpp) and copy propagation passes (HighCopyProp.cpp).
///
/// This header is an implementation detail of the high/ library and
/// should NOT be included by code outside lib/ir/high/pass/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_PASS_HIGHDCEDETAIL_H
#define NEVERD_IR_HIGH_PASS_HIGHDCEDETAIL_H

#include "neverd/ir/high/HighIR.h"

#include <unordered_set>

namespace neverd {

/// Short alias used pervasively in DCE / copy-prop code.
inline VarKey VK(const MedVar &V) { return varKey(V); }

//===----------------------------------------------------------------------===//
// Shared helpers  (defined in HighCopyProp.cpp)
//===----------------------------------------------------------------------===//

void resolveCopyChains(VarKeyMap<ExprPtr> &Map);
void rewriteRhsVars(std::vector<HighStmt> &Stmts,
                    const VarKeyMap<ExprPtr> &Map);
void countExprVarUses(const ExprPtr &E, VarKeyMap<int> &Uses,
                      std::unordered_set<const HighExpr *> &Seen);
void inlineSingleDefs(std::vector<HighStmt> &Stmts,
                      const VarKeyMap<ExprPtr> &Defs);

//===----------------------------------------------------------------------===//
// Copy propagation phases  (defined in HighCopyProp.cpp)
//===----------------------------------------------------------------------===//

void resolveRegAliases(std::vector<HighStmt> &Stmts);
void foldCopyChains(HighFunc &Func);
void propagatePhiCopies(std::vector<HighStmt> &Stmts);
void foldMultiUseCopies(std::vector<HighStmt> &Stmts);
void inlineSingleDefSingleUse(std::vector<HighStmt> &Stmts);
void scopedCopyPropagation(std::vector<HighStmt> &Stmts);
void eliminateRegAliasCopies(HighFunc &Func);
void eliminateLoopAliases(std::vector<HighStmt> &Stmts);

//===----------------------------------------------------------------------===//
// Dead store elimination  (defined in HighDeadStoreElim.cpp)
//===----------------------------------------------------------------------===//

void elimConsecutiveDeadStores(std::vector<HighStmt> &Stmts);
void eliminateRedundantStackStores(HighFunc &Func, Arch TargetArch);

//===----------------------------------------------------------------------===//
// Expression simplification  (defined in HighExprSimplify.cpp)
//===----------------------------------------------------------------------===//

void simplifyAllExprs(std::vector<HighStmt> &Stmts);

//===----------------------------------------------------------------------===//
// Variable renaming and post-rename cleanup  (defined in HighVarRename.cpp)
//===----------------------------------------------------------------------===//

void renameVars(std::vector<HighStmt> &Stmts);
void postRenameCleanup(std::vector<HighStmt> &Stmts);

} // namespace neverd

#endif // NEVERD_IR_HIGH_PASS_HIGHDCEDETAIL_H
