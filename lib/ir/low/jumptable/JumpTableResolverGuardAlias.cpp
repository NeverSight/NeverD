//===- JumpTableResolverGuardAlias.cpp - Non-copy guard matching ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guard strategies that tie a comparison to the table index by something
/// other than a register copy chain: the COND_BR flag-consumption polarity
/// that reveals an inclusive (`ja`/`jbe`) upper bound worth one extra entry,
/// and the same-location reload equivalence that matches a guard on one reload
/// of a spilled switch variable to the separate reload that feeds the index.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

namespace {

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

uint64_t widthMask(uint16_t Bytes) {
  return Bytes == 0 || Bytes >= sizeof(uint64_t)
             ? std::numeric_limits<uint64_t>::max()
             : (uint64_t(1) << (unsigned(Bytes) * 8u)) - 1;
}

uint64_t truncateToWidth(uint64_t Value, uint16_t Bytes) {
  return Value & widthMask(Bytes);
}

uint64_t signExtendTo64(uint64_t Value, uint16_t Bytes) {
  const unsigned Bits = Bytes == 0 ? 64u : unsigned(Bytes) * 8u;
  if (Bits >= 64)
    return Value;
  const uint64_t Mask = (uint64_t(1) << Bits) - 1;
  Value &= Mask;
  if ((Value & (uint64_t(1) << (Bits - 1))) != 0)
    Value |= ~Mask;
  return Value;
}

} // namespace

std::optional<uint64_t>
evaluateJumpTableGuardPrimitive(NdOp Opcode, uint16_t OutputSize,
                                llvm::ArrayRef<uint64_t> Inputs,
                                llvm::ArrayRef<uint16_t> InputSizes) {
  if (OutputSize == 0 || OutputSize > sizeof(uint64_t) ||
      Inputs.size() != InputSizes.size() || Inputs.empty() ||
      std::any_of(InputSizes.begin(), InputSizes.end(), [](uint16_t Size) {
        return Size == 0 || Size > sizeof(uint64_t);
      }))
    return std::nullopt;
  auto input = [&](size_t I) -> uint64_t {
    return I < Inputs.size() ? truncateToWidth(Inputs[I], InputSizes[I]) : 0;
  };
  auto commonWidth = [&]() -> uint16_t {
    return *std::max_element(InputSizes.begin(), InputSizes.end());
  };
  switch (Opcode) {
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
    return truncateToWidth(~input(0), InputSizes[0]) & widthMask(OutputSize);
  case NdOp::INT_NEG2:
    return truncateToWidth(~input(0) + 1, InputSizes[0]) &
           widthMask(OutputSize);
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS: {
    if (Inputs.size() != 2)
      return std::nullopt;
    const uint16_t W = commonWidth();
    const uint64_t Sign =
        W >= 8 ? (uint64_t(1) << 63) : (uint64_t(1) << (unsigned(W) * 8u - 1));
    const uint64_t L = truncateToWidth(input(0), W);
    const uint64_t R = truncateToWidth(input(1), W);
    switch (Opcode) {
    case NdOp::INT_EQUAL:
      return L == R;
    case NdOp::INT_NOTEQUAL:
      return L != R;
    case NdOp::INT_LESS:
      return L < R;
    case NdOp::INT_LESSEQUAL:
      return L <= R;
    case NdOp::INT_SLESS:
      return (L ^ Sign) < (R ^ Sign);
    default:
      return std::nullopt;
    }
  }
  case NdOp::INT_SLESSEQUAL: {
    if (Inputs.size() != 2)
      return std::nullopt;
    const uint16_t W = commonWidth();
    const uint64_t Sign =
        W >= 8 ? (uint64_t(1) << 63) : (uint64_t(1) << (unsigned(W) * 8u - 1));
    return (truncateToWidth(input(0), W) ^ Sign) <=
           (truncateToWidth(input(1), W) ^ Sign);
  }
  default:
    return std::nullopt;
  }
}

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
  case NdOp::BOOL_AND:
    return finish(Ctx.mkAnd(L, R));
  case NdOp::INT_OR:
  case NdOp::BOOL_OR:
    return finish(Ctx.mkOr(L, R));
  case NdOp::INT_XOR:
  case NdOp::BOOL_XOR:
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

namespace {

struct GuardSymbolization {
  SymContext Ctx;
  SymRef Index;
  uint16_t IndexSize = 0;
};

std::optional<SymRef> symbolizeGuardExpr(const GuardExprPtr &Expr,
                                         GuardSymbolization &State) {
  if (!Expr || Expr->Size == 0 || Expr->Size > sizeof(uint64_t))
    return std::nullopt;
  const uint32_t Width = uint32_t(Expr->Size) * 8u;
  if (Expr->K == GuardExpr::Kind::Constant)
    return State.Ctx.mkConst(Width, Expr->Constant);
  if (Expr->K == GuardExpr::Kind::Index) {
    if (!State.Index) {
      State.IndexSize = Expr->Size;
      State.Index = State.Ctx.mkVar("jt_guard_index", Width);
    }
    if (State.IndexSize != Expr->Size)
      return std::nullopt;
    return State.Index;
  }

  std::vector<SymRef> Inputs;
  Inputs.reserve(Expr->Inputs.size());
  for (const GuardExprPtr &Input : Expr->Inputs) {
    std::optional<SymRef> Symbolic = symbolizeGuardExpr(Input, State);
    if (!Symbolic)
      return std::nullopt;
    Inputs.push_back(*Symbolic);
  }
  return symbolizeJumpTableIntegerOperation(State.Ctx, Expr->Opcode, Expr->Size,
                                            Inputs);
}

bool proveDenseGuardPrefix(GuardSymbolization &State, SymRef ReachesTable,
                           uint32_t Prefix) {
  if (!State.Index || !ReachesTable || State.Ctx.width(ReachesTable) != 1 ||
      Prefix < limits::kMinJumpTableEntries ||
      Prefix > limits::kMaxJumpTableEntries)
    return false;
  const uint32_t IndexWidth = State.Ctx.width(State.Index);
  SymRef Expected = State.Ctx.mkUlt(
      State.Index, State.Ctx.mkConst(IndexWidth, uint64_t(Prefix)));
  SymRef Counterexample = State.Ctx.mkXor(ReachesTable, Expected);

  solver::SolverOptions Options;
  Options.BuildModel = false;
  Options.Sat.MaxConflicts = limits::kMaxJumpTableGuardSolverConflicts;
  Options.Sat.MaxPropagations = limits::kMaxJumpTableGuardSolverPropagations;
  Options.Sat.MaxWatchVisits = limits::kMaxJumpTableGuardSolverWatchVisits;
  Options.Blast.MaxWidth = 64;
  Options.Blast.MaxGates = limits::kMaxJumpTableGuardSolverGates;
  return solver::checkSat(State.Ctx, Counterexample, nullptr, Options) ==
         solver::SatResult::Unsat;
}

std::optional<uint64_t> evaluateGuardExpr(const GuardExprPtr &Expr,
                                          uint64_t Index) {
  if (!Expr)
    return std::nullopt;
  if (Expr->K == GuardExpr::Kind::Constant)
    return truncateToWidth(Expr->Constant, Expr->Size);
  if (Expr->K == GuardExpr::Kind::Index)
    return truncateToWidth(Index, Expr->Size);

  std::vector<uint64_t> Values;
  Values.reserve(Expr->Inputs.size());
  for (const GuardExprPtr &Input : Expr->Inputs) {
    std::optional<uint64_t> Value = evaluateGuardExpr(Input, Index);
    if (!Value)
      return std::nullopt;
    Values.push_back(*Value);
  }
  auto input = [&](size_t I) -> uint64_t {
    return I < Values.size() ? Values[I] : 0;
  };
  auto inputSize = [&](size_t I) -> uint16_t {
    return I < Expr->Inputs.size() ? Expr->Inputs[I]->Size : Expr->Size;
  };
  const uint64_t Mask = widthMask(Expr->Size);
  const unsigned Bits = Expr->Size == 0 ? 64u : unsigned(Expr->Size) * 8u;
  uint64_t Result = 0;
  switch (Expr->Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
    Result = input(0);
    break;
  case NdOp::INT_SEXT:
    Result = signExtendTo64(input(0), inputSize(0));
    break;
  case NdOp::SUBBYTES:
    if (Values.size() < 2 || input(1) >= sizeof(uint64_t))
      return std::nullopt;
    Result = input(0) >> (unsigned(input(1)) * 8u);
    break;
  case NdOp::CONCAT: {
    if (Values.size() < 2)
      return std::nullopt;
    const unsigned LowBits = unsigned(inputSize(1)) * 8u;
    Result = LowBits >= 64 ? input(1) : (input(0) << LowBits) | input(1);
    break;
  }
  case NdOp::INT_ADD:
    Result = input(0) + input(1);
    break;
  case NdOp::INT_SUB:
    Result = input(0) - input(1);
    break;
  case NdOp::INT_MULT:
    Result = input(0) * input(1);
    break;
  case NdOp::INT_AND:
    Result = input(0) & input(1);
    break;
  case NdOp::INT_OR:
    Result = input(0) | input(1);
    break;
  case NdOp::INT_XOR:
    Result = input(0) ^ input(1);
    break;
  case NdOp::INT_LEFT: {
    const unsigned Shift = static_cast<unsigned>(input(1));
    Result = Shift < Bits ? input(0) << Shift : 0;
    break;
  }
  case NdOp::INT_RIGHT: {
    const unsigned Shift = static_cast<unsigned>(input(1));
    Result = Shift < Bits ? truncateToWidth(input(0), Expr->Size) >> Shift : 0;
    break;
  }
  case NdOp::INT_ASHR: {
    const unsigned Shift = static_cast<unsigned>(input(1));
    const uint64_t Signed = signExtendTo64(input(0), inputSize(0));
    if (Shift >= Bits)
      Result = (Signed >> 63) ? Mask : 0;
    else if (Shift == 0)
      Result = Signed;
    else {
      Result = Signed >> Shift;
      if ((Signed >> 63) != 0 && Bits < 64)
        Result |= Mask & (~uint64_t(0) << (Bits - Shift));
    }
    break;
  }
  case NdOp::INT_NEGATE:
  case NdOp::INT_NEG2:
  case NdOp::INT_NOT:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
    return evaluateJumpTableGuardPrimitive(
        Expr->Opcode, Expr->Size, Values, [&] {
          std::vector<uint16_t> Sizes;
          Sizes.reserve(Expr->Inputs.size());
          for (const GuardExprPtr &InputExpr : Expr->Inputs)
            Sizes.push_back(InputExpr->Size);
          return Sizes;
        }());
  case NdOp::INT_CARRY: {
    const uint16_t W = inputSize(0);
    const uint64_t WMask = widthMask(W);
    return truncateToWidth(input(0), W) > WMask - truncateToWidth(input(1), W);
  }
  case NdOp::INT_SOVF: {
    const uint16_t W = inputSize(0);
    const unsigned WBits = W >= 8 ? 64u : unsigned(W) * 8u;
    const uint64_t Sign = uint64_t(1) << (WBits - 1);
    const uint64_t A = truncateToWidth(input(0), W);
    const uint64_t B = truncateToWidth(input(1), W);
    const uint64_t Sum = truncateToWidth(A + B, W);
    return ((~(A ^ B) & (A ^ Sum) & Sign) != 0);
  }
  case NdOp::INT_SBOR: {
    const uint16_t W = inputSize(0);
    const unsigned WBits = W >= 8 ? 64u : unsigned(W) * 8u;
    const uint64_t Sign = uint64_t(1) << (WBits - 1);
    const uint64_t A = truncateToWidth(input(0), W);
    const uint64_t B = truncateToWidth(input(1), W);
    const uint64_t Diff = truncateToWidth(A - B, W);
    return (((A ^ B) & (A ^ Diff) & Sign) != 0);
  }
  case NdOp::BOOL_NOT:
    return input(0) == 0;
  case NdOp::BOOL_AND:
    return input(0) != 0 && input(1) != 0;
  case NdOp::BOOL_OR:
    return input(0) != 0 || input(1) != 0;
  case NdOp::BOOL_XOR:
    return (input(0) != 0) != (input(1) != 0);
  default:
    return std::nullopt;
  }
  return Result & Mask;
}

} // namespace

//===----------------------------------------------------------------------===//
// inferBoundsFromPreciseGuards — shared CFG/value/polarity bound evidence
//===----------------------------------------------------------------------===//

bool CFGBuilder::inferBoundsFromPreciseGuards(
    const InsnRecord &Rec, JumpTableInfo &Info,
    size_t *CandidateEvidenceBudget) {
  if ((!Info.IndexValueAtUse.isReg() && !Info.IndexValueAtUse.isTemp()) ||
      Info.IndexUseAddr == InvalidVA || Info.IndexUseSeq < 0 ||
      Info.TableLoadAddr == InvalidVA)
    return false;
  if (Info.IndexValueAtUse.Size == 0 ||
      Info.IndexValueAtUse.Size > sizeof(uint64_t)) {
    Info.IncompleteGuardDomain = true;
    return false;
  }
  if (!CandidateEvidenceBudget) {
    Info.IncompleteGuardDomain = true;
    return false;
  }

  bool SawControllingGuard = false;
  bool GuardBuildExhausted = false;
  size_t GuardBuildWork = limits::kMaxJumpTableEvidenceWork;
  auto consumeCandidateEvidence = [&](size_t Amount = 1) {
    if (Amount > *CandidateEvidenceBudget) {
      *CandidateEvidenceBudget = 0;
      GuardBuildExhausted = true;
      return false;
    }
    *CandidateEvidenceBudget -= Amount;
    return true;
  };
  auto consumeCandidateProduct = [&](size_t Left, size_t Right,
                                     size_t Extra = 0) {
    if (Left != 0 && Right >
                         (std::numeric_limits<size_t>::max() - Extra) / Left) {
      *CandidateEvidenceBudget = 0;
      GuardBuildExhausted = true;
      return false;
    }
    return consumeCandidateEvidence(Left * Right + Extra);
  };
  auto consumeGuardBuildWork = [&](size_t Amount = 1) {
    if (Amount > GuardBuildWork) {
      GuardBuildWork = 0;
      GuardBuildExhausted = true;
      return false;
    }
    if (!consumeCandidateEvidence(Amount))
      return false;
    GuardBuildWork -= Amount;
    return true;
  };
  auto failIncomplete = [&]() {
    Info.IncompleteGuardDomain = true;
    return false;
  };
  struct GuardSyntaxNode;
  using GuardSyntaxPtr = std::shared_ptr<GuardSyntaxNode>;
  struct GuardSyntaxNode {
    NdVar Value = {};
    va_t UseAddr = InvalidVA;
    int UseSeq = -1;
    bool IsConstant = false;
    LowOp Def = {};
    bool HasDef = false;
    std::vector<GuardSyntaxPtr> Inputs;
    size_t IndexQuery = std::numeric_limits<size_t>::max();
    size_t DefQuery = std::numeric_limits<size_t>::max();
  };
  using BuildKey = std::tuple<uint8_t, uint64_t, uint16_t, va_t, int>;
  std::map<BuildKey, GuardSyntaxPtr> SyntaxMemo;
  if (!consumeCandidateEvidence(Info.IndexValueAlternatives.size()))
    return failIncomplete();
  std::vector<JumpTableValueOccurrence> IndexAlternatives(
      Info.IndexValueAlternatives);
  if (IndexAlternatives.empty()) {
    if (!consumeCandidateEvidence())
      return failIncomplete();
    IndexAlternatives.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                 Info.IndexUseSeq,
                                 Info.IndexValueDefinedAtUse});
  }
  std::vector<JumpTableValueQuery> ProofQueries;
  std::vector<std::pair<GuardSyntaxPtr, bool>> GuardRoots;

  auto supportedGuardOpcode = [](NdOp Opcode) {
    switch (Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
    case NdOp::SUBBYTES:
    case NdOp::CONCAT:
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
    case NdOp::INT_MULT:
    case NdOp::INT_AND:
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR:
    case NdOp::INT_NEGATE:
    case NdOp::INT_NEG2:
    case NdOp::INT_NOT:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::BOOL_NOT:
      return true;
    default:
      return false;
    }
  };

  std::vector<va_t> GuardBranchAddrs;
  for (const auto &[BranchAddr, BranchRec] : Insns) {
    if (!consumeCandidateEvidence())
      return failIncomplete();
    if (BranchAddr == Rec.Addr || !BranchRec.IsCond || !BranchRec.IsBranch)
      continue;
    if (!consumeCandidateEvidence())
      return failIncomplete();
    GuardBranchAddrs.push_back(BranchAddr);
  }
  bool ControlAnalysisComplete = false;
  const std::vector<std::optional<bool>> TableConditions =
      tableLoadConditionValues(GuardBranchAddrs, Info,
                               &ControlAnalysisComplete,
                               CandidateEvidenceBudget);
  if (!ControlAnalysisComplete ||
      TableConditions.size() != GuardBranchAddrs.size()) {
    if (!GuardBranchAddrs.empty()) {
      Info.HasControllingGuard = true;
      Info.IncompleteGuardDomain = true;
    }
    return false;
  }

  // ARM condition execution can rewrite the architectural index with a
  // SELECT immediately before a guarded early return:
  //
  //   selected = predicate ? replacement : old_index
  //   if (!predicate) goto table
  //
  // On the table edge the selected arm is exactly the old index compared by
  // the range guard, but a path-insensitive must-value query quite correctly
  // sees the SELECT as a merge and cannot equate the pre-SELECT comparison
  // operand with the later table index.  Derive a path-qualified alias only
  // after proving both exact occurrence relations in the shared CFG resolver:
  // the branch predicate equals (or is the complement of) the SELECT
  // condition, and the SELECT output reaches every exact table-index use.
  // Merely sharing a physical register or a lexical temp is never sufficient.
  struct PredicatedIndexAlias {
    JumpTableValueOccurrence Arm;
    std::vector<size_t> Queries;
  };
  std::vector<JumpTableValueQuery> AliasQueries;
  std::vector<PredicatedIndexAlias> AliasCandidates;
  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  auto beforePoint = [](const LowOp &A, va_t Addr, int Seq) {
    return A.Addr < Addr || (A.Addr == Addr && A.Seq < Seq);
  };
  struct LocalPredicateExpr;
  using LocalPredicatePtr = std::shared_ptr<LocalPredicateExpr>;
  struct LocalPredicateExpr {
    enum class Kind : uint8_t { Constant, Leaf, Operation } K = Kind::Leaf;
    uint16_t Size = 0;
    uint64_t Constant = 0;
    NdOp Opcode = NdOp::COPY;
    JumpTableValueOccurrence Leaf;
    std::vector<LocalPredicatePtr> Inputs;
  };
  auto isLocalPredicateOpcode = [](NdOp Opcode) {
    switch (Opcode) {
    case NdOp::BOOL_NOT:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESS:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::INT_CARRY:
    case NdOp::INT_SOVF:
    case NdOp::INT_SBOR:
      return true;
    default:
      return false;
    }
  };
  bool AliasEvidenceExhausted = false;
  size_t AliasSyntaxWork = limits::kMaxJumpTableEvidenceWork;
  auto consumeAliasSyntaxWork = [&](size_t Amount = 1) {
    if (Amount > AliasSyntaxWork) {
      AliasSyntaxWork = 0;
      AliasEvidenceExhausted = true;
      return false;
    }
    if (!consumeCandidateEvidence(Amount)) {
      AliasEvidenceExhausted = true;
      return false;
    }
    AliasSyntaxWork -= Amount;
    return true;
  };
  auto consumeAliasQueryWork = [&](size_t QueryCount,
                                   size_t EvidenceAmount) {
    if (AliasQueries.size() > limits::kMaxJumpTableEvidenceWork ||
        QueryCount >
            limits::kMaxJumpTableEvidenceWork - AliasQueries.size() ||
        !consumeCandidateEvidence(EvidenceAmount)) {
      AliasEvidenceExhausted = true;
      return false;
    }
    return true;
  };
  auto buildLocalPredicate = [&](const InsnRecord &Insn, const NdVar &Root,
                                 size_t Before) -> LocalPredicatePtr {
    if (!consumeAliasSyntaxWork())
      return {};
    std::function<LocalPredicatePtr(const NdVar &, size_t, uint32_t)> Build =
        [&](const NdVar &Value, size_t Cutoff,
            uint32_t Depth) -> LocalPredicatePtr {
      if (Depth > limits::kMaxJumpTableGuardExpressionDepth ||
          Value.Size == 0 || Value.Size > sizeof(uint64_t))
        return {};
      if (!consumeAliasSyntaxWork())
        return {};
      auto Node = std::make_shared<LocalPredicateExpr>();
      Node->Size = Value.Size;
      if (Value.isConst()) {
        Node->K = LocalPredicateExpr::Kind::Constant;
        Node->Constant = Value.Offset;
        return Node;
      }
      if (!Value.isReg() && !Value.isTemp())
        return {};

      auto overlaps = [](const NdVar &A, const NdVar &B) {
        if (A.Space != B.Space || A.Size == 0 || B.Size == 0)
          return false;
        const uint64_t AEnd = A.Offset + A.Size;
        const uint64_t BEnd = B.Offset + B.Size;
        if (AEnd < A.Offset || BEnd < B.Offset)
          return true;
        return A.Offset < BEnd && B.Offset < AEnd;
      };
      size_t DefIndex = Insn.Ops.size();
      for (size_t I = std::min(Cutoff, Insn.Ops.size()); I > 0; --I) {
        if (!consumeAliasSyntaxWork())
          return {};
        const LowOp &Def = Insn.Ops[I - 1];
        if (!overlaps(Def.Output, Value))
          continue;
        if (!sameVar(Def.Output, Value))
          return {};
        DefIndex = I - 1;
        break;
      }
      if (DefIndex == Insn.Ops.size()) {
        const LowOp &Use = Insn.Ops[std::min(Cutoff, Insn.Ops.size() - 1)];
        Node->K = LocalPredicateExpr::Kind::Leaf;
        Node->Leaf = {Value, Use.Addr, Use.Seq,
                      /*DefinedAtPoint=*/false};
        return Node;
      }
      const LowOp &Def = Insn.Ops[DefIndex];
      if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1)
        return Build(Def.Inputs[0], DefIndex, Depth + 1);
      if (!isLocalPredicateOpcode(Def.Opcode) || Def.NumInputs == 0)
        return {};
      Node->K = LocalPredicateExpr::Kind::Operation;
      Node->Opcode = Def.Opcode;
      Node->Size = Def.Output.Size;
      for (uint8_t Input = 0; Input < Def.NumInputs; ++Input) {
        if (!consumeAliasSyntaxWork())
          return {};
        LocalPredicatePtr Child = Build(Def.Inputs[Input], DefIndex, Depth + 1);
        if (!Child)
          return {};
        if (!consumeAliasSyntaxWork())
          return {};
        Node->Inputs.push_back(std::move(Child));
      }
      return Node;
    };
    return Build(Root, Before, 0);
  };
  auto pairLocalPredicates = [&](const LocalPredicatePtr &Left,
                                 const LocalPredicatePtr &Right,
                                 std::vector<size_t> &Queries) {
    if (!consumeAliasSyntaxWork())
      return false;
    std::function<bool(const LocalPredicatePtr &, const LocalPredicatePtr &,
                       uint32_t)>
        Pair = [&](const LocalPredicatePtr &A, const LocalPredicatePtr &B,
                   uint32_t Depth) {
          if (!consumeAliasSyntaxWork())
            return false;
          if (!A || !B || Depth > limits::kMaxJumpTableGuardExpressionDepth ||
              A->Size != B->Size)
            return false;
          if (A->K == LocalPredicateExpr::Kind::Constant ||
              B->K == LocalPredicateExpr::Kind::Constant)
            return A->K == LocalPredicateExpr::Kind::Constant &&
                   B->K == LocalPredicateExpr::Kind::Constant &&
                   A->Constant == B->Constant;
          if (A->K == LocalPredicateExpr::Kind::Leaf ||
              B->K == LocalPredicateExpr::Kind::Leaf) {
            if (A->K != LocalPredicateExpr::Kind::Leaf ||
                B->K != LocalPredicateExpr::Kind::Leaf ||
                !consumeAliasQueryWork(/*QueryCount=*/1,
                                       /*query + alternative + index=*/3))
              return false;
            Queries.push_back(AliasQueries.size());
            AliasQueries.push_back({A->Leaf.Value,
                                    A->Leaf.Addr,
                                    A->Leaf.Seq,
                                    {B->Leaf},
                                    /*AllowZeroExtension=*/false,
                                    /*AllowSignExtension=*/false});
            return true;
          }
          if (A->Opcode != B->Opcode || A->Inputs.size() != B->Inputs.size())
            return false;
          for (size_t I = 0; I < A->Inputs.size(); ++I)
            if (!Pair(A->Inputs[I], B->Inputs[I], Depth + 1))
              return false;
          return true;
        };
    return Pair(Left, Right, 0);
  };
  for (size_t GuardIndex = 0; GuardIndex < GuardBranchAddrs.size();
       ++GuardIndex) {
    if (!consumeAliasSyntaxWork())
      return failIncomplete();
    if (!TableConditions[GuardIndex])
      continue;
    const va_t BranchAddr = GuardBranchAddrs[GuardIndex];
    auto BranchIt = Insns.find(BranchAddr);
    if (BranchIt == Insns.end())
      continue;
    const LowOp *CondBranch = nullptr;
    size_t CondBranchIndex = 0;
    for (size_t I = 0; I < BranchIt->second.Ops.size(); ++I) {
      if (!consumeAliasSyntaxWork())
        return failIncomplete();
      const LowOp &Op = BranchIt->second.Ops[I];
      if (Op.Opcode == NdOp::COND_BR && Op.NumInputs >= 2)
        CondBranch = &Op, CondBranchIndex = I;
    }
    if (!CondBranch)
      continue;

    NdVar Predicate = CondBranch->Inputs[1];
    size_t PredicateUseIndex = CondBranchIndex;
    bool Complemented = false;
    int BeforeSeq = CondBranch->Seq;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeAliasSyntaxWork())
        return failIncomplete();
      const LowOp *Def = nullptr;
      size_t DefIndex = BranchIt->second.Ops.size();
      for (size_t I = PredicateUseIndex; I > 0; --I) {
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        const LowOp &Candidate = BranchIt->second.Ops[I - 1];
        if (Candidate.Seq >= BeforeSeq || !sameVar(Candidate.Output, Predicate))
          continue;
        Def = &Candidate;
        DefIndex = I - 1;
        break;
      }
      if (!Def || Def->NumInputs < 1 ||
          (Def->Opcode != NdOp::COPY && Def->Opcode != NdOp::BOOL_NOT))
        break;
      if (Def->Opcode == NdOp::BOOL_NOT)
        Complemented = !Complemented;
      Predicate = Def->Inputs[0];
      PredicateUseIndex = DefIndex;
      BeforeSeq = Def->Seq;
    }
    const LocalPredicatePtr BranchPredicate =
        buildLocalPredicate(BranchIt->second, Predicate, PredicateUseIndex);
    if (AliasEvidenceExhausted) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    if (!BranchPredicate)
      continue;

    for (const auto &[Addr, Insn] : Insns) {
      if (!consumeAliasSyntaxWork())
        return failIncomplete();
      if (Insn.IsInstructionGuard || Addr > BranchAddr)
        continue;
      for (size_t SelectIndex = 0; SelectIndex < Insn.Ops.size();
           ++SelectIndex) {
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        const LowOp &Select = Insn.Ops[SelectIndex];
        if (Select.Opcode != NdOp::SELECT || Select.NumInputs < 3 ||
            Select.Output.Size == 0 ||
            !beforePoint(Select, BranchAddr, CondBranch->Seq))
          continue;
        const bool SelectConditionOnTable =
            *TableConditions[GuardIndex] != Complemented;
        const NdVar &SelectedArm =
            Select.Inputs[SelectConditionOnTable ? 1 : 2];
        if (SelectedArm.Size == 0 || SelectedArm.Size != Select.Output.Size)
          continue;
        PredicatedIndexAlias Candidate;
        Candidate.Arm = {SelectedArm, Select.Addr, Select.Seq,
                         /*DefinedAtPoint=*/false};
        const LocalPredicatePtr SelectPredicate = buildLocalPredicate(
            Insn, Select.Inputs[0], SelectIndex);
        if (AliasEvidenceExhausted) {
          Info.IncompleteGuardDomain = true;
          return false;
        }
        const bool PredicatesMatch =
            SelectPredicate &&
            pairLocalPredicates(BranchPredicate, SelectPredicate,
                                Candidate.Queries);
        if (AliasEvidenceExhausted) {
          Info.IncompleteGuardDomain = true;
          return false;
        }
        if (!PredicatesMatch)
          continue;
        if (IndexAlternatives.size() >
                std::numeric_limits<size_t>::max() / 3 ||
            !consumeAliasQueryWork(IndexAlternatives.size(),
                                   IndexAlternatives.size() * 3)) {
          Info.IncompleteGuardDomain = true;
          return false;
        }
        for (const JumpTableValueOccurrence &Index : IndexAlternatives) {
          Candidate.Queries.push_back(AliasQueries.size());
          AliasQueries.push_back({Index.Value,
                                  Index.Addr,
                                  Index.Seq,
                                  {{Select.Output, Select.Addr, Select.Seq,
                                    /*DefinedAtPoint=*/true}},
                                  /*AllowZeroExtension=*/false,
                                  /*AllowSignExtension=*/false});
        }
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        AliasCandidates.push_back(std::move(Candidate));
      }
    }
  }
  if (!AliasQueries.empty()) {
    bool AliasProofComplete = false;
    const std::vector<bool> AliasResults = tableValuesMatchAtUses(
        AliasQueries, &AliasProofComplete, nullptr, InvalidVA, nullptr,
        CandidateEvidenceBudget);
    if (!AliasProofComplete || AliasResults.size() != AliasQueries.size()) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    for (const PredicatedIndexAlias &Candidate : AliasCandidates) {
      if (!consumeAliasSyntaxWork())
        return failIncomplete();
      bool CandidateMatches = true;
      for (size_t Query : Candidate.Queries) {
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        if (Query >= AliasResults.size() || !AliasResults[Query]) {
          CandidateMatches = false;
          break;
        }
      }
      if (!CandidateMatches)
        continue;
      bool KnownAlternative = false;
      for (const JumpTableValueOccurrence &Known : IndexAlternatives) {
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        if (Known == Candidate.Arm) {
          KnownAlternative = true;
          break;
        }
      }
      if (!KnownAlternative) {
        if (!consumeAliasSyntaxWork())
          return failIncomplete();
        IndexAlternatives.push_back(Candidate.Arm);
      }
    }
  }

  for (size_t GuardIndex = 0; GuardIndex < GuardBranchAddrs.size();
       ++GuardIndex) {
    if (!consumeGuardBuildWork())
      return failIncomplete();
    const va_t BranchAddr = GuardBranchAddrs[GuardIndex];
    const std::optional<bool> &TableCondition = TableConditions[GuardIndex];
    if (!TableCondition)
      continue;
    SawControllingGuard = true;
    Info.HasControllingGuard = true;

    auto BlockIt = BlockStarts.upper_bound(BranchAddr);
    if (BlockIt == BlockStarts.begin())
      continue;
    --BlockIt;
    const va_t BlockStart = *BlockIt;
    std::vector<LowOp> Ops;
    for (auto It = Insns.lower_bound(BlockStart);
         It != Insns.end() && It->first <= BranchAddr; ++It) {
      if (!consumeGuardBuildWork())
        break;
      auto NextBlock = BlockStarts.upper_bound(It->first);
      if (NextBlock == BlockStarts.begin() ||
          *std::prev(NextBlock) != BlockStart)
        break;
      for (const LowOp &Op : It->second.Ops) {
        if (!consumeGuardBuildWork(/*scan + retention=*/2))
          break;
        Ops.push_back(Op);
      }
      if (GuardBuildExhausted)
        break;
    }
    if (GuardBuildExhausted) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    int CondIndex = -1;
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      if (!consumeGuardBuildWork())
        break;
      if (Ops[I].Addr == BranchAddr && Ops[I].Opcode == NdOp::COND_BR &&
          Ops[I].NumInputs >= 2) {
        CondIndex = I;
        break;
      }
    }
    if (GuardBuildExhausted) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    if (CondIndex < 0)
      continue;

    std::set<BuildKey> Active;
    if (!consumeGuardBuildWork())
      return failIncomplete();
    std::function<GuardSyntaxPtr(const NdVar &, int, va_t, int, uint32_t)>
        Collect = [&](const NdVar &V, int Before, va_t UseAddr, int UseSeq,
                      uint32_t Depth) -> GuardSyntaxPtr {
      if (Depth > limits::kMaxJumpTableGuardExpressionDepth ||
          !consumeGuardBuildWork()) {
        GuardBuildExhausted = true;
        return {};
      }
      if (V.Size == 0 || V.Size > sizeof(uint64_t))
        return {};
      if (V.isConst()) {
        if (!consumeGuardBuildWork())
          return {};
        auto Node = std::make_shared<GuardSyntaxNode>();
        Node->Value = V;
        Node->UseAddr = UseAddr;
        Node->UseSeq = UseSeq;
        Node->IsConstant = true;
        return Node;
      }
      if (!V.isReg() && !V.isTemp())
        return {};

      BuildKey Key{static_cast<uint8_t>(V.Space), V.Offset, V.Size, UseAddr,
                   UseSeq};
      if (!consumeGuardBuildWork())
        return {};
      if (auto It = SyntaxMemo.find(Key); It != SyntaxMemo.end())
        return It->second;
      if (!consumeGuardBuildWork())
        return {};
      if (!Active.insert(Key).second)
        return {};
      auto Finish = [&](GuardSyntaxPtr Result) {
        Active.erase(Key);
        if (!consumeGuardBuildWork())
          return GuardSyntaxPtr{};
        SyntaxMemo.emplace(Key, Result);
        return Result;
      };

      if (!consumeGuardBuildWork())
        return Finish({});
      auto Node = std::make_shared<GuardSyntaxNode>();
      Node->Value = V;
      Node->UseAddr = UseAddr;
      Node->UseSeq = UseSeq;
      if (IndexAlternatives.size() ==
              std::numeric_limits<size_t>::max() ||
          !consumeGuardBuildWork(IndexAlternatives.size() + 1)) {
        return Finish({});
      }
      Node->IndexQuery = ProofQueries.size();
      ProofQueries.push_back({V, UseAddr, UseSeq, IndexAlternatives,
                              /*AllowZeroExtension=*/true,
                              /*AllowSignExtension=*/true});

      int DefIndex = -1;
      for (int I = std::min(Before, static_cast<int>(Ops.size()) - 1); I >= 0;
           --I) {
        if (!consumeGuardBuildWork())
          return Finish({});
        if (Ops[I].Output.Space == V.Space &&
            Ops[I].Output.Offset == V.Offset && Ops[I].Output.Size == V.Size) {
          DefIndex = I;
          break;
        }
      }
      if (DefIndex < 0)
        return Finish(Node);
      const LowOp &Def = Ops[DefIndex];
      auto Source = Insns.find(Def.Addr);
      if (Source != Insns.end() && Source->second.IsInstructionGuard) {
        // A predicated terminal effect is represented as unconditional
        // condition-building ops, then COND_BR to the skip edge, then the
        // guarded RETURN/branch/call.  Definitions after that split are not
        // must-execute producers; definitions before it are the guard itself
        // and are safe to symbolize.  Rejecting the whole InsnRecord loses
        // ordinary ARM `bxhi` range guards because BOOL_NOT/BOOL_AND live in
        // the same record as the guarded return.
        const auto GuardSplit = std::find_if(
            Source->second.Ops.begin(), Source->second.Ops.end(),
            [](const LowOp &Op) { return Op.Opcode == NdOp::COND_BR; });
        if (GuardSplit == Source->second.Ops.end() ||
            Def.Seq >= GuardSplit->Seq)
          return Finish(Node);
      }
      if (!supportedGuardOpcode(Def.Opcode))
        return Finish(Node);
      if (!consumeGuardBuildWork(/*query + one alternative=*/2))
        return Finish({});
      Node->HasDef = true;
      Node->Def = Def;
      Node->DefQuery = ProofQueries.size();
      ProofQueries.push_back({
          V,
          UseAddr,
          UseSeq,
          {{Def.Output, Def.Addr, Def.Seq, /*DefinedAtPoint=*/true}},
          /*AllowZeroExtension=*/false,
          /*AllowSignExtension=*/false,
      });
      for (int I = 0; I < Def.NumInputs; ++I) {
        // Charge the input scan independently from recursive collection.
        if (!consumeGuardBuildWork())
          return Finish({});
        GuardSyntaxPtr Input =
            Collect(Def.Inputs[I], DefIndex - 1, Def.Addr, Def.Seq, Depth + 1);
        if (!Input)
          return Finish({});
        if (!consumeGuardBuildWork())
          return Finish({});
        Node->Inputs.push_back(std::move(Input));
      }
      return Finish(Node);
    };

    const LowOp &Cond = Ops[CondIndex];
    GuardSyntaxPtr Root = Collect(Cond.Inputs[1], CondIndex - 1, Cond.Addr,
                                  Cond.Seq, /*Depth=*/0);
    if (GuardBuildExhausted) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    if (Root) {
      if (!consumeGuardBuildWork())
        return failIncomplete();
      GuardRoots.emplace_back(std::move(Root), *TableCondition);
    }
  }

  if (GuardBuildExhausted) {
    Info.IncompleteGuardDomain = true;
    return false;
  }

  // Resolve every syntax leaf and def edge in one CFG/lane query session.
  // Its ResolverFlowGraph, memo tables, and MatchBudget are shared across the
  // entire guard DAG; the outer evidence budget bounds collection and result
  // materialisation, so no node can restart a whole-function proof.
  bool ProofComplete = false;
  std::vector<bool> ProofResults;
  if (!ProofQueries.empty())
    ProofResults = tableValuesMatchAtUses(
        ProofQueries, &ProofComplete, nullptr, InvalidVA, nullptr,
        CandidateEvidenceBudget);
  if (!ProofComplete || ProofResults.size() != ProofQueries.size()) {
    if (SawControllingGuard)
      Info.IncompleteGuardDomain = true;
    return false;
  }

  std::map<const GuardSyntaxNode *, GuardExprPtr> ExprMemo;
  std::set<const GuardSyntaxNode *> ActiveExprs;
  if (!consumeGuardBuildWork())
    return failIncomplete();
  std::function<GuardExprPtr(const GuardSyntaxPtr &, uint32_t)> Materialize =
      [&](const GuardSyntaxPtr &Node, uint32_t Depth) -> GuardExprPtr {
    if (!Node || Depth > limits::kMaxJumpTableGuardExpressionDepth ||
        !consumeGuardBuildWork()) {
      GuardBuildExhausted = true;
      return {};
    }
    if (!consumeGuardBuildWork())
      return {};
    if (auto It = ExprMemo.find(Node.get()); It != ExprMemo.end())
      return It->second;
    if (!consumeGuardBuildWork())
      return {};
    if (!ActiveExprs.insert(Node.get()).second)
      return {};
    auto Finish = [&](GuardExprPtr Result) {
      ActiveExprs.erase(Node.get());
      if (!consumeGuardBuildWork())
        return GuardExprPtr{};
      ExprMemo.emplace(Node.get(), Result);
      return Result;
    };
    if (!consumeGuardBuildWork())
      return Finish({});
    auto Expr = std::make_shared<GuardExpr>();
    Expr->Size = Node->Value.Size;
    if (Node->IsConstant) {
      Expr->K = GuardExpr::Kind::Constant;
      Expr->Constant = Node->Value.Offset;
      return Finish(Expr);
    }
    if (Node->IndexQuery < ProofResults.size() &&
        ProofResults[Node->IndexQuery]) {
      Expr->K = GuardExpr::Kind::Index;
      Expr->ContainsIndex = true;
      return Finish(Expr);
    }
    // The lexical def is only syntax.  Its exact output must be the value
    // reaching this exact use in the shared lane-aware resolver; an EAX/W
    // write therefore blocks a stale older RAX/X definition here.
    if (!Node->HasDef || Node->DefQuery >= ProofResults.size() ||
        !ProofResults[Node->DefQuery])
      return Finish({});
    Expr->K = GuardExpr::Kind::Operation;
    Expr->Size = Node->Def.Output.Size;
    Expr->Opcode = Node->Def.Opcode;
    for (const GuardSyntaxPtr &InputNode : Node->Inputs) {
      // Charge both the child-edge scan and the retained expression edge.
      if (!consumeGuardBuildWork())
        return Finish({});
      GuardExprPtr Input = Materialize(InputNode, Depth + 1);
      if (!Input)
        return Finish({});
      if (!consumeGuardBuildWork())
        return Finish({});
      Expr->ContainsIndex |= Input->ContainsIndex;
      Expr->Inputs.push_back(std::move(Input));
    }
    return Finish(Expr);
  };

  std::vector<std::pair<GuardExprPtr, bool>> IndexGuards;
  if (!consumeGuardBuildWork(GuardRoots.size()))
    return failIncomplete();
  IndexGuards.reserve(GuardRoots.size());
  auto indexWidth = [&](const GuardExprPtr &Root) -> std::optional<uint16_t> {
    std::optional<uint16_t> Width;
    bool Conflict = false;
    if (!consumeGuardBuildWork())
      return std::nullopt;
    std::function<void(const GuardExprPtr &, uint32_t)> Visit =
        [&](const GuardExprPtr &Expr, uint32_t Depth) {
          if (!Expr || Conflict || GuardBuildExhausted)
            return;
          if (Depth > limits::kMaxJumpTableGuardExpressionDepth ||
              !consumeGuardBuildWork()) {
            GuardBuildExhausted = true;
            return;
          }
          if (Expr->K == GuardExpr::Kind::Index) {
            if (Width && *Width != Expr->Size)
              Conflict = true;
            else
              Width = Expr->Size;
            return;
          }
          for (const GuardExprPtr &Input : Expr->Inputs)
            Visit(Input, Depth + 1);
        };
    Visit(Root, 0);
    return Conflict || GuardBuildExhausted ? std::nullopt : Width;
  };
  struct MaterializedGuard {
    GuardExprPtr Expr;
    bool TableCondition = false;
    uint16_t IndexWidth = 0;
  };
  std::vector<MaterializedGuard> MaterializedGuards;
  if (!consumeGuardBuildWork(GuardRoots.size()))
    return failIncomplete();
  MaterializedGuards.reserve(GuardRoots.size());
  for (const auto &[Root, TableCondition] : GuardRoots) {
    if (!consumeGuardBuildWork())
      return failIncomplete();
    GuardExprPtr Expr = Materialize(Root, /*Depth=*/0);
    if (GuardBuildExhausted) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    // Record the exact source-lane width of this constraint.  Constraints over
    // different views cannot share one unconstrained symbolic variable; the
    // coherent view used for the proof is selected below.
    const std::optional<uint16_t> Width = indexWidth(Expr);
    if (GuardBuildExhausted)
      return failIncomplete();
    if (Expr && Expr->ContainsIndex && Width && *Width != 0 &&
        *Width <= Info.IndexValueAtUse.Size) {
      if (!consumeGuardBuildWork())
        return failIncomplete();
      MaterializedGuards.push_back({std::move(Expr), TableCondition, *Width});
    }
  }

  // Prefer constraints written directly over the table-address coordinate.
  // When none exist, a guard over a narrower source lane is still usable only
  // because the batched reaching-value query above proved that exact lane is
  // zero- or sign-extended into the runtime index.  Keep one coherent source
  // width; ignoring guards over other views merely enlarges the proven domain
  // and is therefore conservative.  A later bound check excludes the part of
  // a narrow signed lane where sign- and zero-extension no longer agree.
  uint16_t SelectedIndexWidth = Info.IndexValueAtUse.Size;
  bool HasCanonicalGuard = false;
  for (const MaterializedGuard &Guard : MaterializedGuards) {
    if (!consumeGuardBuildWork())
      return failIncomplete();
    if (Guard.IndexWidth == Info.IndexValueAtUse.Size) {
      HasCanonicalGuard = true;
      break;
    }
  }
  if (!HasCanonicalGuard && !MaterializedGuards.empty())
    SelectedIndexWidth = MaterializedGuards.front().IndexWidth;
  for (MaterializedGuard &Guard : MaterializedGuards) {
    if (!consumeGuardBuildWork())
      return failIncomplete();
    if (Guard.IndexWidth == SelectedIndexWidth) {
      if (!consumeGuardBuildWork())
        return failIncomplete();
      IndexGuards.emplace_back(std::move(Guard.Expr), Guard.TableCondition);
    }
  }

  if (IndexGuards.empty()) {
    if (SawControllingGuard)
      Info.IncompleteGuardDomain = true;
    return false;
  }

  // All dominating table-reaching guards constrain the same runtime path.
  // Prove their conjunction, not each comparison in isolation: a signed
  // upper bound is sound only when a separate dominating guard excludes the
  // negative half-domain, while either comparison alone is not a dense
  // zero-based table domain.
  if (!consumeGuardBuildWork())
    return failIncomplete();
  GuardSymbolization Symbolic;
  std::vector<SymRef> SymbolicConstraints;
  if (!consumeGuardBuildWork(IndexGuards.size()))
    return failIncomplete();
  SymbolicConstraints.reserve(IndexGuards.size());
  if (!consumeGuardBuildWork())
    return failIncomplete();
  std::function<std::optional<size_t>(const GuardExprPtr &, uint32_t)>
      symbolicGuardWork = [&](const GuardExprPtr &Expr,
                              uint32_t Depth) -> std::optional<size_t> {
    if (!Expr || Depth > limits::kMaxJumpTableGuardExpressionDepth ||
        !consumeGuardBuildWork())
      return std::nullopt;
    // One symbolic expression node plus its retained input vector.  Integer
    // symbolization can add coercions and, for carry/overflow, a small fixed
    // expansion; four units per input plus twelve covers every opcode above.
    size_t Work = Expr->K == GuardExpr::Kind::Operation ? 12 : 2;
    if (Expr->Inputs.size() >
        (std::numeric_limits<size_t>::max() - Work) / 4)
      return std::nullopt;
    Work += Expr->Inputs.size() * 4;
    for (const GuardExprPtr &Input : Expr->Inputs) {
      std::optional<size_t> Child = symbolicGuardWork(Input, Depth + 1);
      if (!Child || *Child > std::numeric_limits<size_t>::max() - Work)
        return std::nullopt;
      Work += *Child;
    }
    return Work;
  };
  for (const auto &[Expr, TableCondition] : IndexGuards) {
    std::optional<size_t> SymbolWork = symbolicGuardWork(Expr, 0);
    if (!SymbolWork || !consumeGuardBuildWork(*SymbolWork))
      return failIncomplete();
    std::optional<SymRef> Condition = symbolizeGuardExpr(Expr, Symbolic);
    if (!Condition) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
    if (!consumeGuardBuildWork(/*zero + ne + optional not + retention=*/4))
      return failIncomplete();
    SymRef NonZero = Symbolic.Ctx.mkNe(
        *Condition, Symbolic.Ctx.mkZero(Symbolic.Ctx.width(*Condition)));
    SymbolicConstraints.push_back(TableCondition ? NonZero
                                                 : Symbolic.Ctx.mkNot(NonZero));
  }
  if (!Symbolic.Index || Symbolic.IndexSize == 0 ||
      Symbolic.IndexSize > sizeof(uint64_t)) {
    Info.IncompleteGuardDomain = true;
    return false;
  }
  SymRef ReachesTable;
  if (SymbolicConstraints.size() == 1) {
    ReachesTable = SymbolicConstraints.front();
  } else {
    if (!consumeGuardBuildWork(SymbolicConstraints.size() + 1))
      return failIncomplete();
    ReachesTable = Symbolic.Ctx.mkAnd(SymbolicConstraints);
  }

  uint64_t LastSample = limits::kMaxJumpTableEntries;
  const unsigned IndexBits = unsigned(Symbolic.IndexSize) * 8u;
  if (IndexBits < 64)
    LastSample = std::min<uint64_t>(LastSample, (uint64_t(1) << IndexBits) - 1);

  if (!consumeGuardBuildWork())
    return failIncomplete();
  std::function<std::optional<size_t>(const GuardExprPtr &, uint32_t)>
      concreteGuardWork = [&](const GuardExprPtr &Expr,
                              uint32_t Depth) -> std::optional<size_t> {
    if (!Expr || Depth > limits::kMaxJumpTableGuardExpressionDepth ||
        !consumeGuardBuildWork())
      return std::nullopt;
    // Each replay visits the node and materializes its input values; primitive
    // comparisons additionally materialize the parallel input-width vector.
    size_t Work = 1;
    if (Expr->Inputs.size() >
        (std::numeric_limits<size_t>::max() - Work) / 3)
      return std::nullopt;
    Work += Expr->Inputs.size() * 3;
    for (const GuardExprPtr &Input : Expr->Inputs) {
      std::optional<size_t> Child = concreteGuardWork(Input, Depth + 1);
      if (!Child || *Child > std::numeric_limits<size_t>::max() - Work)
        return std::nullopt;
      Work += *Child;
    }
    return Work;
  };
  size_t ConcreteWorkPerValue = 0;
  for (const auto &[Expr, TableCondition] : IndexGuards) {
    (void)TableCondition;
    std::optional<size_t> GuardWork = concreteGuardWork(Expr, 0);
    if (!GuardWork ||
        *GuardWork > std::numeric_limits<size_t>::max() -
                         ConcreteWorkPerValue)
      return failIncomplete();
    ConcreteWorkPerValue += *GuardWork;
  }
  if (!consumeCandidateProduct(ConcreteWorkPerValue,
                               static_cast<size_t>(LastSample) + 1))
    return failIncomplete();

  uint32_t FirstRejected = 0;
  bool SawRejected = false;
  for (uint64_t Value = 0; Value <= LastSample; ++Value) {
    bool Reaches = true;
    for (const auto &[Expr, TableCondition] : IndexGuards) {
      std::optional<uint64_t> Condition = evaluateGuardExpr(Expr, Value);
      if (!Condition) {
        Info.IncompleteGuardDomain = true;
        return false;
      }
      Reaches &= ((*Condition != 0) == TableCondition);
    }
    if (!SawRejected && !Reaches) {
      FirstRejected = static_cast<uint32_t>(Value);
      SawRejected = true;
    } else if (SawRejected && Reaches) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
  }
  if (!SawRejected || FirstRejected < limits::kMinJumpTableEntries ||
      FirstRejected > limits::kMaxJumpTableEntries) {
    if (SawControllingGuard)
      Info.IncompleteGuardDomain = true;
    return false;
  }
  if (SelectedIndexWidth < Info.IndexValueAtUse.Size) {
    const unsigned SourceBits = unsigned(SelectedIndexWidth) * 8u;
    if (SourceBits == 0 ||
        (SourceBits < 64 &&
         FirstRejected > (uint64_t(1) << (SourceBits - 1)))) {
      Info.IncompleteGuardDomain = true;
      return false;
    }
  }
  constexpr size_t SolverProofWork =
      size_t(limits::kMaxJumpTableGuardSolverConflicts) +
      size_t(limits::kMaxJumpTableGuardSolverPropagations) +
      size_t(limits::kMaxJumpTableGuardSolverWatchVisits) +
      limits::kMaxJumpTableGuardSolverGates;
  if (!consumeGuardBuildWork(/*constant + ult + xor=*/3) ||
      !consumeCandidateEvidence(SolverProofWork))
    return failIncomplete();
  if (!proveDenseGuardPrefix(Symbolic, ReachesTable, FirstRejected)) {
    Info.IncompleteGuardDomain = true;
    return false;
  }

  if (Info.MaxEntries == 0 || FirstRejected < Info.MaxEntries)
    Info.MaxEntries = FirstRejected;
  Info.IncompleteGuardDomain = false;
  LLVM_DEBUG(llvm::dbgs() << "  precise-guard: bound " << FirstRejected
                          << " from CFG/value/polarity evidence\n");
  return true;
}

//===----------------------------------------------------------------------===//
// guardUsesInclusiveCompare — COND_BR-polarity-aware off-by-one recovery
//===----------------------------------------------------------------------===//

/// clang lowers `idx > N -> default` as `cmp idx,N; ja default`, so the table
/// covers idx in [0, N] = N+1 entries.  The lifted CF flag is `idx < N`, which
/// the range analysis reports as only N.  The strict `ja`/`jbe` family also
/// consumes the ZF equality `idx == N`; the `jae`/`jb` family consumes only CF.
/// Return true when the guarding COND_BR transitively consumes both, so the
/// inclusive upper bound is Bound+1.
bool CFGBuilder::guardUsesInclusiveCompare(const InsnRecord &Rec,
                                           const JumpTableInfo &Info,
                                           uint64_t Bound) const {
  std::vector<LowOp> Ops;
  for (auto &[A, IR] : Insns) {
    if (A > Rec.Addr)
      break;
    for (auto &Op : IR.Ops)
      Ops.push_back(Op);
  }

  if ((!Info.IndexValueAtUse.isReg() && !Info.IndexValueAtUse.isTemp()) ||
      Info.IndexValueAtUse.Size == 0 || Info.IndexUseAddr == InvalidVA ||
      Info.IndexUseSeq < 0 || Info.TableLoadAddr == InvalidVA)
    return false;

  auto reachingDef = [&](const NdVar &V, int Before) -> int {
    return reachingDefIdx(Ops, Before, V);
  };
  auto isIndex = [&](const NdVar &V, int Before, bool AllowZero,
                     bool AllowSign) -> bool {
    int UseIdx = Before + 1;
    if (UseIdx < 0 || UseIdx >= static_cast<int>(Ops.size()))
      return false;
    return tableIndexMatchesValueAtUse(V, Ops[UseIdx].Addr, Ops[UseIdx].Seq,
                                       Info, AllowZero, AllowSign);
  };
  auto isLessBound = [&](const LowOp &Op, int At) -> bool {
    if ((Op.Opcode != NdOp::INT_LESS && Op.Opcode != NdOp::INT_SLESS) ||
        Op.NumInputs < 2)
      return false;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, At - 1, Op.Inputs[1], C))
      return false;
    const bool Signed = Op.Opcode == NdOp::INT_SLESS;
    return C == Bound && isIndex(Op.Inputs[0], At - 1, !Signed, Signed);
  };
  auto isEqualBound = [&](const LowOp &Op, int At) -> bool {
    if (Op.Opcode != NdOp::INT_EQUAL || Op.NumInputs < 2 ||
        !Op.Inputs[1].isConst())
      return false;
    if (Op.Inputs[1].Offset == Bound &&
        isIndex(Op.Inputs[0], At - 1, true, true))
      return true;
    // ZF of `cmp idx,Bound` is `(idx - Bound) == 0`; the subtraction result may
    // live in a temp or, on x86 where `cmp` overwrites the register, a
    // register.
    if (Op.Inputs[1].Offset == 0 &&
        (Op.Inputs[0].isTemp() || Op.Inputs[0].isReg())) {
      int D = reachingDef(Op.Inputs[0], At - 1);
      if (D >= 0 && Ops[D].Opcode == NdOp::INT_SUB && Ops[D].NumInputs >= 2 &&
          Ops[D].Inputs[1].isConst() && Ops[D].Inputs[1].Offset == Bound &&
          isIndex(Ops[D].Inputs[0], D - 1, true, true))
        return true;
    }
    return false;
  };
  auto isSignedFlagRelationBound = [&](const LowOp &Op, int At) -> bool {
    // x86 signed Jcc does not consume a direct `idx < bound` boolean.  CMP
    // publishes SF from `(idx-bound)<0` and OF from signed subtraction
    // overflow; JG/JLE then compares SF with OF and also consumes ZF.  Tie the
    // two flags back to the same exact index/bound pair so the equality bit can
    // safely distinguish the inclusive signed family without falling back to
    // physical flag/register identity.
    if ((Op.Opcode != NdOp::INT_EQUAL && Op.Opcode != NdOp::INT_NOTEQUAL) ||
        Op.NumInputs < 2)
      return false;
    for (int SignSide = 0; SignSide < 2; ++SignSide) {
      int SignDef = reachingDef(Op.Inputs[SignSide], At - 1);
      int OverflowDef = reachingDef(Op.Inputs[1 - SignSide], At - 1);
      if (SignDef < 0 || OverflowDef < 0)
        continue;
      const LowOp &Sign = Ops[SignDef];
      const LowOp &Overflow = Ops[OverflowDef];
      if (Sign.Opcode != NdOp::INT_SLESS || Sign.NumInputs < 2 ||
          !Sign.Inputs[1].isConst() || Sign.Inputs[1].Offset != 0 ||
          Overflow.Opcode != NdOp::INT_SBOR || Overflow.NumInputs < 2 ||
          !Overflow.Inputs[1].isConst() || Overflow.Inputs[1].Offset != Bound ||
          !isIndex(Overflow.Inputs[0], OverflowDef - 1,
                   /*AllowZero=*/false, /*AllowSign=*/true))
        continue;
      int SubDef = reachingDef(Sign.Inputs[0], SignDef - 1);
      if (SubDef < 0 || Ops[SubDef].Opcode != NdOp::INT_SUB ||
          Ops[SubDef].NumInputs < 2 || !Ops[SubDef].Inputs[1].isConst() ||
          Ops[SubDef].Inputs[1].Offset != Bound ||
          !isIndex(Ops[SubDef].Inputs[0], SubDef - 1,
                   /*AllowZero=*/false, /*AllowSign=*/true))
        continue;
      return true;
    }
    return false;
  };

  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Addr >= Rec.Addr || Ops[I].Opcode != NdOp::COND_BR ||
        Ops[I].NumInputs < 2)
      continue;
    if (!branchControlsTableLoad(Ops[I].Addr, Info))
      continue;
    bool SawLess = false, SawEqual = false;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> SeenDefs;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = reachingDef(V, Before);
      // Dedup by definition site, not nd-var identity: a temp offset may be
      // reused for distinct defs (ARM flag chains), each needing its own walk.
      if (D < 0 || !SeenDefs.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      if (isLessBound(Def, D)) {
        SawLess = true;
        continue;
      }
      if (isEqualBound(Def, D)) {
        SawEqual = true;
        continue;
      }
      if (isSignedFlagRelationBound(Def, D)) {
        SawLess = true;
        continue;
      }
      switch (Def.Opcode) {
      case NdOp::BOOL_AND:
      case NdOp::BOOL_OR:
      case NdOp::BOOL_XOR:
      case NdOp::BOOL_NOT:
      case NdOp::COPY:
        for (int K = 0; K < Def.NumInputs; ++K)
          Work.push_back({Def.Inputs[K], D - 1});
        break;
      default:
        break;
      }
    }
    if (SawLess && SawEqual)
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// inferBoundsFromLoadAliasGuard — bound via same-location reload equivalence
//===----------------------------------------------------------------------===//

bool CFGBuilder::inferBoundsFromLoadAliasGuard(const InsnRecord &Rec,
                                               JumpTableInfo &Info) {
  if (Info.IndexReg == InvalidVA || !CurrentImg)
    return false;

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
  int VarSize = TRI.PointerSize;

  // Flatten the function prefix through the dispatch so the guard, its compared
  // reload, and the index's own reload are all visible to the backward walk.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);
  if (Ops.empty())
    return false;

  // A location key for the value feeding a register: either a fixed address
  // (Kind 0, read-only source) or a stack/frame slot (Kind 1).
  struct MemKey {
    int Kind = -1;
    uint64_t Addr = 0;
    uint64_t Base = 0;
    int64_t Off = 0;
    int LoadIdx = -1;
  };

  // Trace a value backward through value-preserving reshapes (copy / extend /
  // low-half subpiece) to the LOAD that produced it, and key that load's
  // address.  Returns nullopt if the value is not a plain reload.
  auto keyOfLoadFeeding = [&](NdVar V, int From) -> std::optional<MemKey> {
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        return std::nullopt;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::LOAD && O.NumInputs >= 1) {
        const NdVar &A = (O.NumInputs >= 2) ? O.Inputs[1] : O.Inputs[0];
        MemKey K;
        K.LoadIdx = D;
        if (A.isConst()) {
          K.Kind = 0;
          K.Addr = A.Offset;
          return K;
        }
        uint64_t B = InvalidVA;
        int64_t Off = 0;
        if (frameSlotKey(Ops, D - 1, A, TRI, B, Off)) {
          K.Kind = 1;
          K.Base = B;
          K.Off = Off;
          return K;
        }
        return std::nullopt;
      }
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
          (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      return std::nullopt;
    }
    return std::nullopt;
  };

  auto sameKey = [](const MemKey &A, const MemKey &B) {
    if (A.Kind != B.Kind)
      return false;
    return A.Kind == 0 ? (A.Addr == B.Addr)
                       : (A.Base == B.Base && A.Off == B.Off);
  };

  std::optional<MemKey> IdxKey = keyOfLoadFeeding(
      NdVar::reg(Info.IndexReg, VarSize), static_cast<int>(Ops.size()) - 1);
  if (!IdxKey)
    return false;

  // A fixed-address source is only a stable value if it lives in a non-writable
  // segment (a store could otherwise change it between the two reads); a frame
  // slot's stability is checked per-guard by the no-intervening-store test.
  if (IdxKey->Kind == 0) {
    const auto *Seg = CurrentImg->getSegmentFor(IdxKey->Addr);
    if (!Seg || Seg->isWritable() || CurrentImg->isCodeAddress(IdxKey->Addr))
      return false;
  }

  auto sameSlotStoreBetween = [&](int A, int B) -> bool {
    int Lo = std::min(A, B);
    int Hi = std::max(A, B);
    for (int I = Lo + 1; I < Hi; ++I) {
      const LowOp &S = Ops[I];
      if (S.Opcode != NdOp::STORE || S.NumInputs < 2)
        continue;
      uint64_t B2 = InvalidVA;
      int64_t Off2 = 0;
      if (frameSlotKey(Ops, I - 1, S.Inputs[0], TRI, B2, Off2) &&
          B2 == IdxKey->Base && Off2 == IdxKey->Off)
        return true;
    }
    return false;
  };

  // Only comparisons whose boolean reaches a conditional branch are guards; an
  // equality/range test buried in a case body is not a dispatch bound.  Mark
  // each range-compare op index whose result flows (through BOOL_*/COPY) into a
  // COND_BR condition.
  std::set<int> GuardCmp;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (Ops[I].Opcode != NdOp::COND_BR || Ops[I].NumInputs < 2)
      continue;
    std::vector<std::pair<NdVar, int>> Work{{Ops[I].Inputs[1], I - 1}};
    std::set<int> Seen;
    int Steps = 0;
    while (!Work.empty() && Steps++ < limits::kMaxGuardScanOps) {
      auto [V, Before] = Work.back();
      Work.pop_back();
      if (V.isConst())
        continue;
      int D = -1;
      for (int K = Before; K >= 0 && K < static_cast<int>(Ops.size()); --K)
        if (Ops[K].Output.Space == V.Space &&
            Ops[K].Output.Offset == V.Offset) {
          D = K;
          break;
        }
      if (D < 0 || !Seen.insert(D).second)
        continue;
      const LowOp &Def = Ops[D];
      switch (Def.Opcode) {
      case NdOp::INT_LESS:
      case NdOp::INT_SLESS:
      case NdOp::INT_LESSEQUAL:
      case NdOp::INT_SLESSEQUAL:
        GuardCmp.insert(D);
        break;
      case NdOp::BOOL_AND:
      case NdOp::BOOL_OR:
      case NdOp::BOOL_XOR:
      case NdOp::BOOL_NOT:
      case NdOp::COPY:
        for (int L = 0; L < Def.NumInputs; ++L)
          Work.push_back({Def.Inputs[L], D - 1});
        break;
      default:
        break;
      }
    }
  }
  if (GuardCmp.empty())
    return false;

  auto boundFromCmp = [&](int GI) -> uint32_t {
    const LowOp &Op = Ops[GI];
    if (Op.NumInputs < 2)
      return 0;
    uint64_t C;
    if (!resolveConstThroughCopy(Ops, GI - 1, Op.Inputs[1], C))
      return 0;
    uint64_t Bound;
    switch (Op.Opcode) {
    case NdOp::INT_LESS:
    case NdOp::INT_SLESS:
      Bound = C;
      break;
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESSEQUAL:
      Bound = C + 1;
      break;
    default:
      return 0;
    }
    if (Bound < limits::kMinJumpTableEntries ||
        Bound > limits::kMaxJumpTableEntries)
      return 0;
    return static_cast<uint32_t>(Bound);
  };

  uint32_t Best = 0;
  for (int GI : GuardCmp) {
    const LowOp &Cmp = Ops[GI];
    if (Cmp.NumInputs < 1 ||
        (!Cmp.Inputs[0].isReg() && !Cmp.Inputs[0].isTemp()))
      continue;
    std::optional<MemKey> GKey = keyOfLoadFeeding(Cmp.Inputs[0], GI - 1);
    if (!GKey || !sameKey(*GKey, *IdxKey))
      continue;
    // A store to the shared frame slot between the two reloads breaks the value
    // equivalence, so the guard no longer bounds the index.
    if (IdxKey->Kind == 1 &&
        sameSlotStoreBetween(GKey->LoadIdx, IdxKey->LoadIdx))
      continue;
    uint32_t Bnd = boundFromCmp(GI);
    if (Bnd == 0)
      continue;
    if (Best == 0 || Bnd < Best)
      Best = Bnd;
  }

  if (Best == 0)
    return false;
  if (Info.MaxEntries == 0 || Best < Info.MaxEntries) {
    Info.MaxEntries = Best;
    LLVM_DEBUG(llvm::dbgs() << "  load-alias-guard: bound " << Best
                            << " from same-location reload of the index\n");
    return true;
  }
  return false;
}

} // namespace neverd
