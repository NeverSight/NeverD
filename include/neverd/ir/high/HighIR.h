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

#include <memory>
#include <optional>
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

inline VarKey varKey(const MedVar &V) { return {V.Id, V.SSAVer}; }

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
  Phi
};

struct HighExpr {
  ExprKind Kind = ExprKind::Const;
  NdOp Op = NdOp::NOP;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  TypeRef Type;

  /// For Var
  MedVar Var = {};

  /// For Const
  uint64_t ConstVal = 0;

  /// For BinOp / UnaryOp
  std::vector<std::shared_ptr<HighExpr>> Operands;

  /// For Call
  std::string CallTarget;
  va_t CallAddr = 0;
  bool IsIndirectCall = false;
  int IndirectParamIdx = -1;
  Intrinsic IntrinsicId = Intrinsic::None;
  std::vector<MedVar> IntrinsicOutputs;

  /// For Cast
  TypeRef CastTo;

  std::string str() const;
  bool structuralEq(const HighExpr &Other) const;
  bool hasOrderedMemoryAccess() const;

  static std::shared_ptr<HighExpr> makeVar(MedVar V, TypeRef Ty = nullptr);
  static std::shared_ptr<HighExpr> makeConst(uint64_t Val, uint16_t Size);
  static std::shared_ptr<HighExpr> makeUndef(uint16_t Size);
  static std::shared_ptr<HighExpr>
  makeBinop(NdOp Op, std::shared_ptr<HighExpr> L, std::shared_ptr<HighExpr> R);
  static std::shared_ptr<HighExpr> makeUnary(NdOp Op,
                                             std::shared_ptr<HighExpr> Operand);
  static std::shared_ptr<HighExpr>
  makeLoad(std::shared_ptr<HighExpr> Addr, TypeRef Ty,
           NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None);
  static std::shared_ptr<HighExpr>
  makeCall(const std::string &Target, va_t Addr,
           std::vector<std::shared_ptr<HighExpr>> Args);
};

using ExprPtr = std::shared_ptr<HighExpr>;

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
};

struct HighStmt {
  StmtKind Kind = StmtKind::Nop;
  va_t Addr = 0;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;

  /// For Assign
  ExprPtr Dst;
  ExprPtr Val;

  /// For If / While / DoWhile
  ExprPtr Cond;
  std::vector<HighStmt> Body;
  std::vector<HighStmt> ElseBody;

  /// For Return
  ExprPtr RetVal;

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

//===----------------------------------------------------------------------===//
// High-level function
//===----------------------------------------------------------------------===//

struct HighParam {
  std::string Name;
  TypeRef Type;
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
  TypeRef ReturnType;
  std::vector<HighParam> Params;
  std::vector<HighLocal> Locals;
  std::vector<HighStmt> Body;
  std::optional<ExceptionFunction> ExceptionMetadata;
  unsigned StructuredExceptionRegions = 0;
  unsigned UnstructuredExceptionRegions = 0;
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHIR_H
