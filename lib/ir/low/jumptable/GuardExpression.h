//===- GuardExpression.h - Exact guard replay partitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
// Internal immutable guard syntax and exact finite-window replay partitions.
// This partition never replaces the all-domain guard proof.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_LOW_JUMPTABLE_GUARDEXPRESSION_H
#define NEVERD_IR_LOW_JUMPTABLE_GUARDEXPRESSION_H

#include "neverd/ir/NdOps.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::guard_detail {

struct GuardExpr;
using GuardExprPtr = std::shared_ptr<const GuardExpr>;

struct GuardExpr {
  enum class Kind : uint8_t { Constant, Index, Operation } K;
  uint16_t Size = 0;
  uint64_t Constant = 0;
  NdOp Opcode = NdOp::NOP;
  std::vector<GuardExprPtr> Inputs;
  bool ContainsIndex = false;
};

inline uint64_t guardIndexMask(uint16_t Size) {
  return Size >= sizeof(uint64_t) ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t(1) << (unsigned(Size) * 8u)) - 1;
}

// A modular affine index has slope +1 or -1 at the index's original width.
// Restrict this summary to width-preserving operations: a narrowed index can
// wrap repeatedly within the discovery window and needs the ordinary replay.
struct GuardAffineIndex {
  uint16_t Size;
  bool Negative;
  uint64_t Offset;
};

inline std::optional<GuardAffineIndex>
guardAffineIndex(const GuardExprPtr &Expr) {
  if (!Expr || Expr->Size == 0 || Expr->Size > sizeof(uint64_t))
    return std::nullopt;
  if (Expr->K == GuardExpr::Kind::Index)
    return GuardAffineIndex{Expr->Size, false, 0};
  if (Expr->K != GuardExpr::Kind::Operation)
    return std::nullopt;
  if (Expr->Opcode == NdOp::COPY && Expr->Inputs.size() == 1 &&
      Expr->Inputs[0]->Size == Expr->Size)
    return guardAffineIndex(Expr->Inputs[0]);
  if ((Expr->Opcode != NdOp::INT_ADD && Expr->Opcode != NdOp::INT_SUB) ||
      Expr->Inputs.size() != 2 || Expr->Inputs[0]->Size != Expr->Size ||
      Expr->Inputs[1]->Size != Expr->Size)
    return std::nullopt;
  for (unsigned I = 0; I != 2; ++I) {
    const auto &Constant = Expr->Inputs[1 - I];
    if (Constant->K != GuardExpr::Kind::Constant)
      continue;
    auto Affine = guardAffineIndex(Expr->Inputs[I]);
    if (!Affine)
      continue;
    if (Expr->Opcode == NdOp::INT_ADD)
      Affine->Offset += Constant->Constant;
    else if (I == 0)
      Affine->Offset -= Constant->Constant;
    else {
      Affine->Negative = !Affine->Negative;
      Affine->Offset = Constant->Constant - Affine->Offset;
    }
    Affine->Offset &= guardIndexMask(Affine->Size);
    return Affine;
  }
  return std::nullopt;
}

// These are exact change points of a guard's truth value, not samples from
// which monotonicity is assumed. A comparison of an affine index and a literal
// can change only at the literal, the unsigned wrap, or the signed boundary.
// Include both sides of each point for either slope and strict/inclusive ops.
// Boolean compositions inherit the union. Unsupported syntax falls back to
// evaluating every index; the full-domain solver proof is still mandatory.
inline bool guardChangePoints(const GuardExprPtr &Expr, uint64_t LastSample,
                              std::vector<uint64_t> &Points) {
  if (!Expr || Expr->Size == 0 || Expr->Size > sizeof(uint64_t))
    return false;
  auto Add = [&](uint64_t Point) {
    if (Point <= LastSample)
      Points.push_back(Point);
  };
  if (!Expr->ContainsIndex)
    return true;
  if (Expr->K == GuardExpr::Kind::Index) {
    if (LastSample > guardIndexMask(Expr->Size))
      return false;
    Add(1);
    return true;
  }
  if (Expr->K != GuardExpr::Kind::Operation)
    return false;
  switch (Expr->Opcode) {
  case NdOp::BOOL_NOT:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    if (Expr->Inputs.size() != (Expr->Opcode == NdOp::BOOL_NOT ? 1u : 2u))
      return false;
    for (const auto &Input : Expr->Inputs)
      if (!guardChangePoints(Input, LastSample, Points))
        return false;
    return true;
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
    break;
  default:
    return false;
  }
  if (Expr->Inputs.size() != 2)
    return false;
  for (unsigned I = 0; I != 2; ++I) {
    const auto &Constant = Expr->Inputs[1 - I];
    if (Constant->K != GuardExpr::Kind::Constant)
      continue;
    auto Affine = guardAffineIndex(Expr->Inputs[I]);
    if (!Affine || Constant->Size != Affine->Size ||
        LastSample > guardIndexMask(Affine->Size))
      continue;
    const uint64_t Sign = uint64_t(1) << (unsigned(Affine->Size) * 8u - 1);
    for (uint64_t Value : {uint64_t(0), Constant->Constant, Sign}) {
      const uint64_t Point =
          (Affine->Negative ? Affine->Offset - Value : Value - Affine->Offset) &
          guardIndexMask(Affine->Size);
      Add(Point);
      if (Point != std::numeric_limits<uint64_t>::max())
        Add(Point + 1);
    }
    return true;
  }
  return false;
}

} // namespace neverd::guard_detail
#endif
