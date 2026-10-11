//===- HighIR.h - High-level IR definitions -----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the high-level IR: tree-structured expressions (HighExpr),
/// structured statements (HighStmt) with if/while/switch constructs,
/// and the top-level HighFunc container.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHIR_H
#define NEVERD_IR_HIGH_HIGHIR_H

#include "neverd/Common.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace neverd {

using VarKey = std::pair<int, int>;

struct VarKeyHash {
  size_t operator()(const VarKey &K) const {
    return std::hash<int>()(K.first) ^ (std::hash<int>()(K.second) << 16);
  }
};

inline VarKey varKey(const MedVar &V) {
  // Ids are unique inside MedIR SSA, but MedToHigh deliberately remaps an
  // entry register to a zero-based Param id for source-level names (arg0,
  // arg1, ...).  Put only those synthetic parameter versions in a disjoint
  // (negative) namespace so Param id 1 cannot alias Reg id 1.  Other kinds
  // retain the historic {Id, SSAVer} identity: some stack/register forwarding
  // deliberately relies on that shared SSA numbering.
  if (V.Kind == MedVar::Param)
    return {V.Id, -V.SSAVer - 1};
  return {V.Id, V.SSAVer};
}

using VarKeySet = std::unordered_set<VarKey, VarKeyHash>;
template <typename V>
using VarKeyMap = std::unordered_map<VarKey, V, VarKeyHash>;

//===----------------------------------------------------------------------===//
// Expressions (tree-structured)
//===----------------------------------------------------------------------===//

enum class ExprKind : uint8_t {
  Var,
  Const,
  Undef,
  BinOp,
  UnaryOp,
  Load,
  Store,
  Call,
  Addr,
  Cast,
  Field,
  Phi,
  /// Reinterpret the bits of Operands[0] as Type. Both scalar types have the
  /// same byte size; this never performs an integer/floating numeric
  /// conversion.
  BitCast,
  /// One naturally laid-out source record, with one operand per direct field.
  Record,
  /// A register supplied by the runtime at a nonordinary entry. Capture it
  /// at EntryVA; it is not the register's value at the expression's use.
  EntryRegister
};

struct HighExpr {
  ExprKind Kind = ExprKind::Const;
  NdOp Op = NdOp::NOP;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  TypeRef Type;

  /// For Var
  MedVar Var = {};

  /// For EntryRegister: the owning function and runtime entry. Var names the
  /// physical input, not a local variable or an ordinary function parameter.
  va_t EntryFunctionVA = InvalidVA;
  va_t EntryVA = InvalidVA;

  /// For Const
  uint64_t ConstVal = 0;
  /// Preserve the exact MedIR occurrence: matching bits do not make a numeric
  /// immediate an image address, or allow an address to become a scalar.
  ConstantAddressProvenance ConstProvenance =
      ConstantAddressProvenance::Unknown;
  uint64_t AddressOwnerVA = InvalidVA;

  /// For BinOp / UnaryOp
  std::vector<std::shared_ptr<HighExpr>> Operands;

  /// For Call
  std::string CallTarget;
  va_t CallAddr = 0;
  bool IsIndirectCall = false;
  int IndirectParamIdx = -1;
  /// Unresolved `INDIR_CALL` callee.  Not a printed argument.
  std::shared_ptr<HighExpr> IndirectTarget;
  std::shared_ptr<const SourceCallTypeHint> SourceCallHint;
  Intrinsic IntrinsicId = Intrinsic::None;
  std::vector<MedVar> IntrinsicOutputs;
  /// The MedIR call never returns: its callee is a routine the name list
  /// knows or an internal one proved not to return.
  bool DoesNotReturn = false;

  /// Operands plus \ref IndirectTarget.
  template <typename F> void forEachChildExpr(F &&Fn) const {
    for (const auto &Op : Operands)
      if (Op)
        Fn(Op);
    if (IndirectTarget)
      Fn(IndirectTarget);
  }

  /// For Cast
  TypeRef CastTo;

  std::string str() const;
  bool structuralEq(const HighExpr &Other) const;
  bool hasOrderedMemoryAccess() const;

  static std::shared_ptr<HighExpr> makeVar(MedVar V, TypeRef Ty = nullptr);
  static std::shared_ptr<HighExpr> makeConst(
      uint64_t Val, uint16_t Size,
      ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown,
      uint64_t AddressOwner = InvalidVA);
  static std::shared_ptr<HighExpr> makeUndef(uint16_t Size);
  static std::shared_ptr<HighExpr> makeBitCast(std::shared_ptr<HighExpr> Value,
                                               TypeRef Type);
  static std::shared_ptr<HighExpr>
  makeRecord(TypeRef Type, std::vector<std::shared_ptr<HighExpr>> Leaves);
  /// Select one complete leaf by layout offset. A partial field is unknown.
  static std::shared_ptr<HighExpr>
  makeRecordField(std::shared_ptr<HighExpr> Record, uint16_t Offset,
                  uint16_t Bytes);
  static std::shared_ptr<HighExpr>
  makeBinop(NdOp Op, std::shared_ptr<HighExpr> L, std::shared_ptr<HighExpr> R);
  static std::shared_ptr<HighExpr> makeUnary(NdOp Op,
                                             std::shared_ptr<HighExpr> Operand);
  static std::shared_ptr<HighExpr> makeLoad(
      std::shared_ptr<HighExpr> Addr, TypeRef Ty,
      NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None,
      NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default);
  static std::shared_ptr<HighExpr>
  makeCall(const std::string &Target, va_t Addr,
           std::vector<std::shared_ptr<HighExpr>> Args);
};

using ExprPtr = std::shared_ptr<HighExpr>;
/// Expressions a walk over a shared expression graph has met, for membership
/// only: its order is unspecified.  Walks run per statement and pass, so the
/// set keeps small graphs inline and allocates nothing per member.
using HighExprSet = llvm::SmallPtrSet<const HighExpr *, 16>;

/// All reachable expressions containing ordered or non-default-address-space
/// memory, including through an indirect call target. Analyze shared nodes
/// once. The borrowed identities remain valid only while this graph is kept
/// alive and no transformation introduces or relocates a memory access.
HighExprSet findOrderedMemoryAncestors(const std::vector<ExprPtr> &Roots);

/// Termination promised by the bound source routine, independently of a
/// native CFG flag or call spelling. Source admission revalidates the binding.
bool isNonReturningSourceCall(const ExprPtr &Expression);
/// A bound noreturn call or an exact unconditional source trap. Resumable
/// debugger traps and unbound call spellings do not terminate source flow.
bool isTerminatingHighCall(const ExprPtr &Expression);

//===----------------------------------------------------------------------===//
// Statements (structured control flow)
//===----------------------------------------------------------------------===//

enum class StmtKind : uint8_t {
  Assign,
  ExprStmt,
  If,
  IfElse,
  While,
  DoWhile,
  For,
  Switch,
  Return,
  Goto,
  Block,
  Store,
  Call,
  Nop,
  Break,
  Continue,
  /// A reducible MSVC table-SEH protected region.  Body is the protected
  /// body; EHClauses/EHClauseBodies describe the except/finally arms.
  SEHTry,
  /// A reducible MSVC C++ state-map region.  The C backend renders this as
  /// faithful pseudocode because it intentionally remains a C emitter.
  CxxTry,
  /// A region an Itanium LSDA call-site table proved to be guarded.  Kept
  /// distinct from \ref CxxTry because the two models disagree about what a
  /// clause names: an MSVC catch owns an out-of-line funclet, while an Itanium
  /// clause names a landing pad shared with every other clause of the region.
  ItaniumTry
};

enum class HighEHClauseKind : uint8_t {
  SEHExcept,
  SEHFinally,
  CxxCatch,
  CxxCleanup,
  /// A positive Itanium action filter: the region stops an exception whose
  /// `std::type_info` matches the named type-table entry.
  ItaniumCatch,
  /// A negative Itanium action filter: the region names an exception
  /// specification, which the personality resolves by calling the unexpected
  /// handler rather than by entering a handler body.  An empty
  /// \ref HighEHClause::SpecTypeNames is `throw()`/`noexcept`.
  ItaniumSpec,
};

/// Language-neutral payload attached to a structured HighIR exception arm.
/// Native addresses and frame offsets are retained even when the handler body
/// belongs to an out-of-line funclet and therefore cannot be embedded safely.
struct HighEHClause {
  HighEHClauseKind Kind = HighEHClauseKind::SEHExcept;
  ExceptionParseStatus ParseStatus = ExceptionParseStatus::Complete;
  va_t FilterOrActionVA = 0;
  va_t HandlerVA = 0;
  va_t TypeDescriptorVA = 0;
  uint32_t Adjectives = 0;
  int32_t CatchObjectOffset = 0;
  int32_t ParentFrameOffset = 0;
  int32_t State = -1;
  CxxUnwindAction::ActionKind UnwindActionKind =
      CxxUnwindAction::ActionKind::None;
  int32_t UnwindObjectOffset = 0;
  std::vector<va_t> ContinuationVAs;
  /// x86 registration EH: the value the filter thunk at \ref FilterOrActionVA
  /// returns, an expression in the function's frame, when the thunk computes
  /// nothing else.  The thunk runs inside the exception dispatch rather than
  /// at the try, so no HighIR pass reads or rewrites this as code of the body.
  ExprPtr FilterValue;

  /// Itanium: position of this clause's action in the call-site action chain,
  /// which is the order the personality tests the clauses in and therefore the
  /// order the source wrote them in.
  uint32_t ChainDepth = 0;
  /// Itanium: the action record's filter, exactly as the table spells it.  A
  /// positive value selects a 1-based type-table entry and a negative one a
  /// 1-based exception-specification list, so the sign is what tells the two
  /// clause kinds apart in the native record.
  int64_t TypeFilter = 0;
  /// Mangled RTTI symbol or `std::type_info::__type_name` for the caught type,
  /// when the type table proved one.  Empty for a catch-all, and for a slot
  /// whose `std::type_info` could not be named.
  std::string TypeName;
  /// Types an exception specification permits, in the order it lists them.
  std::vector<std::string> SpecTypeNames;
  /// Itanium: every landing pad through which this clause is reached.  One
  /// try block legitimately has several — a call made after another local
  /// object was constructed unwinds through a pad that destroys one more
  /// thing — so a single address would misreport the region.  \ref HandlerVA
  /// is the first of these, for consumers that only need one.
  std::vector<va_t> LandingPadVAs;
};

struct SwitchCase {
  uint64_t Value = 0;
  std::vector<struct HighStmt> Body;
  /// An empty case that shares the next case's body (`case A: case B:`)
  /// instead of ending with a break.
  bool FallsThrough = false;
};

struct HighStmt {
  StmtKind Kind = StmtKind::Nop;
  va_t Addr = 0;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;

  /// For Assign
  ExprPtr Dst;
  ExprPtr Val;

  /// For If / While / DoWhile
  ExprPtr Cond;
  std::vector<HighStmt> Body;
  std::vector<HighStmt> ElseBody;

  /// For Return
  ExprPtr RetVal;
  /// Exact catch RETURN evidence bound in final MedIR. It becomes a goto only
  /// when this funclet body is attached to the validated parent continuation.
  std::vector<va_t> CxxContinuationReturnTargets;

  /// For Store
  ExprPtr StoreAddr;
  ExprPtr StoreVal;

  /// For Call
  ExprPtr CallExpr;

  /// For Goto
  va_t GotoTarget = 0;

  /// For While: original back-edge target address.
  va_t LoopHeaderAddr = 0;

  /// For Switch
  ExprPtr SwitchExpr;
  std::vector<SwitchCase> Cases;
  std::vector<HighStmt> DefaultBody;

  /// For SEHTry / CxxTry.  The clause and body vectors have identical sizes.
  /// An empty clause body denotes a validated out-of-line native funclet; its
  /// exact address remains in the corresponding descriptor.
  ExceptionAddressRange EHRange;
  std::vector<HighEHClause> EHClauses;
  std::vector<std::vector<HighStmt>> EHClauseBodies;
  bool EHIsReducible = false;

  bool IsPhiCopy = false;
  /// An assignment naming a value so that the statements reading it stay
  /// short (nameRepeatedValues).  Folding it back into a reader would print
  /// the value there in full again.
  bool KeepsName = false;

  std::string str(int Indent = 0) const;
};

//===----------------------------------------------------------------------===//
// Statement visitors
//===----------------------------------------------------------------------===//

template <typename F> void forEachExpr(HighStmt &S, F &&Fn) {
  if (S.Dst)
    Fn(S.Dst);
  if (S.Val)
    Fn(S.Val);
  if (S.Cond)
    Fn(S.Cond);
  if (S.RetVal)
    Fn(S.RetVal);
  if (S.StoreAddr)
    Fn(S.StoreAddr);
  if (S.StoreVal)
    Fn(S.StoreVal);
  if (S.CallExpr)
    Fn(S.CallExpr);
  if (S.SwitchExpr)
    Fn(S.SwitchExpr);
}

template <typename F> void forEachExpr(const HighStmt &S, F &&Fn) {
  if (S.Dst)
    Fn(S.Dst);
  if (S.Val)
    Fn(S.Val);
  if (S.Cond)
    Fn(S.Cond);
  if (S.RetVal)
    Fn(S.RetVal);
  if (S.StoreAddr)
    Fn(S.StoreAddr);
  if (S.StoreVal)
    Fn(S.StoreVal);
  if (S.CallExpr)
    Fn(S.CallExpr);
  if (S.SwitchExpr)
    Fn(S.SwitchExpr);
}

template <typename F> void forEachRhsExpr(HighStmt &S, F &&Fn) {
  if (S.Val)
    Fn(S.Val);
  if (S.Cond)
    Fn(S.Cond);
  if (S.RetVal)
    Fn(S.RetVal);
  if (S.StoreAddr)
    Fn(S.StoreAddr);
  if (S.StoreVal)
    Fn(S.StoreVal);
  if (S.CallExpr)
    Fn(S.CallExpr);
  if (S.SwitchExpr)
    Fn(S.SwitchExpr);
}

template <typename F> void forEachRhsExpr(const HighStmt &S, F &&Fn) {
  if (S.Val)
    Fn(S.Val);
  if (S.Cond)
    Fn(S.Cond);
  if (S.RetVal)
    Fn(S.RetVal);
  if (S.StoreAddr)
    Fn(S.StoreAddr);
  if (S.StoreVal)
    Fn(S.StoreVal);
  if (S.CallExpr)
    Fn(S.CallExpr);
  if (S.SwitchExpr)
    Fn(S.SwitchExpr);
}

template <typename F> void walkStmts(std::vector<HighStmt> &Stmts, F &&Fn) {
  for (auto &S : Stmts) {
    Fn(S);
    walkStmts(S.Body, Fn);
    walkStmts(S.ElseBody, Fn);
    for (auto &C : S.Cases)
      walkStmts(C.Body, Fn);
    walkStmts(S.DefaultBody, Fn);
    for (auto &ClauseBody : S.EHClauseBodies)
      walkStmts(ClauseBody, Fn);
  }
}

template <typename F>
void walkStmts(const std::vector<HighStmt> &Stmts, F &&Fn) {
  for (const auto &S : Stmts) {
    Fn(S);
    walkStmts(S.Body, Fn);
    walkStmts(S.ElseBody, Fn);
    for (const auto &C : S.Cases)
      walkStmts(C.Body, Fn);
    walkStmts(S.DefaultBody, Fn);
    for (const auto &ClauseBody : S.EHClauseBodies)
      walkStmts(ClauseBody, Fn);
  }
}

/// Erase the statements of \p Stmts that \p ShouldErase selects.  One whose
/// address a goto in \p Targets still names leaves an empty block behind, so
/// the label keeps its place.
template <typename Pred>
void eraseKeepingBranchEntries(std::vector<HighStmt> &Stmts,
                               const std::set<va_t> &Targets,
                               Pred &&ShouldErase) {
  std::vector<HighStmt> Result;
  Result.reserve(Stmts.size());
  for (auto &S : Stmts) {
    if (!ShouldErase(S)) {
      Result.push_back(std::move(S));
      continue;
    }
    if (S.Addr != 0 && S.Addr != InvalidVA && Targets.count(S.Addr)) {
      HighStmt Anchor;
      Anchor.Kind = StmtKind::Block;
      Anchor.Addr = S.Addr;
      Result.push_back(std::move(Anchor));
    }
  }
  Stmts = std::move(Result);
}

/// The addresses goto statements in \p Stmts jump to.
inline std::set<va_t> gotoTargets(const std::vector<HighStmt> &Stmts) {
  std::set<va_t> Targets;
  walkStmts(Stmts, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Goto)
      Targets.insert(S.GotoTarget);
    for (const HighEHClause &Clause : S.EHClauses)
      Targets.insert(Clause.ContinuationVAs.begin(),
                     Clause.ContinuationVAs.end());
  });
  return Targets;
}

//===----------------------------------------------------------------------===//
// High-level function
//===----------------------------------------------------------------------===//

/// True when \p Stmts contains a break that would leave a loop wrapped around
/// it. Loops and switches own the breaks inside them.
inline bool hasLooseBreak(const std::vector<HighStmt> &Stmts) {
  for (const HighStmt &S : Stmts) {
    switch (S.Kind) {
    case StmtKind::Break:
      return true;
    case StmtKind::While:
    case StmtKind::DoWhile:
    case StmtKind::For:
    case StmtKind::Switch:
      break;
    default:
      if (hasLooseBreak(S.Body) || hasLooseBreak(S.ElseBody))
        return true;
      for (const auto &ClauseBody : S.EHClauseBodies)
        if (hasLooseBreak(ClauseBody))
          return true;
      break;
    }
  }
  return false;
}

/// A loop without a condition or a break of its own: it is left only by a
/// jump or a return, so it never falls through to the next statement.
inline bool isEndlessLoop(const HighStmt &S) {
  if (S.Kind != StmtKind::While && S.Kind != StmtKind::DoWhile)
    return false;
  const bool Forever =
      !S.Cond || (S.Cond->Kind == ExprKind::Const && S.Cond->ConstVal != 0);
  return Forever && !hasLooseBreak(S.Body);
}

/// Prove that a switch returns on every selector value. A missing default,
/// a switch break, or a goto can still reach the following statements.
inline bool switchAlwaysReturns(const HighStmt &Stmt) {
  if (Stmt.Kind != StmtKind::Switch || Stmt.Cases.empty() ||
      Stmt.DefaultBody.empty())
    return false;
  auto Returns = [](const std::vector<HighStmt> &Body) {
    if (Body.empty() || Body.back().Kind != StmtKind::Return)
      return false;
    bool HasTransfer = false;
    walkStmts(Body, [&](const HighStmt &S) {
      HasTransfer |= S.Kind == StmtKind::Goto || S.Kind == StmtKind::Break ||
                     S.Kind == StmtKind::Continue;
    });
    return !HasTransfer;
  };
  if (!Returns(Stmt.DefaultBody))
    return false;
  for (const auto &Case : Stmt.Cases)
    if (!Case.FallsThrough && !Returns(Case.Body))
      return false;
  return true;
}

struct HighParam {
  std::string Name;
  TypeRef Type;
  /// Where the caller passes it: the register, or kNoParamReg for a stack
  /// argument, whose MedIR parameter index \ref MedIndex names its slot.
  /// Lowering sets both; a hand-built function may leave them unknown.
  uint64_t RegOff = kNoParamReg;
  int MedIndex = -1;
};

struct HighLocal {
  std::string Name;
  TypeRef Type;
  int64_t StackOff = 0;
};

struct HighFunc {
  va_t Entry = 0;
  uint64_t OriginalSize = 0;
  /// Bytes reserved below and above the synthetic entry stack pointer.
  int64_t FrameSize = 0;
  int64_t FrameHeadroom = 0;
  std::string Name;
  std::string DebugName;
  std::string SourceFile;
  uint32_t SourceLine = 0;
  bool DoesNotReturn = false;
  /// MedFunc::ReturnsNoValue: C shows the function as void.
  bool ReturnsNoValue = false;
  /// How execution reaches Entry (MedFunc::EntryKind).
  StackEntryKind EntryKind = StackEntryKind::Call;
  TypeRef ReturnType;
  std::optional<SourceFunctionTypeHint> SourceTypeHint;
  /// Source-only projection of Swift's in/out error register. The entry value
  /// is captured once; every normal return writes its current value to the
  /// logical error slot. Consumers must revalidate the current body and ABI.
  struct SwiftErrorEntryProjection {
    SourceFunctionTypeHint Signature;
    MedVar Input;
  };
  std::optional<SwiftErrorEntryProjection> SwiftErrorEntry;
  SourceRegisterCopies RegisterCopyProjections;
  SourceClassGetterCalls ClassGetterCallFacts;
  std::vector<HighParam> Params;
  std::vector<HighLocal> Locals;
  std::vector<HighStmt> Body;
  std::optional<ExceptionFunction> ExceptionMetadata;
  std::set<va_t> CxxContinuationTargets;
  unsigned StructuredExceptionRegions = 0;
  unsigned UnstructuredExceptionRegions = 0;
  /// The case values of one recovered switch, in the coordinate its `switch`
  /// statement prints. They are recorded when the switch is built, because
  /// later structuring merges and folds case bodies and Body no longer shows
  /// which case selects each table position.
  struct SwitchLabels {
    /// The selector value that reaches each table position, as the
    /// selector's two's complement bits.
    std::vector<uint64_t> Values;
    /// The position printed as `default` (the bounds guard's target), or -1.
    int DefaultPosition = -1;
    /// The selector's width in bits.
    unsigned SelectorBits = 0;
  };
  /// Switch labels by the address of the jump that dispatches through the
  /// table.
  std::map<va_t, SwitchLabels> SwitchLabelsByJump;
};

/// After EH wrapping, invert `if (c) goto L; work; L:` in try/catch lists.
/// Med `ExceptionalPreds` must not block this: the handler is already a clause.
void invertSkipGotos(HighFunc &Func);

/// Copy catch-funclet and C++ unwind-funclet HighFunc bodies into empty
/// `CxxCatch` / `CxxCleanup` clause slots of the parent. MSVC x64 catch
/// handlers and destructor unwind actions are separate pdata functions.
/// Only a function that received a body is simplified again: the others
/// leave HighIR conversion final, as its flow check saw them.
/// A funclet body may itself be a structured `CxxTry` whose handler VA is
/// this function or another funclet already on the attach stack; copying
/// those bodies without a cycle guard overflows the stack on full-image
/// HighC of MSVC catch-all probes.
inline void attachCxxFuncletBodies(std::vector<HighFunc> &Funcs) {
  // Always copy the original bodies. Otherwise attaching a nested catch to an
  // earlier function can change what a later parent receives.
  std::map<va_t, std::vector<HighStmt>> ByEntry;
  std::map<va_t, std::set<va_t>> ContinuationsByEntry;
  for (const HighFunc &Func : Funcs)
    if (Func.Entry) {
      ByEntry[Func.Entry] = Func.Body;
      ContinuationsByEntry[Func.Entry] = Func.CxxContinuationTargets;
    }
  for (HighFunc &Func : Funcs) {
    // A PE32 callback uses the parent's EBP and a private runtime stack.
    // A separately converted ordinary HighFunc carries no projection of
    // those coordinates. Retain its native clause target until that contract
    // is proved; bodies already recovered inside the parent remain intact.
    if (Func.ExceptionMetadata && Func.ExceptionMetadata->Registration)
      continue;
    std::set<va_t> Active;
    std::set<va_t> Continuations = Func.CxxContinuationTargets;
    bool Attached = false;
    auto Attach = [&](auto &&Self, std::vector<HighStmt> &Stmts) -> void {
      for (HighStmt &Stmt : Stmts) {
        if (!Active.empty() && Stmt.Kind == StmtKind::Return &&
            !Stmt.CxxContinuationReturnTargets.empty() &&
            std::all_of(Stmt.CxxContinuationReturnTargets.begin(),
                        Stmt.CxxContinuationReturnTargets.end(),
                        [&](va_t VA) { return Continuations.count(VA); })) {
          const auto Targets = Stmt.CxxContinuationReturnTargets;
          Stmt.CxxContinuationReturnTargets.clear();
          auto Jump = [&](va_t Target) {
            HighStmt Result;
            Result.Kind = StmtKind::Goto;
            Result.Addr = Stmt.Addr;
            Result.GotoTarget = Target;
            return Result;
          };
          if (Targets.size() == 1) {
            // The original expression still executes: a projected return may
            // have folded an effectful call into its value.
            auto HasEffect = [&](auto &&Self, const ExprPtr &Expr) -> bool {
              if (!Expr)
                return false;
              bool Result =
                  Expr->Kind == ExprKind::Call ||
                  Expr->Kind == ExprKind::Store ||
                  Expr->MemoryOrdering != NdMemoryOrdering::None ||
                  Expr->MemoryAddressSpace != NdMemoryAddressSpace::Default;
              Expr->forEachChildExpr(
                  [&](const ExprPtr &Child) { Result |= Self(Self, Child); });
              return Result;
            };
            if (HasEffect(HasEffect, Stmt.RetVal)) {
              HighStmt Value;
              Value.Kind = StmtKind::Call;
              Value.CallExpr = Stmt.RetVal;
              Stmt.Kind = StmtKind::Block;
              Stmt.Body = {std::move(Value), Jump(Targets.front())};
              Stmt.RetVal.reset();
            } else {
              Stmt = Jump(Targets.front());
            }
          } else if (Stmt.RetVal) {
            Stmt.Kind = StmtKind::Switch;
            Stmt.SwitchExpr = Stmt.RetVal;
            Stmt.RetVal.reset();
            for (size_t I = 0; I + 1 < Targets.size(); ++I)
              Stmt.Cases.push_back({Targets[I], {Jump(Targets[I])}});
            // The exact RETURN proof enumerated the entire domain. Once all
            // earlier values are excluded, only the last target remains.
            Stmt.DefaultBody = {Jump(Targets.back())};
          }
        }
        if (Stmt.Kind == StmtKind::CxxTry) {
          if (Stmt.EHClauseBodies.size() < Stmt.EHClauses.size())
            Stmt.EHClauseBodies.resize(Stmt.EHClauses.size());
          for (size_t I = 0; I < Stmt.EHClauses.size(); ++I) {
            if (Stmt.EHClauseBodies[I].empty()) {
              const HighEHClause &Clause = Stmt.EHClauses[I];
              va_t Target = 0;
              if (Clause.Kind == HighEHClauseKind::CxxCatch)
                Target = Clause.HandlerVA;
              else if (Clause.Kind == HighEHClauseKind::CxxCleanup)
                Target = Clause.FilterOrActionVA;
              if (Target && Target != Func.Entry && !Active.count(Target)) {
                auto It = ByEntry.find(Target);
                if (It != ByEntry.end()) {
                  Active.insert(Target);
                  const auto SavedContinuations = Continuations;
                  const auto &NestedContinuations =
                      ContinuationsByEntry[Target];
                  Continuations.insert(NestedContinuations.begin(),
                                       NestedContinuations.end());
                  Stmt.EHClauseBodies[I] = It->second;
                  Attached = true;
                  Self(Self, Stmt.EHClauseBodies[I]);
                  Continuations = SavedContinuations;
                  Active.erase(Target);
                  continue;
                }
              }
            }
            Self(Self, Stmt.EHClauseBodies[I]);
          }
        }
        Self(Self, Stmt.Body);
        Self(Self, Stmt.ElseBody);
        for (auto &Case : Stmt.Cases)
          Self(Self, Case.Body);
        Self(Self, Stmt.DefaultBody);
      }
    };
    Attach(Attach, Func.Body);
    if (Attached)
      invertSkipGotos(Func);
  }
}

/// The entry register represented by the source projection's private frame.
bool isSyntheticEntryStackPointer(const MedVar &Value, const HighFunc &Function,
                                  Arch Architecture);

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHIR_H
