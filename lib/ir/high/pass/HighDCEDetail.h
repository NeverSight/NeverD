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

#include <unordered_map>
#include <unordered_set>

namespace neverd {

/// Short alias used pervasively in DCE / copy-prop code.
inline VarKey VK(const MedVar &V) { return varKey(V); }

//===----------------------------------------------------------------------===//
// Shared helpers  (defined in HighCopyProp.cpp)
//===----------------------------------------------------------------------===//

void filterStableCopyCandidates(const std::vector<HighStmt> &Stmts,
                                VarKeyMap<ExprPtr> &Candidates);
void resolveCopyChains(VarKeyMap<ExprPtr> &Map);
/// Drop a jump to the loop right after it when falling in does the same.
void eliminateGotoToLoop(std::vector<HighStmt> &Stmts);
void rewriteRhsVars(std::vector<HighStmt> &Stmts,
                    const VarKeyMap<ExprPtr> &Map);
void countExprVarUses(const ExprPtr &E, VarKeyMap<int> &Uses,
                      HighExprSet &Seen);
/// Single-use inlining must count a shared expression once per use site,
/// rather than once per DAG node. Counts saturate at two because only the
/// distinction between one and multiple uses matters here.
void countExprVarUsesUpToTwo(
    const ExprPtr &E, VarKeyMap<int> &Uses,
    std::unordered_map<const HighExpr *, uint8_t> &Visits);
/// Reads and effects belong to their original statement. Without an ordering
/// proof, expression propagation cannot move or duplicate them at a use.
bool containsNonMovableEffect(const ExprPtr &E);
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
void elimUnreadPrivateFrameStores(HighFunc &Func, Arch Architecture);
void forwardPrivateFrameLoads(HighFunc &Func, Arch Architecture);
/// Forward scalar copies through nonescaping private slots on the shared
/// source-flow graph. Calls must already carry complete source ABI bindings.
/// Every reaching context must agree; cycles and exhausted budgets do nothing.
bool forwardBoundPrivateFrameCopies(HighFunc &Func, Arch Architecture);
void narrowSourceConcatLocals(HighFunc &Func);
/// Define each register value whose every read selects the same low bytes as
/// that zero-extended prefix, dropping only pure upper bytes.
void narrowUnreadRegisterBytes(HighFunc &Func,
                               Arch Architecture = Arch::Unknown);
/// Replace with zero each unknown entry register or undefined value that no
/// observable result reads a byte of (HighDemandedBytes.cpp).
void zeroUndemandedUnknownBytes(HighFunc &Func, Arch Architecture);
/// Whether evaluating an unused integer value can be discarded without a
/// memory access, call, or trap. This never supplies values for unknown bits.
bool discardableIntegerValue(const ExprPtr &Root, size_t &Budget);
/// Whether an integer value may go unevaluated: arithmetic, bit operations,
/// comparisons and selections of locals and constants, which cannot access
/// memory, call, divide or trap.
bool harmlessIntegerValue(const ExprPtr &Root, size_t &Budget);

//===----------------------------------------------------------------------===//
// Expression simplification  (defined in HighExprSimplify.cpp)
//===----------------------------------------------------------------------===//

void simplifyAllExprs(std::vector<HighStmt> &Stmts);
void removeUnreachableCode(std::vector<HighStmt> &Stmts);
/// Normalize native entry ownership before dead edge copies lose their PHI
/// provenance. Uses the same coalescer as final source normalization.
void coalesceBranchEntryStatements(std::vector<HighStmt> &Stmts);
void eliminateUnusedValues(std::vector<HighStmt> &Stmts);
void eliminateUnusedValues(HighFunc &Func);

//===----------------------------------------------------------------------===//
// Variable renaming and post-rename cleanup  (defined in HighVarRename.cpp)
//===----------------------------------------------------------------------===//

void renameVars(std::vector<HighStmt> &Stmts);
void postRenameCleanup(HighFunc &Func);
void postRenameCleanup(std::vector<HighStmt> &Stmts);

} // namespace neverd

#endif // NEVERD_IR_HIGH_PASS_HIGHDCEDETAIL_H
