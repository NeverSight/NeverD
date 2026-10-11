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
#include "../CSourceRecorder.h"
#include "../FloatConversion.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/pass/HighC/HighCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/DataSymbolBinding.h"

#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

/// Address of frame byte storage at \p Displacement from the entry SP.
std::string frameStorageAddress(int64_t Displacement);

/// The bytes of a pointer, and of an integer argument register, on \p A.
uint16_t pointerBytes(Arch A);

/// The bytes of the integer \p Operand a bit count counts, or 0.
uint16_t countedBytes(const HighExpr &Operand);

/// The bits C's counting builtins count the integer \p Operand in: 32 for
/// at most 4 bytes (unsigned int), 64 for 8 bytes, and 0 for any other width.
unsigned countedBits(const HighExpr &Operand);

/// Whether \p E prints as integer arithmetic, whose C type follows from its
/// operands' by C's conversions rather than from the IR's type.
bool printsIntegerArithmetic(const HighExpr &E);

/// \p E prints as a C expression whose value is 0 or 1: a comparison, a
/// logical operation, or a carry or overflow test.  Its zero or sign
/// extension is that value at any width, with no view of its own byte.
bool printsTruthValue(const HighExpr &E);

class HighCWriter {
public:
  static llvm::StringRef
  sourceConventionAttribute(SourceFunctionTypeHint::ConventionKind Convention);
  static std::string
  sourceParameterType(const SourceParameterTypeHint &Parameter,
                      llvm::StringRef Name = {});
  static std::optional<std::string> sourceValue(llvm::StringRef Text,
                                                const TypeRef &Carrier,
                                                const TypeRef &Source);
  HighCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg, bool GuardAnalysisOnlyFunctions = true,
              const std::unordered_set<std::string_view> *SharedNames = nullptr,
              CSourceRecorder *Recorder = nullptr)
      : Out(OS), OS(Out), Opts(Opts), Dbg(Dbg),
        GuardAnalysisOnlyFunctions(GuardAnalysisOnlyFunctions),
        SharedImageFunctionNames(SharedNames), SourceRecorder(Recorder) {}

  //--- Module-level (HighCEmitter.cpp) ---
  void writeAll(const std::vector<HighFunc> &Funcs);
  /// Records the functions and objects the text names in the source map
  /// (HighCSourceNames.cpp).
  void recordSourceNames(const std::vector<HighFunc> &Funcs);
  void writeSourceRecordDeclarations(const std::vector<HighFunc> &Funcs);
  TypeRef declaredFunctionReturnType(const HighFunc &Func) const;
  void prepareFunctionReturns(std::vector<HighFunc> &Funcs) const;
  void prepareFunctionIdentifiers(const std::vector<HighFunc> &Funcs);
  std::string functionIdentifier(const HighFunc &Func) const;
  std::string functionIdentifier(llvm::StringRef SourceName) const;
  void collectMemoryTypes(const std::vector<HighFunc> &Funcs);
  std::optional<c_float::Conversion>
  floatToIntegerConversion(const HighExpr &E) const;
  void collectImageObjects(const std::vector<HighFunc> &Funcs);
  void writeImageObjects();
  std::optional<va_t> constAddress(const HighExpr &E) const;
  std::optional<uint64_t> foldReadonlyScalar(va_t Addr, uint16_t Size) const;
  std::optional<std::string> imageObjectName(va_t Addr) const;
  std::optional<std::string> imageBackingAddress(va_t Addr) const;
  bool isImageDataAddress(va_t Addr) const;
  void noteImageObject(va_t Addr, const TypeRef &Ty, bool Written,
                       bool MemoryAccess = false);
  /// Notes an object the code reaches only by its address so far.
  void noteImageAddress(va_t Addr);
  std::string memoryTypeName(const TypeRef &Ty) const;
  void writeIncludes(const std::vector<HighFunc> &Funcs);
  void writeMemoryHelpers();
  void writeX64SyscallHelper();
  void writeX64WindowsSyscallHelper();
  // X86/HighCRegistration.cpp: explicit runtime ABI inputs in the EH view.
  void writeRegistrationEntryDeclarations(const std::vector<HighFunc> &Funcs);
  std::string registrationEntryExpression(const HighExpr &E) const;
  static bool preservesRegistrationMemory(const HighFunc &Func);
  bool isEmbeddedRegistrationCallback(const HighStmt &Stmt, size_t I) const;
  void writeEmbeddedRegistrationCallbacks(const HighStmt &Stmt, int Indent);
  unsigned RegistrationRegionNumber = 0;
  struct MemoryLoadDestination {
    std::string Name;
    bool Written = false;
  };
  /// \p Text, an expression printed as a statement of its own.
  static std::string statementText(std::string Text);
  std::string memoryLoadExpr(
      const TypeRef &Ty, llvm::StringRef Addr,
      NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
      NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default,
      bool ExactImageBytes = false,
      MemoryLoadDestination *Destination = nullptr);
  std::string memoryStoreExpr(
      const TypeRef &Ty, llvm::StringRef Addr, llvm::StringRef Val,
      NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
      NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default,
      bool ExactImageBytes = false);
  std::string memoryTemporary(llvm::StringRef Type, llvm::StringRef Base);
  void writeMemoryStore(const TypeRef &Ty, llvm::StringRef Addr,
                        llvm::StringRef Val, NdMemoryOrdering Ordering,
                        NdMemoryAddressSpace AddressSpace, bool ExactImageBytes,
                        int Indent);
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
  void writeStmtImpl(const HighStmt &Stmt, int Indent);
  void writeCxxThrowExpr(const HighStmt &Stmt, const HighExpr &ThrowCall);
  void writeCExceptionRegion(const HighStmt &Stmt, int Indent);
  void writeStmts(const std::vector<HighStmt> &Stmts, int Indent,
                  size_t End = static_cast<size_t>(-1));
  bool tryWriteCursorForLoop(const std::vector<HighStmt> &Stmts, size_t I,
                             size_t End, int Indent, size_t &Last);
  void writeStmtsIsolated(const std::vector<HighStmt> &Stmts, int Indent);
  void writeTryBody(const std::vector<HighStmt> &Stmts, int Indent);
  void writeTryBodyUnisolated(const std::vector<HighStmt> &Stmts, int Indent);
  bool isCompilerEHConstant(const HighExpr &Val) const;
  void emitIndent(int Indent);
  /// Emit a possibly multi-line rendered statement, indenting every line.
  void emitRenderedStatement(int Indent, llvm::StringRef Text);
  void collectGotoTargets(const std::vector<HighStmt> &Stmts,
                          bool TryBody = false);
  /// True when a printed goto may name \p Addr: some goto of the function
  /// targets it, or a handler clause jumps there.
  bool isLabelAddress(va_t Addr) const;
  /// A try body's trailing goto that lands where the try falls through, so
  /// it is not printed.
  bool isFallthroughTryExit(const HighStmt &Stmt) const;
  /// Addresses control reaches by falling off Stmts[Index]; the end of the
  /// list reaches \p Continuation.
  std::vector<va_t>
  fallthroughAddresses(const std::vector<HighStmt> &Stmts, size_t Index,
                       const std::vector<va_t> &Continuation) const;
  void decideTryExits(const std::vector<HighStmt> &Stmts,
                      const std::vector<va_t> &Continuation,
                      std::set<std::pair<va_t, va_t>> &Kept);
  /// Labels where the try Stmts[Index] falls through that print right after
  /// it: no statement printed earlier, the try included, starts them.
  std::vector<va_t> tryLeaveTargets(const std::vector<HighStmt> &Stmts,
                                    size_t Index) const;

  //--- Expression rendering (HighCExprWriter.cpp) ---
  std::string exprStr(const HighExpr &Expr, int ParentPrec = 0,
                      MemoryLoadDestination *Destination = nullptr);
  std::string exprStrImpl(const HighExpr &Expr, int ParentPrec,
                          MemoryLoadDestination *Destination = nullptr);
  /// Operands of the integer operator being printed. A string literal among
  /// them is an array in C and prints as its integer address instead.
  std::set<const HighExpr *> LiteralAddressOperands;
  /// Operands of an integer-only C operator (bitwise, shift, multiply,
  /// divide): a frame slot address among them is printed as an integer.
  std::set<const HighExpr *> IntegerViewOperands;
  /// Call arguments whose parameter takes a pointer: a string object (its
  /// array, or a pointer to one) prints there as the pointer it is in C, and
  /// elsewhere as the integer the machine holds.
  std::set<const HighExpr *> PointerArgumentOperands;
  /// Address used by a load/store/atomic. Peels integer views and prints
  /// `base + imm` without sanitizer wrap. Value uses of the same add still
  /// wrap. Segmented offsets disable image backing projection to stay numeric.
  std::string addrStr(const HighExpr &Expr, int ParentPrec = 0,
                      bool ProjectImageBacking = true);
  std::string renderUnaryOp(const HighExpr &E, int ParentPrec);
  std::string resolvedCallTarget(const HighExpr &E) const;
  /// The C identifier a direct call names; see HighCExprWriter.cpp.
  std::string callIdentifier(const HighExpr &E) const;
  /// The C name of the import slot call \p E goes through, itself or by the
  /// stub that jumps through it: a function pointer (`__imp_calloc`), or
  /// empty when it calls no import or the format names no slot.
  std::string importSlotIdentifier(const HighExpr &E) const;
  /// The identifier of the import slot at \p Addr when \p Addr is an
  /// import's own slot in the import address table, not a stub that jumps
  /// through it; empty otherwise.
  std::string importDataSlotIdentifier(va_t Addr) const;
  /// The import slot a read of \p Size bytes at \p Address reads as data, a
  /// data import's address: the slot at a constant address, or the slot a
  /// read-only pointer holds (MinGW's `.refptr.__imp__fmode`).
  std::optional<va_t> importDataSlotRead(const HighExpr &Address,
                                         uint16_t Size) const;
  /// The function this file defines that call \p E runs, or null: one named
  /// like it at another address is a different function.
  const HighFunc *calledDefinition(const HighExpr &E) const;
  /// Whether \p Func is the stub of a variadic C library import, which a
  /// call runs as a call through the import's slot.
  bool isVariadicImportStub(const HighFunc &Func) const;
  std::string renderCallExpr(const HighExpr &E);
  std::string renderSourceCallExpr(const HighExpr &E);
  const HighFunc *sourceCallDefinition(const SourceCallTypeHint &Hint,
                                       llvm::StringRef Name) const;
  std::string varName(const MedVar &V) const;
  std::string debugNameForDisplacement(va_t Entry, int64_t Disp) const;
  TypeRef debugTypeForDisplacement(va_t Entry, int64_t Disp) const;
  TypeRef declaredParamType(const MedVar &V) const;
  /// The type \p Func's definition declares its HighIR parameter \p Index
  /// with, which its prototype and every use of the parameter share.
  TypeRef emittedParamType(const HighFunc &Func, size_t Index) const;
  /// What a parameter holds of its function's debug signature, matched by
  /// where the convention passes each (SourceParameterPlacement), never by
  /// position: System V passes `f(double x, int n)` with n in the first
  /// integer register.
  struct DebugParamBinding {
    /// The debug parameter it holds a piece of, or -1.
    int Index = -1;
    /// Where the piece starts in that parameter's value.
    uint16_t Offset = 0;
    /// It holds the whole value the debug type describes.
    bool Whole = false;
    /// It holds the hidden pointer to the result's storage.
    bool Result = false;
  };
  DebugParamBinding debugParamBinding(const HighFunc &Func, size_t Index) const;
  /// The name \p Func's parameter \p Index reads as by its debug signature:
  /// the debug parameter's, `<name>_<offset>` for a later piece of one, or
  /// empty.
  std::string debugParamName(const HighFunc &Func, size_t Index) const;
  /// Whether a call passes \p FS's parameters in signature order: one
  /// integer or one floating value each, and no hidden result pointer.  A
  /// call's arguments come in the convention's order, integer registers
  /// first, so another signature cannot type them by position.
  bool positionalDebugSignature(const FunctionSym &FS) const;
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
  std::optional<std::string>
  typedMemberAccess(const HighExpr &Addr, uint16_t AccessSize = 0,
                    bool EnterNestedAtZero = true) const;
  std::optional<std::string> typedMemberAddress(const HighExpr &Addr) const;
  TypeRef typedMemberType(const HighExpr &Addr, uint16_t AccessSize = 0) const;
  /// Frame displacement that is a unique interior field of a named record
  /// slot (`record.p` at wrapper+8). Exact slot addresses stay unnamed here.
  std::optional<std::string>
  frameTypedMemberAccess(int64_t Disp, uint16_t AccessSize = 0) const;
  TypeRef frameTypedMemberType(int64_t Disp, uint16_t AccessSize = 0) const;
  /// Frame displacement of `Base` (or of the slot under `Load(slot)`) plus
  /// `Rel`. Used so a call-site ArgList overlay wins over IR TPtr at +8.
  std::optional<int64_t> frameOverlayDisplacement(const HighExpr &Base,
                                                  uint64_t Rel) const;
  /// `var = 43` on a 16-byte ArgList slot is `var.types_ = 43` when the
  /// stored scalar is smaller than the record and field 0 is int/ptr.
  std::optional<std::string>
  scalarRecordFieldDest(llvm::StringRef SlotName, const HighExpr &Val,
                        llvm::StringRef PrintedValue = {}) const;
  /// Integer-return Call stuffed into a pointer field of a slot that a call
  /// retyped (`ArgList.values_` over `TPtr.p`). Const / `p = 0` / integer
  /// temps keep the PDB name.
  std::optional<std::string>
  callOverlayIntegerMemberStore(llvm::StringRef Member,
                                const HighExpr &Val) const;
  bool isIntegerOverlayStore(const HighExpr &Val) const;
  bool pointerNeedsIntegerView(const TypeRef &Ty) const;
  /// The name of the pointer variable \p E when \p Text is the integer view
  /// it prints with, so an access can take the pointer itself.
  std::optional<std::string> declaredPointerName(const HighExpr &E,
                                                 llvm::StringRef Text) const;
  /// Whether \p Text is the bare name of pointer variable \p E.
  bool isBarePointerName(const HighExpr &E, llvm::StringRef Text) const;
  /// The C type variable \p E named \p Name is declared with, if known.
  TypeRef declaredTypeOf(const HighExpr &E, llvm::StringRef Name) const;
  /// A pointer to void, or to nothing the type records.
  static bool pointsToVoid(const TypeRef &Ty);
  /// Pointer object used as an `INDIR_CALL` base, without `(uintptr_t)`.
  std::string pointerObjectStr(const HighExpr &E);
  /// The failure an unknown value prints at its use, or empty when \p E
  /// names a value the function defines.
  std::string unknownVarUse(const HighExpr &E, const std::string &Name,
                            const std::string &RawName);
  /// Pointer-sized Load chain used as a callee: `*(void **)p` / `**(void
  /// ***)p`. A loaded vtable slot `Load(Load(obj)+imm)` is
  /// `*(void **)((uintptr_t)(*(void **)(obj)) + imm)`, not integer soup.
  std::string indirectCalleeStr(const HighExpr &E,
                                const TypeRef &ReturnType = nullptr);
  const HighExpr *unwrapIntegerView(const HighExpr *E) const;
  /// The variable \p E converts between integer or pointer types of one
  /// width, or null when a step changes the width or converts a float: a
  /// narrowed or extended view is another value than the variable it reads
  /// (`(int32_t)(int8_t)v` is not `v`).
  const HighExpr *sameWidthVariable(const HighExpr &E) const;
  const HighExpr *forwardedExpr(const HighExpr *E) const;
  /// \p Operand's text as an operand of a floating-point operator.  An
  /// operation whose type C may compute wider (CFloatTypes.def) is cast to
  /// that type, which rounds it where the instruction rounded.
  std::string floatOperandStr(const HighExpr &Operand, int ParentPrec);
  /// True when \p E prints as an unsigned integer of exactly \p Width bytes.
  /// Widening views and untyped add/sub/mul stay wrapped.
  bool isSameWidthUnsigned(const HighExpr &E, uint16_t Width) const;
  std::optional<FunctionSym> debugCallee(const HighExpr &E) const;
  TypeRef expectedDebugCallArgType(const FunctionSym &FS, size_t Index) const;
  TypeRef displayCallArgType(const HighExpr &Call, size_t Index) const;
  /// Display-only printed arity from TPI/PDB or \ref msvcCallee.
  /// MSVC member rows live in MsvcCallees.def; `_ctor` keeps this plus a
  /// pointer/string/fill source and drops leftover register clobbers.
  size_t debugCallArgLimit(const HighExpr &E) const;
  /// The fixed arity of the known function \p Symbol, which the C name
  /// \p Identifier reads; a C++ stem is no name the tables know.
  static std::optional<libc::LibCArity> knownArity(llvm::StringRef Symbol,
                                                   llvm::StringRef Identifier);
  /// How many arguments the plain declaration of the external function \p E
  /// calls gives it (writeForwardDecls), when its arity is known.
  std::optional<size_t> plainDeclarationArity(const HighExpr &E) const;
  /// A comment showing the string the call argument \p Arg points to, as an
  /// address or as a pointer the image holds; empty when it points to none
  /// or prints as a literal already.
  std::string stringArgumentNote(const HighExpr &Arg);
  void collectUnknownOnlyNames(const HighFunc &Func);
  void collectCtorSourceNames(const HighFunc &Func);
  bool isUnknownCallOperand(const HighExpr *Op) const;
  bool isCtorSourceExpr(const HighExpr *Op) const;
  bool isCtorDisplayOperand(const HighExpr *Op) const;
  TypeRef knownCallReturnType(const HighExpr &E) const;
  /// A call whose prototype returns nothing: TPI or the C library tables say
  /// `void`, or the callee is a destructor. The result register it leaves
  /// holds no defined value.
  bool knownVoidCall(const HighExpr &E) const;
  const HighExpr *typedCallResult(const HighExpr *E) const;
  const HighExpr *peelIntegerViewOps(const HighExpr *E) const;
  /// Peel zext/trunc around a named scalar or field load for x86 intrinsic
  /// snippets. Keep wraps on add/sub/mul.
  std::string intrinsicOperandStr(const HighExpr &E);
  /// Cast / zext / `SUBBYTES` 0 of a Var/Phi. Not add/sub/mul, Call, or Load.
  bool isIntegerViewOfScalar(const HighExpr &E) const;
  /// Where \p Bits come from, under views that keep at least the bytes of
  /// \p Float, reinterpretations and forwarded copies: a value of type
  /// \p Float, a call, or the first expression that is neither a view nor a
  /// value; null where a view drops bytes.
  const HighExpr *floatBitsSource(const HighExpr &Bits,
                                  const TypeRef &Float) const;
  /// The call whose result \p Bits are (floatBitsSource), rendered as
  /// \p Float when its known return and actual carrier allow it; else nullopt.
  std::optional<std::string> floatCallResultText(const HighExpr &Bits,
                                                 const TypeRef &Float);
  /// The value of type \p Float whose bits \p Bits are (floatBitsSource), or
  /// null.
  const HighExpr *floatBitsValue(const HighExpr &Bits,
                                 const TypeRef &Float) const;
  /// The \p Float literal whose bits \p Bits are (floatBitsSource): a
  /// constant, or one read from constant data; else nullopt.
  std::optional<std::string> floatConstantBitsText(const HighExpr &Bits,
                                                   const TypeRef &Float) const;
  /// \p Arg passed to a parameter of floating type \p Expected, when its
  /// integer bits carry the value.
  std::optional<std::string> floatArgumentText(const HighExpr &Arg,
                                               const TypeRef &Expected);
  /// \p E as the argument of a parameter of type \p Expected, which a
  /// prototype declares: an integer converts to a pointer parameter, and a
  /// string or an address to an integer one.
  std::string exprStrAsTypedArg(const HighExpr &E, const TypeRef &Expected);
  /// The text \p E passes to a parameter of type \p Expected, before a
  /// pointer converts to an integer parameter.
  std::string typedArgumentText(const HighExpr &E, const TypeRef &Expected);
  /// Whether \p E, printed as \p Text, has an integer type in C that a
  /// pointer parameter does not take as it is (not a null pointer constant).
  bool printsAsInteger(const HighExpr &E, llvm::StringRef Text) const;
  /// Whether \p Text prints a pointer: a string, an address, or a name the
  /// function declares as a pointer.
  bool printsAsPointer(llvm::StringRef Text) const;
  /// The type this function declares for the identifier \p Text: a local,
  /// a parameter or a named frame slot; null for anything else.
  TypeRef declaredTypeNamed(llvm::StringRef Text) const;
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
  int debugSymRichness(const FunctionSym &FS) const;
  static TypeRef cDisplayType(const TypeRef &Ty);
  std::optional<int64_t> frameDisplacement(const HighExpr &E) const;
  std::optional<int64_t>
  certifiedFrameStorageDisplacement(const HighExpr &E) const;
  /// The C lvalue for the frame slot at \p E.  When \p Access is narrower
  /// than the slot, the access goes through the slot's address so it
  /// changes only those bytes.
  std::optional<std::string> namedFrameSlot(const HighExpr &E,
                                            const TypeRef &Access = {}) const;
  bool isNamedFrameMemory(const HighExpr &E) const;
  std::string constStr(uint64_t Val, TypeRef Type = nullptr);
  /// \p E prints as a local declared as a \p Width-byte integer of the given
  /// signedness.
  bool declaredLocalInteger(const HighExpr &E, uint16_t Width, bool Signed);
  /// The integer type, as {byte width, signed}, of the text exprStr last
  /// printed for \p E, when that spelling fixes it: a declared local, an
  /// integer cast or carrier arithmetic.
  std::optional<std::pair<uint16_t, bool>>
  printedIntegerType(const HighExpr &E) const;
  /// Record that \p Text, printed for \p E, is a \p Width-byte integer.
  std::string typedText(const HighExpr &E, std::string Text, uint16_t Width,
                        bool Signed);
  /// The signed view of the unsigned carrier \p Value computed for \p E,
  /// whose top operator has precedence \p ValuePrec.  Consumers that convert
  /// the result anyway may read \p Value instead (unsignedCarrierText).
  std::string signedCarrierResult(const HighExpr &E, std::string Value,
                                  int ValuePrec, uint16_t Size);
  /// The unsigned carrier exprStr last printed inside \p E's signed view,
  /// parenthesized for \p ParentPrec.
  std::optional<std::string> unsignedCarrierText(const HighExpr &E,
                                                 int ParentPrec) const;
  /// The expression inside \p E whose conversion to a \p Width-byte integer
  /// gives the same value: integer conversions that keep at least the low
  /// \p Width bytes are looked through.
  const HighExpr &lowBytesSource(const HighExpr &E, uint16_t Width) const;
  /// \p Text, printed for \p E, certainly has an integer type.
  bool integerText(const HighExpr &E, llvm::StringRef Text) const;
  /// \p E converted to the integer type \p To, printed as an operand at
  /// \p ParentPrec.  Conversions that keep the bytes \p To holds are left
  /// out, and so is the cast when the text already has that type.
  std::string integerView(const HighExpr &E, const TypeRef &To, int ParentPrec);
  /// \p E as the value assigned or returned to the integer type \p To, without
  /// the conversions that C's implicit conversion performs; nullopt when the
  /// text could not be shown to be an integer.
  std::optional<std::string> implicitIntegerConversion(const HighExpr &E,
                                                       const TypeRef &To);
  /// \p Value as stored to memory of type \p To with \p Ordering.
  std::string storedValueText(const HighExpr &Value, const TypeRef &To,
                              NdMemoryOrdering Ordering);
  std::string formatReturnExpr(const HighExpr &Expr);
  std::string collapseHiLo(const HighExpr &Expr);
  std::string unwrapCastVar(const HighExpr &E);
  std::string invertCondStr(const HighExpr &E);
  std::string condStr(const HighExpr &E);
  std::string condStrImpl(const HighExpr &E);
  std::string invertCondStrImpl(const HighExpr &E);
  /// `!(a <= b && a != b)` is `a >= b`. When both IfElse arms print, Hex-Rays
  /// prefers `if (b > a) else-arm else then-arm`.
  std::optional<std::string> preferGreaterIfElseCond(const HighExpr &E);
  std::string copyForwardName(const std::string &Name) const;
  /// Whether \p E reads \p Name, itself or through the values already
  /// forwarded into it.  A forward that does stands for itself: printing it
  /// in place never ends.
  bool readsThroughForwards(const HighExpr &E, const std::string &Name) const;
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
  bool isRegistrationEstablisherFrame(const MedVar &V) const;
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
  void collectFieldLoadTypes(const HighFunc &Func);
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
  bool isTypedIndexLoad(const HighExpr &E);
  bool isReloadableLoad(const HighExpr &E) {
    return isImageObjectLoad(E) || isTypedMemberLoad(E) || isTypedIndexLoad(E);
  }
  /// `!(x == 0 || x < 0)` → `!(x <= 0)` so the cond mentions `x` once.
  void foldSignedJleConds(std::vector<HighStmt> &Stmts);
  bool isCxxCatchObjectName(llvm::StringRef Name) const;
  std::optional<std::string> cxxCatchPointerName(const HighExpr &E) const;
  std::optional<std::string> cxxCatchFieldAccess(const HighExpr &Addr) const;
  void collectValueForward(const HighFunc &Func);
  /// MSVC ctor returns `this`. Print the call as a statement and reuse
  /// the this operand at later uses instead of a leftover dest temp.
  void aliasCtorReturnThis(const HighFunc &Func);
  void collectUnusedCallStoreAlias(const HighFunc &Func);
  void collectCallResultNames(const HighFunc &Func);
  /// \p PrintedFrom receives the expression whose exprStr text is returned.
  std::string printedForwardedVar(const std::string &Name, int ParentPrec,
                                  const HighExpr **PrintedFrom = nullptr);

  //--- Binary expression rendering (HighCExprBinOp.cpp) ---
  std::string renderBinOp(const HighExpr &E, int ParentPrec);
  std::string renderBinOpOperands(const HighExpr &E, int ParentPrec);

  //--- State ---
  RedirectableStream Out;
  llvm::raw_ostream &OS;
  /// Declarations skipped because copy forwarding or liveness predicted the
  /// name would not be printed.  Emitted if the rendered body names it anyway.
  /// The C text is built only then: a name that is never printed may carry a
  /// type with no C spelling (a scalable vector temporary).
  struct DeferredDecl {
    TypeRef Type;
    /// Exact declaration text; when empty, `declarationToC(Type, name)`.
    std::string Text;
  };
  std::map<std::string, DeferredDecl> DeferredDecls;
  CProjectionIdentifierAllocator MemoryIdentifiers;
  std::map<std::string, std::string> MemoryTemporaries;
  std::set<std::string> AddressTakenNames;
  std::map<std::string, TypeRef> DeclaredCTypes;
  /// See printedIntegerType.
  std::unordered_map<const HighExpr *, std::pair<uint16_t, bool>>
      PrintedIntegerTypes;
  /// Variables printed as forwarded integer arithmetic of no known C type:
  /// their own types do not decide how the text reads.
  std::unordered_set<const HighExpr *> UntypedArithmeticTexts;
  /// See signedCarrierResult: the carrier text and its precedence.
  std::unordered_map<const HighExpr *, std::pair<std::string, int>>
      UnsignedCarrierTexts;
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
  /// Views remain valid for this writer because its BinaryImage is immutable
  /// for the duration of one emission.
  std::optional<std::unordered_set<std::string_view>> ImageFunctionNames;
  const std::unordered_set<std::string_view> *SharedImageFunctionNames;
  std::map<std::string, const HighFunc *> DefinedFunctionsByIdentifier;
  std::map<va_t, const HighFunc *> DefinedFunctionsByAddress;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::map<const HighFunc *, std::string> FunctionIdentifiers;
  /// The C name each defined function's symbol spells, for its comment.
  std::map<const HighFunc *, std::string> FunctionSymbolNames;
  std::map<std::string, std::string> FunctionIdentifiersBySourceName;
  std::map<std::string, std::string> ExternalFunctionIdentifiers;
  // External prototypes use projected C names, while call expressions still
  // carry source names. Keep their mapping separate when both _foo and __foo
  // occur in one function.
  std::map<std::string, std::set<std::string>> ExternalCallSources;
  /// Callees with a call that never returns (HighExpr::DoesNotReturn); their
  /// declarations say so, as for a routine the name list knows.
  std::set<std::string> NoReturnCallTargets;
  /// Call identifiers that name an import's slot (importSlotIdentifier):
  /// declared as function pointers, linked by their own names.
  std::set<std::string> ImportSlotIdentifiers;
  /// Import slots the code reads as data (importDataSlotRead), and the
  /// identifiers that read them once declared: a pointer linked by the
  /// slot's own name, or the function pointer a call through it declares.
  std::set<va_t> ImportDataSlotReads;
  std::map<va_t, std::string> ImportDataSlotNames;
  std::map<std::string, std::string> ExternalSourceIdentifiers;
  /// The identifiers functionIdentifier() spelled for functions no map
  /// names, and the symbols they stand for (recordSourceNames()).
  mutable std::map<std::string, std::string> ReferencedFunctionSymbols;
  std::set<va_t> GotoTargets;
  /// How many gotos target each address in the current function.
  std::map<va_t, unsigned> GotoTargetUses;
  /// (address, target) of try-body gotos left out as fall-through exits.
  std::set<std::pair<va_t, va_t>> FallthroughTryExits;
  /// While the protected body of a __try prints: the labels where that try
  /// falls through.  A goto there leaves the body as `__leave` does.
  std::vector<va_t> LeaveTargets;
  /// tryLeaveTargets of the try statement writeStmts is about to print.
  std::vector<va_t> NextTryLeaveTargets;
  /// Labels already printed in the current function; C allows each once.
  std::set<va_t> EmittedLabels;
  bool HasCIntrinsics = false;
  /// Handler entry address -> the name its __except arm captures
  /// GetExceptionCode() into, for each handler whose code is read.
  std::map<va_t, std::string> SEHExceptionCodeNames;
  void writeSEHExceptionCodeCapture(va_t HandlerVA, int Indent);
  std::string sehFilterValueText(const HighExpr &Value);
  /// The x87 helpers the output calls, and whether it computes with the
  /// x87 extended `long double`.
  std::set<X87CHelper> X87Helpers;
  std::map<std::pair<Intrinsic, unsigned>, std::string> X86FPStateHelpers;
  bool UsesX87Extended = false;
  /// The bytes a plain access of the C memory type \p Type copies inline: 0
  /// for its whole object, the value's bytes for a bit-precise integer
  /// narrower than its storage on a little-endian target, none when only
  /// the byte-order-aware helper reads it.
  std::optional<unsigned> inlineMemoryBytes(llvm::StringRef Type) const;
  /// An x87 extended value: what `long double` holds.
  static bool isX87Value(const HighExpr &E);
  /// The x87 helper \p E prints through, if any.
  std::optional<X87CHelper> x87HelperFor(const HighExpr &E) const;
  /// The name of \p Helper, which the output must declare.
  std::string useX87Helper(X87CHelper Helper) const;
  bool NeedsX64SyscallHelper = false;
  bool NeedsX64WindowsSyscallHelper = false;
  /// A Windows x86 function renders an <intrin.h>-only intrinsic.
  bool NeedsMsvcIntrinsics = false;
  /// A non-Windows x86 function renders an <x86intrin.h>-only intrinsic.
  bool NeedsGnuX86Intrinsics = false;
  bool NeedsFEnvAccess = false;
  std::set<std::string> CIntrinsicNames;
  bool NeedsObjCRuntime = false;
  bool NeedsObjCSuper2 = false;
  bool NeedsDarwinLocks = false;
  bool NeedsDarwinBlocks = false;
  bool NeedsDarwinStackGuard = false;
  bool NeedsDarwinStackFailure = false;
  bool NeedsDarwinAffineTransformBridge = false;
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
  std::map<std::string, bool> SourceRuntimeDataWeakImports;
  std::set<std::string> ConflictingSourceRuntimeDataIdentities;
  std::map<std::string, bool> SourceCallTermination;
  std::set<std::string> ConflictingSourceNativeSignatures;
  std::map<std::string, FunctionSym> DebugExternSigs;
  std::map<std::string, std::vector<FunctionSym>> DebugExternAlts;
  std::set<std::string> ConflictingDebugExternSigs;
  /// Pointer-to-class TPI callees whose call sites pass a real hidden result.
  std::set<std::string> DebugExternHiddenSret;
  std::set<std::string> MemoryTypes;
  std::map<std::tuple<unsigned, unsigned, bool>, std::string>
      FloatToIntegerHelpers;
  /// The helper counting a zero's leading zeros as its width, by the bits it
  /// counts in (32 or 64).
  std::map<unsigned, std::string> LeadingZeroHelpers;
  /// The helper of each x86 saturating lane operation and lane width
  /// (X86SaturatingLanes.def).
  std::map<std::pair<Intrinsic, unsigned>, std::string> SaturatingLaneHelpers;
  std::map<std::string, unsigned> PartialIntegerBytes;
  std::set<std::pair<std::string, NdMemoryAddressSpace>> SegmentedMemoryTypes;
  std::set<std::tuple<std::string, NdMemoryOrdering, NdMemoryAddressSpace>>
      AtomicLoadTypes;
  std::set<std::tuple<std::string, NdMemoryOrdering, NdMemoryAddressSpace>>
      AtomicStoreTypes;
  bool HasSegmentedMemory = false;
  /// An ordinary load or store has a type with an aligned(1), may_alias
  /// alias, so the aliases are declared when UseUnalignedPointers is set.
  bool NeedsUnalignedTypes = false;
  /// The aliases were declared; an access spelled through one needs that.
  bool UnalignedTypesWritten = false;
  bool Has256BitInteger = false;
  bool Has512BitInteger = false;
  /// A 16-byte integer on a target whose C has no __int128, which the
  /// prelude then spells as the C23 _BitInt(128).
  bool Int128AsBitInt = false;

  HighCAnalysisState Analysis;
  bool InferredVoid = false;
  TypeRef FuncReturnType;
  const HighFunc *CurrentFunc = nullptr;
  /// The parameter bindings and emitted types of the function last asked
  /// about.
  mutable const HighFunc *ParamCacheOf = nullptr;
  mutable va_t ParamCacheEntry = 0;
  mutable std::vector<DebugParamBinding> ParamBindings;
  mutable std::vector<TypeRef> EmittedParamTypes;
  mutable std::vector<std::string> ParamDebugNames;
  /// The convention's rules placed the cached function's debug parameters
  /// by where they arrive, rather than by position.
  mutable bool ParamsPlaced = false;
  /// Fills the cache for \p Func.
  void bindParams(const HighFunc &Func) const;
  /// Where a stack parameter of \p Func arrives, from the entry stack
  /// pointer; none for a register parameter or an unknown location.
  std::optional<int64_t> stackParamOffset(const HighFunc &Func,
                                          size_t Index) const;
  /// emittedParamIndices' answers, which walk the function's body.
  mutable std::map<std::pair<const HighFunc *, va_t>, std::vector<size_t>>
      EmittedParamIndices;
  CSourceRecorder *SourceRecorder = nullptr;
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
  /// Slot identity retained for value forwarding and catch-object recovery
  /// after their memory representation switches to stack_storage.
  std::map<int64_t, NamedFrameSlot> FrameStorageSlots;
  std::map<std::string, int64_t> FrameAliases;
  /// Bounds nested alignment-to-base certification as well as flat offsets.
  mutable unsigned FrameDisplacementDepth = 0;
  /// Slot names and interior accesses that share storage with another slot;
  /// copy forwarding must not treat them as independent variables.
  std::set<std::string> SharedFrameStorage;
  /// A dynamic address derived from a known frame alias uses the same byte
  /// storage as fixed stack accesses. Keep the alias displacement at uses.
  bool ProjectFrameAliasesIntoStorage = false;
  /// Names assigned both a member address and a frame slot (join PHI).
  std::set<std::string> AmbiguousFrameAliases;
  std::map<std::string, std::string> CopyForward;
  /// Dest assigned in more than one statement (if/else join PHI).
  std::set<std::string> JoinPhiNames;
  /// Temps that only carry `this->field` print as the member at each use.
  std::map<std::string, std::string> FieldForward;
  /// Original load expressions whose field spelling is forwarded at each use.
  std::map<std::string, const HighExpr *> FieldForwardSources;
  std::map<std::string, TypeRef> FieldForwardTypes;
  std::map<std::string, TypeRef> EnumDestTypes;
  /// Temps used as a named-class pointer arg (`CStringT_dtor(v26)`).
  std::map<std::string, TypeRef> PointerArgDestTypes;
  /// Single-use call / image-global loads print at the use, not as a temp.
  std::map<std::string, const HighExpr *> ValueForward;
  /// Values assigned to each printed variable name in the current function,
  /// built on first use by incrementBaseMatchesAddr.
  mutable std::unordered_map<std::string, std::vector<const HighExpr *>>
      AssignedValuesByName;
  mutable bool AssignedValuesIndexed = false;
  /// Multi-use MSVC ctor dests print as the this operand (`&var`), not `v21`.
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
  /// Frame displacement of each named catch object's slot.
  std::map<int64_t, std::string> CxxCatchObjectDisps;
  /// Catch objects in scope at the statement being written: the clauses
  /// whose bodies enclose it.
  std::vector<std::string> OpenCatchObjects;
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
  struct CatchAliasBinding {
    std::string Name;
    bool IsPointer = false;
  };
  /// Exact definitions certified before named slots become backing storage.
  std::map<const HighStmt *, CatchAliasBinding> CatchAliasDefinitions;
  void foldCxxThrowConstructors(const HighFunc &Func);
  void discoverHiddenCxxThrowCtors(const std::vector<HighFunc> &Funcs);
  void nameCxxCatchObjects(const HighFunc &Func);
  void noteCatchReaching(const HighStmt &Stmt);
  void simulateCatchReaching(const HighFunc &Func);
  std::map<int, std::string> ParamDisplayNames;
  bool InEHClauseBody = false;
  bool FrameStorageActive = false;
  /// C++ destructor unwind funclets `ret` to the personality, not the parent.
  bool InCxxCleanupBody = false;

  struct ImageObject {
    std::string Name;
    /// The name the user, debug information or a symbol gives it, which
    /// \ref Name is spelled from; empty for a name the emitter made.
    std::string Symbol;
    /// The symbol its name is spelled from, as its language spells it, for
    /// the comment that declares it; empty when the name is the symbol.
    std::string Readable;
    TypeRef Type;
    std::set<uint16_t> MemoryWidths;
    /// The code stores to it.
    bool Written = false;
    /// The type stands in for one the code never read or wrote it with: any
    /// access's type replaces it.
    bool WeakType = false;
    /// The code calls or jumps through the pointer it holds.
    bool CallSlot = false;
    /// The code uses its address as a value, not only to access it there.
    bool AddressTaken = false;
    /// The image relocates pointer slots inside it: each holds an address.
    bool HoldsPointers = false;
    /// A string referenced by its address alone: declared as its array, with
    /// \ref ArrayBytes elements' bytes, initialized by the string.
    std::optional<ImageCString> String;
    uint64_t ArrayBytes = 0;
    /// A pointer slot only loaded, holding the address of a read-only string:
    /// declared as that string's pointer, initialized by its literal.
    std::optional<ImageCString> PointsTo;
    /// The code indexes the object (`table[i]`): its whole extent, as the
    /// image's symbol sizes it, is declared, as bytes.
    uint64_t IndexedBytes = 0;
    /// The widest atomic access (an ordered load or store, or a
    /// read-modify-write) at its address: C keeps it as aligned as the image.
    uint16_t AtomicBytes = 0;
  };
  std::map<va_t, ImageObject> ImageObjects;
  std::map<va_t, DataSymbolBinding> DataSymbolBindings;
  std::set<va_t> UsedDataBindings;
  std::map<std::string, std::string> ExternalDataNames;
  std::optional<std::string> dataSymbolAddress(va_t Slot) const;

  /// The C initializer of an object's scalar value, as the image holds it;
  /// none for zero, for bytes the image does not hold, and for values C
  /// cannot spell exactly.
  std::optional<std::string> imageObjectInitializer(va_t Addr,
                                                    const ImageObject &Obj);
  /// The exact C constant for the float with bits \p Value.
  std::optional<std::string> floatConstantText(uint64_t Value,
                                               const TypeRef &Type) const;
  /// The address constant of a sized image object that \p Address indexes
  /// by a variable byte offset (`i + &table`), or null.  The constant may
  /// point into the object (`i + &table[2]`).
  const HighExpr *indexedImageBase(const HighExpr &Address) const;
  /// The address and size of the outermost data object, sized by its
  /// symbol, that holds \p Addr.
  std::optional<std::pair<va_t, uint64_t>> sizedObjectAt(va_t Addr) const;
  /// The image's sized data objects by address, each as large as the
  /// largest symbol there, and the furthest end of any object up to each.
  std::vector<std::pair<va_t, uint64_t>> SizedObjects;
  std::vector<va_t> SizedObjectReach;
  /// The image object a call argument prints as when it is a string: the
  /// array's address, or a load of a pointer to one.  The expression is the
  /// one that prints the object's name.
  std::optional<std::pair<const HighExpr *, const ImageObject *>>
  stringObjectArgument(const HighExpr &Arg);
  /// Whether parameter \p Index of the call \p E takes a pointer as C
  /// passes it: a variadic argument, an unprototyped declaration's, or for
  /// a \p NarrowString a known string parameter.
  bool takesPointerArgument(const HighExpr &E, size_t Index,
                            bool NarrowString) const;
  /// The C library prototype the call \p E's callee is declared with: a
  /// routine no header declares and nothing else gives a signature.
  const libc::LibCPrototype *calleePrototype(const HighExpr &E) const;
  /// The standard C function the call \p E calls, whose header declares its
  /// parameters, or empty.
  llvm::StringRef headerDeclaredCallee(const HighExpr &E) const;
  /// The C library prototype of the routine \p Symbol links to in this
  /// image: an import keeps its export's name, an object symbol the format's
  /// decoration.
  const libc::LibCPrototype *prototypeForSymbol(llvm::StringRef Symbol) const;
  /// The prototype of the external function a call target \p Name names
  /// (writeForwardDecls), by the one symbol its calls link to.
  const libc::LibCPrototype *externalPrototype(const std::string &Name) const;
  /// The C type \p Type of a prototype on this target: `WINAPI` is stdcall
  /// on 32-bit x86 and nothing elsewhere.
  std::string prototypeType(std::string_view Type) const;
  /// The call a statement makes for its effect alone, whose result no
  /// conversion prints.
  const HighExpr *StatementCall = nullptr;
  /// \p E printed as a statement for its effect alone.
  std::string statementCallText(const HighExpr &E) {
    const HighExpr *Outer = std::exchange(StatementCall, &E);
    std::string Text = exprStr(E);
    StatementCall = Outer;
    return Text;
  }
  /// Functions the code takes the address of, by entry: the C name the
  /// address prints as.
  std::map<va_t, std::string> FunctionAddressNames;
  /// The symbols of those functions this output does not define, declared as
  /// the functions it calls are.
  std::set<std::string> AddressTakenFunctions;
  /// Those it defines, declared before any body: a use can precede the
  /// definition as a call can.
  std::set<const HighFunc *> AddressTakenDefinitions;
  void noteFunctionAddress(va_t Addr, const std::vector<HighFunc> &Funcs);
  /// A constant known to be an address, which a function entry there makes
  /// that function's.
  static bool isAddressProvenance(ConstantAddressProvenance Provenance) {
    return Provenance == ConstantAddressProvenance::Address ||
           Provenance == ConstantAddressProvenance::CodeAddress;
  }
  /// The constant a call argument prints as when it is a function's address.
  const HighExpr *functionAddressArgument(const HighExpr &Arg);
  struct ImageBacking {
    va_t Base;
    va_t End;
    /// The bytes, as an address names them (`table[8]`).
    std::string Name;
    /// A backing that holds relocated pointer slots is a union of words and
    /// bytes: its C object, and the slots, each word-aligned in it.
    std::string Words;
    std::vector<va_t> PointerSlots;
    /// The alignment of the bytes, and the bytes before Base that keep each
    /// address as aligned modulo it as in the image: an atomic access in them
    /// needs that.  1 and 0 for a backing without one.
    unsigned Align = 1;
    uint64_t Pad = 0;
  };
  std::vector<ImageBacking> ImageBackings;
  /// The C address a relocated pointer slot holds: the function or data it
  /// names, or none when no C object names it.
  std::optional<std::string> relocatedSlotTarget(va_t Slot) const;
  /// A pointer-sized object at a relocated slot: the address it holds, as
  /// its initializer.
  std::optional<std::string>
  relocatedSlotInitializer(va_t Addr, const ImageObject &Obj) const;
  /// Declares the objects whose relocated pointer slots name other objects:
  /// the backings that hold them, as words, and the single slots \p Deferred.
  void writePointerBackings(const std::vector<va_t> &Deferred);

  std::vector<HiLoPair> HiLoPairs;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_HIGHC_HIGHCWRITER_H
