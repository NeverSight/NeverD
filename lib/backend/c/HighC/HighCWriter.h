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

#include "../CIdentifier.h"

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
#include <tuple>
#include <vector>

namespace neverd {

/// Internal writer that converts HighIR functions to C source text.
/// Split across HighCEmitter.cpp (module-level orchestration),
/// HighCFuncWriter.cpp (function rendering), HighCStmtWriter.cpp (statement
/// rendering), HighCExprWriter.cpp (general expression rendering), and
/// HighCExprBinOp.cpp (binary operator rendering).
class HighCWriter {
public:
  HighCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg, bool GuardAnalysisOnlyFunctions = true)
      : OS(OS), Opts(Opts), Dbg(Dbg),
        GuardAnalysisOnlyFunctions(GuardAnalysisOnlyFunctions) {}

  //--- Module-level (HighCEmitter.cpp) ---
  void writeAll(const std::vector<HighFunc> &Funcs);
  void prepareFunctionIdentifiers(const std::vector<HighFunc> &Funcs);
  std::string functionIdentifier(const HighFunc &Func) const;
  std::string functionIdentifier(llvm::StringRef SourceName) const;
  void collectMemoryTypes(const std::vector<HighFunc> &Funcs);
  std::string memoryTypeName(const TypeRef &Ty) const;
  void writeIncludes(const std::vector<HighFunc> &Funcs);
  void writeMemoryHelpers();
  std::string
  memoryLoadExpr(const TypeRef &Ty, llvm::StringRef Addr,
                 NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
                 NdMemoryAddressSpace MemoryAddressSpace =
                     NdMemoryAddressSpace::Default) const;
  std::string
  memoryStoreExpr(const TypeRef &Ty, llvm::StringRef Addr, llvm::StringRef Val,
                  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
                  NdMemoryAddressSpace MemoryAddressSpace =
                      NdMemoryAddressSpace::Default) const;
  std::string atomicExchangeExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                 llvm::StringRef Val,
                                 NdMemoryOrdering MemoryOrdering,
                                 NdMemoryAddressSpace MemoryAddressSpace) const;
  std::string atomicFetchAddExpr(const TypeRef &Ty, llvm::StringRef Addr,
                                 llvm::StringRef Val,
                                 NdMemoryOrdering MemoryOrdering,
                                 NdMemoryAddressSpace MemoryAddressSpace) const;
  std::string
  atomicCompareExchangeExpr(const TypeRef &Ty, llvm::StringRef Addr,
                            llvm::StringRef Expected, llvm::StringRef Desired,
                            NdMemoryOrdering MemoryOrdering,
                            NdMemoryAddressSpace MemoryAddressSpace) const;
  void writeForwardDecls(const std::vector<HighFunc> &Funcs);
  void collectCallTargets(const std::vector<HighStmt> &Stmts,
                          std::set<std::string> &Targets);
  void collectCallTargetsExpr(const HighExpr &Expr,
                              std::set<std::string> &Targets);

  //--- Function rendering (HighCFuncWriter.cpp) ---
  bool isAnalysisOnlyFunction(const HighFunc &Func) const;
  void writeFunction(const HighFunc &Func);
  void writeAnalysisOnlyFunction(const HighFunc &Func);
  void writeFunctionProjection(const HighFunc &Func);
  void runAnalysisPasses(const HighFunc &Func);
  void writeExceptionAnnotation(const HighFunc &Func);
  void emitLocalDecls(const HighFunc &Func,
                      const std::set<std::string> &ParamNames);

  //--- Statement rendering (HighCStmtWriter.cpp) ---
  void writeStmt(const HighStmt &Stmt, int Indent);
  void writeStmts(const std::vector<HighStmt> &Stmts, int Indent);
  void emitIndent(int Indent);
  void collectGotoTargets(const std::vector<HighStmt> &Stmts);

  //--- Expression rendering (HighCExprWriter.cpp) ---
  std::string exprStr(const HighExpr &Expr, int ParentPrec = 0);
  std::string renderUnaryOp(const HighExpr &E, int ParentPrec);
  std::string renderCallExpr(const HighExpr &E);
  std::string varName(const MedVar &V);
  std::string constStr(uint64_t Val);
  std::string formatReturnExpr(const HighExpr &Expr);
  std::string collapseHiLo(const HighExpr &Expr);
  std::string unwrapCastVar(const HighExpr &E);

  //--- Binary expression rendering (HighCExprBinOp.cpp) ---
  std::string renderBinOp(const HighExpr &E, int ParentPrec);

  //--- State ---
  llvm::raw_ostream &OS;
  CEmitterOptions Opts;
  DebugContext *Dbg;
  bool GuardAnalysisOnlyFunctions;

  std::set<std::string> ExternFuncs;
  std::map<std::string, const HighFunc *> DefinedFuncs;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::map<const HighFunc *, std::string> FunctionIdentifiers;
  std::map<std::string, std::string> FunctionIdentifiersBySourceName;
  std::map<std::string, std::string> ExternalFunctionIdentifiers;
  std::set<va_t> GotoTargets;
  bool HasCIntrinsics = false;
  bool NeedsFEnvAccess = false;
  std::set<std::string> CIntrinsicNames;
  std::map<std::string, unsigned> MemoryTypes;
  std::set<std::pair<std::string, NdMemoryAddressSpace>> SegmentedMemoryTypes;
  std::set<std::tuple<std::string, NdMemoryOrdering, NdMemoryAddressSpace>>
      AtomicLoadTypes;
  std::set<std::tuple<std::string, NdMemoryOrdering, NdMemoryAddressSpace>>
      AtomicStoreTypes;
  bool HasSegmentedMemory = false;
  bool Has256BitInteger = false;
  bool Has512BitInteger = false;

  HighCAnalysisState Analysis;
  bool InferredVoid = false;
  TypeRef FuncReturnType;
  const HighFunc *CurrentFunc = nullptr;

  std::vector<HiLoPair> HiLoPairs;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
