//===- LLVMCWriter.h - Internal LLVM IR C writer class ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Internal class declaration for the LLVM-IR-to-C source writer.
/// This header is used only within the backend/c library; do not
/// install it in include/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H
#define NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H

#include "../CIdentifier.h"
#include "../CSourceRecorder.h"
#include "../DebugParamBinding.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

/// Retargetable stream so one function can be buffered, then filtered,
/// before it is written to the caller's ostream.
class LLVMCOut {
  llvm::raw_ostream *Target;

public:
  explicit LLVMCOut(llvm::raw_ostream &Primary) : Target(&Primary) {}
  llvm::raw_ostream &stream() const { return *Target; }
  void retarget(llvm::raw_ostream *Next) { Target = Next; }
  template <typename T> llvm::raw_ostream &operator<<(T &&Value) {
    return (*Target) << std::forward<T>(Value);
  }
};

/// Internal writer that converts LLVM IR modules to goto-style C source.
/// Split across LLVMCEmitter.cpp (module-level orchestration),
/// LLVMCFuncWriter.cpp (function rendering), LLVMCStmtWriter.cpp (instruction
/// rendering), and LLVMCExprWriter.cpp (value/expression rendering).
class LLVMCWriter {
public:
  LLVMCWriter(llvm::raw_ostream &OS, const CEmitterOptions &Opts,
              DebugContext *Dbg, const BinaryImage *Img = nullptr,
              bool GuardAnalysisOnlyFunctions = true,
              CSourceRecorder *Recorder = nullptr)
      : OS(OS), Opts(Opts), Dbg(Dbg), Img(Img),
        GuardAnalysisOnlyFunctions(GuardAnalysisOnlyFunctions),
        SourceRecorder(Recorder) {
    if (Img && importNamesAreCNames(Opts.Format))
      for (const Import &Imp : Img->Imports)
        if (!Imp.Name.empty())
          ImportEntryCNames.insert(Imp.Name);
  }

  /// The C library whose declarations the program's calls follow.
  libc::LibCVariant libcVariant() const {
    return Img ? libc::libcVariantOf(*Img) : Opts.libcVariant();
  }

  //--- Module-level (LLVMCEmitter.cpp) ---
  void writeModule(llvm::Module &Mod, const llvm::Function *Only = nullptr);
  void prepareFunctionIdentifiers(llvm::Module &Mod);
  /// One declaration per imported symbol, admitted only when every printed
  /// call has the header's complete scalar or HFA operand layout.
  void prepareSourceImportPrototypes(llvm::Module &Mod);
  const FunctionSym *sourceImportCallee(const llvm::CallBase &Call) const;
  bool groupedFloatingCall(const llvm::CallBase &Call,
                           const FunctionSym &Function) const;
  std::string sourceImportDeclaration(const FunctionSym &Function,
                                      llvm::StringRef Name);
  std::map<std::string, FunctionSym> SourceImportPrototypes;
  /// Records which symbol each function identifier stands for
  /// (CSourceMap::Names), as HighC does, from the ordinary render.
  void recordSourceNames(const llvm::Module &Mod);
  std::string functionIdentifier(const llvm::Function &Fn) const;
  std::string sourceScalarType(llvm::Type *Type, bool Signed) const;
  /// \p Fn's C declaration of \p Declarator; with \p Params, of its first
  /// that many parameters only.
  std::string sourceFunctionDeclaration(
      const llvm::Function &Fn, llvm::StringRef Declarator,
      std::optional<unsigned> Params = std::nullopt) const;
  /// The C name of global \p Name: the module names a global by its symbol
  /// (cNameOfLLVMName), but a declaration of an import by the name its
  /// import entry lists, which is already the C name on some formats.
  llvm::StringRef cNameOfGlobal(llvm::StringRef Name, bool Declaration) const;
  void writeIncludes(llvm::Module &Mod);
  /// What writeModule writes after the includes.
  void writeModuleBody(llvm::Module &Mod);
  /// The program's records \p Text names, as its local types define them
  /// (CEmitterOptions::RecordDefinitions), and the typedefs of their tags
  /// it spells them by; a record only a pointer names, which they do not
  /// define, is declared incomplete.
  void writeRecordDefinitions(std::string &Text);
  /// A pointer to \p Record, by its tag, which the unit then declares; empty
  /// when the record has no C name.
  std::string recordPointerSpelling(const NdType &Record);
  /// Records whose size is required by an emitted object, including arrays.
  void noteRecordValue(TypeRef Type) const;
  void writeStructDefs(llvm::Module &Mod);
  std::string aggregateMemberPath(llvm::Type *Ty,
                                  llvm::ArrayRef<unsigned> Indices);
  void writeGlobals(llvm::Module &Mod);
  void writeImageByteArray(const llvm::GlobalVariable &Global,
                           llvm::StringRef Name);
  /// Declare the image objects \p Fns read or write, and those whose
  /// addresses alone their printed \p Text names, with each pointer mirror
  /// the text names that the environment holds rather than the module.
  void writeReferencedImageObjects(llvm::ArrayRef<const llvm::Function *> Fns,
                                   llvm::StringRef Text);
  void writeForwardDecls(llvm::Module &Mod);

  //--- Function rendering (LLVMCFuncWriter.cpp) ---
  bool isAnalysisOnlyFunction(const llvm::Function &Fn) const;
  /// Whether printed code or data names \p Fn: a body's instruction, a
  /// definition's personality or a global's initializer.
  bool isReferencedByProjection(const llvm::Function &Fn) const;
  void writeFunction(llvm::Function &Fn);
  void writeAnalysisOnlyFunction(llvm::Function &Fn);
  void writeFunctionProjection(llvm::Function &Fn);
  void renderFunctionProjection(llvm::Function &Fn);
  void setupFunction(llvm::Function &Fn);
  void emitFunctionDecls(llvm::Function &Fn);
  void scanReferencedBlocks(llvm::Function &Fn);
  void planExpressionMaterialization(llvm::Function &Fn);
  void markInlinable(llvm::Function &Fn);
  static bool isFloatingPointBitcast(const llvm::Instruction &Inst);
  static bool isNativeVectorIntrinsic(const llvm::CallBase &Call, Arch TheArch);
  std::string fcmpInlineText(const llvm::FCmpInst &Compare);
  std::string gepExpr(const llvm::GEPOperator &GEP);
  bool writeRawMemoryCopy(llvm::Instruction &Inst, int Indent);
  void planRawMemoryUpdates(llvm::Function &Fn);
  bool writeRawMemoryUpdate(const llvm::StoreInst &Store, int Indent);
  std::string rawMemoryLoadText(const llvm::LoadInst &Load);
  /// A call whose result is printed at exactly one later instruction is
  /// inlined there. Ctor returns, noreturn, and values with two printed
  /// uses stay assigned. A call that mentions a slot is not moved past a
  /// destructor of that slot.
  void markSinglePrintedUseCalls(llvm::Function &Fn);
  bool sameSlotDestructorBetween(const llvm::CallBase &Call,
                                 const llvm::Instruction &Sink) const;
  void writeExceptionAnnotation(const llvm::Function &Fn);
  bool functionHasWindowsEHPads(const llvm::Function &Fn) const;
  bool functionIsCxxEH(const llvm::Function &Fn) const;
  /// Personality / metadata names a language EH runtime. Unwind-only pdata
  /// (`encoding=unknown` / x64 unwind, empty personality) is not a `try`.
  bool functionNeedsAnalysisOnlyEHWrap(const llvm::Function &Fn) const;
  struct EHWrapClause {
    enum class Kind { SEHExcept, SEHFinally, CxxCatch, CxxCleanup };
    Kind Kind = Kind::SEHExcept;
    std::string Clause;
    const llvm::CatchSwitchInst *Switch = nullptr;
    const llvm::CleanupPadInst *Cleanup = nullptr;
    std::vector<const llvm::BasicBlock *> Body;
  };
  std::vector<EHWrapClause> collectWindowsEHWraps(const llvm::Function &Fn);
  void writeEHWrapOpen(const EHWrapClause &Clause, int Indent);
  void writeEHWrapClose(const EHWrapClause &Clause, int Indent);
  void writeBasicBlock(const llvm::BasicBlock &BB, int Indent);
  /// Plan a complete bounded scalar CFG before rendering nested loops/ifs.
  /// Refusal leaves both the output and writer state unchanged.
  bool tryWriteScalarRegions(llvm::Function &Fn, int Indent);
  void coalesceScalarPhiNames(llvm::Function &Fn);
  /// Typed pure-integer expressions for the admitted scalar source contract.
  /// Unknown nodes or exhausted bounds keep the existing expression writer.
  std::optional<std::string> scalarExpressionText(const llvm::Value *V,
                                                  bool ForceExpression = false,
                                                  bool InvertCompare = false,
                                                  bool Parenthesize = true);
  std::optional<std::string> scalarConditionText(const llvm::Value *V,
                                                 bool Invert = false);
  std::optional<std::string> scalarUpdateText(const llvm::Value *Destination,
                                              const llvm::Value *Incoming);
  struct ScalarConditionalUpdate {
    std::string Initial;
    std::string Condition;
    std::string Update;
  };
  std::optional<ScalarConditionalUpdate>
  scalarConditionalUpdate(const llvm::Value *Destination,
                          const llvm::Value *Incoming);

  static bool isCallClobberName(llvm::StringRef Name) {
    return Name.contains("_call_clobber");
  }

  static bool isCallClobberValue(const llvm::Value *V) {
    return V && V->hasName() && isCallClobberName(V->getName());
  }

  bool isUnknownPlaceholder(const llvm::Value *V) const {
    if (!V)
      return false;
    if (auto It = UnknownPlaceholderCache.find(V);
        It != UnknownPlaceholderCache.end())
      return It->second;
    bool Unknown = isCallClobberValue(V) || llvm::isa<llvm::UndefValue>(V) ||
                   llvm::isa<llvm::PoisonValue>(V);
    if (!Unknown)
      if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
          Inst && Inst->getNumOperands() == 1 &&
          (llvm::isa<llvm::CastInst>(Inst) ||
           llvm::isa<llvm::FreezeInst>(Inst)))
        Unknown = isUnknownPlaceholder(Inst->getOperand(0));
    UnknownPlaceholderCache.insert({V, Unknown});
    return Unknown;
  }

  bool isUnknownCopyInst(const llvm::Instruction &Inst) const;
  bool allocaAddressTaken(const llvm::AllocaInst *AI) const;
  /// Same-block `store 0; store computed; load` must reprint the computed
  /// address. A later non-immediate store invalidates `AllocaImmediates`.
  bool computedAllocaLoadIsForwarded(const llvm::LoadInst *LI) const;
  bool allocaLoadHasReachingStore(const llvm::LoadInst *LI) const;
  std::optional<std::pair<const llvm::Value *, uint64_t>>
  peelPointerOffset(const llvm::Value *V) const;
  struct TypedAccess {
    std::string Text;
    TypeRef Type;
  };
  /// \p Text, a member of type \p Member that \p Load reads, as the value
  /// the load gives: an integer read of a pointer member converts it.
  std::string memberAsAccessed(const llvm::LoadInst &Load, std::string Text,
                               const TypeRef &Member) const;
  /// The parameters the signature types as pointers, from the debug
  /// information or the local types, which the code computes with as
  /// integers.
  std::set<const llvm::Argument *> PointerParams;
  /// The tags recordPointerSpelling spelled.
  std::set<std::string> RecordPointerTags;
  /// The pointer type the signature returns, where the code returns it as
  /// an integer; empty otherwise.
  std::string PointerReturnType;
  TypeRef typeOfValue(const llvm::Value *V) const;
  std::string indirectCalleeStr(const llvm::Value *Callee,
                                bool MarkChain = false);
  /// The member at \p Ptr an access of \p AccessSize bytes reads or
  /// writes; of a union's alternatives of that width, the one its \p Class
  /// holds (NdType::scalarOfClassAt).
  std::optional<TypedAccess>
  typedRecordAccess(const llvm::Value *Ptr, uint16_t AccessSize,
                    bool EnterNestedAtZero = true,
                    NdAccessClass Class = NdAccessClass::Unknown) const;
  /// The member \p LI reads, and the member \p SI writes, as wide as the
  /// value and of its class.
  std::optional<TypedAccess> typedLoadAccess(const llvm::LoadInst &LI) const;
  std::optional<TypedAccess> typedStoreAccess(const llvm::StoreInst &SI) const;
  /// PDB/S_LOCAL or HighC `var_mHEX` name for a synthetic `[N x i8] frame`
  /// GEP.  Address-of a slot is `&record` / `&var_m188`; a load of an
  /// interior field is `record.p`.  `Synthesize` creates a `var_m*` only
  /// for call-argument addresses; unnamed load/store soup stays soup.
  /// `Overlay` uses `CallType` (`ArgList.types_`) on integer stores of a
  /// richer PDB record (`TPtr.p`). Loads and address-of stay the PDB field.
  std::optional<TypedAccess>
  /// \p ValueType is the type a value access reads or writes; a frame
  /// record member serves only integer accesses.
  frameSlotAccess(const llvm::Value *Ptr, uint16_t AccessSize, bool AddressOf,
                  bool Synthesize = false, bool Overlay = false,
                  const llvm::Type *ValueType = nullptr) const;
  std::optional<TypedAccess> typedIndexAccess(const llvm::Value *Ptr);
  std::string indexExprStr(const llvm::Value *V);
  bool isComposedRemValue(const llvm::Value *V);
  bool isRedundantFlagAnd(const llvm::Instruction &Inst) const;
  bool usersOnlyFeedInlinable(const llvm::Value *V) const;
  void markComposedPrints(llvm::Function &Fn);
  void markRedundantPhiCopies(llvm::Function &Fn);
  /// A temp whose one composed value is copied into another slot, and whose
  /// other uses are only flag tests, is not printed.
  void omitSingleCopyCursorTemp(llvm::Function &Fn);
  /// A field stored beside a dead null arm is the call argument.
  void forwardDeadNullFieldArg(
      llvm::ArrayRef<
          std::pair<const llvm::CallBase *, const llvm::AllocaInst *>>
          Uses);
  std::string composedReprintText(const llvm::Value *V);
  bool isNamedParamValue(const llvm::Value *V) const;
  bool allocaOnlyHoldsImmediates(const llvm::AllocaInst *Slot) const;
  bool allocaLoadsComposeImmediate(const llvm::AllocaInst *Slot) const;
  std::optional<std::string>
  uniqueAllocaImmediate(const llvm::AllocaInst *Slot) const;
  bool isKilledEntryZeroStore(const llvm::StoreInst *Store,
                              const llvm::AllocaInst *Slot) const;
  bool allocaHomeIsNamedParam(const llvm::AllocaInst *Slot) const;
  bool isDeadImmediateInit(const llvm::AllocaInst *Slot,
                           const llvm::Value *Stored) const;
  bool storedTypedMemberLoad(const llvm::Value *Stored) const;
  bool isJoinFieldVsImmediateHome(const llvm::AllocaInst *Slot) const;
  std::optional<std::string>
  joinFieldDefaultText(const llvm::AllocaInst *Slot) const;
  bool isJoinCallArgAlloca(const llvm::AllocaInst *Slot) const;
  bool isJoinArmStore(const llvm::StoreInst *SI);
  bool isJoinArmStored(const llvm::AllocaInst *Slot,
                       const llvm::Value *Stored) const;
  bool allocaHasPrintedLoad(const llvm::AllocaInst *Slot);
  bool allocaStoreIsHidden(const llvm::AllocaInst *Slot,
                           const llvm::Value *Stored);
  bool instructionIsPrinted(const llvm::Instruction &Inst);
  bool isPrintPassthrough(const llvm::BasicBlock *BB);
  const llvm::BasicBlock *printBranchTarget(const llvm::BasicBlock *To);
  /// A block that only computes one store and branches to \p Join. Null when
  /// the arm has a call, a phi, or more than one predecessor.
  const llvm::BasicBlock *
  straightAssignArmJoin(const llvm::BasicBlock *Arm) const;
  bool joinPrintsNext(const llvm::BasicBlock *From,
                      const llvm::BasicBlock *Join);
  /// False successor that is not the next block, but whose only predecessor
  /// is \p From and whose unconditional branch targets that next block.
  /// Null when the block is already next, has another entry, or branches
  /// elsewhere.
  const llvm::BasicBlock *elseBodyFallsIntoNext(const llvm::BasicBlock *From,
                                                const llvm::BasicBlock *Else);
  /// True successor with only this edge, printed inside the if. A chain of
  /// print-passthrough hops is followed when the block after them still has
  /// only that edge. Null when another edge enters it, it is an EH pad, or
  /// it has already been printed.
  /// \p InlineAssignArm also accepts one store-only block when the other edge
  /// is not the same assign diamond, the join is not already the next printed
  /// block, and a false-skip does not own the condition.
  const llvm::BasicBlock *straightLineTrueArm(const llvm::BasicBlock *From,
                                              const llvm::BasicBlock *Then,
                                              bool InlineAssignArm = false);
  /// Single-entry blocks reached by an inlined arm, up to a rejoin that
  /// already has another entry. Empty when the tail returns or is shared.
  bool
  singleEntryUncondTail(const llvm::BasicBlock *From,
                        const llvm::BasicBlock *Arm,
                        const llvm::BasicBlock *Edge,
                        llvm::SmallVectorImpl<const llvm::BasicBlock *> &Chain);
  /// Single-entry conditional block whose two arms are straight tails to the
  /// same rejoin. The block is printed at the branch instead of a goto.
  bool singleEntryCondRegion(
      const llvm::BasicBlock *From, const llvm::BasicBlock *Edge,
      const llvm::BasicBlock *&Region,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &TrueChain,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &FalseChain);
  /// Inverted assign, then one or more true-edge assigns, then a default
  /// assign, all to one join. The default may have other entries.
  /// A null-checked cursor loop whose latch only stores that cursor.
  bool cursorForLoop(const llvm::BasicBlock *Header,
                     const llvm::BasicBlock *&Latch,
                     const llvm::BasicBlock *&Exit,
                     const llvm::BasicBlock *&Body,
                     const llvm::AllocaInst *&Slot);
  void writeCursorFor(const llvm::BasicBlock *Header,
                      const llvm::BasicBlock *Latch,
                      const llvm::BasicBlock *Exit,
                      const llvm::BasicBlock *Body,
                      const llvm::AllocaInst *Slot, int Indent);
  /// Cursor loop whose header compares a field. The null test and the
  /// `m_pNext` store sit in a predecessor and a phi latch.
  bool splitPhiCursorLoop(const llvm::BasicBlock *Header,
                          const llvm::BasicBlock *&Latch,
                          const llvm::BasicBlock *&Step,
                          const llvm::AllocaInst *&Slot);
  void writeSplitPhiCursorFor(const llvm::BasicBlock *Header,
                              const llvm::BasicBlock *Latch,
                              const llvm::BasicBlock *Step,
                              const llvm::AllocaInst *Slot, int Indent);
  bool cursorCopyLatch(const llvm::BasicBlock *BB);
  /// Last `->` field load in the block, stopping before `Stop` when set.
  /// Uses the cached load text when the pointer peel needs stores that have
  /// not been visited yet.
  std::string cursorFieldIncText(const llvm::BasicBlock *BB,
                                 const llvm::Instruction *Stop);
  bool assignSelectElseIf(
      const llvm::BasicBlock *From, const llvm::BasicBlock *FirstArm,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Conds,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Arms,
      const llvm::BasicBlock *&Default, const llvm::BasicBlock *&Join);
  /// False successor that is the next printed block. Its true target is the
  /// block printed immediately after it, so the body prints inside `if (!cond)`
  /// and that target falls through. Null when a straight true arm already
  /// owns the if, the body has another entry, or either edge assigns a phi.
  const llvm::BasicBlock *straightLineFalseSkip(const llvm::BasicBlock *From,
                                                const llvm::BasicBlock *Else);
  /// False edge is the next block. Every path through it reaches the true
  /// edge's target, and that target already has another entry. The region
  /// prints once inside the inverted test; the shared target stays outside.
  bool sameTargetReleaseRegion(
      const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
      const llvm::BasicBlock *&Join);
  /// Successive tests whose taken arms return or fall into one shared tail.
  /// The tests print as if/else-if and that tail follows once.
  bool exclusiveSkipElseIf(
      const llvm::BasicBlock *From,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Headers,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Arms,
      llvm::SmallVectorImpl<char> &ArmOnTrue, const llvm::BasicBlock *&Tail,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Owned,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &FlatConds,
      llvm::SmallVectorImpl<char> &FlatTaken,
      llvm::SmallVectorImpl<unsigned> &FlatCounts,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Elided);
  bool writeExclusiveSkip(llvm::ArrayRef<const llvm::BasicBlock *> Headers,
                          llvm::ArrayRef<const llvm::BasicBlock *> Arms,
                          llvm::ArrayRef<char> ArmOnTrue,
                          const llvm::BasicBlock *Tail,
                          llvm::ArrayRef<const llvm::BasicBlock *> Owned,
                          llvm::ArrayRef<const llvm::BasicBlock *> FlatConds,
                          llvm::ArrayRef<char> FlatTaken,
                          llvm::ArrayRef<unsigned> FlatCounts,
                          llvm::ArrayRef<const llvm::BasicBlock *> Elided,
                          int Indent);
  void writeExclusiveArm(
      const llvm::BasicBlock *BB, const llvm::BasicBlock *Tail,
      const llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &Owned,
      const llvm::BasicBlock *Stop,
      llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &Seen, int Indent);
  /// Two or more tests whose arms only reach one private join. The arms
  /// print as if/else and that join stays a single tail.
  bool
  privateJoinElseIf(const llvm::BasicBlock *From,
                    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Conds,
                    llvm::SmallVectorImpl<const llvm::BasicBlock *> &TrueEdges,
                    const llvm::BasicBlock *&ElseEdge,
                    const llvm::BasicBlock *&Join,
                    llvm::SmallVectorImpl<const llvm::BasicBlock *> &Skip);
  /// False edge is a region that ends before the true edge's only block.
  /// The region prints inside the inverted test and that block falls through.
  bool regionBeforeSkipTarget(
      const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
      const llvm::BasicBlock *&SkipTarget);
  /// True arm sits after a fallthrough that reaches the same join, and that
  /// join has no other entry. Both arms print as if/else and the join follows.
  bool laterTrueArmJoinsFallthrough(
      const llvm::BasicBlock *From, const llvm::BasicBlock *Else,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Region,
      llvm::SmallVectorImpl<const llvm::BasicBlock *> &Side,
      const llvm::BasicBlock *&Arm, const llvm::BasicBlock *&Join);
  /// Two or more condition-only blocks share one false block. The last true
  /// edge is a straight arm to the same join that false block reaches, and
  /// nothing else enters that false block.
  bool conditionChainElse(const llvm::BasicBlock *From,
                          std::vector<const llvm::BasicBlock *> &Conds,
                          const llvm::BasicBlock *&Arm,
                          const llvm::BasicBlock *&Def,
                          const llvm::BasicBlock *&Join, bool &InvertCond);
  bool blockOnlyFeedsBranch(const llvm::BasicBlock *BB);
  /// Next block's false edge rejoins this condition's true target, and its
  /// true edge is a straight arm. Null when the block has a printed
  /// statement, another entry, or a different rejoin.
  const llvm::BasicBlock *andRejoinTest(const llvm::BasicBlock *From,
                                        const llvm::BasicBlock *Else);
  /// Next block is only a conditional goto to \p Target, same as \p Pred.
  /// Null when it has a printed statement, another entry, or a different
  /// target.
  bool pureSameTargetSkip(const llvm::BasicBlock *BB,
                          const llvm::BasicBlock *Target,
                          const llvm::BasicBlock *Pred);
  void writeCondTrueEdge(const llvm::BasicBlock *From,
                         const llvm::BasicBlock *Then, int Indent,
                         bool AllowFallthrough = true);
  void writeInvertedFalseSkip(const llvm::BasicBlock *From,
                              const llvm::BasicBlock *Else, int Indent);
  /// Inverted skip whose inner false edge is one assign and whose true
  /// edge is the same assign block as the outer true edge. Both assigns
  /// branch to one join, and nothing else enters the shared block.
  bool sharedAssignElse(const llvm::BasicBlock *From,
                        const llvm::BasicBlock *Body,
                        const llvm::BasicBlock *&Alt,
                        const llvm::BasicBlock *&Def,
                        const llvm::BasicBlock *&Join);
  void writeGoto(const llvm::BasicBlock *From, const llvm::BasicBlock *To,
                 int Indent, bool EmitGoto = true);
  /// Successor to take when this branch repeats a pointer test that already
  /// dominates it and nothing stored that pointer. Null when the test is live.
  const llvm::BasicBlock *
  impliedPointerSuccessor(const llvm::CondBrInst *Br) const;
  /// Untaken edge of an implied pointer test when its printed instructions
  /// only store 0 into slots the taken edge also stores.
  bool
  deadNullAssignBlocks(const llvm::CondBrInst *Br,
                       const llvm::BasicBlock *Taken,
                       llvm::SmallVectorImpl<const llvm::BasicBlock *> &Blocks);
  /// True when \p Succ leaves the cursor for that is currently printing.
  /// The exit can be the next block in layout and still sit outside the
  /// for, so that edge stays a break.
  bool cursorForExitsAt(const llvm::BasicBlock *Succ);
  /// The else edge only continues this loop, and no later loop statement
  /// is printed after the if. The continue is the fallthrough.
  bool redundantLoopElseContinue(const llvm::BasicBlock *From,
                                 const llvm::BasicBlock *Else);
  /// One printed call reached only through conditional passthroughs. Each
  /// edge prints that call, so the shared block is not a goto target.
  const llvm::BasicBlock *duplicatedCallTail(const llvm::BasicBlock *Edge);
  /// One call whose only argument is a slot stored on every incoming edge.
  /// Each edge prints that call with its own stored value. A call with
  /// another printed argument keeps the edge assignments.
  const llvm::BasicBlock *phiFedCallTail(const llvm::BasicBlock *Tail);
  /// The function's only return when its one printed call is a destructor.
  /// A goto to that block prints the destructor and the return.
  const llvm::BasicBlock *sharedReturnEpilogue(const llvm::BasicBlock *Tail);
  bool storeFeedsPhiCallTail(const llvm::StoreInst *SI);
  /// One call laid out immediately before the header it branches to. Its
  /// only predecessor is later, so the call prints on that back edge.
  bool backEdgeCallBeforeHeader(const llvm::BasicBlock *BB);
  /// Unconditional branch into an EH marker printed next by the main walk.
  /// Intervening handler blocks print later inside the exception clause.
  bool uncondBranchFallsIntoEHBoundary(const llvm::BasicBlock *From,
                                       const llvm::BasicBlock *To) const;
  /// Bounded structural proof that the C spelling is already zero or one.
  bool isNormalizedBoolean(const llvm::Value *V) const;
  const llvm::Value *peelIntegerView(const llvm::Value *V) const;
  /// Logical shift whose amount does not fit the peeled source. Keep the
  /// widening cast so `>> 32` is not applied to a 32-bit C operand.
  std::string logicalShiftLhs(const llvm::Instruction &Shift, std::string LHS);
  std::string condStr(const llvm::Value *V);
  std::string condStrImpl(const llvm::Value *V);
  /// Value tested by the x86 `ZF || SF` pattern. `condStr` prints it as
  /// `value <= 0`. Null for every other `or`.
  const llvm::Value *jleZeroCore(const llvm::BinaryOperator *BO);
  /// Swapped relational text for a skip that prints the false body.
  /// Empty when the condition is not one compare, so the caller keeps `!`.
  std::optional<std::string> invertedRelationalText(const llvm::Value *V);
  std::optional<std::string> invertedRelationalTextImpl(const llvm::Value *V);
  uint16_t llvmAccessSize(const llvm::Type *Ty) const;
  void noteAllocaStore(const llvm::AllocaInst *Slot, const llvm::Value *Stored);
  void discoverSyntheticFrame(llvm::Function &Fn);
  void collectTypedHomes(llvm::Function &Fn);
  /// The pointer type the callee's prototype gives argument \p Index of
  /// \p Call: its debug information or the MSVC callee table.
  TypeRef expectedCallArgType(const llvm::CallBase &Call, unsigned Index) const;
  std::optional<FunctionSym> debugCallee(const llvm::CallBase &Call) const;
  /// The debug information's function, or the prototype of its name, that
  /// \p Callee is: by its original address, its slot, or the address its
  /// name spells.
  std::optional<FunctionSym> debugFunction(const llvm::Function &Callee) const;
  /// Whether the user declares the routine of C name \p CName
  /// (userPrototype): the unit declares it as the user does, not through the
  /// header that declares the C library's routine of the name.
  bool userDeclaresRoutine(llvm::StringRef CName) const;
  /// How many arguments the calls to \p Callee pass where its prototype
  /// decides them (printedCallArgLimit), which its declaration then takes;
  /// none where its machine parameters do.
  std::optional<unsigned>
  prototypedParamCount(const llvm::Function &Callee) const;
  bool looksLikeHiddenSretOperand(const llvm::Value *Arg) const;
  std::string printedCalleeName(const llvm::CallBase &Call) const;
  /// The C library routine \p Call reaches, directly or through its import,
  /// whose declaration types its arguments and result: one a header the unit
  /// includes declares, or one the runtime's tables type (Prototype), which
  /// the unit declares itself (writeImportCalleeDecls).  Empty for another
  /// call, which passes and returns what the machine did.
  struct LibraryCallee {
    std::string Name;
    const libc::LibCPrototype *Prototype = nullptr;
    explicit operator bool() const { return !Name.empty(); }
  };
  LibraryCallee libraryCallee(const llvm::CallBase &Call) const;
  /// Whether the unit declares \p Call's callee to return nothing where the
  /// call's own type returns a value: a routine declared void, or the
  /// module's definition whose result is settled as none, which prints as
  /// returning void (analyzeVoidReturn) unless it returns a record's address
  /// as an MSVC indirect result.
  bool calleeReturnsNothing(const llvm::CallBase &Call) const;
  /// \p Call's text \p Expr as a value of the call's own type: the pointer a
  /// library routine returns as the integer that holds it, and the result a
  /// callee that returns nothing leaves undefined as zero.
  std::string callResult(const llvm::CallBase &Call, std::string Expr) const;
  /// MSVC ctor/dtor that returns `this` and whose result is unread. A store
  /// into an alloca that nothing loads does not count. Print the call as a
  /// statement in a non-void function too.
  bool unreadMsvcThisReturn(const llvm::CallBase &Call) const;
  unsigned printedCallArgLimit(const llvm::CallBase &Call,
                               llvm::StringRef CalleeName) const;
  bool isTrailingLiveInCopy(const llvm::Value *V) const;
  bool valueFeedsPrintedUse(const llvm::Value *V);
  bool loadOnlyUsedAsCallArgs(const llvm::Value *V) const;
  bool isResultParamValue(const llvm::Value *V) const;
  TypeRef cDisplayType(const TypeRef &Ty) const;
  bool isReservedFrameName(llvm::StringRef Name) const;
  std::optional<VariableSym> debugFrameVariable(int64_t Disp) const;
  void markIndirectCalleeChains(llvm::Function &Fn);
  bool usersOnlySeeImmediate(const llvm::Value *V) const;
  bool usersAreDeadCopies(const llvm::Value *V) const;
  void collectOmittedUnknowns(llvm::Function &Fn);
  void collectOmittedInlinedImmediates(llvm::Function &Fn);

  /// Omit ABI clobber placeholders and unused copies of them. Uses print
  /// `0 /* unknown */` so RaiseException successors stay readable.
  bool isUnusedCallClobber(const llvm::Instruction &Inst) const {
    return OmittedUnknowns.count(&Inst) != 0 ||
           OmittedInlined.count(&Inst) != 0;
  }

  bool isSimpleEntry(const llvm::BasicBlock *BB, const llvm::Function &Fn) {
    return BB == &Fn.getEntryBlock() && !ReferencedBlocks.count(BB);
  }

  //--- Instruction rendering (LLVMCStmtWriter.cpp) ---
  std::optional<uint64_t> syntheticFrameOwnerOff(const llvm::Value *V) const;
  bool slotIsArgList(uint64_t Off) const;
  bool isLeftoverArgListSibling(uint64_t Off) const;
  const llvm::CallBase *
  leftoverPackedCallResult(const llvm::BasicBlock *BB) const;
  bool priorUnknownStoreTo(const llvm::Instruction *SI, uint64_t Off) const;
  bool peelsToUnknown(const llvm::Value *V) const;
  const llvm::CallBase *
  leftoverPackedSiblingFill(const llvm::StoreInst &SI) const;
  const llvm::CallBase *integerCallValue(const llvm::Value *V) const;
  /// i32 call result stored on a home, then copied with a wide load into the
  /// next ArgList slot. The wide store prints `slot.types_ = call` and the
  /// narrow store is omitted.
  void collectLeftoverNarrowCallStores(llvm::Function &Fn);
  bool storeIsLeftoverHomeUndef(const llvm::StoreInst &SI) const;
  void writeInstruction(llvm::Instruction &Inst, int Indent);
  void writeInstructionImpl(llvm::Instruction &Inst, int Indent);
  void writeCall(llvm::CallInst &Call, const std::string &Name, int Indent);
  void writeCallLike(llvm::CallBase &Call, const std::string &Name, int Indent);
  /// Join `p ? field : 0` is the PHI dest, not a rematerialized then-arm
  /// field load used as a call arg (`Find(..., this->m.p->id)`).
  const llvm::PHINode *joinPhiForCallArg(const llvm::Value *Arg,
                                         const llvm::BasicBlock *BB);
  bool mixedPredLastStores(const llvm::AllocaInst *Slot,
                           const llvm::BasicBlock *BB) const;
  const llvm::AllocaInst *joinAllocaForCallArg(const llvm::Value *Arg,
                                               const llvm::CallBase &Call);
  bool phiPrintedAsJoinCallArg(const llvm::PHINode *Phi);
  /// \p Arg as the call spells it; \p PointerSpelling, when given, says
  /// whether the text is an address, as valueStr reports it.
  std::string callArgStr(const llvm::Value *Arg, const llvm::CallBase &Call,
                         unsigned ArgIdx, bool *PointerSpelling = nullptr);
  /// Whether the declaration written for \p Call's callee, or the type a
  /// call through a pointer casts it to, is its LLVM prototype, with an
  /// integer parameter at \p ArgIdx: C converts no address to it
  /// implicitly.
  bool declaredParameterIsInteger(const llvm::CallBase &Call,
                                  llvm::StringRef CalleeName,
                                  unsigned ArgIdx) const;
  std::optional<std::string> enumeratorDisplay(const TypeRef &Ty,
                                               uint64_t Val) const;
  TypeRef enumTypeUsedAsCallArg(const llvm::AllocaInst *Slot) const;
  std::string comparedOperandText(const llvm::Value *V,
                                  const llvm::Value *Other);
  std::string enumStoredText(const llvm::AllocaInst *Slot,
                             const llvm::Value *Stored);
  std::string integerCallStoredText(const llvm::Value *Stored,
                                    bool AsInteger = false);
  std::string inplaceIntegerUpdate(const llvm::StoreInst &SI,
                                   llvm::StringRef Dest,
                                   const llvm::Value *Stored);
  void writePhiCopies(const llvm::BasicBlock *From, const llvm::BasicBlock *To,
                      int Indent, bool ForceMaterialized = false,
                      const llvm::PHINode *Deferred = nullptr,
                      const llvm::PHINode *Declare = nullptr);
  bool phiIncomingIsPrinted(const llvm::PHINode *Phi, llvm::Value *Incoming,
                            bool ForceMaterialized = false);
  bool phiEdgeNeedsMaterialization(const llvm::BasicBlock *From,
                                   const llvm::BasicBlock *To) const;
  bool isReservedEHNormalTarget(const llvm::BasicBlock *BB) const;
  bool edgePrintsPhiCopy(const llvm::BasicBlock *From,
                         const llvm::BasicBlock *To);
  std::string resolveImportCalleeName(const llvm::Value *Callee) const;
  /// resolveImportCalleeName before UserImportIdentifiers renames it.
  std::string importCalleeCName(const llvm::Value *Callee) const;
  bool isImportCalleeOnlyLoad(const llvm::LoadInst *LI) const;
  bool writeIntrinsicCall(llvm::CallBase &Call, int Indent);
  bool writeInlineAsmCall(llvm::CallInst &Call, const std::string &Name,
                          int Indent);
  /// A forward REP STOS or MOVS of \p Element-byte elements (\p Movs) as
  /// the C library's fill or copy, where that writes exactly its bytes, as
  /// HighC prints it (c_memory::fillCall, c_memory::copyCall).  False, with
  /// nothing written, otherwise.
  bool writeFlatStringRoutine(const llvm::CallInst &Call, bool Movs,
                              unsigned Element, int Indent);
  bool callDoesNotReturn(const llvm::CallBase &Call) const;
  void writeGEP(llvm::GetElementPtrInst &GEP, const std::string &Name,
                int Indent);
  void writeReturn(llvm::ReturnInst &Ret, int Indent);
  void writeInvoke(llvm::InvokeInst &Invoke, const std::string &Name,
                   int Indent);
  void writeCatchSwitch(llvm::CatchSwitchInst &CS, int Indent);
  void writeCleanupRet(llvm::CleanupReturnInst &CR, int Indent);
  std::string windowsEHFilterExpr(const llvm::CatchSwitchInst &CS);
  std::string SEHFilterFrameName;
  std::string windowsCxxCatchType(const llvm::CatchPadInst &Pad);
  void emitIndent(int N);
  const llvm::AllocaInst *asAllocaPointer(const llvm::Value *V) const;
  const llvm::Value *allocaStoredValueBefore(const llvm::LoadInst *Load,
                                             bool BoundExpansion = true) const;
  bool isThisFieldAddress(const llvm::Value *V) const;
  /// Last home is a load of `this+imm` (the field *value*, not the address).
  bool isThisFieldValueHome(const llvm::AllocaInst *Slot) const;
  /// Last home is a load of a named synthetic-frame record field.
  bool isFrameFieldValueHome(const llvm::AllocaInst *Slot) const;
  bool isTypedRecordCursorSlot(const llvm::AllocaInst *Slot) const;
  bool isTypedRecordCursorValue(const llvm::Value *V) const;
  bool allocaHasLoad(const llvm::AllocaInst *Slot) const;
  bool cursorSlotIsObserved(const llvm::AllocaInst *Slot) const;
  std::string ultimateComposedText(const llvm::Value *V);

  //--- Expression rendering (LLVMCExprWriter.cpp) ---
  std::string resolveNdDataName(llvm::StringRef Name) const;
  bool isImageDataAddress(va_t Addr) const;
  /// The address among the code that \p CI holds, which the LLVM emitter
  /// keeps as a number: it gives only data sections globals to point into.
  std::optional<va_t> codeSectionAddress(const llvm::ConstantInt &CI) const;
  /// \p CI as the literal or the debug information's data object at its
  /// address among the code, as HighC spells it.
  std::optional<std::string>
  codeSectionAddressText(const llvm::ConstantInt &CI) const;
  std::optional<va_t>
  uniqueAllocaImageImmediate(const llvm::AllocaInst *Slot) const;
  std::string namedImageObject(va_t Addr) const;
  /// Recognized image text beside the load that reads it, even when the
  /// object's declaration is folded out of the displayed function body.
  std::string imageLoadComment(const llvm::LoadInst &Load) const;
  std::optional<va_t> imageDataVA(const llvm::Value *V) const;
  static unsigned imageIntegerAccessSize(const llvm::Type *Ty) {
    if (!Ty || !Ty->isIntegerTy())
      return 0;
    const unsigned Bits = Ty->getIntegerBitWidth();
    return Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64 || Bits == 80 ||
                   Bits == 128
               ? Bits / 8
               : 0;
  }
  /// A shared image range and byte offset within its IR byte-array global.
  std::optional<std::pair<const llvm::GlobalVariable *, uint64_t>>
  imageByteArrayBacking(const llvm::Value *V, uint64_t AccessSize) const;
  /// Pointer into that backing range.  Interior offsets must not become
  /// independent C globals.
  std::optional<std::string> imageByteArrayPointer(const llvm::Value *V,
                                                   uint64_t AccessSize) const;
  std::optional<uint64_t> foldReadonlyScalar(va_t Addr, uint16_t Size) const;
  std::optional<std::string> foldImmediate(const llvm::Value *V) const;
  /// The image address of the object the unit declares for pointer \p V,
  /// which imageDataCName names; nullopt for none, or for bytes of an array
  /// the module defines.
  std::optional<va_t> declaredImageDataVA(const llvm::Value *V) const;
  std::string imageDataCName(const llvm::Value *V) const;
  std::string getName(const llvm::Value *V);
  std::string rawSourceName(const llvm::Value *V);
  TypeRef statedVariableType(const llvm::Value *V) const;
  const SourceCallTypeHint *analystCallHint(const llvm::CallBase &Call) const;
  std::string sourceVariableDeclaration(const llvm::Instruction &I,
                                        llvm::StringRef Name);
  std::map<const llvm::Value *, std::string> SourceValueIds;
  std::string freshVar(const std::string &Hint = "v");
  std::string valueStr(const llvm::Value *V, bool *PointerSpelling = nullptr);
  std::string globalNameStr(const llvm::GlobalValue *Global);
  /// Storage and aggregate integer constants retain their literal bits;
  /// expression uses may additionally carry an image-address spelling.
  std::string constStr(const llvm::Constant *C, bool AllowImageAddress = true);
  std::string blockLabel(const llvm::BasicBlock *BB);
  std::string binopStr(unsigned Opcode, const std::string &LHS,
                       const std::string &RHS, llvm::Type *Ty);
  std::string integerPointerOperandStr(const llvm::Value *Operand);
  std::optional<std::string>
  targetPointerMaskIdentity(const llvm::BinaryOperator &Op);
  std::optional<std::string> lowBitsMaskCast(const llvm::BinaryOperator &Op);
  std::string castStr(unsigned Opcode, const std::string &Src,
                      llvm::Type *SrcTy, llvm::Type *DstTy);
  /// The text \p Cast reads as where its operand reads as \p Text: the
  /// same, unless the cast moves between an integer and a pointer, whose C
  /// types differ.
  std::string castOfText(const llvm::CastInst &Cast, const std::string &Text);
  std::string cmpStr(llvm::CmpInst::Predicate Pred, const std::string &LHS,
                     const std::string &RHS, bool IsFP,
                     bool CastUnsigned = true);
  /// Width-aware spelling shared by assigned, inline and inverted icmp.
  /// No outer parentheses; renderInline adds them for expression context.
  std::string icmpInlineText(const llvm::ICmpInst &CI, bool Invert = false);
  /// True when \p V prints as an unsigned integer of exactly \p Bits.
  /// Untyped values and widening views stay cast.
  bool operandIsUnsignedWidth(const llvm::Value *V, unsigned Bits) const;
  std::string unsignedCompareOperand(const llvm::Value *V, std::string Text);
  std::string renderInline(const llvm::Instruction &Inst);
  std::string renderInlineImpl(const llvm::Instruction &Inst);
  /// \p Call as C; as a \p Statement, whose value nothing reads.
  std::string callExpr(const llvm::CallBase &Call, bool Statement = false);
  /// The C function-pointer type of \p Call's callee, from the call's own
  /// type and convention.  \p Strict refuses what shown C may approximate.
  std::string indirectCalleeType(const llvm::CallBase &Call, bool Strict);
  /// The parameters an indirect \p Call's callee takes where it loads the
  /// callee from a typed record's member that points to a function, as
  /// SystemTable->ConOut->OutputString does; none otherwise.
  std::optional<unsigned> typedSlotParamCount(const llvm::CallBase &Call) const;
  std::string preservedIndirectCalleeStr(const llvm::CallBase &Call);
  std::string atomicRMWText(const llvm::AtomicRMWInst &AI);
  std::string ctorThisAddress(const llvm::CallBase &Call);

  //--- State ---
  LLVMCOut OS;
  CEmitterOptions Opts;
  DebugContext *Dbg;
  const BinaryImage *Img;
  const llvm::Module *CurMod = nullptr;
  bool GuardAnalysisOnlyFunctions;
  CSourceRecorder *SourceRecorder = nullptr;
  const llvm::Function *OnlyFunction = nullptr;
  std::set<const llvm::GlobalValue *> SelectedGlobals;
  std::set<std::string> EmittedGlobalNames;
  /// The routines writeImportCalleeDecls declares, ordinary identifiers a
  /// record of the same name leaves to its tag (`struct stat64`).
  std::set<std::string> DeclaredCalleeNames;
  /// When false, emit recovered statements without a C wrapper so analysis-only
  /// functions can nest the listing inside `#if 0` of the trap stub.
  bool EmitFunctionWrapper = true;
  CProjectionIdentifierAllocator GlobalIdentifierAllocator;
  std::string Win64CallABI;
  std::map<std::pair<Intrinsic, unsigned>, std::string> FPStateHelperNames;
  /// The helper each modeled supervisor call in the unit prints through.
  std::map<Intrinsic, std::string> SyscallHelperNames;
  std::map<const llvm::Function *, std::string> FunctionIdentifiers;
  /// The identifiers FunctionIdentifiers gives the module's functions.
  std::set<std::string> FunctionIdentifierNames;
  /// The identifier each routine the user declares, called through its
  /// import, takes where a header the unit includes declares the C library's
  /// routine of its name, by that name (writeIncludes), and the name each
  /// such identifier links by.
  std::map<std::string, std::string> UserImportIdentifiers;
  std::map<std::string, std::string> UserImportLinkNames;
  /// Exact projected prototypes captured while rendering definitions. They
  /// include source presentation choices such as inferred void and sret.
  std::map<const llvm::Function *, std::string> DefinitionDeclarations;
  std::map<const llvm::GlobalVariable *, std::string> ExternalDataIdentifiers;
  /// The C name each function's symbol spells, for its definition's comment.
  std::map<const llvm::Function *, std::string> FunctionSymbolNames;
  /// Each function's symbol as the image spells it (CSourceName::Symbol).
  std::map<const llvm::Function *, std::string> FunctionSources;
  /// Import entry names that are C names (SymbolDecorations.def).
  llvm::StringSet<> ImportEntryCNames;
  /// The calls a function's next render keeps as statements: its text
  /// printed each other than once where the analysis inlined it.
  std::set<const llvm::CallBase *> StatementCalls;

  int NextVar = 0;
  std::map<const llvm::Value *, std::string> ValNames;
  std::set<std::string> UsedNames;
  /// Separate FPREM/FNSTSW IR calls whose status was captured inside the
  /// FPREM asm before C compilation can spill and pop the x87 stack.
  std::set<const llvm::CallInst *> CapturedX87StatusCalls;
  std::map<const llvm::BasicBlock *, std::string> BlockLabels;
  std::map<const llvm::BasicBlock *, size_t> DeferredBlockLabels;
  std::set<const llvm::BasicBlock *> ReferencedBlocks;
  bool HasCIntrinsics = false;
  /// A simple load or store has a type with an aligned(1), may_alias alias,
  /// so the aliases are declared when UseUnalignedPointers is set.
  bool NeedsUnalignedTypes = false;
  /// The aliases were declared; an access spelled through one needs that.
  bool UnalignedTypesWritten = false;
  std::set<std::string> IntrinsicMappedNames;
  LLVMCAnalysisState Analysis;
  llvm::DominatorTree Dominators;
  std::map<const llvm::Value *, std::string> InlineCache;
  /// Fixed expression boundaries, chosen before declarations or assignments
  /// can be omitted and retained by later display-only inlining passes.
  std::set<const llvm::Value *> MaterializedExpressions;
  /// A captured scalar update can move to its own store only after every
  /// remaining SSA use and intervening effect has been checked.
  std::map<const llvm::StoreInst *, const llvm::BinaryOperator *>
      RawMemoryUpdates;
  std::set<const llvm::Instruction *> RawMemoryUpdateInputs;
  std::set<const llvm::LoadInst *> RawMemoryExpressionLoads;
  /// Calls currently being printed as expressions, so a cyclic reprint
  /// falls back to the instruction name.
  std::vector<const llvm::CallBase *> RenderingCalls;
  /// The records whose members the text reads, by their names in the
  /// source, which writeRecordDefinitions has the local types define: the
  /// text reaches them through members and need not spell them.
  mutable std::set<std::string> MemberRecords;
  /// Record call adapters use types already declared by an included header.
  /// The textual type scan must not define those records a second time.
  std::set<std::string> HeaderCallRecords;
  mutable std::map<const llvm::Value *, std::string> KnownImmediates;
  bool UseScalarExpressionTypes = false;
  std::set<std::string> ScopedScalarNames;
  mutable std::set<const llvm::Value *> ActiveImmediateFolds;
  mutable unsigned ImmediateFoldWork = 0;
  mutable const llvm::Function *LocalLoadValuesFor = nullptr;
  mutable llvm::DenseMap<const llvm::LoadInst *, const llvm::Value *>
      LocalLoadValues;
  mutable llvm::DenseMap<const llvm::AllocaInst *, bool> ExactLocalLoadSlots;
  /// Exact local definitions for bounded address/type analysis, independent
  /// of the expression-size limit used when printing an expanded value.
  mutable llvm::DenseMap<const llvm::LoadInst *, const llvm::Value *>
      LocalReachingValues;
  mutable llvm::DenseMap<const llvm::LoadInst *, bool> InitializedAllocaLoads;
  mutable llvm::DenseMap<const llvm::LoadInst *, const llvm::Value *>
      CrossBlockLoadValues;
  mutable std::map<const llvm::AllocaInst *, std::string> AllocaImmediates;
  /// Value names and cast operands do not change while this function prints.
  mutable llvm::DenseMap<const llvm::Value *, bool> UnknownPlaceholderCache;
  /// Read-before-overwrite for an entry zero store depends only on immutable
  /// function IR. Clear when setupFunction selects another function.
  mutable llvm::DenseMap<const llvm::StoreInst *, bool> KilledEntryZeroCache;
  /// The LLVM function is immutable while it is projected to C.  Cache the
  /// address-escape classification until setupFunction selects another one.
  mutable llvm::DenseMap<const llvm::AllocaInst *, bool>
      AllocaAddressTakenCache;
  struct ImmediateUser {
    const llvm::StoreInst *Store = nullptr;
    bool IsSelfCopy = false;
  };
  /// Retain ordered non-load users of immutable function IR, but fold their
  /// values against current print state.  Nested walks add other slots, so
  /// their insertion must not invalidate an iterator in the outer walk.
  mutable std::map<const llvm::AllocaInst *,
                   llvm::SmallVector<ImmediateUser, 2>>
      UniqueImmediateUsers;
  /// The headers the output includes.
  std::set<std::string> IncludedHeaders;
  /// Whether an included header declares data object \p Name
  /// (LibCDataObjects.def), which C then reaches as the header declares it.
  bool headerDeclaresObject(llvm::StringRef Name) const {
    const char *Header = libc::headerOfDataObject(Name);
    return Header && IncludedHeaders.count(Header);
  }
  mutable CProjectionIdentifierAllocator ImageIdentifierAllocator;
  mutable std::map<va_t, std::string> ImageObjectNames;
  /// The name the image's data symbol at \p Addr gives the object there, and
  /// the symbol as C spells it, which namedImageObject spells an identifier
  /// from; nullopt where the image names no data there.
  std::optional<std::pair<std::string, std::string>>
  imageDataSymbolName(va_t Addr) const;
  /// Original symbol spellings and recognized image text, by identifier.
  mutable std::map<std::string, std::string> ImageObjectSymbols;
  /// Text already scanned while naming an image object, reused at its reads.
  mutable std::map<va_t, std::string> ImageObjectStrings;
  /// Original symbol spelling or recognized text beside an image declaration.
  std::string imageObjectSymbolLine(llvm::StringRef Name) const;
  /// Image data no symbol or debug information names: its name is the
  /// writer's own.
  mutable std::set<va_t> SynthesizedImageObjects;
  /// Whether namedImageObject(\p Addr) is a name the writer made up.
  bool synthesizedImageObject(va_t Addr) const {
    namedImageObject(Addr);
    return SynthesizedImageObjects.count(Addr) != 0;
  }
  /// How the module's code reaches each image data address: the sizes of its
  /// accesses and whether it calls through the pointer there, which name
  /// unnamed data as the listing names it.
  struct ImageDataUse {
    std::set<uint64_t> AccessBytes;
    bool CallSlot = false;
  };
  std::map<va_t, ImageDataUse> ImageDataUses;
  void collectImageDataUses(llvm::Module &Mod);
  /// Declare the imports that calls through slots the loader binds outside
  /// the import table print by name (resolveImportCalleeName), with the type
  /// the calls return.
  void writeImportCalleeDecls(llvm::Module &Mod);
  /// The import a call through a slot the loader binds, or to the symbol
  /// such a slot holds, reaches; empty for any other call.
  std::string boundImportCallee(const llvm::CallBase &Call) const;
  /// Last store in the current block. Cleared with `AllocaImmediates`.
  std::map<const llvm::AllocaInst *, const llvm::Value *> AllocaLastValues;
  /// Last store seen in `collectTypedHomes`. Survives per-block
  /// `AllocaLastValues` resets so a later-block rem can still name
  /// `key % this->m_table.m_nBins`.
  std::map<const llvm::AllocaInst *, const llvm::Value *> AllocaHomeValues;
  /// Allocas stored exactly once. Safe to follow `AllocaHomeValues`
  /// across blocks; reused cursor homes stay out.
  std::set<const llvm::AllocaInst *> UniqueHomes;
  std::set<const llvm::AllocaInst *> JoinCallArgAllocas;
  std::map<const llvm::AllocaInst *, std::set<const llvm::BasicBlock *>>
      JoinArmBlocks;
  /// Assign-and-branch arms already printed inside a conditional.
  std::set<const llvm::BasicBlock *> FoldedJoinArms;
  /// Single-predecessor else bodies printed at the branch so their later
  /// copy is skipped.
  std::set<const llvm::BasicBlock *> InlinedFallthroughBlocks;
  /// Collect-time immediates for omitted allocas whose every store folds to
  /// the same value. Survives per-block `AllocaImmediates` resets.
  std::map<const llvm::AllocaInst *, std::string> OmittedAllocaImmediates;
  std::optional<FunctionSym> DebugFn;
  /// DebugFn's parameters are the function's arguments in order.
  bool DebugPositional = true;
  TypeRef DebugThisRecord;
  const llvm::Argument *DebugThisArg = nullptr;
  /// The type the debug information gives each parameter.
  std::map<const llvm::Argument *, TypeRef> ParamDebugTypes;
  /// The debug parameter each argument holds, bound by where the convention
  /// passes it (bindDebugParams), and the name each reads as.  Unbound for
  /// an MSVC indirect result, whose arguments keep their positions.
  std::vector<DebugParamBinding> ArgDebugBindings;
  std::vector<std::string> ArgDebugNames;
  bool DebugArgsBound = false;
  /// Binds the debug signature's parameters to \p Fn's arguments.
  void bindDebugArguments(const llvm::Function &Fn);
  /// The name the debug signature gives \p Arg, or empty.
  std::string debugArgumentName(const llvm::Argument &Arg) const;
  /// The debug parameter argument \p ArgNo holds whole, past an MSVC
  /// indirect result at \p SretParamIdx; none when it holds none.
  std::optional<size_t> debugParamIndexOf(unsigned ArgNo,
                                          int SretParamIdx) const;
  /// The function's address in the image, which a relocatable object's
  /// first function has as zero; none for a function of no address.
  std::optional<va_t> FunctionEntry;
  const llvm::AllocaInst *SyntheticFrame = nullptr;
  uint64_t FrameBaseOffset = 0;
  /// PDB stack-pointer offset -> frame-alloca offset of an address-taken use.
  /// A later access at a different frame offset must not reuse that local
  /// (a dword beside a string field is not the string field).
  std::map<int64_t, uint64_t> SpLocalOwner;
  /// Possible frame-alloca offsets for each SP-relative PDB local. CodeView's
  /// S_FRAMEPROC-relative offset can also map the name to a different slot
  /// when saved registers are excluded from S_FRAMEPROC's frame size.
  std::map<std::string, std::set<uint64_t>> SpLocalNameCandidates;
  /// Reused PDB local names at distinct SP offsets have no unique C owner.
  std::set<std::string> SpLocalAmbiguousNames;
  struct NamedFrameSlot {
    uint64_t Off = 0;
    std::string Name;
    TypeRef Type;
    /// Call-site pointee (`ArgList* args`) kept when PDB `Type` is richer
    /// (`TPtr`). Integer stores print `types_` / `values_`; loads stay `.p`.
    TypeRef CallType;
    /// Address used as `&slot` or stored into `values_`. Leftover EAX
    /// siblings sit next to these homes and are not themselves taken.
    bool AddressTaken = false;
    /// In a frame record, the type the member was laid out with. Later
    /// display typing does not move or widen it.
    TypeRef MemberType;
    /// `Type` is a debug symbol's: the object's extent, which a prototype's
    /// pointee (one element of an array, say) must not replace.
    bool DebugType = false;
  };
  mutable std::map<uint64_t, NamedFrameSlot> FrameSlots;
  /// The frame must stay one object (an escape no type bounds, a span that
  /// crosses slots): its named slots print as members of one record laid out
  /// exactly like the frame bytes, spelled `FrameRecordName.slot`.
  bool FrameRecord = false;
  std::string FrameRecordName;
  /// \p Slot's spelling: its name, or its member of the frame record.
  std::string slotText(const NamedFrameSlot &Slot) const;
  /// Whether a member of type \p Ty (`uint64_t` when null) at frame offset
  /// \p Off fits the frame record: inside the frame, aligned for its type,
  /// and clear of every other member.
  bool frameRecordAdmits(uint64_t Off, const TypeRef &Ty) const;
  /// The frame record's declaration, from the members the body printed.
  std::string frameRecordDeclaration(const llvm::AllocaInst &Frame) const;
  /// Whether a named frame slot \p Load reads can print at each use: every
  /// use prints later in its block, and nothing between may write memory.
  /// Otherwise the load reads the slot into its temporary where it is.
  bool slotReadAtUses(const llvm::LoadInst &Load) const;
  mutable std::map<const llvm::LoadInst *, bool> SlotReadAtUses;
  /// Different overlapping memory extents cannot become independent scalar
  /// locals. Keep unnamed views of those bytes in the shared backing frame.
  std::set<uint64_t> OverlappingFrameAccessOffsets;
  mutable std::map<int64_t, std::optional<VariableSym>> FrameDebugCache;
  /// Allocas whose current value is exactly `this` (ptrtoint). Survives
  /// per-block `AllocaLastValues` resets so `this+imm` in a later block
  /// still names the field.
  std::set<const llvm::AllocaInst *> ThisHomes;
  std::map<const llvm::Value *, TypeRef> ValueTypes;
  std::map<const llvm::AllocaInst *, TypeRef> AllocaTypes;
  std::map<const llvm::Value *, std::string> ValueTexts;
  std::map<const llvm::AllocaInst *, std::string> AllocaTexts;
  bool InferredVoid = false;
  int EHTryDepth = 0;
  bool EHWrapIsCxx = false;
  /// Blocks whose terminator is `llvm.seh.try.begin` or `llvm.seh.try.end`.
  std::set<const llvm::BasicBlock *> EHBoundaryBlocks;
  /// Handler/dispatch blocks omitted from the main EH block walk.
  std::set<const llvm::BasicBlock *> EHSkippedMainBlocks;
  /// Current block of that walk; recursive handler/structured prints differ.
  const llvm::BasicBlock *EHMainBlock = nullptr;
  /// Non-fallthrough normal invoke edges in a projected SEH try body.
  std::map<const llvm::BasicBlock *, const llvm::BasicBlock *>
      EHInvokeNormalGotos;
  /// Empty handler blocks whose edge effects still need statements.
  std::set<const llvm::BasicBlock *> EHPrintedPassthroughHandlers;
  /// PHIs in a continuation moved beyond the EH wrap need path-local copies.
  std::set<const llvm::BasicBlock *> EHMovedContinuationBlocks;
  /// Try-end labels whose normal incoming branch was actually printed as
  /// fallthrough. Other printed references are checked after rendering.
  std::set<const llvm::BasicBlock *> EHFallthroughLabelCandidates;
  std::set<const llvm::Value *> OmittedUnknowns;
  std::set<const llvm::Value *> OmittedInlined;
  std::set<const llvm::StoreInst *> OmittedLeftoverNarrow;
  std::map<const llvm::StoreInst *, const llvm::CallBase *> LeftoverSiblingFill;
  /// True after a noreturn call (`throw`, `__fastfail`, libc abort/exit/…).
  /// The next `return` / `unreachable` / debugtrap is the compiler's
  /// fall-through, not source.
  bool AfterCxxThrow = false;
  /// Condition-chain blocks printed inside one if/else. The head stays.
  std::set<const llvm::BasicBlock *> ConditionChainBodies;
  /// One-line assigns already printed in both elses of an inverted skip.
  std::set<const llvm::BasicBlock *> DuplicatedAssignBlocks;
  /// Call results declared while printing this function. A name the body
  /// never uses is removed before the function text is committed.
  std::vector<std::string> CallDeclNames;
  /// Locals whose only use was folded into a later expression.
  std::vector<std::string> FoldedLocalNames;
  /// One-use assign diamond printed as `cond ? a : b` at that load.
  std::map<const llvm::LoadInst *, std::string> SelectUseText;
  /// Join slot whose only live value is one field. The call prints that
  /// field and the assignment is omitted.
  std::map<const llvm::AllocaInst *, std::string> ForwardedFieldArgs;
  /// Cleanup-return targets printed after the wrap. The resume goto is omitted.
  std::set<const llvm::BasicBlock *> OmitCleanupRetTo;
  /// While printing a phi-fed call at an incoming edge, that slot's argument
  /// prints the value stored on the edge.
  const llvm::AllocaInst *PhiTailSlot = nullptr;
  const llvm::Value *PhiTailIncoming = nullptr;
};

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_LLVMC_LLVMCWRITER_H
