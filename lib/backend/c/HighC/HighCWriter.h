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
#include <optional>
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
  static llvm::StringRef
  sourceConventionAttribute(SourceFunctionTypeHint::ConventionKind Convention);
  static std::string
  sourceParameterType(const SourceParameterTypeHint &Parameter);
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
  void collectImageObjects(const std::vector<HighFunc> &Funcs);
  void writeImageObjects();
  std::optional<va_t> constAddress(const HighExpr &E) const;
  std::optional<uint64_t> foldReadonlyScalar(va_t Addr, uint16_t Size) const;
  std::optional<std::string> imageObjectName(va_t Addr) const;
  bool isImageDataAddress(va_t Addr) const;
  void noteImageObject(va_t Addr, const TypeRef &Ty, bool Written);
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
  void collectNamedFrameSlots(const HighFunc &Func);
  size_t emittedParamCount(const HighFunc &Func) const;
  std::vector<size_t> emittedParamIndices(const HighFunc &Func) const;

  //--- Statement rendering (HighCStmtWriter.cpp) ---
  void writeStmt(const HighStmt &Stmt, int Indent);
  void writeStmts(const std::vector<HighStmt> &Stmts, int Indent);
  void writeTryBody(const std::vector<HighStmt> &Stmts, int Indent);
  bool isCompilerEHConstant(const HighExpr &Val) const;
  void emitIndent(int Indent);
  void collectGotoTargets(const std::vector<HighStmt> &Stmts);

  //--- Expression rendering (HighCExprWriter.cpp) ---
  std::string exprStr(const HighExpr &Expr, int ParentPrec = 0);
  std::string renderUnaryOp(const HighExpr &E, int ParentPrec);
  std::string renderCallExpr(const HighExpr &E);
  std::string renderSourceCallExpr(const HighExpr &E);
  const HighFunc *sourceCallDefinition(const SourceCallTypeHint &Hint,
                                       llvm::StringRef Name) const;
  std::string varName(const MedVar &V) const;
  TypeRef declaredParamType(const MedVar &V) const;
  bool pointerNeedsIntegerView(const TypeRef &Ty) const;
  const HighExpr *unwrapIntegerView(const HighExpr *E) const;
  std::optional<int64_t> frameDisplacement(const HighExpr &E) const;
  std::optional<std::string> namedFrameSlot(const HighExpr &E) const;
  bool isNamedFrameMemory(const HighExpr &E) const;
  std::string constStr(uint64_t Val);
  std::string formatReturnExpr(const HighExpr &Expr);
  std::string collapseHiLo(const HighExpr &Expr);
  std::string unwrapCastVar(const HighExpr &E);
  std::string invertCondStr(const HighExpr &E);
  std::string copyForwardName(const std::string &Name) const;
  std::optional<std::string>
  forwardedStoreValue(const HighExpr &Addr, const std::string &Printed) const;
  bool isCopyForwardDestination(const MedVar &V) const;
  bool isParamCopy(const HighExpr &E) const;
  std::optional<std::string> copyForwardSource(const HighExpr &E) const;
  bool isHiddenCopyForwardAssign(const HighStmt &Stmt) const;
  bool stmtHiddenFromC(const HighStmt &Stmt) const;
  bool stmtsEffectivelyEmpty(const std::vector<HighStmt> &Stmts) const;
  void collectCopyForward(const HighFunc &Func);

  //--- Binary expression rendering (HighCExprBinOp.cpp) ---
  std::string renderBinOp(const HighExpr &E, int ParentPrec);

  //--- State ---
  llvm::raw_ostream &OS;
  CEmitterOptions Opts;
  DebugContext *Dbg;
  bool GuardAnalysisOnlyFunctions;
  /// When false, emit recovered locals and statements without a C wrapper.
  /// Analysis-only functions nest that listing inside `#if 0` of the trap stub.
  bool EmitFunctionWrapper = true;

  std::set<std::string> ExternFuncs;
  std::map<std::string, const HighFunc *> DefinedFuncs;
  std::map<std::string, const HighFunc *> DefinedFunctionsByIdentifier;
  std::map<va_t, const HighFunc *> DefinedFunctionsByAddress;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::map<const HighFunc *, std::string> FunctionIdentifiers;
  std::map<std::string, std::string> FunctionIdentifiersBySourceName;
  std::map<std::string, std::string> ExternalFunctionIdentifiers;
  std::set<va_t> GotoTargets;
  bool HasCIntrinsics = false;
  bool NeedsFEnvAccess = false;
  std::set<std::string> CIntrinsicNames;
  bool NeedsObjCRuntime = false;
  bool NeedsObjCSuper2 = false;
  bool NeedsDarwinLocks = false;
  bool NeedsDarwinBlocks = false;
  bool NeedsDarwinStackGuard = false;
  bool NeedsDarwinStackFailure = false;
  bool NeedsSwiftStringBridge = false;
  std::set<std::string> SwiftBooleanProjectionImports;
  bool NeedsSwiftStringFromNSString = false;
  std::set<std::string> SourceObjectAddressHelpers;
  std::set<std::string> SourceBlockIsaNames;
  std::set<const HighFunc *> SourceAddressDefinitions;
  struct SourceNativeDeclaration {
    const SourceFunctionTypeHint *Signature;
    std::optional<unsigned> VariadicFixedCount;
    bool WeakImport = false;
  };
  std::map<std::string, SourceNativeDeclaration> SourceNativeSignatures;
  std::map<std::string, std::string> SourceRuntimeLinkNames;
  std::map<std::string, std::string> SourceRuntimeDataIdentifiers;
  std::map<std::string, bool> SourceCallTermination;
  std::set<std::string> ConflictingSourceNativeSignatures;
  std::map<std::string, unsigned> MemoryTypes;
  std::map<std::string, unsigned> PartialIntegerBytes;
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

  struct NamedFrameSlot {
    std::string Name;
    TypeRef Type;
    bool AddressTaken = false;
    bool UsedAsMemory = false;
  };
  std::map<int64_t, NamedFrameSlot> FrameSlots;
  std::map<std::string, int64_t> FrameAliases;
  std::map<std::string, std::string> CopyForward;
  std::map<int, std::string> ParamDisplayNames;
  bool InEHClauseBody = false;

  struct ImageObject {
    std::string Name;
    TypeRef Type;
  };
  std::map<va_t, ImageObject> ImageObjects;

  std::vector<HiLoPair> HiLoPairs;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
