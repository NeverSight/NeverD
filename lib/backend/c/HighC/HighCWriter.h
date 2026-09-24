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

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
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
  void invalidateJoinPhiFrameAliases(const HighFunc &Func);
  size_t emittedParamCount(const HighFunc &Func) const;
  std::vector<size_t> emittedParamIndices(const HighFunc &Func) const;

  //--- Statement rendering (HighCStmtWriter.cpp) ---
  void writeStmt(const HighStmt &Stmt, int Indent);
  void writeCxxThrowExpr(const HighStmt &Stmt, const HighExpr &ThrowCall);
  void writeStmts(const std::vector<HighStmt> &Stmts, int Indent,
                 size_t End = static_cast<size_t>(-1));
  bool tryWriteCursorForLoop(const std::vector<HighStmt> &Stmts, size_t I,
                             size_t End, int Indent, size_t &Last);
  void writeStmtsIsolated(const std::vector<HighStmt> &Stmts, int Indent);
  void writeTryBody(const std::vector<HighStmt> &Stmts, int Indent);
  void writeTryBodyUnisolated(const std::vector<HighStmt> &Stmts, int Indent);
  bool isCompilerEHConstant(const HighExpr &Val) const;
  void emitIndent(int Indent);
  void collectGotoTargets(const std::vector<HighStmt> &Stmts,
                         bool DropTrailingGoto = false);

  //--- Expression rendering (HighCExprWriter.cpp) ---
  std::string exprStr(const HighExpr &Expr, int ParentPrec = 0);
  /// Address used by a load/store/atomic. Peels integer views and prints
  /// `base + imm` without sanitizer wrap. Value uses of the same add still wrap.
  std::string addrStr(const HighExpr &Expr, int ParentPrec = 0);
  std::string renderUnaryOp(const HighExpr &E, int ParentPrec);
  std::string resolvedCallTarget(const HighExpr &E) const;
  std::string renderCallExpr(const HighExpr &E);
  std::string renderSourceCallExpr(const HighExpr &E);
  const HighFunc *sourceCallDefinition(const SourceCallTypeHint &Hint,
                                       llvm::StringRef Name) const;
  std::string varName(const MedVar &V) const;
  std::string debugNameForDisplacement(va_t Entry, int64_t Disp) const;
  TypeRef debugTypeForDisplacement(va_t Entry, int64_t Disp) const;
  TypeRef declaredParamType(const MedVar &V) const;
  TypeRef debugParamType(const MedVar &V) const;
  TypeRef declaredRecordPointee(const HighExpr &Base) const;
  std::optional<std::pair<const HighExpr *, uint64_t>>
  typedPointerOffset(const HighExpr &Addr) const;
  /// `base[index]` when Addr is `typed_ptr + index * sizeof(*typed_ptr)`.
  /// ElemType is the loaded pointer/element, not the address of `base`.
  struct TypedIndexAccess {
    std::string Base;
    std::string Index;
    TypeRef ElemType;
  };
  std::optional<TypedIndexAccess> typedIndexAccess(const HighExpr &Addr);
  std::optional<std::string> typedMemberAccess(const HighExpr &Addr,
                                               uint16_t AccessSize = 0,
                                               bool EnterNestedAtZero = true) const;
  std::optional<std::string> typedMemberAddress(const HighExpr &Addr) const;
  TypeRef typedMemberType(const HighExpr &Addr, uint16_t AccessSize = 0) const;
  /// Frame displacement that is a unique interior field of a named record
  /// slot (`record.p` at wrapper+8). Exact slot addresses stay unnamed here.
  std::optional<std::string>
  frameTypedMemberAccess(int64_t Disp, uint16_t AccessSize = 0) const;
  TypeRef frameTypedMemberType(int64_t Disp,
                               uint16_t AccessSize = 0) const;
  /// Frame displacement of `Base` (or of the slot under `Load(slot)`) plus
  /// `Rel`. Used so a call-site ArgList overlay wins over IR TPtr at +8.
  std::optional<int64_t> frameOverlayDisplacement(const HighExpr &Base,
                                                  uint64_t Rel) const;
  /// `var = 43` on a 16-byte ArgList slot is `var.types_ = 43` when the
  /// stored scalar is smaller than the record and field 0 is int/ptr.
  std::optional<std::string> scalarRecordFieldDest(llvm::StringRef SlotName,
                                                   const HighExpr &Val,
                                                   llvm::StringRef PrintedValue = {}) const;
  /// Integer-return Call stuffed into a pointer field of a slot that a call
  /// retyped (`ArgList.values_` over `TPtr.p`). Const / `p = 0` / integer
  /// temps keep the PDB name.
  std::optional<std::string>
  callOverlayIntegerMemberStore(llvm::StringRef Member,
                                const HighExpr &Val) const;
  bool isIntegerOverlayStore(const HighExpr &Val) const;
  bool pointerNeedsIntegerView(const TypeRef &Ty) const;
  /// Pointer object used as an `INDIR_CALL` base, without `(uintptr_t)`.
  std::string pointerObjectStr(const HighExpr &E);
  /// Pointer-sized Load chain used as a callee: `*(void **)p` / `**(void ***)p`.
  /// A loaded vtable slot `Load(Load(obj)+imm)` is
  /// `*(void **)((uintptr_t)(*(void **)(obj)) + imm)`, not integer soup.
  std::string indirectCalleeStr(const HighExpr &E);
  const HighExpr *unwrapIntegerView(const HighExpr *E) const;
  const HighExpr *forwardedExpr(const HighExpr *E) const;
  /// True when \p E prints as an unsigned integer of exactly \p Width bytes.
  /// Widening views and untyped add/sub/mul stay wrapped.
  bool isSameWidthUnsigned(const HighExpr &E, uint16_t Width) const;
  std::optional<FunctionSym> debugCallee(const HighExpr &E) const;
  TypeRef expectedDebugCallArgType(const FunctionSym &FS, size_t Index) const;
  TypeRef displayCallArgType(const HighExpr &Call, size_t Index) const;
  /// Display-only printed arity from TPI/PDB or \ref msvcAtlCallee.
  /// ATL/STL rows live in MsvcAtlCallees.def; `_ctor` keeps this plus a
  /// pointer/string/fill source and drops leftover register clobbers.
  size_t debugCallArgLimit(const HighExpr &E) const;
  void collectUnknownOnlyNames(const HighFunc &Func);
  void collectCtorSourceNames(const HighFunc &Func);
  bool isUnknownCallOperand(const HighExpr *Op) const;
  bool isCtorSourceExpr(const HighExpr *Op) const;
  bool isCtorDisplayOperand(const HighExpr *Op) const;
  TypeRef knownCallReturnType(const HighExpr &E) const;
  const HighExpr *typedCallResult(const HighExpr *E) const;
  const HighExpr *peelIntegerViewOps(const HighExpr *E) const;
  /// Peel zext/trunc around a named scalar or field load for x86 intrinsic
  /// snippets. Keep wraps on add/sub/mul.
  std::string intrinsicOperandStr(const HighExpr &E);
  /// Cast / zext / `SUBBYTES` 0 of a Var/Phi. Not add/sub/mul, Call, or Load.
  bool isIntegerViewOfScalar(const HighExpr &E) const;
  std::string exprStrAsTypedArg(const HighExpr &E, const TypeRef &Expected);
  bool looksLikeHiddenSretOperand(const HighExpr *Op) const;
  bool debugExternUsesHiddenSret(const FunctionSym &FS,
                                 llvm::StringRef ExternName) const;
  void noteDebugExternCallSret(const std::string &Name, const FunctionSym &FS,
                               const HighExpr &Call);
  std::string debugExternPrototype(const FunctionSym &FS,
                                   const std::string &Identifier,
                                   llvm::StringRef ExternName = {}) const;
  static std::string debugSignatureKey(const FunctionSym &FS);
  /// Prefer more TPI params / a typed or sret return when two decorations
  /// collapse to one C identifier. Display-only; does not invent call args.
  static int debugSymRichness(const FunctionSym &FS);
  static TypeRef cDisplayType(const TypeRef &Ty);
  std::optional<int64_t> frameDisplacement(const HighExpr &E) const;
  std::optional<std::string> namedFrameSlot(const HighExpr &E) const;
  bool isNamedFrameMemory(const HighExpr &E) const;
  std::string constStr(uint64_t Val);
  std::string formatReturnExpr(const HighExpr &Expr);
  std::string collapseHiLo(const HighExpr &Expr);
  std::string unwrapCastVar(const HighExpr &E);
  std::string invertCondStr(const HighExpr &E);
  std::string condStr(const HighExpr &E);
  /// `!(a <= b && a != b)` is `a >= b`. When both IfElse arms print, Hex-Rays
  /// prefers `if (b > a) else-arm else then-arm`.
  std::optional<std::string> preferGreaterIfElseCond(const HighExpr &E);
  std::string copyForwardName(const std::string &Name) const;
  std::optional<std::string>
  forwardedStoreValue(const HighExpr &Addr, const std::string &Printed) const;
  bool isCopyForwardDestination(const MedVar &V) const;
  bool isParamCopy(const HighExpr &E) const;
  /// True when `Name` is an emitted parameter (`arg0`, `this`, `item`, …).
  bool isEmittedParamName(llvm::StringRef Name) const;
  /// Parameter display names (`item`, injected `result`) cannot also name a
  /// frame home. The incoming C parameter already owns that identifier.
  bool isReservedParamDisplayName(llvm::StringRef Name) const;
  bool isAddressTakenSlot(llvm::StringRef Name) const;
  bool hidesAddressTakenParamHome(llvm::StringRef Slot,
                                  const HighExpr &Val) const;
  /// `item = result` / `item = v24` is register-home reuse, not a C
  /// assignment to the incoming parameter.
  bool isIncomingParamReuseAssign(const HighStmt &Stmt) const;
  /// x64 catch funclets receive the parent frame in rdx (`Param` id 1). After
  /// attach that param is not the parent's rdx argument.
  bool isCatchFuncletParentFrame(const MedVar &V) const;
  const HighExpr *parentFrameStoredValue(const HighStmt &Stmt) const;
  std::optional<std::string> copyForwardSource(const HighExpr &E) const;
  bool isHiddenCopyForwardAssign(const HighStmt &Stmt) const;
  void hideUnusedFrameSlotWrites(const HighFunc &Func);
  void hideX86CallPushSetup(const HighFunc &Func);
  void hideX86SehRegistration(const HighFunc &Func);
  void hideIncrementOnlyLoads(const HighFunc &Func);
  void hideBitClearSlotCopies(const HighFunc &Func);
  void hideCleanupSlotCopyIntoCall(const HighFunc &Func);
  std::optional<std::string> namedSlotLoadDisplay(const HighExpr &E) const;
  std::optional<std::string> incomingHomeParamName(int64_t Disp) const;
  bool samePeeledAddr(const HighExpr &A, const HighExpr &B) const;
  const HighExpr *asIncrementBase(const HighExpr &Val, int64_t &Delta) const;
  bool incrementBaseMatchesAddr(const HighExpr &Base,
                                const HighExpr &Addr) const;
  bool isInplaceAddStore(const HighStmt &S, int64_t &Delta) const;
  std::string formatInplaceAdd(const TypeRef &Ty, const HighExpr &Addr,
                               int64_t Delta);
  const HighExpr *asAndWithConst(const HighExpr &Val, uint64_t &Mask) const;
  std::string formatBitAndMask(uint64_t Mask, unsigned Size) const;
  std::string formatInplaceAnd(const std::string &Dest, uint64_t Mask,
                              unsigned Size) const;
  bool isInplaceAndStore(const HighStmt &S, uint64_t &Mask) const;
  bool stmtHiddenFromC(const HighStmt &Stmt) const;
  bool stmtsEffectivelyEmpty(const std::vector<HighStmt> &Stmts) const;
  void markHiddenControlDead(const std::vector<HighStmt> &Stmts,
                             bool Cleanup = false);
  void collectCopyForward(const HighFunc &Func);
  void applyDebugCallSlotTypes(const HighFunc &Func);
  void propagateFrameSlotCopyTypes(const HighFunc &Func);
  void hideInteriorRecordFieldSlots();
  void overlayPackedValueHomes(const HighFunc &Func);
  void collectFieldLoadForward(const HighFunc &Func);
  void collectEnumConstForward(const HighFunc &Func);
  void collectTypedPointerArgDests(const HighFunc &Func);
  void noteDebugExtern(const std::string &Name, const FunctionSym &FS);
  std::optional<std::string> enumeratorDisplay(const TypeRef &Ty,
                                               uint64_t Val) const;
  TypeRef enumTypeOfExpr(const HighExpr &E) const;
  TypeRef enumTypeForCallArg(const HighExpr &Call, size_t Index,
                             std::optional<uint64_t> Val = std::nullopt) const;
  std::optional<std::string> enumConstDisplay(const std::string &DestName,
                                              const HighExpr &Val) const;
  bool isForwardableValueExpr(const HighExpr &E) const;
  bool isImageObjectLoad(const HighExpr &E) const;
  bool isTypedMemberLoad(const HighExpr &E) const;
  bool isReloadableLoad(const HighExpr &E) const {
    return isImageObjectLoad(E) || isTypedMemberLoad(E);
  }
  /// `!(x == 0 || x < 0)` → `!(x <= 0)` so the cond mentions `x` once.
  void foldSignedJleConds(std::vector<HighStmt> &Stmts);
  bool isCxxCatchObjectName(llvm::StringRef Name) const;
  std::optional<std::string> cxxCatchPointerName(const HighExpr &E) const;
  std::optional<std::string> cxxCatchFieldAccess(const HighExpr &Addr) const;
  void collectValueForward(const HighFunc &Func);
  /// ATL/MSVC ctor returns `this`. Print the call as a statement and reuse
  /// the this operand at later uses instead of a leftover dest temp.
  void aliasCtorReturnThis(const HighFunc &Func);
  void collectUnusedCallStoreAlias(const HighFunc &Func);
  void collectPostIfElseValueForward(const HighFunc &Func);
  void collectCallResultNames(const HighFunc &Func);
  std::string printedForwardedVar(const std::string &Name, int ParentPrec);

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
  // External prototypes use projected C names, while call expressions still
  // carry source names. Keep their mapping separate when both _foo and __foo
  // occur in one function.
  std::map<std::string, std::set<std::string>> ExternalCallSources;
  std::map<std::string, std::string> ExternalSourceIdentifiers;
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
  std::map<std::string, FunctionSym> DebugExternSigs;
  std::map<std::string, std::vector<FunctionSym>> DebugExternAlts;
  std::set<std::string> ConflictingDebugExternSigs;
  /// Pointer-to-class TPI callees whose call sites pass a real hidden result.
  std::set<std::string> DebugExternHiddenSret;
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
  /// Win64 hidden sret parameter name (`result`) when TPI returns a class.
  std::string IndirectReturnName;
  bool PrintedIndirectReturn = false;
  bool highIRIncludesIndirectReturn(const HighFunc &Func,
                                    const FunctionSym &FS) const;
  /// Win64 members keep `this` in rcx; the hidden sret pointer is rdx.
  bool isWin64MemberIndirectReturn(const FunctionSym &FS) const;
  int indirectReturnParamId(const FunctionSym &FS) const;

  struct NamedFrameSlot {
    std::string Name;
    TypeRef Type;
    /// Call-site pointee (`ArgList* args`) kept even when PDB `Type` is richer
    /// (`TPtr`). Interior stores prefer this overlay (`values_` vs `p`).
    TypeRef CallType;
    bool AddressTaken = false;
    bool UsedAsMemory = false;
  };
  std::map<int64_t, NamedFrameSlot> FrameSlots;
  std::map<std::string, int64_t> FrameAliases;
  /// Names assigned both a member address and a frame slot (join PHI).
  std::set<std::string> AmbiguousFrameAliases;
  std::map<std::string, std::string> CopyForward;
  /// Dest assigned in more than one statement (if/else join PHI).
  std::set<std::string> JoinPhiNames;
  /// Temps that only carry `this->field` print as the member at each use.
  std::map<std::string, std::string> FieldForward;
  std::map<std::string, TypeRef> FieldForwardTypes;
  std::map<std::string, TypeRef> EnumDestTypes;
  /// Temps used as a named-class pointer arg (`CStringT_dtor(v26)`).
  std::map<std::string, TypeRef> PointerArgDestTypes;
  /// Single-use call / image-global loads print at the use, not as a temp.
  std::map<std::string, const HighExpr *> ValueForward;
  /// Multi-use ATL ctor dests print as the this operand (`&var`), not `v21`.
  std::map<std::string, const HighExpr *> CtorThisForward;
  /// Assigned call results that stay as temps take a Get* stem (`FontColor`).
  std::map<std::string, std::string> CallResultNames;
  std::map<std::string, TypeRef> CallResultTypes;
  struct CxxThrowPrint {
    std::string Type;
    std::vector<const HighExpr *> Args;
  };
  std::map<const HighStmt *, CxxThrowPrint> CxxThrowPrints;
  std::map<const HighEHClause *, std::string> CxxCatchNames;
  std::set<std::string> HiddenCxxCtorIdentifiers;
  std::set<std::string> CatchAliasTemps;
  /// Destinations whose only assignments are Undef / unknown copies of those.
  /// Assigns are hidden; stores of the name print `0`.
  std::set<std::string> UnknownOnlyNames;
  /// Names that appear as assign destinations in this function.
  std::set<std::string> AssignedNames;
  /// Temps assigned a pointer/string/frame address (copy/string ctor src).
  std::set<std::string> CtorSourceNames;
  /// C identifiers actually emitted by `emitLocalDecls` / frame slots.
  std::set<std::string> DeclaredCNames;
  std::map<std::string, std::string> ReachingCatchPtrs;
  std::map<std::string, std::string> ReachingCatchFields;
  void foldCxxThrowConstructors(const HighFunc &Func);
  void discoverHiddenCxxThrowCtors(const std::vector<HighFunc> &Funcs);
  void nameCxxCatchObjects(const HighFunc &Func);
  void noteCatchReaching(const HighStmt &Stmt);
  void simulateCatchReaching(const HighFunc &Func);
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
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
