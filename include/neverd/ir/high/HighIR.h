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

  static std::shared_ptr<HighExpr> makeVar(MedVar V, TypeRef Ty = nullptr);
  static std::shared_ptr<HighExpr> makeConst(uint64_t Val, uint16_t Size);
  static std::shared_ptr<HighExpr> makeUndef(uint16_t Size);
  static std::shared_ptr<HighExpr>
  makeBinop(NdOp Op, std::shared_ptr<HighExpr> L, std::shared_ptr<HighExpr> R);
  static std::shared_ptr<HighExpr> makeUnary(NdOp Op,
                                             std::shared_ptr<HighExpr> Operand);
  static std::shared_ptr<HighExpr> makeLoad(std::shared_ptr<HighExpr> Addr,
                                            TypeRef Ty);
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
  Continue
};

struct SwitchCase {
  uint64_t Value = 0;
  std::vector<struct HighStmt> Body;
};

struct HighStmt {
  StmtKind Kind = StmtKind::Nop;
  va_t Addr = 0;

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
};

} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHIR_H
