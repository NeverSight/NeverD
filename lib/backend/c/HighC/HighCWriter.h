//===- HighCWriter.h - Internal HighIR C writer class ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal class declaration for the HighIR-to-C source writer.
/// This header is used only within the backend/c library; do not
/// install it in include/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
#define NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace neverd {

/// Internal writer that converts HighIR functions to C source text.
/// Split across HighCExprWriter.cpp (expression rendering),
/// HighCStmtWriter.cpp (statement/function rendering), and
/// HighCEmitter.cpp (module-level orchestration).
class HighCWriter {
public:
  HighCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg)
      : OS(OS), Opts(Opts), Dbg(Dbg) {}

  //--- Module-level (HighCEmitter.cpp) ---
  void writeAll(const std::vector<HighFunc> &Funcs);
  void collectMemoryTypes(const std::vector<HighFunc> &Funcs);
  void writeIncludes(const std::vector<HighFunc> &Funcs);
  void writeMemoryHelpers();
  void writeForwardDecls(const std::vector<HighFunc> &Funcs);
  void collectCallTargets(const std::vector<HighStmt> &Stmts,
                          std::set<std::string> &Targets);
  void collectCallTargetsExpr(const HighExpr &Expr,
                              std::set<std::string> &Targets);

  //--- Statement / function rendering (HighCStmtWriter.cpp) ---
  void writeFunction(const HighFunc &Func);
  void writeStmt(const HighStmt &Stmt, int Indent);
  void writeStmts(const std::vector<HighStmt> &Stmts, int Indent);
  void emitIndent(int Indent);
  void collectGotoTargets(const std::vector<HighStmt> &Stmts);

  //--- Statement / function rendering helpers (HighCStmtWriter.cpp) ---
  void runAnalysisPasses(const HighFunc &Func);
  void emitLocalDecls(const HighFunc &Func,
                      const std::set<std::string> &ParamNames);

  //--- Expression rendering (HighCExprWriter.cpp) ---
  std::string exprStr(const HighExpr &Expr, int ParentPrec = 0);
  std::string renderBinOp(const HighExpr &E, int ParentPrec);
  std::string renderUnaryOp(const HighExpr &E, int ParentPrec);
  std::string renderCallExpr(const HighExpr &E);
  std::string varName(const MedVar &V);
  std::string constStr(uint64_t Val);
  std::string formatReturnExpr(const HighExpr &Expr);
  std::string collapseHiLo(const HighExpr &Expr);
  std::string unwrapCastVar(const HighExpr &E);
  std::string memoryTypeName(const TypeRef &Ty) const;
  std::string memoryLoadExpr(const TypeRef &Ty, llvm::StringRef Addr) const;
  std::string memoryStoreExpr(const TypeRef &Ty, llvm::StringRef Addr,
                              llvm::StringRef Val) const;

  //--- State ---
  llvm::raw_ostream &OS;
  CEmitterOptions Opts;
  DebugContext *Dbg;

  std::set<std::string> ExternFuncs;
  std::map<std::string, const HighFunc *> DefinedFuncs;
  std::set<va_t> GotoTargets;
  bool HasCIntrinsics = false;
  std::set<std::string> CIntrinsicNames;
  std::map<std::string, unsigned> MemoryTypes;

  HighCAnalysisState Analysis;
  bool InferredVoid = false;
  TypeRef FuncReturnType;
  const HighFunc *CurrentFunc = nullptr;

  std::vector<HiLoPair> HiLoPairs;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
