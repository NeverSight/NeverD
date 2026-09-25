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
/// An unbuffered stream that forwards to a replaceable target.  HighCWriter
/// renders a function body into a buffer first, so the declarations it prints
/// before the body can follow what the body actually names.
class RedirectableStream : public llvm::raw_ostream {
public:
  explicit RedirectableStream(llvm::raw_ostream &Target) : Target(&Target) {
    SetUnbuffered();
  }
  /// Send output to \p Next; returns the previous target.
  llvm::raw_ostream *redirect(llvm::raw_ostream *Next) {
    flush();
    std::swap(Target, Next);
    return Next;
  }

private:
  void write_impl(const char *Ptr, size_t Size) override {
    Target->write(Ptr, Size);
  }
  uint64_t current_pos() const override { return Target->tell(); }
  llvm::raw_ostream *Target;
};

class HighCWriter {
public:
  static llvm::StringRef
  sourceConventionAttribute(SourceFunctionTypeHint::ConventionKind Convention);
  static std::string
  sourceParameterType(const SourceParameterTypeHint &Parameter);
  HighCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg, bool GuardAnalysisOnlyFunctions = true)
      : Out(OS), OS(Out), Opts(Opts), Dbg(Dbg),
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
  /// Emit a possibly multi-line rendered statement, indenting every line.
  void emitRenderedStatement(int Indent, llvm::StringRef Text);
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
  /// The C lvalue for the frame slot at \p E.  When \p Access is narrower
  /// than the slot, the access goes through the slot's address so it
  /// changes only those bytes.
  std::optional<std::string> namedFrameSlot(const HighExpr &E,
                                            const TypeRef &Access = {}) const;
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
  /// x64 catch funclets receive the parent frame in rdx (`Param` id 1). After
  /// attach that param is not the parent's rdx argument.
  bool isCatchFuncletParentFrame(const MedVar &V) const;
  const HighExpr *parentFrameStoredValue(const HighStmt &Stmt) const;
  std::optional<std::string> copyForwardSource(const HighExpr &E) const;
  bool isHiddenCopyForwardAssign(const HighStmt &Stmt) const;
  bool stmtHiddenFromC(const HighStmt &Stmt) const;
  bool stmtsEffectivelyEmpty(const std::vector<HighStmt> &Stmts) const;
  void collectCopyForward(const HighFunc &Func);

  //--- Binary expression rendering (HighCExprBinOp.cpp) ---
  std::string renderBinOp(const HighExpr &E, int ParentPrec);

  //--- State ---
  RedirectableStream Out;
  llvm::raw_ostream &OS;
  /// Declarations skipped because copy forwarding or liveness predicted the
  /// name would not be printed.  Emitted if the rendered body names it anyway.
  std::map<std::string, std::string> DeferredDecls;
  CEmitterOptions Opts;
  DebugContext *Dbg;
  bool GuardAnalysisOnlyFunctions;
  /// When false, emit recovered locals and statements without a C wrapper.
  /// Analysis-only functions nest that listing inside `#if 0` of the trap stub.
  bool EmitFunctionWrapper = true;

  std::set<std::string> ExternFuncs;
  std::map<std::string, const HighFunc *> DefinedFuncs;
  /// True when \p Name is a function this file or the image defines, not a
  /// libc routine of the same name (ntoskrnl implements its own `setjmp`).
  /// A call to the function being written when it is printed as void: its
  /// result cannot be assigned.
  bool isVoidSelfCall(const HighExpr &E) const {
    return E.Kind == ExprKind::Call && E.IntrinsicId == Intrinsic::None &&
           CurrentFunc && InferredVoid &&
           (E.CallAddr == CurrentFunc->Entry ||
            (!E.CallTarget.empty() && E.CallTarget == CurrentFunc->Name));
  }
  bool isOwnFunctionName(llvm::StringRef Name,
                         const std::vector<HighFunc> &Funcs);
  std::optional<std::set<std::string>> ImageFunctionNames;
  std::map<std::string, const HighFunc *> DefinedFunctionsByIdentifier;
  std::map<va_t, const HighFunc *> DefinedFunctionsByAddress;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::map<const HighFunc *, std::string> FunctionIdentifiers;
  std::map<std::string, std::string> FunctionIdentifiersBySourceName;
  std::map<std::string, std::string> ExternalFunctionIdentifiers;
  std::set<va_t> GotoTargets;
  /// How many gotos target each address in the current function.
  std::map<va_t, unsigned> GotoTargetUses;
  /// Labels already printed in the current function; C allows each once.
  std::set<va_t> EmittedLabels;
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
    /// For a slot that shares bytes with others: the name of the storage that
    /// holds them all, this slot's byte offset in it, and its access through
    /// it, `(*(T *)((char *)&outer + k))`.
    std::string Outer;
    int64_t OuterOffset = 0;
    std::string Interior;
    /// When no single slot covers an overlapping group, the group's first
    /// slot declares the storage as this many bytes.
    int64_t RegionBytes = 0;
    /// Width of the narrowest store at this displacement, 0 if none.
    unsigned MinStoreSize = 0;
  };
  std::map<int64_t, NamedFrameSlot> FrameSlots;
  std::map<std::string, int64_t> FrameAliases;
  /// Slot names and interior accesses that share storage with another slot;
  /// copy forwarding must not treat them as independent variables.
  std::set<std::string> SharedFrameStorage;
  std::map<std::string, std::string> CopyForward;
  std::map<int, std::string> ParamDisplayNames;
  bool InEHClauseBody = false;
  /// C++ destructor unwind funclets `ret` to the personality, not the parent.
  bool InCxxCleanupBody = false;

  struct ImageObject {
    std::string Name;
    TypeRef Type;
  };
  std::map<va_t, ImageObject> ImageObjects;

  std::vector<HiLoPair> HiLoPairs;
  /// Assignments whose multi-output intrinsic renders its own outputs.
  std::set<const HighStmt *> MultiOutputRenderedStmts;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
