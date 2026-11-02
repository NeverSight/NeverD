//===- LowScalarSymbolic.cpp - Shared symbolic integer semantics ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep scalar control and complete-domain table proofs on one word contract.

#include "LowScalarSymbolic.h"

#include <algorithm>
#include <vector>

namespace neverd {
namespace {

using symbolic::SymContext;
using symbolic::SymRef;

std::optional<SymRef> coerceGuardValue(SymContext &Ctx, SymRef Value,
                                       uint32_t Width) {
  if (!Value || Width == 0 || Width > 64)
    return std::nullopt;
  const uint32_t InputWidth = Ctx.width(Value);
  if (InputWidth == Width)
    return Value;
  if (InputWidth < Width)
    return Ctx.mkZExt(Value, Width);
  return Ctx.mkExtract(Value, 0, Width);
}

} // namespace

std::optional<SymRef>
symbolizeJumpTableIntegerOperation(SymContext &Ctx, NdOp Opcode,
                                   uint16_t OutputSize,
                                   llvm::ArrayRef<SymRef> RawInputs) {
  if (OutputSize == 0 || OutputSize > sizeof(uint64_t) || RawInputs.empty() ||
      std::any_of(RawInputs.begin(), RawInputs.end(), [&](SymRef Input) {
        return !Input || Ctx.width(Input) == 0 || Ctx.width(Input) > 64;
      }))
    return std::nullopt;
  const uint32_t OutputWidth = uint32_t(OutputSize) * 8u;
  auto finish = [&](SymRef Value) -> std::optional<SymRef> {
    return coerceGuardValue(Ctx, Value, OutputWidth);
  };
  auto commonInputs = [&]() -> std::optional<std::vector<SymRef>> {
    uint32_t Width = 0;
    for (SymRef Input : RawInputs)
      Width = std::max(Width, Ctx.width(Input));
    std::vector<SymRef> Inputs;
    Inputs.reserve(RawInputs.size());
    for (SymRef Input : RawInputs) {
      std::optional<SymRef> Coerced = coerceGuardValue(Ctx, Input, Width);
      if (!Coerced)
        return std::nullopt;
      Inputs.push_back(*Coerced);
    }
    return Inputs;
  };
  auto binaryInputs = [&]() -> std::optional<std::vector<SymRef>> {
    if (RawInputs.size() != 2)
      return std::nullopt;
    return commonInputs();
  };
  auto predicateResult = [&](SymRef Predicate) -> std::optional<SymRef> {
    if (!Predicate || Ctx.width(Predicate) != 1)
      return std::nullopt;
    return finish(Predicate);
  };

  switch (Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    if (RawInputs.size() != 1)
      return std::nullopt;
    return finish(RawInputs[0]);
  case NdOp::INT_SEXT:
    if (RawInputs.size() != 1)
      return std::nullopt;
    if (Ctx.width(RawInputs[0]) < OutputWidth)
      return Ctx.mkSExt(RawInputs[0], OutputWidth);
    return finish(RawInputs[0]);
  case NdOp::SUBBYTES: {
    if (RawInputs.size() != 2)
      return std::nullopt;
    const std::optional<llvm::APInt> Offset = Ctx.asConst(RawInputs[1]);
    if (!Offset || Offset->getActiveBits() > 64)
      return std::nullopt;
    const uint64_t ByteOffset = Offset->getZExtValue();
    if (ByteOffset > Ctx.width(RawInputs[0]) / 8u)
      return std::nullopt;
    const uint64_t Low = ByteOffset * 8u;
    if (Low > Ctx.width(RawInputs[0]) ||
        OutputWidth > Ctx.width(RawInputs[0]) - Low)
      return std::nullopt;
    return Ctx.mkExtract(RawInputs[0], static_cast<uint32_t>(Low), OutputWidth);
  }
  case NdOp::CONCAT: {
    if (RawInputs.size() < 2)
      return std::nullopt;
    return finish(Ctx.mkConcat(RawInputs));
  }
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
    if (RawInputs.size() != 1)
      return std::nullopt;
    return finish(Ctx.mkNot(RawInputs[0]));
  case NdOp::INT_NEG2:
    if (RawInputs.size() != 1)
      return std::nullopt;
    return finish(Ctx.mkNeg(RawInputs[0]));
  case NdOp::BOOL_NOT:
    if (RawInputs.size() != 1)
      return std::nullopt;
    return predicateResult(
        Ctx.mkEq(RawInputs[0], Ctx.mkZero(Ctx.width(RawInputs[0]))));
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR: {
    if (RawInputs.size() != 2)
      return std::nullopt;
    const auto L = Ctx.mkNe(RawInputs[0], Ctx.mkZero(Ctx.width(RawInputs[0])));
    const auto R = Ctx.mkNe(RawInputs[1], Ctx.mkZero(Ctx.width(RawInputs[1])));
    return predicateResult(Opcode == NdOp::BOOL_AND  ? Ctx.mkAnd(L, R)
                           : Opcode == NdOp::BOOL_OR ? Ctx.mkOr(L, R)
                                                     : Ctx.mkXor(L, R));
  }
  default:
    break;
  }

  std::optional<std::vector<SymRef>> Inputs = binaryInputs();
  if (!Inputs)
    return std::nullopt;
  SymRef L = (*Inputs)[0];
  SymRef R = (*Inputs)[1];
  switch (Opcode) {
  case NdOp::INT_ADD:
    return finish(Ctx.mkAdd(L, R));
  case NdOp::INT_SUB:
    return finish(Ctx.mkSub(L, R));
  case NdOp::INT_MULT:
    // AArch64's narrow multiply is explicitly performed at OutputWidth while
    // the generic emitter multiplies at the common input width.  They agree
    // only when that width is already the output width; otherwise the guard is
    // not architecture-neutral and must not authorize a table.
    if (Ctx.width(L) != OutputWidth)
      return std::nullopt;
    return finish(Ctx.mkMul(L, R));
  case NdOp::INT_DIV:
    return finish(Ctx.mkUDiv(L, R));
  case NdOp::INT_SDIV:
    return finish(Ctx.mkSDiv(L, R));
  case NdOp::INT_REM:
    return finish(Ctx.mkURem(L, R));
  case NdOp::INT_SREM:
    return finish(Ctx.mkSRem(L, R));
  case NdOp::INT_AND:
    return finish(Ctx.mkAnd(L, R));
  case NdOp::INT_OR:
    return finish(Ctx.mkOr(L, R));
  case NdOp::INT_XOR:
    return finish(Ctx.mkXor(L, R));
  case NdOp::INT_LEFT:
    return finish(Ctx.mkShl(L, R));
  case NdOp::INT_RIGHT:
    return finish(Ctx.mkLShr(L, R));
  case NdOp::INT_ASHR:
    return finish(Ctx.mkAShr(L, R));
  case NdOp::INT_EQUAL:
    return predicateResult(Ctx.mkEq(L, R));
  case NdOp::INT_NOTEQUAL:
    return predicateResult(Ctx.mkNe(L, R));
  case NdOp::INT_LESS:
    return predicateResult(Ctx.mkUlt(L, R));
  case NdOp::INT_LESSEQUAL:
    return predicateResult(Ctx.mkUle(L, R));
  case NdOp::INT_SLESS:
    return predicateResult(Ctx.mkSlt(L, R));
  case NdOp::INT_SLESSEQUAL:
    return predicateResult(Ctx.mkSle(L, R));
  case NdOp::INT_CARRY: {
    SymRef Sum = Ctx.mkAdd(L, R);
    return predicateResult(Ctx.mkUlt(Sum, L));
  }
  case NdOp::INT_SOVF: {
    SymRef Sum = Ctx.mkAdd(L, R);
    SymRef OverflowBits =
        Ctx.mkAnd(Ctx.mkNot(Ctx.mkXor(L, R)), Ctx.mkXor(Sum, L));
    return predicateResult(Ctx.mkExtract(OverflowBits, Ctx.width(L) - 1, 1));
  }
  case NdOp::INT_SBOR: {
    SymRef Diff = Ctx.mkSub(L, R);
    SymRef OverflowBits = Ctx.mkAnd(Ctx.mkXor(L, R), Ctx.mkXor(Diff, L));
    return predicateResult(Ctx.mkExtract(OverflowBits, Ctx.width(L) - 1, 1));
  }
  default:
    return std::nullopt;
  }
}

} // namespace neverd
