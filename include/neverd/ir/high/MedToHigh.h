//===- MedToHigh.h - MedIR to HighIR conversion -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares MedToHighConverter which transforms a MedFunc into a HighFunc
/// by building expression trees, structuring control flow (if/while/switch),
/// inferring types, and performing cleanup passes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_MEDTOHIGH_H
#define NEVERD_IR_HIGH_MEDTOHIGH_H

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/med/MedIR.h"

#include <functional>
#include <map>
#include <set>
#include <tuple>

namespace neverd {

struct BinaryImage;
struct CompareTreeSwitch;
namespace detail {
class HighEntryStackOffsets;
}

Intrinsic intrinsicId(const MedOp &Op);
std::string intrinsicName(const MedOp &Op);
TypeRef inferReturnType(const MedFunc &Med);
std::set<uint64_t> detectPtrParamRegs(const MedFunc &Med);
/// Stack-passed parameter ids (`MedVar::Param`) used as memory addresses.
std::set<int> detectPtrParamIds(const MedFunc &Med);

/// Rewrite expressions by what they compute rather than by how they are
/// written, replacing each with the shortest equivalent the symbolic engine
/// can find.
///
/// This is what reaches a mixture of arithmetic and bitwise operators, where
/// every rewrite either algebra can state is blocked by an operator belonging
/// to the other — the shape obfuscation is built out of, and the one a
/// peephole pass cannot touch.  Only whole-word bitvector arithmetic is
/// carried across; a load, a call, a cast or a comparison becomes one opaque
/// input and comes back untouched.
///
/// The standard HighIR cleanup runs this after copy propagation, dead-code
/// elimination and renaming have assembled the final expression DAG.  A
/// rewrite must strictly improve the engine's rendered-tree cost, which avoids
/// replacing a compact shared form with text that repeats one of its
/// subexpressions.
void simplifyExprSemantics(std::vector<HighStmt> &Stmts);

/// Fold shared branch continuations after dead assignments have been removed.
/// Preserve external entries and require exact fallthrough destinations.
void foldStructuredContinuations(HighFunc &Func, const MedFunc *Med = nullptr);

/// Replace `goto L` with a copy of L's tail when L is a few pure
/// assignments followed by a return, or ending in a call that never returns.
/// The original stays for other paths. Returns true when a goto was replaced.
/// With \p PrintedSize, a temporary read once later in the tail does not
/// count toward the limit, since it prints inside that read; that is meant
/// for the jumps left once structuring is done, where each copy removes one.
bool duplicateSmallReturnTails(std::vector<HighStmt> &Body,
                               bool PrintedSize = false);

/// Move the jump that ends a `__try` body after the try statement when every
/// `__except` body ends in a jump or return of its own, so the protected code
/// is unchanged.  Returns true when a jump moved.
bool hoistTryExitJumps(std::vector<HighStmt> &Body);

/// Late goto reduction: merge conditional jumps to one target, move a block
/// entered by a single forward jump into that `if`, and turn a jump over the
/// fall-through path into `if`/`else`.  Returns true when anything changed.
/// With \p SpliceRegions, a single-use label may also start a multi-block
/// region that is entered only from inside itself; that region moves too.
bool reduceSingleUseGotos(std::vector<HighStmt> &Body,
                          bool SpliceRegions = false);
/// Share one body among switch cases that go to the same place, and drop
/// cases that go where `default` goes.
bool groupSwitchCases(std::vector<HighStmt> &Body);
/// Splice the statements of each non-empty block into the list holding it;
/// a block address a jump enters keeps an empty anchor ahead of them.
/// Returns true when a block went.
bool flattenBlocks(std::vector<HighStmt> &Body);
/// Replace each jump to a label nothing falls into with a copy of its tail
/// when that tail is a few pure assignments followed by a forward jump or by
/// the next label. Returns true when a jump was replaced.
bool duplicateSmallJumpTails(std::vector<HighStmt> &Body);
/// Remove a goto whose target is exactly the next statement in its list,
/// when no other statement starts at that address. Returns true when a goto
/// was removed.
bool dropJumpsToTheNextStatement(std::vector<HighStmt> &Body);
/// A goto at loop or switch level whose target is what runs after the loop
/// or switch, exactly as falling out of it would, becomes `break`. Code that
/// runs only after a loop's one break or a switch's one falling case moves
/// there first. Returns true when anything changed.
/// `switch (..) { ..goto J.. } C...; J:` where more of the switch's jumps go
/// to J than reach C: C moves into the switch at one of its ways out, the
/// others jump to C, and J becomes what follows the switch, so
/// breakToTheLoopFollow turns the jumps to J into breaks.  C runs past the
/// end of its list where nothing else falls into what follows: a block's
/// body, or an if/else arm whose other arm never falls out.  Nothing outside
/// the switch and C may enter C.  Returns true when a switch changed.
bool busiestExitFollowsTheSwitch(std::vector<HighStmt> &Body);
bool breakToTheLoopFollow(std::vector<HighStmt> &Body);
/// `X: S...` where jumps from inside S return to X becomes
/// `while (1) { S...; break; }` with those jumps as `continue`. A jump to X
/// from elsewhere still lands on the first statement of the loop body.
bool loopifyBackwardGotos(std::vector<HighStmt> &Body);
/// A label on the first statement of a `while (1)` or do-while body that is
/// entered only from outside the loop moves onto the loop statement.
bool hoistLoopEntryLabels(std::vector<HighStmt> &Body);
/// `while (1) { S1; X: S2 }` whose top is reached neither by fall-through nor
/// by a jump, entered at X, becomes `while (1) { X: S2; S1 }`.
bool rotateLoopsToTheirEntry(std::vector<HighStmt> &Body);
/// An endless loop that begins with `if (c) goto X;` becomes
/// `while (!c) {..} goto X;`; one that ends with it, `do {..} while (!c);`.
bool hoistLoopExitTests(std::vector<HighStmt> &Body);
/// `while (1) { ..break..; X: .. } T...` where T never falls through and
/// jumps back to X: T moves to the break, its only way in.
bool moveLoopTailsToTheirBreak(std::vector<HighStmt> &Body);
/// `switch (..) { ..break.. default: .. } T...` where one break, or one
/// case running off its end, is the switch's only way out and T never falls
/// through: T moves there, so `case 18: break; } return f();` reads
/// `case 18: return f(); }`.  T is straight-line code that nothing jumps
/// into.
bool moveSwitchTailsToTheirExit(std::vector<HighStmt> &Body);
/// `if (c) { switch (x) {..} } D...` where every case label passes c and the
/// default runs D (a jump to it or a copy of it): a value that fails c
/// matches no case and reaches D through the default anyway, so the switch
/// alone does the same.  This drops the range check a jump table needs.
bool absorbSwitchRangeGuards(std::vector<HighStmt> &Body);
/// `t = e; S...; x = t;` where t has no other use and S neither touches x
/// nor t, nor leaves nor is entered: `x = e; S...;`.  Out of SSA a loop
/// update reads `t = i + 1; ...; i = t;`; it reads `i = i + 1;`.
bool foldCopiesIntoDefinitions(HighFunc &Func);
/// `t = *p; S;` where t has no other use and S, the next statement, reads
/// its operands before any effect of its own: S reads *p in t's place, as
/// `*q = *p;`.  The read keeps its place among the program's effects; a
/// loop condition, which runs again, never takes it.
bool inlineAdjacentLoads(HighFunc &Func);
/// A loop whose body never reaches its end and has no break or continue
/// runs its body once: the body replaces the loop.
bool unwrapLoopsThatNeverRepeat(std::vector<HighStmt> &Body);
/// Run the join-default sink of structureIfElse again on the late tree.
bool sinkJoinDefaultsLate(HighFunc &Func);

/// Once the other rewrites have settled: `if (c) { T; goto L; } S...` ending
/// a list whose fall-through is L, such as a __try body or an __except
/// handler followed by L, becomes `if (c) { T } else { S... }`.
bool elseArmsForFallthroughJumps(HighFunc &Func);

/// Once the other rewrites have settled: `X: S...; if (c) { T...; goto X; }`
/// becomes `while (1) { S...; if (c) { T...; continue; } break; }`, where X
/// starts exactly one statement and S and T hold no loose break or continue.
bool loopsForArmsJumpingBack(std::vector<HighStmt> &Body);

/// The same for jumps nested deeper: `X: S...; T` where T holds `goto X`
/// inside if/else arms, blocks or switch cases becomes `while (1) { S...; T;
/// break; }` with those jumps turned into `continue`.  X starts exactly one
/// statement and the region holds no loose break or continue.
bool loopsForNestedJumpsBack(std::vector<HighStmt> &Body);

/// Jumps inside a loop that leave it for the statement after it become
/// `break`, and jumps to a while loop's test, or to the top of an always-true
/// loop's body, become `continue`; each target starts exactly one statement.
bool loopJumpsAsBreakAndContinue(std::vector<HighStmt> &Body);

/// In a run of plain assignments ending in a return, a temporary assigned in
/// the run can only be read in the run, even when copies of the run assign it
/// elsewhere: one read takes its value directly and an unread one goes.
bool foldTempsInReturnTails(std::vector<HighStmt> &Body);

/// When NEVERD_HIGH_FLOW_ORACLE is set, print one FLOWPROTECT line for each
/// structured SEH try of \p Func whose guarded range holds code outside its
/// __try that may raise an exception: in C that code runs unprotected.  Run
/// right after structuring the regions: later tail copies take the address
/// of the jump they replace, which may lie in a range they do not belong to.
void reportUnprotectedGuardedCode(const HighFunc &Func, const char *Stage);

/// Once the other rewrites have settled: `if (c) goto L;` followed by an if
/// whose then arm opens with L becomes `if (c || b)`, and one whose else arm
/// opens with L becomes `if (!c && b)`.  L must start exactly one statement,
/// and nothing but the first test may jump to the second if.
bool mergeJumpsIntoNextIfArms(HighFunc &Func);
/// Declares a register or temporary local only as wide as its reads take,
/// when every read takes at most its low N bytes and every definition is an
/// extension from at most N bytes, a call or a constant.  The upper bytes no
/// read sees leave the program, and with them a narrowing at every read.
bool narrowLocals(HighFunc &Func);
/// Declares each register or temporary local signed or unsigned by what most
/// of its uses read, so that wrapping arithmetic, logical shifts and
/// unsigned comparisons print without casts.  Value bits do not change.
void chooseIntegerSignedness(HighFunc &Func);
/// A large value one statement reads several times is assigned to a fresh
/// local before it, when evaluating the value earlier cannot fault or have
/// an effect.  The C writer prints each read of a shared node in full.
bool nameRepeatedValues(HighFunc &Func);
/// `if (a) {..} else { ..; jump; X: S.. }` followed by `if (c) goto X;`
/// becomes `while (c) { S.. }` in place of the test.
bool loopifyTrailingArmBodies(std::vector<HighStmt> &Body);
/// `if (c) { A; X: B } else { C; goto X; }` (or the mirror image) becomes
/// `if (c) { A } else { C }` followed by B.
bool hoistSharedArmTails(std::vector<HighStmt> &Body);
/// The order structureControlFlow emits \p Med's blocks in: reverse
/// postorder from the entry, visiting each block's successors from the
/// highest address down.  Code laid out in source order keeps that order,
/// while a block that a later block reaches by a forward edge, such as cold
/// code moved away from its branch or a tail shared with later code, comes
/// after the blocks that reach it; blocks the entry never reaches follow in
/// address order.  \p Dispatched blocks transfer only explicitly (a compare
/// tree's switch).  Address order when a block's fall-through edge is unknown
/// or the function has exception regions, except that with
/// \p SEHReversePostorder a frame with only Windows SEH scopes keeps reverse
/// postorder when it leaves every guarded range's blocks next to each other,
/// since a __try wraps one run of statements.
std::vector<int> highBlockLayout(const MedFunc &Med,
                                 const std::set<int> &Dispatched,
                                 bool SEHReversePostorder = true);

/// Emit a label-per-block goto/return skeleton.  Used when structuring would
/// exceed SSA limits, or when conversion fails and identity alone would leave
/// an empty HighFunc that HighC can only trap.
void fillUnstructuredGotoSkeleton(HighFunc &Func, const MedFunc &Med);

class MedToHighConverter {
public:
  /// Convert \p Med.  A frame with only Windows SEH scopes is laid out in
  /// reverse postorder; when that leaves a guarded range unstructured, it is
  /// converted again in address order, which is kept only if it leaves fewer
  /// ranges unstructured.
  HighFunc convert(const MedFunc &Med, Arch TheArch = Arch::Unknown);

  void setBinaryImage(const BinaryImage *Img) { Image = Img; }

  /// Observe expression creation without adding metadata to either IR. The
  /// observer must keep weak references: later transformations can replace an
  /// expression, and an expired reference must not map a recycled address.
  void setExpressionObserver(
      std::function<void(const MedOp &, const ExprPtr &)> Observer) {
    ExpressionObserver = std::move(Observer);
  }
  void setExpressionCloneObserver(
      std::function<void(const ExprPtr &, const ExprPtr &)> Observer) {
    ExpressionCloneObserver = std::move(Observer);
  }
  void setStatementObserver(
      std::function<void(const MedOp &, const HighStmt &)> Observer) {
    StatementObserver = std::move(Observer);
  }

  void setFuncNames(const std::map<va_t, std::string> *Names) {
    FuncNames = Names;
  }
  /// Resolve call targets once before parallel conversion. Workers read the
  /// resulting snapshot while lowering functions from the same image.
  void resolveCalleeNames(const std::set<va_t> &Targets,
                          std::map<va_t, std::string> &Names) const;
  void setResolvedCalleeNames(const std::map<va_t, std::string> *Names) {
    ResolvedCalleeNames = Names;
  }
  void setJumpTables(const std::vector<JumpTable> &JTs) { JumpTables = JTs; }

  struct CallIndTarget {
    std::string Name = "indirect";
    va_t Addr = 0;
    bool IsIndirect = true;
    int IndirectParam = -1;
    /// The constant INDIR_CALL input is the slot holding the target, so the
    /// call goes through a load of it.
    bool ThroughSlot = false;
  };

private:
  HighFunc convertOnce(const MedFunc &Med, Arch TheArch);
  /// Lay out an SEH frame in address order for this conversion.
  bool SEHAddressOrder = false;
  void buildExpressions(const MedFunc &Med);
  void structureControlFlow(HighFunc &Func, const MedFunc &Med);
  void structureExceptionRegions(HighFunc &Func, const MedFunc &Med);
  /// Put the copies a handler block's PHIs take on the dispatcher's entry in
  /// the `__except` arm that reaches it (see PhiNode::ExceptionalEntry).
  void attachSEHHandlerEntryCopies(HighFunc &Func, const MedFunc &Med);
  void inferTypes(HighFunc &Func);
  void simplifyControlFlow(HighFunc &Func, const MedFunc &Med);
  void inlineGotoReturns(HighFunc &Func, const MedFunc &Med);
  void eliminateDeadStmts(HighFunc &Func);
  void stripPrologueEpilogue(HighFunc &Func);
  void ensureTrailingReturn(HighFunc &Func, const MedFunc &Med);

  ExprPtr medOpToExpr(const MedOp &Op);
  ExprPtr medOpToExprImpl(const MedOp &Op);
  ExprPtr medvarToExpr(const MedVar &V);
  /// i386 ELF PIC (I386PicAddresses.cpp): the values the CFG proved to be the
  /// GOT base of an unlinked object, at address zero, and the sums of the
  /// base and terms that are no constants.
  void collectI386GotBase(const MedFunc &Med);
  /// The input of \p Op that is the GOT-relative displacement it adds to
  /// the base, if any.
  std::optional<unsigned> i386GotDisplacement(const MedOp &Op) const;
  /// \p Op, which adds its input \p Displacement to the GOT base: the
  /// address of the data that displacement names.
  ExprPtr i386GotRelativeAddress(const MedOp &Op, unsigned Displacement);
  VarKeySet I386GotBase;
  VarKeySet I386GotRooted;
  /// Recover a target-width memory address from the wider LowIR VA carrier
  /// only when an explicit zero extension proves that no high bits are lost.
  ExprPtr memoryAddressExpr(const MedVar &V, bool InlineDefinition = true);
  void indexMedDefinitions();
  const MedOp *uniqueMedDefinition(const MedVar &V);
  ExprPtr sourceBitSlice(const ExprPtr &Value, uint64_t ByteOffset,
                         uint16_t Bytes, unsigned Depth = 0);
  ExprPtr sourceFloatValue(const MedVar &Value, uint16_t Bytes);
  ExprPtr sourceScalarValue(const MedVar &Value, const TypeRef &Type);
  ExprPtr inlineableDefinition(VarKey Key) const;
  ExprPtr forceInlineExpr(const ExprPtr &E);
  /// Like \ref forceInlineExpr, but a unique Load / add / copy may inline
  /// even when the dest is a memory-read output.  Used for `INDIR_CALL`
  /// callees so HighC prints `(*(*p))(...)` instead of an undeclared temp.
  /// A read inlines only when nothing between it and the call at
  /// \ref CallTargetUse may write memory: the call would otherwise read the
  /// slot again after a store to it.
  ExprPtr forceInlineCallTarget(const ExprPtr &E);
  struct CallTargetUseSite {
    const MedBlock *Block = nullptr;
    size_t CallIdx = 0;
  };
  CallTargetUseSite CallTargetUse;
  bool memoryReadReachesCallTarget(const MedVar &Value) const;

  int regToArgIdx(uint64_t RegOff) const;
  /// Map a MedIR parameter or its entry register to the ABI slot index in
  /// `CurMed->Params`.  MedIR often stores the SSA id in `MedVar::Id`, which
  /// is not the rcx/rdx/r8/r9 (or stack) slot HighC uses.
  int abiParamIndex(const MedVar &V) const;

  /// Prefer a non-synthetic FuncNames entry, then an import or image symbol.
  std::string calleeDisplayName(va_t Target) const;

  /// How many integer parameters the C library import that call \p CallIdx
  /// of \p Ops reaches always reads: a variadic one's fixed parameters,
  /// else all of them; 0 when unknown or when one is floating-point.  The
  /// call names the import by its stub or slot, or through a register
  /// loaded from \p ResolvedSlot.
  unsigned importFixedArgCount(size_t CallIdx, const std::vector<MedOp> &Ops,
                               va_t ResolvedSlot) const;

  /// \p ResolvedSlot is the import slot an indirect call was resolved to
  /// through the register it calls, else 0.
  std::vector<ExprPtr> collectCallArgs(const MedBlock &CurBlock, size_t CallIdx,
                                       va_t ResolvedSlot = 0);

  /// Exact arguments of a retained PE32 leaf/throw ABI, from the current SSA
  /// register values. Nullopt leaves unbound callees to ordinary recovery.
  std::optional<std::vector<ExprPtr>>
  collectRegistrationCallArgs(const MedBlock &Block, size_t CallIdx);

  /// Resolve the SSA variable of register \p RegOff reaching the ENTRY of
  /// \p B (its live-in value: a PHI in B, else the single reaching definition
  /// walked back through predecessors).  Used to recover a register call
  /// argument that is live-in to the call block rather than written before the
  /// call.  Returns false when unresolved.  Requires CurMed.
  /// The parameter a register still holds at the function's only call when
  /// the function never writes that register.
  std::optional<MedVar> untouchedParamRegister(uint64_t RegOff) const;
  bool reachingRegAtBlockEntry(const MedBlock &B, uint64_t RegOff,
                               MedVar &Out) const;
  /// True when an immediate predecessor wrote \p LiveIn into \p RegOff
  /// as call setup. Earlier leftover values (Find's nKey still in r9)
  /// are not arguments of a later call. Also covers `lea r8` in the
  /// shared fork of `mov edx; jmp call` join arms.
  bool isCallArgSetupDef(const MedBlock &CallBlk, const MedVar &LiveIn,
                         uint64_t RegOff) const;

  void lowerStore(HighFunc &Func, const MedOp &CurOp);
  void lowerCall(HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp);
  void lowerIntrinsic(HighFunc &Func, const MedBlock &CurBlock,
                      const MedOp &CurOp, size_t OpIdx,
                      std::set<size_t> &IntrinsicSkip);
  void lowerCallInd(HighFunc &Func, const MedBlock &CurBlock,
                    const MedOp &CurOp);
  void lowerCBranch(HighFunc &Func, const MedOp &CurOp);
  void lowerBranch(HighFunc &Func, const MedOp &CurOp);
  void lowerBranchInd(HighFunc &Func, const MedBlock &CurBlock,
                      const MedOp &CurOp, const MedFunc &Med);
  bool lowerSwitchFromJumpTable(HighFunc &Func, const MedBlock &CurBlock,
                                const MedOp &CurOp, const MedFunc &Med,
                                const JumpTable &JT);
  bool lowerX86RegistrationCatchReturn(HighFunc &Func, const MedBlock &CurBlock,
                                       const MedOp &CurOp, const MedFunc &Med);
  void lowerReturn(HighFunc &Func, const MedBlock &CurBlock, const MedOp &CurOp,
                   const MedFunc &Med);
  void lowerGenericAssign(HighFunc &Func, const MedOp &CurOp,
                          const VarKeySet &PhiArgVars);
  using PhiCopyMap =
      std::map<std::pair<int, int>, std::vector<std::pair<MedVar, MedVar>>>;
  void insertPhiCopies(HighFunc &Func, const MedBlock &CurBlock, int BlkIdx,
                       size_t BlkBodyStart, const PhiCopyMap &PhiCopies);
  /// The PHI writes of the CFG edge From -> To, placed at address \p At.
  std::vector<HighStmt> phiCopiesForEdge(int From, int To, va_t At,
                                         const PhiCopyMap &PhiCopies);
  /// Replace a compare tree's branches with one switch at its root: the
  /// interior blocks' operations, then `case V: <PHI writes> goto T;`.
  void lowerCompareTreeSwitch(HighFunc &Func, const MedFunc &Med,
                              const CompareTreeSwitch &Tree,
                              const PhiCopyMap &PhiCopies,
                              const VarKeySet &PhiArgVars);
  /// Move each compare-tree case's own blocks into its case body, once loops
  /// are recovered: the statements whose addresses lie in the blocks the
  /// case target dominates, with jumps for fall-through the move breaks.
  void pullCompareTreeCases(HighFunc &Func, const MedFunc &Med);
  /// Per compare-tree switch address: each case target's entry and the
  /// blocks that target dominates (set by structureControlFlow).
  std::map<va_t, std::vector<std::pair<va_t, std::vector<int>>>> CaseRegions;

  CallIndTarget resolveCallIndTarget(const MedBlock &CurBlock,
                                     const MedOp &CurOp,
                                     const ExprPtr &TargetExpr);

  VarKeyMap<int> UseCount;
  TypeRef sourceCallResultType(const MedOp &Op) const;
  VarKeyMap<ExprPtr> DefExpr;
  VarKeyMap<TypeRef> SourceRecordValues;
  std::vector<SourceABIParameter> SourceParameters;
  VarKeySet CallOutputs;
  VarKeySet PhiOutputVars;
  /// Per-function indexes for the Win64 callee-save parameter mapping in
  /// medvarToExpr: register COPYs whose source is an entry parameter (in
  /// block/op order, with that parameter's index), and every (Id, SSAVer)
  /// with a computed def (anything but a COPY of a register or parameter).
  /// Built lazily for \c ParamCopyIndexFunc.
  const MedFunc *ParamCopyIndexFunc = nullptr;
  std::vector<std::pair<const MedOp *, int>> ParamSourceCopies;
  std::set<std::pair<int, int>> ComputedVersions;
  /// A native read is evaluated at its statement, then used as an SSA value.
  /// Re-expanding it at a use could cross an aliasing write or repeat the read.
  VarKeySet MemoryReadOutputs;
  /// The function currently being converted; set at the top of convert() so
  /// collectCallArgs can resolve a register argument that is live-in to the
  /// call block (loop-carried via a header PHI) rather than written before the
  /// call.
  /// Late goto reduction on the finished statement tree (tail duplication,
  /// splices, if/else joins, loop recovery), bounded by statement count.
  void reduceLateGotos(HighFunc &Func);
  const MedFunc *CurMed = nullptr;
  /// Entry-relative stack slots CurMed loads anywhere or whose address
  /// escapes; such a slot is a local, not an outgoing argument (cached per
  /// function).
  std::set<int64_t> LoadedEntrySlots;
  const MedFunc *LoadedEntrySlotsFor = nullptr;
  /// Definition index and complete frame-coordinate proofs for one immutable
  /// conversion. A second conversion of the same MedFunc starts fresh.
  std::shared_ptr<detail::HighEntryStackOffsets> EntryStackOffsets;
  const MedFunc *EntryOffsetDefsFor = nullptr;
  const BinaryImage *Image = nullptr;
  std::function<void(const MedOp &, const ExprPtr &)> ExpressionObserver;
  std::function<void(const ExprPtr &, const ExprPtr &)> ExpressionCloneObserver;
  std::function<void(const MedOp &, const HighStmt &)> StatementObserver;
  Arch TargetArch = Arch::Unknown;
  const std::map<va_t, std::string> *FuncNames = nullptr;
  const std::map<va_t, std::string> *ResolvedCalleeNames = nullptr;
  std::vector<JumpTable> JumpTables;
  int NextHighTempId = 0;
  std::optional<MedVar> SwiftErrorEntryInput;
  int ExprRecurseDepth = 0;
  static constexpr int kMaxExprDepth = limits::kMaxExprDepth;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_MEDTOHIGH_H
