//===- HighCPasses.h - High C emitter analysis passes --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Analysis passes for the HighIR C emitter (dead stores, void analysis).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_PASS_HIGHC_HIGHCPASSES_H
#define NEVERD_BACKEND_C_PASS_HIGHC_HIGHCPASSES_H
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/high/HighIR.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace neverd {

using VarNameFn = std::function<std::string(const MedVar &)>;
using ExprStrFn = std::function<std::string(const HighExpr &)>;

struct HighCAnalysisState {
  std::set<const HighStmt *> DeadStmts;
  std::set<std::string> DeadVars;
  std::map<const HighExpr *, std::string> AddressKeys;
  std::map<std::string, std::string> StoreFwd;
  std::map<std::string, std::set<std::string>> StoreFwdDeps;
  std::map<std::string, std::set<std::string>> ForwardedAddressDeps;
};

void analyzeDeadStores(HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn);

void analyzeStoreForwarding(HighCAnalysisState &State, const HighFunc &Func,
                            VarNameFn VarFn, ExprStrFn ExprFn);

bool analyzeVoidReturn(const HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn);

void collectUsedVarsExpr(const HighExpr &Expr,
                         std::map<std::string, TypeRef> &Vars, VarNameFn VarFn);

void collectUsedVars(const std::vector<HighStmt> &Stmts,
                     std::map<std::string, TypeRef> &Vars, VarNameFn VarFn);

void analyzeVoidDeadChain(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn);

struct HiLoPair {
  std::string Lo, Hi;
  std::string CollapseExpr;
  const void *Stmt;
};

struct HiLoCollapseResult {
  std::string Collapsed;
  std::string DeadLo, DeadHi;
  const void *DeadStmt = nullptr;
};

HiLoCollapseResult
tryCollapseHiLo(const HighExpr &Expr, const std::vector<HiLoPair> &Pairs,
                std::function<std::string(const HighExpr &)> UnwrapFn);

} // namespace neverd

#endif // NEVERD_BACKEND_C_PASS_HIGHC_HIGHCPASSES_H
