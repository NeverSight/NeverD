//===- BitBlaster.cpp - Expression nodes to bit-level circuits ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walks an expression graph and gives every node a vector of literals, one
/// per bit.  The circuits themselves live next door; what happens here is the
/// traversal, the cache, and the mapping from each operator to the circuit
/// that means the same thing.
///
/// The traversal relies on one property of the expression context: a node is
/// interned only once its operands exist, so an operand always has a smaller
/// index than the node using it.  Ascending index order over the reachable set
/// is therefore an order in which every operand is already encoded when its
/// user is reached — no recursion, no post-order stack, and no risk of running
/// out of stack on the thousand-deep graphs that obfuscated arithmetic
/// produces.
///
/// The cache is what makes repeated and incremental use affordable.  It is
/// never invalidated, because a context only grows: a node's meaning is fixed
/// the moment it is interned, so the literals standing for its bits stay
/// correct for as long as the context lives.
///
//===----------------------------------------------------------------------===//

#include "neverd/solver/BitBlaster.h"

#include "BitBlastDetail.h"

#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace neverd::solver {

using symbolic::SymOp;
using symbolic::SymRef;

const char *blastErrorName(BlastError E) {
  switch (E) {
  case BlastError::None:
    return "none";
  case BlastError::WidthTooLarge:
    return "width too large";
  case BlastError::TooManyGates:
    return "too many gates";
  case BlastError::Malformed:
    return "malformed expression";
  }
  return "?";
}

BitBlaster::BitBlaster(const symbolic::SymContext &Ctx, CnfEncoder &Enc,
                       const BlastLimits &Limits)
    : Ctx(Ctx), Enc(Enc), Limits(Limits), GateBase(Enc.numGates()) {}

bool BitBlaster::withinGateBudget() const {
  return Enc.numGates() - GateBase <= Limits.MaxGates;
}

bool BitBlaster::fail(BlastError E) {
  if (Error == BlastError::None)
    Error = E;
  return false;
}

void BitBlaster::store(SymRef R, llvm::ArrayRef<SatLit> Bits) {
  Slice S;
  S.First = static_cast<uint32_t>(BitPool.size());
  S.Width = static_cast<uint32_t>(Bits.size());
  BitPool.insert(BitPool.end(), Bits.begin(), Bits.end());

  if (Encoded.size() <= R.index())
    Encoded.resize(R.index() + 1);
  Encoded[R.index()] = S;
}

llvm::ArrayRef<SatLit> BitBlaster::variableBits(uint32_t VarId) const {
  if (VarId >= VarSlices.size() || VarSlices[VarId].Width == 0)
    return {};
  const Slice &S = VarSlices[VarId];
  return llvm::ArrayRef<SatLit>(BitPool.data() + S.First, S.Width);
}

//===----------------------------------------------------------------------===//
// Traversal
//===----------------------------------------------------------------------===//

bool BitBlaster::encodeReachable(SymRef Root) {
  if (!Root.isValid() || Root.index() >= Ctx.numNodes())
    return fail(BlastError::Malformed);

  // Stamping avoids clearing a per-node array on every call.  The counter is
  // wide enough that wrapping needs billions of calls, but a wrap would make
  // stale marks look current, so it is handled rather than assumed away.
  if (++Visit == 0) {
    std::fill(Stamp.begin(), Stamp.end(), 0);
    Visit = 1;
  }
  Stamp.resize(Ctx.numNodes(), 0);

  Order.clear();
  Work.clear();
  Work.push_back(Root.index());

  while (!Work.empty()) {
    uint32_t Index = Work.back();
    Work.pop_back();

    if (Stamp[Index] == Visit)
      continue;
    Stamp[Index] = Visit;

    // Anything encoded by an earlier call is a boundary: its bits are already
    // in the pool and its operands were encoded with it.
    if (isEncoded(SymRef(Index)))
      continue;

    Order.push_back(Index);
    for (SymRef Operand : Ctx.operands(SymRef(Index)))
      Work.push_back(Operand.index());
  }

  // Interning appends a node only after its operands exist, so this is a
  // topological order and one sort replaces a post-order walk.
  llvm::sort(Order);

  for (uint32_t Index : Order) {
    if (!encodeNode(SymRef(Index)))
      return false;
    if (!withinGateBudget())
      return fail(BlastError::TooManyGates);
  }
  return true;
}

bool BitBlaster::blast(SymRef R, BitLits &Out) {
  Out.clear();
  if (!ok())
    return false;
  if (!encodeReachable(R))
    return false;

  llvm::ArrayRef<SatLit> Bits = bitsOf(R);
  Out.assign(Bits.begin(), Bits.end());
  return true;
}

bool BitBlaster::blastPredicate(SymRef R, SatLit &Out) {
  if (!ok())
    return false;
  if (!R.isValid() || R.index() >= Ctx.numNodes())
    return fail(BlastError::Malformed);
  if (Ctx.width(R) != 1)
    return fail(BlastError::Malformed);
  if (!encodeReachable(R))
    return false;

  Out = bitsOf(R)[0];
  return true;
}

//===----------------------------------------------------------------------===//
// One node
//===----------------------------------------------------------------------===//

bool BitBlaster::encodeNode(SymRef R) {
  const symbolic::SymNode &N = Ctx.node(R);
  const uint32_t Width = N.Width;

  if (Width == 0)
    return fail(BlastError::Malformed);
  if (Width > Limits.MaxWidth)
    return fail(BlastError::WidthTooLarge);

  detail::LitVec Result;
  detail::LitVec Scratch;

  auto operandBits = [&](unsigned I) { return bitsOf(Ctx.operand(R, I)); };

  switch (N.Op) {
  case SymOp::Const: {
    llvm::APInt Value = Ctx.constValue(R);
    Result.reserve(Width);
    for (uint32_t I = 0; I < Width; ++I)
      Result.push_back(Enc.constant(Value[I]));
    break;
  }

  case SymOp::Var: {
    // One variable, one set of bits, however many times it occurs: the node is
    // interned, so this runs once per variable and every use shares it.
    Result.reserve(Width);
    for (uint32_t I = 0; I < Width; ++I)
      Result.push_back(Enc.freshLit());
    store(R, Result);

    uint32_t Id = Ctx.varId(R);
    if (VarSlices.size() <= Id)
      VarSlices.resize(Id + 1);
    VarSlices[Id] = Encoded[R.index()];
    EncodedVars.push_back(Id);
    return true;
  }

  case SymOp::Not:
    // Complementing a literal is free, which is why the bitwise complement is
    // a primitive of the expression language rather than a subtraction.
    Result.reserve(Width);
    for (SatLit L : operandBits(0))
      Result.push_back(~L);
    break;

  case SymOp::And:
  case SymOp::Or:
  case SymOp::Xor: {
    Result.assign(operandBits(0).begin(), operandBits(0).end());
    for (unsigned K = 1; K < N.NumOperands; ++K) {
      llvm::ArrayRef<SatLit> Next = operandBits(K);
      for (uint32_t I = 0; I < Width; ++I) {
        if (N.Op == SymOp::And)
          Result[I] = Enc.mkAnd(Result[I], Next[I]);
        else if (N.Op == SymOp::Or)
          Result[I] = Enc.mkOr(Result[I], Next[I]);
        else
          Result[I] = Enc.mkXor(Result[I], Next[I]);
      }
    }
    break;
  }

  case SymOp::Add: {
    Result.assign(operandBits(0).begin(), operandBits(0).end());
    for (unsigned K = 1; K < N.NumOperands; ++K) {
      detail::addBits(Enc, Result, operandBits(K), Enc.falseLit(), Scratch);
      Result.swap(Scratch);
    }
    break;
  }

  case SymOp::Mul: {
    Result.assign(operandBits(0).begin(), operandBits(0).end());
    for (unsigned K = 1; K < N.NumOperands; ++K) {
      detail::multiplyBits(Enc, Result, operandBits(K), Scratch);
      Result.swap(Scratch);
    }
    break;
  }

  case SymOp::Shl:
    detail::shiftBits(Enc, operandBits(0), operandBits(1),
                      detail::ShiftKind::Left, Result);
    break;
  case SymOp::LShr:
    detail::shiftBits(Enc, operandBits(0), operandBits(1),
                      detail::ShiftKind::LogicalRight, Result);
    break;
  case SymOp::AShr:
    detail::shiftBits(Enc, operandBits(0), operandBits(1),
                      detail::ShiftKind::ArithmeticRight, Result);
    break;
  case SymOp::Rol:
    detail::shiftBits(Enc, operandBits(0), operandBits(1),
                      detail::ShiftKind::RotateLeft, Result);
    break;
  case SymOp::Ror:
    detail::shiftBits(Enc, operandBits(0), operandBits(1),
                      detail::ShiftKind::RotateRight, Result);
    break;

  case SymOp::UDiv:
    detail::divideBits(Enc, operandBits(0), operandBits(1), &Result, nullptr);
    break;
  case SymOp::URem:
    detail::divideBits(Enc, operandBits(0), operandBits(1), nullptr, &Result);
    break;
  case SymOp::SDiv:
    detail::divideSignedBits(Enc, operandBits(0), operandBits(1), &Result,
                             nullptr);
    break;
  case SymOp::SRem:
    detail::divideSignedBits(Enc, operandBits(0), operandBits(1), nullptr,
                             &Result);
    break;

  case SymOp::Extract: {
    llvm::ArrayRef<SatLit> Source = operandBits(0);
    auto Low = static_cast<uint32_t>(N.Aux);
    if (Low + Width > Source.size())
      return fail(BlastError::Malformed);
    Result.assign(Source.begin() + Low, Source.begin() + Low + Width);
    break;
  }

  case SymOp::Concat: {
    // Operands run most significant first, and bits run least significant
    // first, so the operand list is consumed backwards.
    Result.reserve(Width);
    for (unsigned K = N.NumOperands; K > 0; --K) {
      llvm::ArrayRef<SatLit> Part = operandBits(K - 1);
      Result.append(Part.begin(), Part.end());
    }
    if (Result.size() != Width)
      return fail(BlastError::Malformed);
    break;
  }

  case SymOp::ZExt: {
    llvm::ArrayRef<SatLit> Source = operandBits(0);
    Result.assign(Source.begin(), Source.end());
    Result.resize(Width, Enc.falseLit());
    break;
  }

  case SymOp::SExt: {
    llvm::ArrayRef<SatLit> Source = operandBits(0);
    if (Source.empty())
      return fail(BlastError::Malformed);
    Result.assign(Source.begin(), Source.end());
    Result.resize(Width, Source.back());
    break;
  }

  case SymOp::Ite: {
    llvm::ArrayRef<SatLit> Cond = operandBits(0);
    if (Cond.size() != 1)
      return fail(BlastError::Malformed);
    detail::selectBits(Enc, Cond[0], operandBits(1), operandBits(2), Result);
    break;
  }

  case SymOp::Eq:
    Result.push_back(detail::equalBits(Enc, operandBits(0), operandBits(1)));
    break;
  case SymOp::Ult:
    Result.push_back(detail::lessThanBits(Enc, operandBits(0), operandBits(1),
                                          /*OrEqual=*/false,
                                          /*Signed=*/false));
    break;
  case SymOp::Ule:
    Result.push_back(detail::lessThanBits(Enc, operandBits(0), operandBits(1),
                                          /*OrEqual=*/true, /*Signed=*/false));
    break;
  case SymOp::Slt:
    Result.push_back(detail::lessThanBits(Enc, operandBits(0), operandBits(1),
                                          /*OrEqual=*/false, /*Signed=*/true));
    break;
  case SymOp::Sle:
    Result.push_back(detail::lessThanBits(Enc, operandBits(0), operandBits(1),
                                          /*OrEqual=*/true, /*Signed=*/true));
    break;
  }

  if (Result.size() != Width)
    return fail(BlastError::Malformed);

  store(R, Result);
  return true;
}

} // namespace neverd::solver
