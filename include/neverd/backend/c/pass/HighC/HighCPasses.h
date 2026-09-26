//===- HighCPasses.h - High C emitter analysis passes --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Analysis passes and shared helpers for the HighIR C emitter.
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
/// Printed C call arity. `numeric_limits<size_t>::max()` keeps every operand.
using CallArgLimitFn = std::function<size_t(const HighExpr &)>;

inline bool isNamedValueExpr(const HighExpr &E) {
  return E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi;
}

struct HighCAnalysisState {
  std::set<const HighStmt *> DeadStmts;
  std::set<std::string> DeadVars;
  std::map<const HighExpr *, std::string> AddressKeys;
  // True only when every memory access is an exact, nonoverlapping slot in
  // the function's private frame and no frame address escapes.
  bool CanElideFrameStores = false;
  std::map<std::string, std::string> StoreFwd;
  // The semantic address key remains stable when the C writer later projects
  // a raw frame expression as a named local.
  std::map<std::string, std::string> StoreFwdByAddressKey;
  std::map<std::string, std::set<std::string>> StoreFwdDeps;
  std::map<std::string, std::set<std::string>> ForwardedAddressDeps;
  std::set<std::string> AssignedVars;
  /// Call assigns whose destination is never read.  Print the call as a
  /// statement so unused `v0` temps are not declared.
  std::set<const HighStmt *> OmittedCallResults;
};

void analyzeDeadStores(HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn);

void analyzeStoreForwarding(HighCAnalysisState &State, const HighFunc &Func,
                            VarNameFn VarFn, ExprStrFn ExprFn);

bool analyzeVoidReturn(const HighCAnalysisState &State, const HighFunc &Func,
                       VarNameFn VarFn, ExprStrFn ExprFn);

/// True for a known libc noreturn call or architectural x86 fast-fail.
/// Caller return shapes cannot prove that an unknown callee never returns.
bool isNoreturnCallExpr(const HighExpr &E);

void collectUsedVarsExpr(const HighExpr &Expr,
                         std::map<std::string, TypeRef> &Vars, VarNameFn VarFn,
                         CallArgLimitFn ArgLimit = {});

void collectUsedVars(const std::vector<HighStmt> &Stmts,
                     std::map<std::string, TypeRef> &Vars, VarNameFn VarFn,
                     CallArgLimitFn ArgLimit = {});

void analyzeVoidDeadChain(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn);

/// Drop assignments whose destination is never read and whose value has no
/// observable effect.  Segmented FS/GS loads are included: an unused TEB/TLS
/// read is not a reason to keep the temp, while a used GS load must stay.
void analyzeUnusedAssigns(HighCAnalysisState &State, const HighFunc &Func,
                          VarNameFn VarFn, CallArgLimitFn ArgLimit = {});

/// Calls whose result is never read print as statements, not `vN = call()`.
void analyzeUnusedCallResults(HighCAnalysisState &State, const HighFunc &Func,
                              VarNameFn VarFn, CallArgLimitFn ArgLimit = {});

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
