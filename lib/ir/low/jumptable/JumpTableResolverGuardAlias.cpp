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

#include "GuardExpression.h"
#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <bit>
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

using guard_detail::guardChangePoints;
using guard_detail::GuardExpr;
using guard_detail::GuardExprPtr;

uint64_t widthMask(uint16_t Bytes) {
  return Bytes == 0 || Bytes >= sizeof(uint64_t)
             ? std::numeric_limits<uint64_t>::max()
             : (uint64_t(1) << (unsigned(Bytes) * 8u)) - 1;
}

uint64_t truncateToWidth(uint64_t Value, uint16_t Bytes) {
  return Value & widthMask(Bytes);
}

// A table immediate is a scalar. An address-owned encoding is a different
// value and must not become a mask, a shift, or an XOR constant.
std::optional<uint64_t> scalarConstantBits(const NdVar &Value) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t))
    return std::nullopt;
  if (Value.AddressOwnerVA != InvalidVA ||
      (Value.Provenance != ConstantAddressProvenance::Unknown &&
       Value.Provenance != ConstantAddressProvenance::Scalar))
    return std::nullopt;
  return truncateToWidth(Value.Offset, Value.Size);
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
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
    if (Inputs.size() != 2)
      return std::nullopt;
    if (Opcode == NdOp::BOOL_AND)
      return input(0) != 0 && input(1) != 0;
    if (Opcode == NdOp::BOOL_OR)
      return input(0) != 0 || input(1) != 0;
    return (input(0) != 0) != (input(1) != 0);
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

std::optional<uint64_t> evaluateJumpTableGuardPartialPrimitive(
    NdOp Opcode, uint16_t OutputSize,
    llvm::ArrayRef<std::optional<uint64_t>> Inputs,
    llvm::ArrayRef<uint16_t> InputSizes) {
  if (OutputSize == 0 || OutputSize > sizeof(uint64_t) ||
      Inputs.size() != InputSizes.size() || Inputs.empty() ||
      std::any_of(InputSizes.begin(), InputSizes.end(), [](uint16_t Size) {
        return Size == 0 || Size > sizeof(uint64_t);
      }))
    return std::nullopt;
  if (Inputs.size() == 2 &&
      (Opcode == NdOp::BOOL_AND || Opcode == NdOp::BOOL_OR))
    for (unsigned I = 0; I != 2; ++I)
      if (Inputs[I]) {
        const bool Truth = truncateToWidth(*Inputs[I], InputSizes[I]) != 0;
        if (Truth == (Opcode == NdOp::BOOL_OR))
          return Truth;
      }
  std::vector<uint64_t> Known;
  for (const auto &Input : Inputs) {
    if (!Input)
      return std::nullopt;
    Known.push_back(*Input);
  }
  return evaluateJumpTableGuardPrimitive(Opcode, OutputSize, Known, InputSizes);
}

namespace {

using symbolic::SymContext;
using symbolic::SymRef;

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

enum class DenseGuardPrefixProof : uint8_t {
  Proven,
  Counterexample,
  Incomplete,
};

DenseGuardPrefixProof proveDenseGuardPrefix(GuardSymbolization &State,
                                            SymRef ReachesTable,
                                            uint32_t Prefix,
                                            bool UpperBoundOnly = false) {
  if (!State.Index || !ReachesTable || State.Ctx.width(ReachesTable) != 1 ||
      Prefix < limits::kMinJumpTableEntries ||
      Prefix > limits::kMaxJumpTableEntries)
    return DenseGuardPrefixProof::Incomplete;
  const uint32_t IndexWidth = State.Ctx.width(State.Index);
  SymRef Expected = State.Ctx.mkUlt(
      State.Index, State.Ctx.mkConst(IndexWidth, uint64_t(Prefix)));
  // A sparse domain needs only the upper bound: no value at or past Prefix
  // reaches the table.  A dense one must also reach every value below it.
  SymRef Counterexample =
      UpperBoundOnly ? State.Ctx.mkAnd(ReachesTable, State.Ctx.mkNot(Expected))
                     : State.Ctx.mkXor(ReachesTable, Expected);

  solver::SolverOptions Options;
  Options.BuildModel = false;
  Options.Sat.MaxConflicts = limits::kMaxJumpTableGuardSolverConflicts;
  Options.Sat.MaxPropagations = limits::kMaxJumpTableGuardSolverPropagations;
  Options.Sat.MaxWatchVisits = limits::kMaxJumpTableGuardSolverWatchVisits;
  Options.Blast.MaxWidth = 64;
  Options.Blast.MaxGates = limits::kMaxJumpTableGuardSolverGates;
  switch (solver::checkSat(State.Ctx, Counterexample, nullptr, Options)) {
  case solver::SatResult::Unsat:
    return DenseGuardPrefixProof::Proven;
  case solver::SatResult::Sat:
    return DenseGuardPrefixProof::Counterexample;
  case solver::SatResult::Unknown:
  case solver::SatResult::Invalid:
    return DenseGuardPrefixProof::Incomplete;
  }
  return DenseGuardPrefixProof::Incomplete;
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
    const InsnRecord &Rec, JumpTableInfo &Info, size_t *CandidateEvidenceBudget,
    bool UseDefinedAlternativesAsRoots,
    const std::map<va_t, std::vector<va_t>> *CertifiedEdgeOverrides,
    bool AllowSparseDomain) const {
  Info.IncompleteGuardDomain = false;
  Info.SemanticGuardDomainAmbiguous = false;
  if ((!Info.IndexValueAtUse.isReg() && !Info.IndexValueAtUse.isTemp()) ||
      Info.IndexUseAddr == InvalidVA || Info.IndexUseSeq < 0 ||
      Info.TableLoadAddr == InvalidVA)
    return false;
  if (Info.IndexValueAtUse.Size == 0 ||
      Info.IndexValueAtUse.Size > sizeof(uint64_t)) {
    Info.SemanticGuardDomainAmbiguous = true;
    return false;
  }
  if (!CandidateEvidenceBudget) {
    Info.IncompleteGuardDomain = true;
    return false;
  }

  // This proof is const, so the builder cannot change while it runs: its value
  // queries validate the instruction snapshot once and share that graph.
  ResolverQueryScope QueryScope(*this);

  // Literals propose a small finite query envelope; they do not prove any
  // index bound. The all-path value query must authenticate every coordinate.
  uint64_t ProposedJoinedBound = 0;
  bool SawControllingGuard = false;
  bool GuardBuildExhausted = false;
  bool GuardExpressionAllowanceExtended = false;
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
  // An optional reconstruction may look and then decline. Spending the last
  // unit must not erase the budget the compare proof still needs.
  auto tryCandidateEvidence = [&](size_t Amount = 1) {
    if (Amount == 0 || Amount > *CandidateEvidenceBudget)
      return false;
    return consumeCandidateEvidence(Amount);
  };
  auto consumeCandidateProduct = [&](size_t Left, size_t Right,
                                     size_t Extra = 0) {
    if (Left != 0 &&
        Right > (std::numeric_limits<size_t>::max() - Extra) / Left) {
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
  auto failSemantic = [&]() {
    Info.SemanticGuardDomainAmbiguous = true;
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
    bool InputsComplete = true;
    std::vector<GuardSyntaxPtr> Inputs;
    size_t IndexQuery = std::numeric_limits<size_t>::max();
    /// The compare reads a register whose later table use is that same lane,
    /// or the lifter's zero-extend of it. The guard branch's conditional
    /// fallthrough is the use block's only incoming edge. The value query
    /// would otherwise resolve every origin of the register, including a
    /// chain longer than the resolver depth, and fail closed on a proved
    /// guard.
    bool StructuralIndex = false;
    /// This node is the structural index leaf, or an ancestor of one.
    /// Materialize builds that compare before unrelated guards. The flag
    /// is not an index identity: only `StructuralIndex` collapses a leaf.
    bool ContainsStructuralIndex = false;
    size_t DefQuery = std::numeric_limits<size_t>::max();
    size_t ExtendedLaneQuery = std::numeric_limits<size_t>::max();
    size_t LaneSourceIndexQuery = std::numeric_limits<size_t>::max();
    size_t LaneSourceDefQuery = std::numeric_limits<size_t>::max();
    bool IndexQueryUsesDefinition = false;
    size_t LiteralQuery = std::numeric_limits<size_t>::max();
    NdVar Literal;
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
  std::vector<std::pair<GuardSyntaxPtr, JumpTableValueQuery>>
      ExtendedLaneQueries;
  // A lane leaf's index and definition identities rooted at the narrower value
  // its complete writer extends; see extendedLaneSource below.
  struct LaneSourceFallback {
    GuardSyntaxPtr Node;
    JumpTableValueQuery Index;
    JumpTableValueQuery Def;
  };
  std::vector<LaneSourceFallback> LaneSourceQueries;
  std::vector<std::pair<GuardSyntaxPtr, bool>> GuardRoots;

  // GuardExpr constants are numeric values after relinking. An address with
  // the same source bits cannot supply a numeric range, even when its exact
  // store/reload identity is known.
  auto isGuardLiteral = [](const NdVar &Value) {
    return Value.isConst() &&
           (Value.Provenance == ConstantAddressProvenance::Unknown ||
            Value.Provenance == ConstantAddressProvenance::Scalar) &&
           Value.AddressOwnerVA == InvalidVA;
  };

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
    if (!BranchRec.IsCond || !BranchRec.IsBranch)
      continue;
    if (!consumeCandidateEvidence())
      return failIncomplete();
    GuardBranchAddrs.push_back(BranchAddr);
  }
  bool ControlAnalysisComplete = false;
  const std::vector<std::optional<bool>> TableConditions =
      tableLoadConditionValues(GuardBranchAddrs, Info, &ControlAnalysisComplete,
                               CandidateEvidenceBudget, CertifiedEdgeOverrides);
  if (!ControlAnalysisComplete ||
      TableConditions.size() != GuardBranchAddrs.size()) {
    if (!GuardBranchAddrs.empty()) {
      Info.HasControllingGuard = true;
      Info.IncompleteGuardDomain = true;
    }
    return false;
  }

  // `ubfx`/`and` with a dense low-bit mask bounds the index no matter what
  // the other operand is. Resolving that operand walks a chain longer than
  // the resolver depth and fails closed on a proved table. Use the mask only
  // when no controlling compare reads the index lane; a compare can be
  // tighter, and this block-local proof must not replace it. A compare of
  // an architectural register that a later full write replaces, without
  // reading that register, is a different value and does not read the index.
  auto localDenseMaskBound = [&]() -> std::optional<uint32_t> {
    // A block-local AND is the AArch64 dense prefix. On other architectures
    // the same bits are not that certificate: an x86 PIC-relative proposal
    // still has to close reentry, sibling rollback, and the runtime
    // certificate. Authenticating the AND there skips that fixed-point.
    if (!CurrentImg || CurrentImg->Arch != Arch::AArch64 ||
        Info.IndexUseAddr == InvalidVA || Info.IndexUseSeq < 0)
      return std::nullopt;
    const NdVar &Index = Info.IndexValueAtUse;
    if ((!Index.isReg() && !Index.isTemp()) || Index.Size == 0)
      return std::nullopt;
    const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
    auto blockOf = [&](va_t Addr) -> std::optional<va_t> {
      auto It = BlockStarts.upper_bound(Addr);
      if (It == BlockStarts.begin())
        return std::nullopt;
      --It;
      return *It;
    };
    auto overlaps = [&](const NdVar &A, const NdVar &B) {
      if (A.Size == 0 || B.Size == 0)
        return false;
      if (A.isReg() && B.isReg()) {
        if (A.Offset == B.Offset)
          return true;
        return TRI.isSubRegOf(A.Offset, A.Size, B.Offset, B.Size) ||
               TRI.isSubRegOf(B.Offset, B.Size, A.Offset, A.Size);
      }
      if (!A.isTemp() || !B.isTemp() || A.Space != B.Space)
        return false;
      if (A.Offset > InvalidVA - A.Size || B.Offset > InvalidVA - B.Size)
        return true;
      return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
    };
    auto sameVar = [](const NdVar &A, const NdVar &B) {
      return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
    };
    const auto IndexBlock = blockOf(Info.IndexUseAddr);
    if (!IndexBlock)
      return std::nullopt;

    // The nearest full write of the index register in the use block is the
    // value that reaches the load. When that write does not read the register,
    // an earlier compare of the same architectural register is a different
    // value. It must not hide the mask. A write that reads the register, a
    // partial write, and a compare after the write still read the lane.
    const LowOp *IndependentKill = nullptr;
    for (auto It = Insns.upper_bound(Info.IndexUseAddr); It != Insns.begin();) {
      if (!tryCandidateEvidence())
        break;
      --It;
      if (It->first < *IndexBlock)
        break;
      bool Stop = false;
      for (auto OpIt = It->second.Ops.rbegin(); OpIt != It->second.Ops.rend();
           ++OpIt) {
        if (!tryCandidateEvidence()) {
          Stop = true;
          break;
        }
        const LowOp &Op = *OpIt;
        if (Op.Addr > Info.IndexUseAddr ||
            (Op.Addr == Info.IndexUseAddr && Op.Seq >= Info.IndexUseSeq))
          continue;
        if (!overlaps(Op.Output, Index))
          continue;
        Stop = true;
        bool ReadsLane = Op.NumInputs > std::size(Op.Inputs);
        if (!ReadsLane) {
          for (int Input = 0; Input < Op.NumInputs; ++Input) {
            if (overlaps(Op.Inputs[Input], Index))
              ReadsLane = true;
          }
        }
        if (sameVar(Op.Output, Index) && !ReadsLane &&
            !It->second.IsInstructionGuard && Op.Opcode != NdOp::CALL &&
            Op.Opcode != NdOp::INDIR_CALL)
          IndependentKill = &Op;
        break;
      }
      if (Stop)
        break;
    }

    auto guardReadsIndex = [&]() {
      for (size_t I = 0; I < GuardBranchAddrs.size(); ++I) {
        if (!TableConditions[I])
          continue;
        if (!consumeCandidateEvidence())
          return true;
        const va_t BranchAddr = GuardBranchAddrs[I];
        const auto GuardBlock = blockOf(BranchAddr);
        if (!GuardBlock)
          return true;
        std::vector<LowOp> Ops;
        for (auto It = Insns.lower_bound(*GuardBlock);
             It != Insns.end() && It->first <= BranchAddr; ++It) {
          if (!consumeCandidateEvidence())
            return true;
          const auto OpBlock = blockOf(It->first);
          if (!OpBlock || *OpBlock != *GuardBlock)
            break;
          for (const LowOp &Op : It->second.Ops) {
            if (!consumeCandidateEvidence())
              return true;
            Ops.push_back(Op);
          }
        }
        int CondIndex = -1;
        for (int OpI = static_cast<int>(Ops.size()) - 1; OpI >= 0; --OpI) {
          if (Ops[OpI].Addr == BranchAddr && Ops[OpI].Opcode == NdOp::COND_BR &&
              Ops[OpI].NumInputs >= 2) {
            CondIndex = OpI;
            break;
          }
        }
        if (CondIndex < 0)
          return true;
        // A later full write replaces an earlier value of the same register.
        // The compare still bounds the index when that write repeats the
        // compare's AND, or the same unsigned field `(src >> lsb) & mask`,
        // from the same unmodified source. A different shift or a write of
        // the source is a different field. An unfinished check keeps the
        // compare.
        auto sameMaskRecompute = [&](const NdVar &Value, int Before) -> bool {
          if (!IndependentKill || IndependentKill->Opcode != NdOp::INT_AND ||
              IndependentKill->NumInputs != 2)
            return false;
          auto exactAt = [&](const NdVar &Want, int From) -> int {
            if ((!Want.isReg() && !Want.isTemp()) || Want.Size == 0)
              return -1;
            for (int I = std::min(From, static_cast<int>(Ops.size()) - 1);
                 I >= 0; --I) {
              if (!tryCandidateEvidence())
                return -2;
              const NdVar &Out = Ops[I].Output;
              if (Out.Space == Want.Space && Out.Offset == Want.Offset &&
                  Out.Size == Want.Size)
                return I;
            }
            return -1;
          };
          struct Field {
            const NdVar *Source = nullptr;
            uint64_t Shift = 0;
            uint64_t Mask = 0;
            va_t AndAddr = InvalidVA;
            int AndSeq = -1;
          };
          auto parseAnd = [&](const LowOp &And,
                              const auto &FindDef) -> std::optional<Field> {
            if (And.Opcode != NdOp::INT_AND || And.NumInputs != 2)
              return std::nullopt;
            const bool Left = And.Inputs[0].isConst();
            const bool Right = And.Inputs[1].isConst();
            if (Left == Right)
              return std::nullopt;
            const NdVar &Mask = And.Inputs[Left ? 0 : 1];
            const NdVar &Dynamic = And.Inputs[Left ? 1 : 0];
            const std::optional<uint64_t> Bits = scalarConstantBits(Mask);
            if (!Bits || *Bits == 0)
              return std::nullopt;
            Field Parsed;
            Parsed.Mask = *Bits;
            Parsed.AndAddr = And.Addr;
            Parsed.AndSeq = And.Seq;
            if (Dynamic.isReg()) {
              Parsed.Source = &Dynamic;
              return Parsed;
            }
            if (!Dynamic.isTemp())
              return std::nullopt;
            const LowOp *Shift = FindDef(Dynamic, And.Addr, And.Seq);
            if (!Shift)
              return std::nullopt;
            if (Shift->Opcode != NdOp::INT_RIGHT || Shift->NumInputs < 2 ||
                Shift->NumInputs > std::size(Shift->Inputs) ||
                !Shift->Inputs[0].isReg())
              return std::nullopt;
            const std::optional<uint64_t> ShiftBits =
                scalarConstantBits(Shift->Inputs[1]);
            if (!ShiftBits || *ShiftBits == 0 || *ShiftBits >= 64)
              return std::nullopt;
            Parsed.Source = &Shift->Inputs[0];
            Parsed.Shift = *ShiftBits;
            return Parsed;
          };
          bool BudgetMiss = false;
          auto findInOps = [&](const NdVar &Want, va_t Addr,
                               int Seq) -> const LowOp * {
            int At = exactAt(Want, static_cast<int>(Ops.size()) - 1);
            if (At == -2) {
              BudgetMiss = true;
              return nullptr;
            }
            while (At >= 0) {
              const LowOp &Op = Ops[At];
              const bool BeforeAnd =
                  Op.Addr < Addr || (Op.Addr == Addr && Op.Seq < Seq);
              if (BeforeAnd)
                return &Op;
              At = exactAt(Want, At - 1);
              if (At == -2) {
                BudgetMiss = true;
                return nullptr;
              }
            }
            return nullptr;
          };
          auto findBeforeKill = [&](const NdVar &Want, va_t Addr,
                                    int Seq) -> const LowOp * {
            for (auto It = Insns.upper_bound(Addr); It != Insns.begin();) {
              if (!tryCandidateEvidence()) {
                BudgetMiss = true;
                return nullptr;
              }
              --It;
              const auto Block = blockOf(It->first);
              if (!Block || *Block != *IndexBlock)
                break;
              for (auto OpIt = It->second.Ops.rbegin();
                   OpIt != It->second.Ops.rend(); ++OpIt) {
                if (!tryCandidateEvidence()) {
                  BudgetMiss = true;
                  return nullptr;
                }
                const LowOp &Op = *OpIt;
                if (Op.Addr > Addr || (Op.Addr == Addr && Op.Seq >= Seq))
                  continue;
                if (Op.Output.Space == Want.Space &&
                    Op.Output.Offset == Want.Offset &&
                    Op.Output.Size == Want.Size)
                  return &Op;
              }
            }
            return nullptr;
          };
          int At = exactAt(Value, Before);
          if (At == -2)
            return true;
          NdVar Cur = Value;
          const LowOp *ComparedAnd = nullptr;
          for (unsigned Depth = 0; Depth < limits::kMaxQuasiCopyDepth;
               ++Depth) {
            if (!tryCandidateEvidence())
              return true;
            if (At < 0)
              return false;
            const LowOp &Op = Ops[At];
            if (Op.NumInputs < 1 || Op.NumInputs > std::size(Op.Inputs))
              return false;
            const NdVar &In = Op.Inputs[0];
            const bool Widen = Op.Opcode == NdOp::INT_ZEXT && In.isReg() &&
                               In.Offset == Cur.Offset && In.Size != 0 &&
                               In.Size < Cur.Size;
            const bool Copy = Op.Opcode == NdOp::COPY &&
                              (In.isReg() || In.isTemp()) &&
                              In.Size == Op.Output.Size;
            if (Widen || Copy) {
              Cur = In;
              At = exactAt(In, At - 1);
              if (At == -2)
                return true;
              continue;
            }
            if (Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2)
              return false;
            ComparedAnd = &Op;
            break;
          }
          if (!ComparedAnd)
            return false;
          const std::optional<Field> Compared =
              parseAnd(*ComparedAnd, findInOps);
          const std::optional<Field> Killed =
              parseAnd(*IndependentKill, findBeforeKill);
          if (BudgetMiss)
            return true;
          if (!Compared || !Killed || !Compared->Source || !Killed->Source)
            return false;
          if (Compared->Mask != Killed->Mask ||
              Compared->Shift != Killed->Shift)
            return false;
          const NdVar &Left = *Compared->Source;
          const NdVar &Right = *Killed->Source;
          if (!Left.isReg() || !Right.isReg() || Left.Space != Right.Space ||
              Left.Offset != Right.Offset)
            return false;
          const uint16_t Narrow = std::min(Left.Size, Right.Size);
          if (Narrow == 0 || Narrow > sizeof(uint64_t) ||
              (Compared->Mask & ~widthMask(Narrow)) != 0 ||
              overlaps(Left, Index) || overlaps(Left, Value) ||
              overlaps(Right, Index) || overlaps(Right, Value))
            return false;
          // Only the instructions that lie between the two ANDs can write the
          // source. Walking the rest of the function spends the candidate
          // budget and drops a compare that already has this domain.
          const auto Begin = Insns.lower_bound(Compared->AndAddr);
          const auto End = Insns.upper_bound(Killed->AndAddr);
          for (auto It = Begin; It != End; ++It) {
            if (!tryCandidateEvidence())
              return true;
            for (const LowOp &Between : It->second.Ops) {
              if (!tryCandidateEvidence())
                return true;
              const bool AfterCompared = Between.Addr > Compared->AndAddr ||
                                         (Between.Addr == Compared->AndAddr &&
                                          Between.Seq > Compared->AndSeq);
              const bool BeforeKill = Between.Addr < Killed->AndAddr ||
                                      (Between.Addr == Killed->AndAddr &&
                                       Between.Seq < Killed->AndSeq);
              if (!AfterCompared || !BeforeKill)
                continue;
              if (overlaps(Between.Output, Left) ||
                  overlaps(Between.Output, Right))
                return false;
            }
          }
          return true;
        };
        std::function<bool(const NdVar &, int, unsigned, va_t, int)> Reads =
            [&](const NdVar &Value, int Before, unsigned Depth, va_t UseAddr,
                int UseSeq) -> bool {
          if (Depth > limits::kMaxJumpTableGuardExpressionDepth)
            return true;
          if (Value.isConst() || Value.Size == 0)
            return false;
          if (!Value.isReg() && !Value.isTemp())
            return true;
          if (Value.isReg() && overlaps(Value, Index)) {
            // This read is the live index unless the use block's independent
            // definition is strictly after it and does not recompute it.
            if (IndependentKill &&
                (UseAddr < IndependentKill->Addr ||
                 (UseAddr == IndependentKill->Addr &&
                  UseSeq < IndependentKill->Seq)) &&
                !sameMaskRecompute(Value, Before))
              return false;
            return true;
          }
          int DefIndex = -1;
          for (int OpI = std::min(Before, static_cast<int>(Ops.size()) - 1);
               OpI >= 0; --OpI) {
            if (!consumeCandidateEvidence())
              return true;
            if (!Ops[OpI].Output.isReg() && !Ops[OpI].Output.isTemp())
              continue;
            if (Ops[OpI].Output.Space == Value.Space &&
                Ops[OpI].Output.Offset == Value.Offset &&
                Ops[OpI].Output.Size == Value.Size) {
              DefIndex = OpI;
              break;
            }
          }
          if (DefIndex < 0)
            return false;
          const LowOp &Def = Ops[DefIndex];
          // The producer does not read a register that is absent from its
          // inputs. A load of some other value is not a compare of this index;
          // follow the address instead of treating the load as a match.
          if (Def.NumInputs > std::size(Def.Inputs))
            return true;
          for (int Input = 0; Input < Def.NumInputs; ++Input) {
            if (!consumeCandidateEvidence())
              return true;
            if (Reads(Def.Inputs[Input], DefIndex - 1, Depth + 1, Def.Addr,
                      Def.Seq))
              return true;
          }
          return false;
        };
        if (Reads(Ops[CondIndex].Inputs[1], CondIndex - 1, 0,
                  Ops[CondIndex].Addr, Ops[CondIndex].Seq))
          return true;
      }
      return false;
    };
    // The mask is not the domain once a guard reads the lane. The one
    // exception is a test that another XOR of the same preimage is zero
    // under that mask: the XOR into the mask permutes a dense prefix, and
    // the test removes one element. Publish only when what remains is
    // exactly 0..N-1 and shorter than the mask. An interior hole, a guard
    // this pattern does not explain, or an unfinished walk leaves the raw
    // mask unpublished.
    auto correlatedMaskPrefix = [&]() -> std::optional<uint32_t> {
      struct Prod {
        const LowOp *Op = nullptr;
        bool Failed = false;
      };
      auto producerInIndexBlock = [&](const NdVar &Want, va_t FromAddr,
                                      int FromSeq) -> Prod {
        Prod Found;
        bool CallBefore = false;
        for (auto It = Insns.upper_bound(FromAddr); It != Insns.begin();) {
          --It;
          if (!tryCandidateEvidence()) {
            Found.Failed = true;
            return Found;
          }
          if (It->first < *IndexBlock)
            break;
          if (It->second.IsCall && It->first < FromAddr)
            CallBefore = true;
          bool Stop = false;
          for (auto OpIt = It->second.Ops.rbegin();
               OpIt != It->second.Ops.rend(); ++OpIt) {
            const LowOp &Op = *OpIt;
            if (!tryCandidateEvidence()) {
              Found.Failed = true;
              return Found;
            }
            if (Op.Addr > FromAddr ||
                (Op.Addr == FromAddr && Op.Seq >= FromSeq))
              continue;
            const bool Same = sameVar(Op.Output, Want);
            const bool LaneExtend =
                Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
                sameVar(Op.Inputs[0], Want) && Op.Output.isReg() &&
                Op.Output.Offset == Want.Offset && Op.Output.Size > Want.Size;
            if (overlaps(Op.Output, Want) && !Same && !LaneExtend) {
              Found.Failed = true;
              return Found;
            }
            if (!Same)
              continue;
            if (It->second.IsInstructionGuard || CallBefore) {
              Found.Failed = true;
              return Found;
            }
            Found.Op = &Op;
            Stop = true;
            break;
          }
          if (Found.Op || Stop || It->first == *IndexBlock)
            break;
        }
        return Found;
      };
      NdVar Value = Index;
      va_t FromAddr = Info.IndexUseAddr;
      int FromSeq = Info.IndexUseSeq;
      const LowOp *MaskAnd = nullptr;
      for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
        const Prod Step = producerInIndexBlock(Value, FromAddr, FromSeq);
        if (Step.Failed || !Step.Op)
          return std::nullopt;
        if (Step.Op->Opcode == NdOp::INT_AND && Step.Op->NumInputs >= 2 &&
            sameVar(Step.Op->Output, Value)) {
          MaskAnd = Step.Op;
          break;
        }
        if (Step.Op->NumInputs < 1 || (Step.Op->Opcode != NdOp::COPY &&
                                       Step.Op->Opcode != NdOp::INT_ZEXT))
          return std::nullopt;
        const NdVar &Input = Step.Op->Inputs[0];
        if ((!Input.isReg() && !Input.isTemp()) ||
            (Step.Op->Opcode == NdOp::COPY &&
             Input.Size != Step.Op->Output.Size) ||
            (Step.Op->Opcode == NdOp::INT_ZEXT &&
             !(Input.Offset == Value.Offset && Input.Size < Value.Size)))
          return std::nullopt;
        Value = Input;
        FromAddr = Step.Op->Addr;
        FromSeq = Step.Op->Seq;
      }
      if (!MaskAnd)
        return std::nullopt;
      const InsnRecord *MaskInsn = nullptr;
      const auto MaskIt = Insns.find(MaskAnd->Addr);
      if (MaskIt != Insns.end())
        MaskInsn = &MaskIt->second;
      if (MaskInsn &&
          std::any_of(MaskInsn->Ops.begin(), MaskInsn->Ops.end(),
                      [&](const LowOp &Merge) {
                        if (Merge.Opcode != NdOp::SELECT || Merge.NumInputs < 3)
                          return false;
                        const int Arms = std::min<int>(Merge.NumInputs, 3);
                        return std::any_of(
                            Merge.Inputs + 1, Merge.Inputs + Arms,
                            [&](const NdVar &Arm) {
                              return sameVar(Arm, MaskAnd->Output);
                            });
                      }))
        return std::nullopt;
      const bool LeftConstant = MaskAnd->Inputs[0].isConst();
      const bool RightConstant = MaskAnd->Inputs[1].isConst();
      if (LeftConstant == RightConstant)
        return std::nullopt;
      const NdVar &Mask = MaskAnd->Inputs[LeftConstant ? 0 : 1];
      const NdVar &Dynamic = MaskAnd->Inputs[LeftConstant ? 1 : 0];
      const std::optional<uint64_t> Effective = effectiveIntegerAndMask(
          Mask.Offset, Mask.Size, Dynamic.Size, MaskAnd->Output.Size);
      if (!Effective || *Effective == 0 ||
          *Effective == std::numeric_limits<uint64_t>::max() ||
          (*Effective & (*Effective + 1)) != 0)
        return std::nullopt;
      const uint64_t MaskBits = *Effective;
      if (MaskBits + 1 < limits::kMinJumpTableEntries ||
          MaskBits + 1 > limits::kMaxJumpTableEntries)
        return std::nullopt;
      NdVar Preimage = Dynamic;
      va_t PreAddr = MaskAnd->Addr;
      int PreSeq = MaskAnd->Seq;
      const LowOp *IndexXor = nullptr;
      uint64_t IndexXorBits = 0;
      for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
        const Prod Step = producerInIndexBlock(Preimage, PreAddr, PreSeq);
        if (Step.Failed || !Step.Op)
          return std::nullopt;
        if (Step.Op->Opcode == NdOp::INT_XOR && Step.Op->NumInputs == 2 &&
            sameVar(Step.Op->Output, Preimage)) {
          const bool XorLeft = Step.Op->Inputs[0].isConst();
          const bool XorRight = Step.Op->Inputs[1].isConst();
          if (XorLeft == XorRight)
            return std::nullopt;
          const NdVar &Constant = Step.Op->Inputs[XorLeft ? 0 : 1];
          const NdVar &Source = Step.Op->Inputs[XorLeft ? 1 : 0];
          const std::optional<uint64_t> Bits = scalarConstantBits(Constant);
          if (!Bits || (!Source.isReg() && !Source.isTemp()))
            return std::nullopt;
          IndexXor = Step.Op;
          IndexXorBits = *Bits;
          Preimage = Source;
          break;
        }
        if (Step.Op->NumInputs < 1 || (Step.Op->Opcode != NdOp::COPY &&
                                       Step.Op->Opcode != NdOp::INT_ZEXT))
          return std::nullopt;
        const NdVar &Input = Step.Op->Inputs[0];
        if ((!Input.isReg() && !Input.isTemp()) ||
            (Step.Op->Opcode == NdOp::COPY &&
             Input.Size != Step.Op->Output.Size) ||
            (Step.Op->Opcode == NdOp::INT_ZEXT &&
             !(Input.Offset == Preimage.Offset && Input.Size < Preimage.Size)))
          return std::nullopt;
        Preimage = Input;
        PreAddr = Step.Op->Addr;
        PreSeq = Step.Op->Seq;
      }
      if (!IndexXor || (!Preimage.isReg() && !Preimage.isTemp()))
        return std::nullopt;

      struct ExcludingTest {
        uint64_t Bits = 0;
        bool TableWhenNonZero = false;
        va_t Addr = InvalidVA;
        int Seq = -1;
      };
      std::vector<ExcludingTest> Tests;
      for (size_t GuardI = 0; GuardI < GuardBranchAddrs.size(); ++GuardI) {
        if (!TableConditions[GuardI])
          continue;
        if (!tryCandidateEvidence())
          return std::nullopt;
        const va_t BranchAddr = GuardBranchAddrs[GuardI];
        const auto GuardBlock = blockOf(BranchAddr);
        if (!GuardBlock)
          return std::nullopt;
        std::vector<LowOp> Ops;
        for (auto It = Insns.lower_bound(*GuardBlock);
             It != Insns.end() && It->first <= BranchAddr; ++It) {
          if (!tryCandidateEvidence())
            return std::nullopt;
          const auto OpBlock = blockOf(It->first);
          if (!OpBlock || *OpBlock != *GuardBlock)
            break;
          for (const LowOp &Op : It->second.Ops) {
            if (!tryCandidateEvidence())
              return std::nullopt;
            Ops.push_back(Op);
          }
        }
        int CondIndex = -1;
        for (int OpI = static_cast<int>(Ops.size()) - 1; OpI >= 0; --OpI) {
          if (Ops[OpI].Addr == BranchAddr && Ops[OpI].Opcode == NdOp::COND_BR &&
              Ops[OpI].NumInputs >= 2) {
            CondIndex = OpI;
            break;
          }
        }
        if (CondIndex < 0)
          return std::nullopt;
        auto defBefore = [&](const NdVar &Want, int Before) -> int {
          if (Want.isConst() || Want.Size == 0)
            return -1;
          for (int OpI = std::min(Before, static_cast<int>(Ops.size()) - 1);
               OpI >= 0; --OpI) {
            if (!tryCandidateEvidence())
              return -2;
            if (sameVar(Ops[OpI].Output, Want))
              return OpI;
          }
          return -1;
        };
        std::function<bool(const NdVar &, int, unsigned)> ReadsIndex =
            [&](const NdVar &Want, int Before, unsigned Depth) -> bool {
          if (!tryCandidateEvidence() ||
              Depth > limits::kMaxJumpTableGuardExpressionDepth)
            return true;
          if (Want.isConst() || Want.Size == 0)
            return false;
          if (!Want.isReg() && !Want.isTemp())
            return true;
          if (Want.isReg() && overlaps(Want, Index))
            return true;
          const int Def = defBefore(Want, Before);
          if (Def < 0)
            return Def == -2;
          const LowOp &DefOp = Ops[Def];
          if (DefOp.NumInputs > std::size(DefOp.Inputs))
            return true;
          for (int Input = 0; Input < DefOp.NumInputs; ++Input)
            if (ReadsIndex(DefOp.Inputs[Input], Def - 1, Depth + 1))
              return true;
          return false;
        };
        const bool Reads =
            ReadsIndex(Ops[CondIndex].Inputs[1], CondIndex - 1, 0);
        int At = defBefore(Ops[CondIndex].Inputs[1], CondIndex - 1);
        va_t XorAddr = InvalidVA;
        int XorSeq = -1;
        uint64_t TestBits = 0;
        bool Matched = At >= 0;
        for (unsigned Depth = 0; Matched && Depth <= limits::kMaxQuasiCopyDepth;
             ++Depth) {
          const LowOp &Def = Ops[At];
          if (Def.Opcode == NdOp::COPY && Def.NumInputs >= 1) {
            At = defBefore(Def.Inputs[0], At - 1);
            Matched = At >= 0;
            continue;
          }
          if (Def.Opcode != NdOp::INT_EQUAL || Def.NumInputs != 2) {
            Matched = false;
            break;
          }
          const bool EqLeft = Def.Inputs[0].isConst();
          const bool EqRight = Def.Inputs[1].isConst();
          if (EqLeft == EqRight) {
            Matched = false;
            break;
          }
          const NdVar &Zero = Def.Inputs[EqLeft ? 0 : 1];
          const NdVar &Compared = Def.Inputs[EqLeft ? 1 : 0];
          const std::optional<uint64_t> ZeroBits = scalarConstantBits(Zero);
          if (!ZeroBits || *ZeroBits != 0) {
            Matched = false;
            break;
          }
          At = defBefore(Compared, At - 1);
          if (At < 0 || Ops[At].Opcode != NdOp::INT_AND ||
              Ops[At].NumInputs != 2) {
            Matched = false;
            break;
          }
          const LowOp &And = Ops[At];
          const bool AndLeft = And.Inputs[0].isConst();
          const bool AndRight = And.Inputs[1].isConst();
          if (AndLeft == AndRight) {
            Matched = false;
            break;
          }
          const std::optional<uint64_t> AndMask = effectiveIntegerAndMask(
              And.Inputs[AndLeft ? 0 : 1].Offset,
              And.Inputs[AndLeft ? 0 : 1].Size,
              And.Inputs[AndLeft ? 1 : 0].Size, And.Output.Size);
          if (!AndMask || *AndMask != MaskBits) {
            Matched = false;
            break;
          }
          NdVar TestValue = And.Inputs[AndLeft ? 1 : 0];
          int TestAt = At;
          for (unsigned Peel = 0; Peel <= limits::kMaxQuasiCopyDepth; ++Peel) {
            const int InputAt = defBefore(TestValue, TestAt - 1);
            if (InputAt < 0)
              break;
            const LowOp &Peeled = Ops[InputAt];
            if (Peeled.Opcode == NdOp::INT_XOR && Peeled.NumInputs == 2 &&
                sameVar(Peeled.Output, TestValue)) {
              const bool XorLeft = Peeled.Inputs[0].isConst();
              const bool XorRight = Peeled.Inputs[1].isConst();
              if (XorLeft == XorRight)
                break;
              const std::optional<uint64_t> Bits =
                  scalarConstantBits(Peeled.Inputs[XorLeft ? 0 : 1]);
              const NdVar &Source = Peeled.Inputs[XorLeft ? 1 : 0];
              if (!Bits || !sameVar(Source, Preimage))
                break;
              XorAddr = Peeled.Addr;
              XorSeq = Peeled.Seq;
              TestBits = *Bits;
              break;
            }
            if (Peeled.NumInputs < 1 || (Peeled.Opcode != NdOp::COPY &&
                                         Peeled.Opcode != NdOp::INT_ZEXT))
              break;
            const NdVar &PeeledInput = Peeled.Inputs[0];
            if ((Peeled.Opcode == NdOp::COPY &&
                 PeeledInput.Size != Peeled.Output.Size) ||
                (Peeled.Opcode == NdOp::INT_ZEXT &&
                 !(PeeledInput.Offset == TestValue.Offset &&
                   PeeledInput.Size < TestValue.Size)))
              break;
            TestValue = PeeledInput;
            TestAt = InputAt;
          }
          if (XorAddr == InvalidVA) {
            Matched = false;
            break;
          }
          break;
        }
        if (!Matched) {
          if (Reads)
            return std::nullopt;
          continue;
        }
        Tests.push_back(
            ExcludingTest{TestBits, *TableConditions[GuardI], XorAddr, XorSeq});
      }
      if (Tests.empty())
        return std::nullopt;
      const auto IndexXorBlock = blockOf(IndexXor->Addr);
      if (!IndexXorBlock)
        return std::nullopt;
      bool CrossBlockXor = false;
      for (const ExcludingTest &Test : Tests) {
        if (Test.Addr == InvalidVA)
          return std::nullopt;
        const auto TestBlock = blockOf(Test.Addr);
        if (!TestBlock)
          return std::nullopt;
        if (*TestBlock != *IndexXorBlock)
          CrossBlockXor = true;
      }
      // A second incoming edge can rewrite the preimage after the test and
      // before the index XOR. Both XORs stay one value only when every test
      // is in this block, or this block's only predecessor holds them all.
      if (CrossBlockXor) {
        const va_t IndexBlockStart = *IndexXorBlock;
        std::set<va_t> Preds;
        const InsnRecord *Pending = nullptr;
        va_t PendingBlock = 0;
        bool PredFailed = false;
        auto consider = [&](const InsnRecord &Term, va_t Block) {
          if (PredFailed)
            return;
          const bool Sized =
              Term.Size != 0 && Term.Addr <= InvalidVA - Term.Size;
          const va_t Fall = Sized ? Term.Addr + Term.Size : InvalidVA;
          bool Hits = false;
          bool Falls = false;
          if (Term.IsRet && Term.IsCond && Term.IsBranch) {
            Hits = Term.BranchTarget == IndexBlockStart;
          } else if (Term.IsRet) {
            return;
          } else if (Term.IsBranch && Term.IsIndirect) {
            Falls = Term.IsCond && Sized;
            for (va_t Target : Term.JumpTableTargets) {
              if (!tryCandidateEvidence()) {
                PredFailed = true;
                return;
              }
              Hits = Hits || Target == IndexBlockStart;
            }
          } else if (Term.IsBranch && !Term.IsIndirect && !Term.IsCall) {
            Falls = Term.IsCond && Sized;
            Hits = Term.BranchTarget == IndexBlockStart;
          } else if ((!Term.IsBranch || Term.IsCall) &&
                     (!Term.IsNoReturnCall || Term.IsCond)) {
            Falls = Sized;
          }
          if ((Falls && Fall == IndexBlockStart) || Hits)
            Preds.insert(Block);
        };
        for (const auto &[Addr, Insn] : Insns) {
          if (!tryCandidateEvidence())
            return std::nullopt;
          const auto Block = blockOf(Addr);
          if (!Block)
            return std::nullopt;
          if (Pending && *Block != PendingBlock)
            consider(*Pending, PendingBlock);
          if (PredFailed)
            return std::nullopt;
          Pending = &Insn;
          PendingBlock = *Block;
        }
        if (Pending)
          consider(*Pending, PendingBlock);
        if (PredFailed || Preds.size() != 1)
          return std::nullopt;
        for (const ExcludingTest &Test : Tests) {
          if (Test.Addr == InvalidVA)
            return std::nullopt;
          const auto XorBlock = blockOf(Test.Addr);
          if (!XorBlock || *XorBlock != *Preds.begin())
            return std::nullopt;
        }
      }
      auto afterPoint = [](const LowOp &Op, va_t Addr, int Seq) {
        return Op.Addr > Addr || (Op.Addr == Addr && Op.Seq > Seq);
      };
      auto beforePoint = [](const LowOp &Op, va_t Addr, int Seq) {
        return Op.Addr < Addr || (Op.Addr == Addr && Op.Seq < Seq);
      };
      for (const ExcludingTest &Test : Tests) {
        if (Test.Addr == InvalidVA)
          return std::nullopt;
        for (const auto &[Addr, Insn] : Insns) {
          if (!tryCandidateEvidence())
            return std::nullopt;
          if (Insn.IsCall && Addr > Test.Addr && Addr < IndexXor->Addr)
            return std::nullopt;
          for (const LowOp &Op : Insn.Ops) {
            if (!tryCandidateEvidence())
              return std::nullopt;
            if (!afterPoint(Op, Test.Addr, Test.Seq) ||
                !beforePoint(Op, IndexXor->Addr, IndexXor->Seq))
              continue;
            if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
              return std::nullopt;
            if (overlaps(Op.Output, Preimage))
              return std::nullopt;
          }
        }
      }
      if (!tryCandidateEvidence(static_cast<size_t>(MaskBits) + 1))
        return std::nullopt;
      std::vector<uint8_t> Reached(static_cast<size_t>(MaskBits) + 1, 0);
      for (uint64_t Candidate = 0; Candidate <= MaskBits; ++Candidate) {
        bool Reaches = true;
        for (const ExcludingTest &Test : Tests) {
          const bool Zero = ((Candidate ^ Test.Bits) & MaskBits) == 0;
          Reaches = Reaches && (Zero == Test.TableWhenNonZero);
        }
        if (!Reaches)
          continue;
        const uint64_t IndexValue = (Candidate ^ IndexXorBits) & MaskBits;
        Reached[static_cast<size_t>(IndexValue)] = 1;
      }
      uint32_t Prefix = 0;
      bool SawGap = false;
      for (uint64_t IndexValue = 0; IndexValue <= MaskBits; ++IndexValue) {
        if (Reached[static_cast<size_t>(IndexValue)]) {
          if (SawGap)
            return std::nullopt;
          ++Prefix;
          continue;
        }
        SawGap = true;
      }
      if (Prefix < limits::kMinJumpTableEntries ||
          Prefix > limits::kMaxJumpTableEntries ||
          uint64_t(Prefix) == MaskBits + 1)
        return std::nullopt;
      return Prefix;
    };
    if (guardReadsIndex())
      return correlatedMaskPrefix();

    NdVar Value = Index;
    va_t FromAddr = Info.IndexUseAddr;
    int FromSeq = Info.IndexUseSeq;
    for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!consumeCandidateEvidence())
        return std::nullopt;
      const LowOp *Producer = nullptr;
      const InsnRecord *ProducerInsn = nullptr;
      bool CallBeforeUse = false;
      for (auto It = Insns.upper_bound(FromAddr); It != Insns.begin();) {
        --It;
        if (!consumeCandidateEvidence())
          return std::nullopt;
        if (It->first < *IndexBlock)
          break;
        if (It->second.IsCall && It->first < FromAddr)
          CallBeforeUse = true;
        for (auto OpIt = It->second.Ops.rbegin(); OpIt != It->second.Ops.rend();
             ++OpIt) {
          const LowOp &Op = *OpIt;
          if (!consumeCandidateEvidence())
            return std::nullopt;
          if (Op.Addr > FromAddr || (Op.Addr == FromAddr && Op.Seq >= FromSeq))
            continue;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            return std::nullopt;
          const bool SameOutput = sameVar(Op.Output, Value);
          const bool LaneExtend =
              Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
              sameVar(Op.Inputs[0], Value) && Op.Output.isReg() &&
              Op.Output.Offset == Value.Offset && Op.Output.Size > Value.Size;
          // A partial overwrite is not this value. Skipping it would prove
          // the mask through a clobber.
          if (overlaps(Op.Output, Value) && !SameOutput && !LaneExtend)
            return std::nullopt;
          if (!SameOutput && !LaneExtend)
            continue;
          if (It->second.IsInstructionGuard || CallBeforeUse)
            return std::nullopt;
          Producer = &Op;
          ProducerInsn = &It->second;
          break;
        }
        if (Producer || It->first == *IndexBlock)
          break;
      }
      if (!Producer)
        return std::nullopt;
      if (Producer->Opcode == NdOp::INT_AND && Producer->NumInputs >= 2 &&
          sameVar(Producer->Output, Value)) {
        // A SELECT in this instruction can put another arm in the register.
        // The AND is not the value that reaches the table.
        const bool ConditionalMerge =
            ProducerInsn &&
            std::any_of(
                ProducerInsn->Ops.begin(), ProducerInsn->Ops.end(),
                [&](const LowOp &Merge) {
                  if (Merge.Opcode != NdOp::SELECT || Merge.NumInputs < 3)
                    return false;
                  const int Arms = std::min<int>(Merge.NumInputs, 3);
                  return std::any_of(Merge.Inputs + 1, Merge.Inputs + Arms,
                                     [&](const NdVar &Arm) {
                                       return sameVar(Arm, Producer->Output);
                                     });
                });
        if (ConditionalMerge)
          return std::nullopt;
        const bool LeftConstant = Producer->Inputs[0].isConst();
        const bool RightConstant = Producer->Inputs[1].isConst();
        if (LeftConstant == RightConstant)
          return std::nullopt;
        const NdVar &Mask = Producer->Inputs[LeftConstant ? 0 : 1];
        const NdVar &Dynamic = Producer->Inputs[LeftConstant ? 1 : 0];
        if (Mask.AddressOwnerVA != InvalidVA ||
            (Mask.Provenance != ConstantAddressProvenance::Unknown &&
             Mask.Provenance != ConstantAddressProvenance::Scalar))
          return std::nullopt;
        const std::optional<uint64_t> Effective = effectiveIntegerAndMask(
            Mask.Offset, Mask.Size, Dynamic.Size, Producer->Output.Size);
        if (!Effective || *Effective == 0 ||
            *Effective == std::numeric_limits<uint64_t>::max() ||
            (*Effective & (*Effective + 1)) != 0)
          return std::nullopt;
        const uint64_t Bound = *Effective + 1;
        if (Bound < limits::kMinJumpTableEntries ||
            Bound > limits::kMaxJumpTableEntries)
          return std::nullopt;
        return static_cast<uint32_t>(Bound);
      }
      if (Producer->NumInputs < 1 ||
          (Producer->Opcode != NdOp::COPY &&
           Producer->Opcode != NdOp::INT_ZEXT) ||
          (!Producer->Inputs[0].isReg() && !Producer->Inputs[0].isTemp()) ||
          (Producer->Opcode == NdOp::COPY &&
           Producer->Inputs[0].Size != Producer->Output.Size) ||
          (Producer->Opcode == NdOp::INT_ZEXT &&
           !(sameVar(Producer->Output, Value) &&
             Producer->Inputs[0].Offset == Value.Offset &&
             Producer->Inputs[0].Size < Value.Size)))
        return std::nullopt;
      Value = Producer->Inputs[0];
      FromAddr = Producer->Addr;
      FromSeq = Producer->Seq;
    }
    return std::nullopt;
  };
  if (const std::optional<uint32_t> MaskBound = localDenseMaskBound()) {
    // The walk charges the candidate account. An empty account means the
    // bound was not finished; do not authenticate it or clear the exhaustion
    // the compare proof still has to see.
    if (GuardBuildExhausted || *CandidateEvidenceBudget == 0)
      return failIncomplete();
    if (Info.MaxEntries == 0 || *MaskBound < Info.MaxEntries)
      Info.MaxEntries = *MaskBound;
    // The domain is this AND, not a compare. Recording it lets the later
    // mask fixed-point skip the origin walk: that walk is the same fact and
    // fails closed once the producer chain exceeds the resolver depth.
    Info.AuthenticatedDenseMaskBound = Info.MaxEntries;
    Info.IncompleteGuardDomain = false;
    Info.SemanticGuardDomainAmbiguous = false;
    return true;
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
  // An unfinished predicated-alias scan is not a fact about the compare.
  // Abandon the unproved alternatives and keep the guard proof.
  size_t AliasSyntaxWork = limits::kMaxJumpTableGuardAliasEvidenceWork;
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
  auto consumeAliasQueryWork = [&](size_t QueryCount, size_t EvidenceAmount) {
    if (AliasQueries.size() > limits::kMaxJumpTableEvidenceWork ||
        QueryCount > limits::kMaxJumpTableEvidenceWork - AliasQueries.size() ||
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
  // Predicated selects are few.  Index them once in address order rather
  // than rescanning every operation of the function for each guard, which
  // exhausts the alias budget on a large function's second switch.
  struct SelectSite {
    va_t Addr;
    const InsnRecord *Insn;
    size_t Index;
  };
  std::vector<SelectSite> SelectSites;
  size_t ControllingGuards = 0;
  for (const std::optional<bool> &Condition : TableConditions)
    ControllingGuards += Condition.has_value();
  const bool HasGuardCondition = ControllingGuards != 0;
  // The controlling guards are candidate-wide inventory too, which
  // tableLoadConditionValues has already paid for.  More of them than the
  // ordinary allowance holds, as in a compare chain before a dense table, give
  // this batch the dedicated allowance.  Their value queries then walk back
  // across the chain's blocks, so they use the large-expression role depth.
  const bool LongGuardChain =
      ControllingGuards > limits::kMaxJumpTableOrdinaryControllingGuards;
  const uint32_t GuardValueDepth =
      LongGuardChain ? limits::kMaxJumpTableLargeExpressionRoleResolverDepth
                     : limits::kMaxJumpTableExpandedResolverDepth;
  if (LongGuardChain) {
    GuardBuildWork += limits::kMaxJumpTableGuardExpressionEvidenceWork -
                      limits::kMaxJumpTableEvidenceWork;
    GuardExpressionAllowanceExtended = true;
  }
  if (HasGuardCondition)
    for (const auto &[Addr, Insn] : Insns) {
      if (!consumeAliasSyntaxWork())
        goto alias_done;
      if (Insn.IsInstructionGuard)
        continue;
      for (size_t Index = 0; Index < Insn.Ops.size(); ++Index)
        if (Insn.Ops[Index].Opcode == NdOp::SELECT)
          SelectSites.push_back({Addr, &Insn, Index});
    }
  for (size_t GuardIndex = 0; GuardIndex < GuardBranchAddrs.size();
       ++GuardIndex) {
    if (!consumeAliasSyntaxWork())
      goto alias_done;
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
        goto alias_done;
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
        goto alias_done;
      const LowOp *Def = nullptr;
      size_t DefIndex = BranchIt->second.Ops.size();
      for (size_t I = PredicateUseIndex; I > 0; --I) {
        if (!consumeAliasSyntaxWork())
          goto alias_done;
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
    if (AliasEvidenceExhausted)
      goto alias_done;
    if (!BranchPredicate)
      continue;

    for (const SelectSite &Site : SelectSites) {
      if (Site.Addr > BranchAddr)
        break;
      const InsnRecord &Insn = *Site.Insn;
      {
        const size_t SelectIndex = Site.Index;
        if (!consumeAliasSyntaxWork())
          goto alias_done;
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
        const LocalPredicatePtr SelectPredicate =
            buildLocalPredicate(Insn, Select.Inputs[0], SelectIndex);
        if (AliasEvidenceExhausted)
          goto alias_done;
        const bool PredicatesMatch =
            SelectPredicate &&
            pairLocalPredicates(BranchPredicate, SelectPredicate,
                                Candidate.Queries);
        if (AliasEvidenceExhausted)
          goto alias_done;
        if (!PredicatesMatch)
          continue;
        if (IndexAlternatives.size() > std::numeric_limits<size_t>::max() / 3 ||
            !consumeAliasQueryWork(IndexAlternatives.size(),
                                   IndexAlternatives.size() * 3))
          goto alias_done;
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
          goto alias_done;
        AliasCandidates.push_back(std::move(Candidate));
      }
    }
  }
  // Value reconstruction crosses CFG predecessors as well as expression
  // definitions. Use the guard batch's graph-walk limit; local guard syntax
  // keeps its separate depth bound and every query retains the shared work
  // budget.
  if (!AliasQueries.empty()) {
    bool AliasProofComplete = false;
    const std::vector<bool> AliasResults = tableValuesMatchAtUses(
        AliasQueries, &AliasProofComplete, nullptr, InvalidVA, nullptr,
        CandidateEvidenceBudget, /*LocalMatchEvidenceLimit=*/0,
        /*CandidateBranchesSharingTargets=*/nullptr,
        /*QueryUnsignedFeasibleMasks=*/nullptr, GuardValueDepth,
        CertifiedEdgeOverrides, /*UseGroupContext=*/true, &QueryScope);
    if (!AliasProofComplete || AliasResults.size() != AliasQueries.size())
      goto alias_done;
    for (const PredicatedIndexAlias &Candidate : AliasCandidates) {
      if (!consumeAliasSyntaxWork())
        goto alias_done;
      bool CandidateMatches = true;
      for (size_t Query : Candidate.Queries) {
        if (!consumeAliasSyntaxWork())
          goto alias_done;
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
          goto alias_done;
        if (Known == Candidate.Arm) {
          KnownAlternative = true;
          break;
        }
      }
      if (!KnownAlternative) {
        if (!consumeAliasSyntaxWork())
          goto alias_done;
        IndexAlternatives.push_back(Candidate.Arm);
      }
    }
  }
alias_done:
  // The compared register reaches the table index when this guard branch's
  // conditional fallthrough is the use block's only incoming edge and every
  // later write of that register is a same-lane copy or the lifter's
  // zero-extend. The same fact holds without that edge walk when the index
  // occurrence is this register in the compare block and nothing writes the
  // lane between the two reads. A W write's zero-extend is emitted before
  // the compare, and a later AND that keeps every bit of the lane writes
  // the wide index from that compared register. An AND that writes the index
  // from the same unmodified source, with the same mask, as the AND that
  // defined the compared lane is that lane too. A byte mask qualifies here;
  // it is not the full-lane mask above. Either one is the index.
  // A W compare of an X reload is that lane when the frame slot's reaching
  // store is the lifter's zero-extend of a W definition. An X store that is
  // not that zero-extend is a different value. Two reloads of that same slot
  // are the compared lane when the store is that zero-extend, or a W write
  // that zero-extends into the stored X. The compare reads the low half of
  // one reload. A later write of the compared register does not replace the
  // other reload. The index reload of a second slot is the compared lane
  // only when that store zero-extends a load of the compared slot and that
  // slot is not stored again before the compare.
  // The register's origin is not part of the fact: unfolding a copy chain
  // longer than the guard depth would discard a compare that already
  // bounds the lane. A direct copy of the compared register into the index,
  // taken before the compare, is that lane. Nothing may write the source or
  // call between that copy and the compare, and every later write of the
  // index must be a same-lane copy or the lifter's zero-extend. A later write
  // of the compared register does not replace the copy. A taken edge, a
  // second predecessor, a call, a copy from any other register, or a mask
  // that does not repeat the compared lane keeps the value query.
  std::optional<bool> IndexIsZeroExtended;
  bool IndexPredecessorsScanned = false;
  std::set<va_t> IndexPredecessors;
  va_t IndexGuardBlock = InvalidVA;
  va_t IndexGuardBranch = InvalidVA;
  auto structuralRegisterIndex = [&](const NdVar &Compared, va_t CompareAddr,
                                     int CompareSeq,
                                     va_t GuardBranchAddr) -> bool {
    const NdVar &Index = Info.IndexValueAtUse;
    if (!CurrentImg || !Compared.isReg() || !Index.isReg() ||
        Compared.Size == 0 || Index.Size < Compared.Size ||
        Info.IndexUseAddr == InvalidVA || Info.IndexUseSeq < 0 ||
        CompareAddr == InvalidVA || CompareSeq < 0 ||
        GuardBranchAddr == InvalidVA)
      return false;
    auto blockOf = [&](va_t Addr) -> std::optional<va_t> {
      auto It = BlockStarts.upper_bound(Addr);
      if (It == BlockStarts.begin())
        return std::nullopt;
      --It;
      return *It;
    };
    const auto CompareBlock = blockOf(CompareAddr);
    const auto IndexBlock = blockOf(Info.IndexUseAddr);
    if (!CompareBlock || !IndexBlock)
      return false;

    const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
    auto overlapsCompared = [&](const NdVar &Output) {
      if (!Output.isReg() || Output.Size == 0)
        return false;
      if (Output.Offset == Compared.Offset)
        return true;
      return TRI.isSubRegOf(Output.Offset, Output.Size, Compared.Offset,
                            Compared.Size) ||
             TRI.isSubRegOf(Compared.Offset, Compared.Size, Output.Offset,
                            Output.Size);
    };
    auto afterCompare = [&](const LowOp &Op) {
      return Op.Addr > CompareAddr ||
             (Op.Addr == CompareAddr && Op.Seq > CompareSeq);
    };
    auto beforeIndex = [&](const LowOp &Op) {
      return Op.Addr < Info.IndexUseAddr ||
             (Op.Addr == Info.IndexUseAddr && Op.Seq < Info.IndexUseSeq);
    };
    auto inSpan = [&](va_t Addr) {
      const auto Block = blockOf(Addr);
      return Block && (*Block == *CompareBlock || *Block == *IndexBlock);
    };
    const bool SameRegister = Index.Space == Compared.Space &&
                              Index.Offset == Compared.Offset &&
                              Index.Size == Compared.Size;
    // The scaled load records the narrow lane at the zero-extend that feeds
    // it. That read is in the compare block, before the compare. It is the
    // compared value exactly when the lane is not written in between.
    if (*CompareBlock == *IndexBlock) {
      if (!SameRegister)
        return false;
      const bool IndexBefore =
          Info.IndexUseAddr < CompareAddr ||
          (Info.IndexUseAddr == CompareAddr && Info.IndexUseSeq < CompareSeq);
      const bool CompareBefore =
          CompareAddr < Info.IndexUseAddr ||
          (CompareAddr == Info.IndexUseAddr && CompareSeq < Info.IndexUseSeq);
      if (!IndexBefore && !CompareBefore)
        return true;
      const va_t LoAddr = IndexBefore ? Info.IndexUseAddr : CompareAddr;
      const int LoSeq = IndexBefore ? Info.IndexUseSeq : CompareSeq;
      const va_t HiAddr = IndexBefore ? CompareAddr : Info.IndexUseAddr;
      const int HiSeq = IndexBefore ? CompareSeq : Info.IndexUseSeq;
      for (auto It = Insns.lower_bound(LoAddr);
           It != Insns.end() && It->first <= HiAddr; ++It) {
        if (!consumeCandidateEvidence())
          return false;
        const auto &[Addr, Insn] = *It;
        if (Insn.IsCall && Addr > LoAddr && Addr < HiAddr)
          return false;
        for (const LowOp &Op : Insn.Ops) {
          if (!consumeCandidateEvidence())
            return false;
          const bool AfterLo =
              Op.Addr > LoAddr || (Op.Addr == LoAddr && Op.Seq > LoSeq);
          const bool BeforeHi =
              Op.Addr < HiAddr || (Op.Addr == HiAddr && Op.Seq < HiSeq);
          if (!AfterLo || !BeforeHi)
            continue;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
            return false;
          if (overlapsCompared(Op.Output))
            return false;
        }
      }
      return true;
    }

    if (!IndexPredecessorsScanned) {
      bool Reject = false;
      const InsnRecord *Pending = nullptr;
      va_t PendingBlock = 0;
      auto considerTerminator = [&](const InsnRecord &Term, va_t Block) {
        if (Reject)
          return;
        const bool Sized = Term.Size != 0 && Term.Addr <= InvalidVA - Term.Size;
        const va_t Fall = Sized ? Term.Addr + Term.Size : InvalidVA;
        bool OtherHit = false;
        auto note = [&](va_t Target) {
          if (Target == *IndexBlock)
            OtherHit = true;
        };
        bool Falls = false;
        if (Term.IsRet && Term.IsCond && Term.IsBranch) {
          note(Term.BranchTarget);
        } else if (Term.IsRet) {
          return;
        } else if (Term.IsBranch && Term.IsIndirect) {
          Falls = Term.IsCond && Sized;
          for (va_t Target : Term.JumpTableTargets) {
            if (!consumeCandidateEvidence()) {
              Reject = true;
              return;
            }
            note(Target);
          }
        } else if (Term.IsBranch && !Term.IsIndirect && !Term.IsCall) {
          Falls = Term.IsCond && Sized;
          note(Term.BranchTarget);
        } else if ((!Term.IsBranch || Term.IsCall) &&
                   (!Term.IsNoReturnCall || Term.IsCond)) {
          Falls = Sized;
        }
        if (Reject)
          return;
        const bool FallEdge = Falls && Fall == *IndexBlock;
        if (!FallEdge && !OtherHit)
          return;
        IndexPredecessors.insert(Block);
        if (FallEdge && !OtherHit && Term.IsCond && Term.IsBranch &&
            !Term.IsCall && !Term.IsIndirect && !Term.IsRet) {
          IndexGuardBlock = Block;
          IndexGuardBranch = Term.Addr;
        }
      };
      for (const auto &[Addr, Insn] : Insns) {
        if (!consumeCandidateEvidence())
          return false;
        const auto Block = blockOf(Addr);
        if (!Block) {
          Reject = true;
          break;
        }
        if (Pending && *Block != PendingBlock)
          considerTerminator(*Pending, PendingBlock);
        Pending = &Insn;
        PendingBlock = *Block;
      }
      if (Pending)
        considerTerminator(*Pending, PendingBlock);
      if (Reject)
        return false;
      IndexPredecessorsScanned = true;
    }
    if (IndexPredecessors.size() != 1 ||
        *IndexPredecessors.begin() != *CompareBlock ||
        IndexGuardBlock != *CompareBlock ||
        IndexGuardBranch != GuardBranchAddr || IndexGuardBranch < CompareAddr)
      return false;

    const bool SameOffset =
        Index.Space == Compared.Space && Index.Offset == Compared.Offset;
    // Equal widths are this register only. A different offset is a different
    // value: the zero-extend or the full-lane AND below is the only proof
    // that another register still holds the compared lane.
    bool SawWideIdentity = Index.Size == Compared.Size && SameOffset;
    const LowOp *LaneExtend = nullptr;
    bool LaneExtendGuarded = false;
    if (!SawWideIdentity && Index.Size > Compared.Size && SameOffset) {
      for (auto It = Insns.lower_bound(*CompareBlock);
           It != Insns.end() && It->first <= CompareAddr; ++It) {
        if (!consumeCandidateEvidence())
          return false;
        const auto &[Addr, Insn] = *It;
        for (const LowOp &Op : Insn.Ops) {
          if (!consumeCandidateEvidence())
            return false;
          if (afterCompare(Op) || Op.Addr > CompareAddr)
            continue;
          if (Op.Addr == CompareAddr && Op.Seq >= CompareSeq)
            continue;
          if (!overlapsCompared(Op.Output))
            continue;
          LaneExtend = &Op;
          LaneExtendGuarded = Insn.IsInstructionGuard;
        }
      }
      const bool ExtendsLane =
          LaneExtend && !LaneExtendGuarded &&
          LaneExtend->Opcode == NdOp::INT_ZEXT && LaneExtend->NumInputs >= 1 &&
          LaneExtend->Output.isReg() && LaneExtend->Output.Size == Index.Size &&
          LaneExtend->Output.Offset == Index.Offset &&
          LaneExtend->Inputs[0].isReg() &&
          LaneExtend->Inputs[0].Offset == Compared.Offset &&
          LaneExtend->Inputs[0].Size == Compared.Size &&
          (Info.IndexUseAddr > LaneExtend->Addr ||
           (Info.IndexUseAddr == LaneExtend->Addr &&
            Info.IndexUseSeq > LaneExtend->Seq));
      if (ExtendsLane) {
        bool CallBeforeCompare = false;
        for (auto It = Insns.upper_bound(LaneExtend->Addr);
             It != Insns.end() && It->first < CompareAddr; ++It) {
          if (!consumeCandidateEvidence())
            return false;
          const auto &[Addr, Insn] = *It;
          if (Insn.IsCall)
            CallBeforeCompare = true;
        }
        if (!CallBeforeCompare)
          SawWideIdentity = true;
      }
    }
    // The W definition may precede this comparison block. Its reaching
    // upper-zero proof belongs to the original index occurrence; the span
    // walk below still has to prove that the compared lane is unchanged.
    if (!SawWideIdentity && SameOffset && CurrentImg->Arch == Arch::AArch64 &&
        Index.Size == 8 && Compared.Size == 4 &&
        (!CertifiedEdgeOverrides || CertifiedEdgeOverrides->empty()) &&
        !UseDefinedAlternativesAsRoots) {
      if (!IndexIsZeroExtended) {
        bool Complete = true;
        IndexIsZeroExtended = aarch64IndexIsZeroExtendedAtUse(
            Index, Info.IndexUseAddr, Info.IndexUseSeq, CandidateEvidenceBudget,
            Complete);
        if (!Complete) {
          GuardBuildExhausted = true;
          return false;
        }
      }
      SawWideIdentity = *IndexIsZeroExtended;
    }
    // `and xIndex, xLane, #0xffffffff` keeps every bit of a W compare and
    // clears the rest. A mask that drops a bit inside the lane (`#0xff`,
    // `#7`) is a different value. The AND defines the index, so an earlier
    // write of that register is dead; a later write is not.
    auto fullLaneAnd = [&](const LowOp &Op) {
      if (Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2)
        return false;
      if (!Op.Output.isReg() || Op.Output.Offset != Index.Offset ||
          Op.Output.Size != Index.Size || Op.Output.Space != Index.Space)
        return false;
      const bool LeftConstant = Op.Inputs[0].isConst();
      const bool RightConstant = Op.Inputs[1].isConst();
      if (LeftConstant == RightConstant)
        return false;
      const NdVar &Mask = Op.Inputs[LeftConstant ? 0 : 1];
      const NdVar &Source = Op.Inputs[LeftConstant ? 1 : 0];
      if (!Source.isReg() || Source.Offset != Compared.Offset ||
          Source.Space != Compared.Space || Source.Size < Compared.Size ||
          Source.Size > Index.Size)
        return false;
      if (Compared.Size == 0 || Compared.Size >= 8 || Mask.Size == 0 ||
          Mask.Size > 8)
        return false;
      if (Mask.AddressOwnerVA != InvalidVA ||
          (Mask.Provenance != ConstantAddressProvenance::Unknown &&
           Mask.Provenance != ConstantAddressProvenance::Scalar))
        return false;
      const uint64_t Lane = (uint64_t(1) << (Compared.Size * 8)) - 1;
      uint64_t Bits = Mask.Offset;
      if (Mask.Size < 8)
        Bits &= (uint64_t(1) << (Mask.Size * 8)) - 1;
      return Bits == Lane;
    };
    auto exactIndexWrite = [&](const LowOp &Op) {
      return Op.Output.isReg() && Op.Output.Space == Index.Space &&
             Op.Output.Offset == Index.Offset && Op.Output.Size == Index.Size;
    };
    auto exactComparedWrite = [&](const LowOp &Op) {
      return Op.Output.isReg() && Op.Output.Space == Compared.Space &&
             Op.Output.Offset == Compared.Offset &&
             Op.Output.Size == Compared.Size;
    };
    auto regOverlaps = [&](const NdVar &A, const NdVar &B) {
      if (!A.isReg() || !B.isReg() || A.Size == 0 || B.Size == 0)
        return false;
      if (A.Offset == B.Offset)
        return true;
      return TRI.isSubRegOf(A.Offset, A.Size, B.Offset, B.Size) ||
             TRI.isSubRegOf(B.Offset, B.Size, A.Offset, A.Size);
    };
    // `and wLane, wSrc, #0xff` then `and xIndex, xSrc, #0xff` recompute the
    // compared lane. The lifter zero-extends the W write before the compare,
    // so the last overlapping write is that extend, not the AND. #0xff is
    // not a full-lane mask; the compare of the first AND still owns the
    // domain. A source write between the two ANDs, or a different mask, is
    // a different value.
    auto sameSourceAnd = [&](const LowOp &Op) {
      if (Op.Opcode != NdOp::INT_AND || Op.NumInputs != 2 ||
          !exactIndexWrite(Op))
        return false;
      const bool LeftConstant = Op.Inputs[0].isConst();
      const bool RightConstant = Op.Inputs[1].isConst();
      if (LeftConstant == RightConstant)
        return false;
      const NdVar &Mask = Op.Inputs[LeftConstant ? 0 : 1];
      const NdVar &Source = Op.Inputs[LeftConstant ? 1 : 0];
      const std::optional<uint64_t> Bits = scalarConstantBits(Mask);
      if (!Bits || *Bits == 0 || !Source.isReg())
        return false;
      if (regOverlaps(Source, Compared) || regOverlaps(Source, Index))
        return false;
      if (Compared.Size == 0 || Compared.Size > sizeof(uint64_t) ||
          (*Bits & ~widthMask(Compared.Size)) != 0)
        return false;
      const LowOp *Defining = nullptr;
      const NdVar *DefiningSource = nullptr;
      bool Foreign = false;
      for (const auto &[Addr, Insn] : Insns) {
        if (!consumeCandidateEvidence())
          return false;
        const auto Block = blockOf(Addr);
        if (!Block || *Block != *CompareBlock)
          continue;
        for (const LowOp &Earlier : Insn.Ops) {
          if (!consumeCandidateEvidence())
            return false;
          if (afterCompare(Earlier) || Earlier.Addr > CompareAddr ||
              (Earlier.Addr == CompareAddr && Earlier.Seq >= CompareSeq))
            continue;
          if (!overlapsCompared(Earlier.Output))
            continue;
          const bool IgnoredExtend =
              Earlier.Opcode == NdOp::INT_ZEXT && Earlier.NumInputs >= 1 &&
              Earlier.Inputs[0].isReg() &&
              Earlier.Inputs[0].Space == Compared.Space &&
              Earlier.Inputs[0].Offset == Compared.Offset &&
              Earlier.Inputs[0].Size == Compared.Size &&
              Earlier.Output.isReg() &&
              Earlier.Output.Offset == Compared.Offset &&
              Earlier.Output.Size > Compared.Size;
          if (IgnoredExtend && Defining && !Foreign)
            continue;
          const bool AndWritesLane = Earlier.Opcode == NdOp::INT_AND &&
                                     Earlier.NumInputs == 2 &&
                                     exactComparedWrite(Earlier);
          bool SameMask = false;
          const NdVar *EarlierSource = nullptr;
          if (AndWritesLane && !Insn.IsInstructionGuard) {
            const bool AndLeft = Earlier.Inputs[0].isConst();
            const bool AndRight = Earlier.Inputs[1].isConst();
            if (AndLeft != AndRight) {
              const NdVar &EarlierMask = Earlier.Inputs[AndLeft ? 0 : 1];
              const NdVar &EarlierReg = Earlier.Inputs[AndLeft ? 1 : 0];
              const std::optional<uint64_t> EarlierBits =
                  scalarConstantBits(EarlierMask);
              if (EarlierBits && *EarlierBits == *Bits && EarlierReg.isReg() &&
                  EarlierReg.Space == Source.Space &&
                  EarlierReg.Offset == Source.Offset && EarlierReg.Size != 0 &&
                  EarlierReg.Size <= sizeof(uint64_t) &&
                  (*Bits &
                   ~widthMask(std::min(EarlierReg.Size, Source.Size))) == 0 &&
                  !regOverlaps(EarlierReg, Compared) &&
                  !regOverlaps(EarlierReg, Index)) {
                SameMask = true;
                EarlierSource = &Earlier.Inputs[AndLeft ? 1 : 0];
              }
            }
          }
          if (SameMask) {
            Defining = &Earlier;
            DefiningSource = EarlierSource;
            Foreign = false;
            continue;
          }
          Foreign = true;
          Defining = nullptr;
          DefiningSource = nullptr;
        }
      }
      if (!Defining || !DefiningSource || Foreign)
        return false;
      for (const auto &[Addr, Insn] : Insns) {
        if (!consumeCandidateEvidence())
          return false;
        if (Insn.IsCall && Addr > Defining->Addr && Addr < Op.Addr)
          return false;
        for (const LowOp &Between : Insn.Ops) {
          if (!consumeCandidateEvidence())
            return false;
          const bool AfterDefining =
              Between.Addr > Defining->Addr ||
              (Between.Addr == Defining->Addr && Between.Seq > Defining->Seq);
          const bool BeforeIndexAnd =
              Between.Addr < Op.Addr ||
              (Between.Addr == Op.Addr && Between.Seq < Op.Seq);
          if (!AfterDefining || !BeforeIndexAnd)
            continue;
          if (Between.Opcode == NdOp::CALL ||
              Between.Opcode == NdOp::INDIR_CALL)
            return false;
          if (regOverlaps(Between.Output, Source) ||
              regOverlaps(Between.Output, *DefiningSource))
            return false;
        }
      }
      return true;
    };
    // A frame reload is the compared lane only when its reaching store is the
    // lifter's zero-extend. The walk is optional: a budget miss declines this
    // identity and leaves the compare's own proof intact.
    bool SlotBudget = false;
    auto chargeSlot = [&]() {
      if (SlotBudget)
        return false;
      if (!tryCandidateEvidence()) {
        SlotBudget = true;
        return false;
      }
      return true;
    };
    auto opsBefore = [&](va_t LimitAddr, int LimitSeq,
                         std::vector<LowOp> &Out) -> bool {
      Out.clear();
      if (SlotBudget)
        return false;
      const auto Block = blockOf(LimitAddr);
      if (!Block)
        return true;
      for (auto It = Insns.lower_bound(*Block); It != Insns.end(); ++It) {
        if (!chargeSlot())
          return false;
        const auto Here = blockOf(It->first);
        if (!Here || *Here != *Block)
          break;
        for (const LowOp &Op : It->second.Ops) {
          if (!chargeSlot())
            return false;
          if (Op.Addr > LimitAddr ||
              (Op.Addr == LimitAddr && Op.Seq >= LimitSeq))
            return true;
          Out.push_back(Op);
        }
      }
      return true;
    };
    auto exactDefBefore = [&](const std::vector<LowOp> &Ops, int Before,
                              const NdVar &Want) -> int {
      if (!Want.isReg() && !Want.isTemp())
        return -1;
      for (int I = Before - 1; I >= 0; --I) {
        const NdVar &Out = Ops[I].Output;
        if (Out.Space == Want.Space && Out.Offset == Want.Offset &&
            Out.Size == Want.Size)
          return I;
      }
      return -1;
    };
    auto peelCopies = [&](const std::vector<LowOp> &Ops, int Idx) -> int {
      for (unsigned Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
        if (!chargeSlot())
          return -1;
        if (Idx < 0 || static_cast<size_t>(Idx) >= Ops.size())
          return -1;
        const LowOp &Op = Ops[Idx];
        if (Op.Opcode != NdOp::COPY || Op.NumInputs < 1 ||
            Op.NumInputs > std::size(Op.Inputs))
          return Idx;
        const NdVar &In = Op.Inputs[0];
        if ((!In.isReg() && !In.isTemp()) || In.Size != Op.Output.Size)
          return Idx;
        const int Next = exactDefBefore(Ops, Idx, In);
        if (Next < 0)
          return Idx;
        Idx = Next;
      }
      return -1;
    };
    auto frameLoad = [&](const std::vector<LowOp> &Ops, int Idx, uint16_t Width,
                         uint64_t &Base, int64_t &Off) -> bool {
      if (Idx < 0 || static_cast<size_t>(Idx) >= Ops.size())
        return false;
      const LowOp &Op = Ops[Idx];
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1 ||
          Op.NumInputs > std::size(Op.Inputs) || Op.Output.Size != Width)
        return false;
      const NdVar &Addr = Op.NumInputs >= 2 ? Op.Inputs[1] : Op.Inputs[0];
      return frameSlotKey(Ops, Idx - 1, Addr, TRI, Base, Off);
    };
    struct StoreSite {
      bool Found = false;
      va_t Addr = InvalidVA;
      int Seq = -1;
      NdVar Value = {};
    };
    // The nearest store of this slot. A write of the frame register between
    // the use and that store is a different memory, so the search stops.
    auto nearestFrameStore = [&](va_t FromAddr, int FromSeq, uint64_t Base,
                                 int64_t Off, StoreSite &Site) -> bool {
      Site = {};
      if (SlotBudget)
        return false;
      const NdVar BaseReg = NdVar::reg(Base, TRI.PointerSize);
      for (auto It = Insns.upper_bound(FromAddr); It != Insns.begin();) {
        if (!chargeSlot())
          return false;
        --It;
        for (auto OpIt = It->second.Ops.rbegin(); OpIt != It->second.Ops.rend();
             ++OpIt) {
          if (!chargeSlot())
            return false;
          const LowOp &Op = *OpIt;
          if (Op.Addr > FromAddr || (Op.Addr == FromAddr && Op.Seq >= FromSeq))
            continue;
          if (Op.Output.isReg() && regOverlaps(Op.Output, BaseReg))
            return true;
          if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
            continue;
          std::vector<LowOp> Prefix;
          if (!opsBefore(Op.Addr, Op.Seq, Prefix))
            return false;
          uint64_t StoreBase = InvalidVA;
          int64_t StoreOff = 0;
          if (!frameSlotKey(Prefix, static_cast<int>(Prefix.size()) - 1,
                            Op.Inputs[0], TRI, StoreBase, StoreOff) ||
              StoreBase != Base || StoreOff != Off)
            continue;
          // The nearest store is the value the reload observes. A guard or
          // an older store is not that value.
          if (It->second.IsInstructionGuard)
            return true;
          Site.Found = true;
          Site.Addr = Op.Addr;
          Site.Seq = Op.Seq;
          Site.Value = Op.Inputs[1];
          return true;
        }
      }
      return true;
    };
    struct ZextSpill {
      bool Ok = false;
      uint64_t SourceBase = InvalidVA;
      int64_t SourceOff = 0;
      va_t SourceAddr = InvalidVA;
      int SourceSeq = -1;
      bool HasSource = false;
    };
    auto zeroExtendSpill = [&](const StoreSite &Site,
                               bool WantSource) -> ZextSpill {
      ZextSpill Result;
      if (SlotBudget || !Site.Found || !Site.Value.isReg() ||
          Site.Value.Size != Index.Size)
        return Result;
      std::vector<LowOp> Prefix;
      if (!opsBefore(Site.Addr, Site.Seq, Prefix))
        return Result;
      int Def =
          exactDefBefore(Prefix, static_cast<int>(Prefix.size()), Site.Value);
      if (Def < 0)
        return Result;
      Def = peelCopies(Prefix, Def);
      if (SlotBudget || Def < 0)
        return Result;
      const LowOp &Root = Prefix[Def];
      if (Root.Opcode != NdOp::INT_ZEXT || Root.NumInputs < 1 ||
          Root.NumInputs > std::size(Root.Inputs))
        return Result;
      const NdVar &In = Root.Inputs[0];
      if (!Root.Output.isReg() || Root.Output.Size != Index.Size ||
          !In.isReg() || In.Size != Compared.Size ||
          In.Offset != Root.Output.Offset || In.Space != Root.Output.Space)
        return Result;
      const auto Guard = Insns.find(Root.Addr);
      if (Guard != Insns.end() && Guard->second.IsInstructionGuard)
        return Result;
      if (!WantSource) {
        Result.Ok = true;
        return Result;
      }
      int Narrow = exactDefBefore(Prefix, Def, In);
      if (Narrow < 0)
        return Result;
      Narrow = peelCopies(Prefix, Narrow);
      if (SlotBudget || Narrow < 0)
        return Result;
      uint64_t LoadBase = InvalidVA;
      int64_t LoadOff = 0;
      if (!frameLoad(Prefix, Narrow, Compared.Size, LoadBase, LoadOff))
        return Result;
      Result.Ok = true;
      Result.HasSource = true;
      Result.SourceBase = LoadBase;
      Result.SourceOff = LoadOff;
      Result.SourceAddr = Prefix[Narrow].Addr;
      Result.SourceSeq = Prefix[Narrow].Seq;
      return Result;
    };
    auto slotStable = [&](uint64_t Base, int64_t Off, va_t FromAddr,
                          int FromSeq, va_t ToAddr, int ToSeq) -> bool {
      if (SlotBudget)
        return false;
      const NdVar BaseReg = NdVar::reg(Base, TRI.PointerSize);
      for (auto It = Insns.lower_bound(FromAddr); It != Insns.end(); ++It) {
        if (!chargeSlot())
          return false;
        if (It->first > ToAddr)
          break;
        for (const LowOp &Op : It->second.Ops) {
          if (!chargeSlot())
            return false;
          const bool AfterFrom =
              Op.Addr > FromAddr || (Op.Addr == FromAddr && Op.Seq > FromSeq);
          const bool BeforeTo =
              Op.Addr < ToAddr || (Op.Addr == ToAddr && Op.Seq < ToSeq);
          if (!AfterFrom || !BeforeTo)
            continue;
          if (Op.Output.isReg() && regOverlaps(Op.Output, BaseReg))
            return false;
          if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
            continue;
          std::vector<LowOp> Prefix;
          if (!opsBefore(Op.Addr, Op.Seq, Prefix))
            return false;
          uint64_t StoreBase = InvalidVA;
          int64_t StoreOff = 0;
          if (frameSlotKey(Prefix, static_cast<int>(Prefix.size()) - 1,
                           Op.Inputs[0], TRI, StoreBase, StoreOff) &&
              StoreBase == Base && StoreOff == Off)
            return false;
        }
      }
      return !SlotBudget;
    };
    bool CopiedSlotIdentity = false;
    if (!SawWideIdentity && LaneExtend && !LaneExtendGuarded && SameOffset &&
        Index.Size > Compared.Size) {
      std::vector<LowOp> Prefix;
      if (!opsBefore(LaneExtend->Addr, LaneExtend->Seq, Prefix))
        return false;
      Prefix.push_back(*LaneExtend);
      const int Root = peelCopies(Prefix, static_cast<int>(Prefix.size()) - 1);
      uint64_t Base = InvalidVA;
      int64_t Off = 0;
      if (!SlotBudget && Root >= 0 &&
          frameLoad(Prefix, Root, Index.Size, Base, Off)) {
        StoreSite Site;
        if (!nearestFrameStore(Prefix[Root].Addr, Prefix[Root].Seq, Base, Off,
                               Site))
          return false;
        const ZextSpill Spill = zeroExtendSpill(Site, false);
        if (Spill.Ok)
          SawWideIdentity = true;
      }
      if (SlotBudget)
        return false;
    }
    if (!SawWideIdentity && !SameOffset && Index.Size > Compared.Size) {
      std::vector<LowOp> IndexPrefix;
      if (!opsBefore(Info.IndexUseAddr, Info.IndexUseSeq, IndexPrefix))
        return false;
      const int IndexDef = exactDefBefore(
          IndexPrefix, static_cast<int>(IndexPrefix.size()), Index);
      const bool Reload =
          IndexDef >= 0 && (IndexPrefix[IndexDef].Opcode == NdOp::COPY ||
                            IndexPrefix[IndexDef].Opcode == NdOp::LOAD);
      if (Reload) {
        bool LaterWrite = false;
        for (int J = IndexDef + 1; J < static_cast<int>(IndexPrefix.size());
             ++J) {
          if (IndexPrefix[J].Output.isReg() &&
              regOverlaps(IndexPrefix[J].Output, Index))
            LaterWrite = true;
        }
        const int Root = peelCopies(IndexPrefix, IndexDef);
        uint64_t IndexBase = InvalidVA;
        int64_t IndexOff = 0;
        if (!SlotBudget && !LaterWrite && Root >= 0 &&
            frameLoad(IndexPrefix, Root, Index.Size, IndexBase, IndexOff)) {
          StoreSite Site;
          if (!nearestFrameStore(IndexPrefix[Root].Addr, IndexPrefix[Root].Seq,
                                 IndexBase, IndexOff, Site))
            return false;
          const ZextSpill Spill = zeroExtendSpill(Site, true);
          std::vector<LowOp> ComparePrefix;
          if (Spill.HasSource &&
              (Spill.SourceBase != IndexBase || Spill.SourceOff != IndexOff)) {
            if (!opsBefore(CompareAddr, CompareSeq, ComparePrefix))
              return false;
          }
          if (Spill.HasSource &&
              (Spill.SourceBase != IndexBase || Spill.SourceOff != IndexOff) &&
              !ComparePrefix.empty()) {
            const int CompareDef = exactDefBefore(
                ComparePrefix, static_cast<int>(ComparePrefix.size()),
                Compared);
            const int CompareRoot =
                CompareDef < 0 ? -1 : peelCopies(ComparePrefix, CompareDef);
            uint64_t CompareBase = InvalidVA;
            int64_t CompareOff = 0;
            if (!SlotBudget && CompareRoot >= 0 &&
                frameLoad(ComparePrefix, CompareRoot, Compared.Size,
                          CompareBase, CompareOff) &&
                CompareBase == Spill.SourceBase &&
                CompareOff == Spill.SourceOff) {
              const LowOp &CompareLoad = ComparePrefix[CompareRoot];
              const bool SameLoad = CompareLoad.Addr == Spill.SourceAddr &&
                                    CompareLoad.Seq == Spill.SourceSeq;
              const bool LaterLoad = CompareLoad.Addr > Spill.SourceAddr ||
                                     (CompareLoad.Addr == Spill.SourceAddr &&
                                      CompareLoad.Seq > Spill.SourceSeq);
              if (SameLoad || (LaterLoad &&
                               slotStable(CompareBase, CompareOff,
                                          Spill.SourceAddr, Spill.SourceSeq,
                                          CompareLoad.Addr, CompareLoad.Seq))) {
                SawWideIdentity = true;
                // The index is the other slot. A later write of the compared
                // register does not replace that reload.
                CopiedSlotIdentity = true;
              }
            }
          }
          // Two reloads of this slot are the compared lane when the store
          // zero-extends a W value into the stored X. The lifter's INT_ZEXT
          // is that write. A W write that architecturally zero-extends into
          // the same X is the same fact when that extend op is absent. The
          // compare reads the low half of one reload. A later write of that
          // register does not replace the other reload. A plain X store is
          // a different value.
          if (!SawWideIdentity && !SlotBudget && Site.Found) {
            const bool ExplicitZext = zeroExtendSpill(Site, false).Ok;
            bool ArchitecturalZext = false;
            if (!ExplicitZext && Site.Value.isReg() &&
                Site.Value.Size == Index.Size) {
              std::vector<LowOp> StorePrefix;
              if (!opsBefore(Site.Addr, Site.Seq, StorePrefix))
                return false;
              int StoreDef = -1;
              for (int I = static_cast<int>(StorePrefix.size()) - 1; I >= 0;
                   --I) {
                if (!chargeSlot())
                  break;
                if (regOverlaps(StorePrefix[I].Output, Site.Value)) {
                  StoreDef = I;
                  break;
                }
              }
              if (!SlotBudget && StoreDef >= 0) {
                StoreDef = peelCopies(StorePrefix, StoreDef);
                if (SlotBudget)
                  return false;
                if (StoreDef >= 0) {
                  const LowOp &Stored = StorePrefix[StoreDef];
                  const auto GuardedWrite = Insns.find(Stored.Addr);
                  const bool IsGuard = GuardedWrite != Insns.end() &&
                                       GuardedWrite->second.IsInstructionGuard;
                  if (!IsGuard && Stored.Output.isReg() &&
                      Stored.Output.Space == Site.Value.Space &&
                      Stored.Output.Size == Compared.Size &&
                      TRI.writeZeroExtends(Stored.Output.Offset,
                                           Stored.Output.Size)) {
                    const auto [WideOff, WideSz] = TRI.findWideReg(
                        Stored.Output.Offset, Stored.Output.Size);
                    ArchitecturalZext = WideOff == Site.Value.Offset &&
                                        WideSz == Site.Value.Size;
                  }
                }
              }
            }
            if (!SlotBudget && (ExplicitZext || ArchitecturalZext)) {
              std::vector<LowOp> SameSlotPrefix;
              if (!opsBefore(CompareAddr, CompareSeq, SameSlotPrefix))
                return false;
              int CompareDef = -1;
              for (int I = static_cast<int>(SameSlotPrefix.size()) - 1; I >= 0;
                   --I) {
                if (!chargeSlot())
                  break;
                if (regOverlaps(SameSlotPrefix[I].Output, Compared)) {
                  CompareDef = I;
                  break;
                }
              }
              const int CompareRoot =
                  CompareDef < 0 || SlotBudget
                      ? -1
                      : peelCopies(SameSlotPrefix, CompareDef);
              uint64_t CompareBase = InvalidVA;
              int64_t CompareOff = 0;
              if (!SlotBudget && CompareRoot >= 0 &&
                  frameLoad(SameSlotPrefix, CompareRoot, Index.Size,
                            CompareBase, CompareOff) &&
                  CompareBase == IndexBase && CompareOff == IndexOff) {
                const LowOp &CompareLoad = SameSlotPrefix[CompareRoot];
                const LowOp &IndexLoad = IndexPrefix[Root];
                const bool BeforeIndex = CompareLoad.Addr < IndexLoad.Addr ||
                                         (CompareLoad.Addr == IndexLoad.Addr &&
                                          CompareLoad.Seq < IndexLoad.Seq);
                if (BeforeIndex && slotStable(IndexBase, IndexOff,
                                              CompareLoad.Addr, CompareLoad.Seq,
                                              IndexLoad.Addr, IndexLoad.Seq)) {
                  SawWideIdentity = true;
                  CopiedSlotIdentity = true;
                }
              }
            }
          }
        }
      }
      if (SlotBudget)
        return false;
    }
    // The copy is in this compare block, before the compare. Recording it
    // here reuses the span walk: a second pass over the function would only
    // repeat the same instructions.
    const bool TrackCopiedRegister = !SawWideIdentity && !SameOffset;
    bool CopiedBeforeCompare = false;
    bool CopyInvalid = false;
    auto copiedLaneRefresh = [&](const LowOp &Op) {
      if (Op.NumInputs < 1 || !Op.Output.isReg() || !Op.Inputs[0].isReg())
        return false;
      if (Op.Output.Space != Index.Space || Op.Inputs[0].Space != Index.Space ||
          Op.Output.Offset != Index.Offset ||
          Op.Inputs[0].Offset != Index.Offset)
        return false;
      if (Op.Opcode == NdOp::COPY && Op.Output.Size == Op.Inputs[0].Size &&
          Op.Output.Size != 0 &&
          (Op.Output.Size == Compared.Size || Op.Output.Size == Index.Size ||
           (Op.Output.Size > Compared.Size && Index.Size == Compared.Size)))
        return true;
      return Op.Opcode == NdOp::INT_ZEXT &&
             Op.Inputs[0].Size == Compared.Size &&
             Op.Output.Size > Compared.Size &&
             (Op.Output.Size == Index.Size || Index.Size == Compared.Size);
    };
    auto copyOfCompared = [&](const LowOp &Op) {
      return Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
             Op.Output.isReg() && Op.Inputs[0].isReg() &&
             Op.Output.Space == Index.Space &&
             Op.Output.Offset == Index.Offset &&
             Op.Output.Size == Compared.Size &&
             Op.Inputs[0].Space == Compared.Space &&
             Op.Inputs[0].Offset == Compared.Offset &&
             Op.Inputs[0].Size == Compared.Size;
    };
    bool LaneAndAccepted = false;
    for (auto It = Insns.lower_bound(std::min(*CompareBlock, *IndexBlock));
         It != Insns.end() && It->first <= Info.IndexUseAddr; ++It) {
      if (!consumeCandidateEvidence())
        return false;
      const auto &[Addr, Insn] = *It;
      if (!inSpan(Addr))
        continue;
      if (Insn.IsCall && Addr > CompareAddr && Addr < Info.IndexUseAddr)
        return false;
      if (TrackCopiedRegister && Insn.IsCall && Addr < CompareAddr &&
          CopiedBeforeCompare)
        CopyInvalid = true;
      for (const LowOp &Op : Insn.Ops) {
        if (!consumeCandidateEvidence())
          return false;
        if (!afterCompare(Op)) {
          if (!TrackCopiedRegister)
            continue;
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
            if (CopiedBeforeCompare)
              CopyInvalid = true;
          } else if (regOverlaps(Op.Output, Index)) {
            if (copyOfCompared(Op) && !Insn.IsInstructionGuard) {
              CopiedBeforeCompare = true;
              CopyInvalid = false;
            } else if (!(CopiedBeforeCompare && !CopyInvalid &&
                         copiedLaneRefresh(Op))) {
              CopiedBeforeCompare = false;
              CopyInvalid = false;
            }
          } else if (CopiedBeforeCompare && !CopyInvalid &&
                     overlapsCompared(Op.Output)) {
            CopyInvalid = true;
          }
          continue;
        }
        if (!beforeIndex(Op))
          continue;
        if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
          return false;
        if (fullLaneAnd(Op)) {
          if (LaneAndAccepted || Insn.IsInstructionGuard)
            return false;
          LaneAndAccepted = true;
          SawWideIdentity = true;
          continue;
        }
        if (sameSourceAnd(Op)) {
          if (LaneAndAccepted || Insn.IsInstructionGuard)
            return false;
          LaneAndAccepted = true;
          SawWideIdentity = true;
          continue;
        }
        if (TrackCopiedRegister && CopiedBeforeCompare && !CopyInvalid &&
            regOverlaps(Op.Output, Index)) {
          if (Insn.IsInstructionGuard || !copiedLaneRefresh(Op))
            return false;
          continue;
        }
        if (LaneAndAccepted && exactIndexWrite(Op))
          return false;
        if (!overlapsCompared(Op.Output))
          continue;
        if ((CopiedSlotIdentity ||
             (TrackCopiedRegister && CopiedBeforeCompare && !CopyInvalid)) &&
            !regOverlaps(Op.Output, Index))
          continue;
        if (Insn.IsInstructionGuard)
          return false;
        const bool NarrowCopy = Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
                                Op.Output.Size == Compared.Size &&
                                Op.Output.Offset == Compared.Offset &&
                                Op.Inputs[0].isReg() &&
                                Op.Inputs[0].Offset == Compared.Offset &&
                                Op.Inputs[0].Size == Compared.Size;
        // Extending another register does not make it the table index.
        // Cross-register identities require the copy proof above.
        const bool WideZeroExtend =
            SameOffset && Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
            Op.Output.Size == Index.Size && Index.Size > Compared.Size &&
            Op.Output.Offset == Compared.Offset && Op.Inputs[0].isReg() &&
            Op.Inputs[0].Offset == Compared.Offset &&
            Op.Inputs[0].Size == Compared.Size;
        if (NarrowCopy)
          continue;
        if (WideZeroExtend) {
          SawWideIdentity = true;
          continue;
        }
        return false;
      }
    }
    if (TrackCopiedRegister && CopiedBeforeCompare && !CopyInvalid)
      SawWideIdentity = true;
    return SawWideIdentity;
  };

  for (size_t GuardIndex = 0; GuardIndex < GuardBranchAddrs.size();
       ++GuardIndex) {
    if (!consumeCandidateEvidence())
      return failIncomplete();
    const va_t BranchAddr = GuardBranchAddrs[GuardIndex];
    const std::optional<bool> &TableCondition = TableConditions[GuardIndex];
    if (!TableCondition)
      continue;
    if (!consumeGuardBuildWork())
      return failIncomplete();
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
      if (!consumeCandidateEvidence())
        break;
      auto NextBlock = BlockStarts.upper_bound(It->first);
      if (NextBlock == BlockStarts.begin() ||
          *std::prev(NextBlock) != BlockStart)
        break;
      for (const LowOp &Op : It->second.Ops) {
        if (!consumeCandidateEvidence(/*scan + retention=*/2))
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
    // Retaining a large compiler-generated block is candidate-wide inventory,
    // not guard-expression complexity.  Once that separately charged
    // inventory establishes a genuinely large guard block, allow its one
    // syntax batch to use the dedicated ceiling without raising the ordinary
    // allowance for every speculative candidate.
    if (!GuardExpressionAllowanceExtended &&
        Ops.size() > limits::kMaxJumpTableEvidenceWork / 4) {
      static_assert(limits::kMaxJumpTableGuardExpressionEvidenceWork >=
                    limits::kMaxJumpTableEvidenceWork);
      GuardBuildWork += limits::kMaxJumpTableGuardExpressionEvidenceWork -
                        limits::kMaxJumpTableEvidenceWork;
      GuardExpressionAllowanceExtended = true;
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

    // The narrower value whose bytes the low lane \p Lane of the register
    // written at \p WriterIndex carries unchanged, as MSVC's
    // `movsxd rcx, dword ptr [rax]` writes RCX from a sign-extended temporary
    // (LOAD t4; INT_SEXT t8, t4; COPY rcx, t8) and an ECX guard reads t4.
    // The writer and every source before that value must be a whole-width
    // copy or a zero/sign extension in this block.  Returns that value's
    // producer in the block, or else the value the extension reads.  This is
    // only a proposal: the lane's identity queries must still prove it.
    auto extendedLaneSource =
        [&](int WriterIndex,
            const NdVar &Lane) -> std::optional<JumpTableValueOccurrence> {
      if (!Lane.isReg() || WriterIndex < 0 ||
          Ops[WriterIndex].Output.Offset != Lane.Offset)
        return std::nullopt;
      int At = WriterIndex;
      for (uint32_t Depth = 0;
           Depth <= limits::kMaxJumpTableGuardExpressionDepth; ++Depth) {
        if (!consumeGuardBuildWork())
          return std::nullopt;
        const LowOp &Op = Ops[At];
        if (Op.NumInputs != 1)
          return std::nullopt;
        const NdVar &Input = Op.Inputs[0];
        if ((!Input.isReg() && !Input.isTemp()) || Input.Size < Lane.Size)
          return std::nullopt;
        const bool Copy =
            Op.Opcode == NdOp::COPY && Input.Size == Op.Output.Size;
        const bool Extension =
            (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT) &&
            Input.Size < Op.Output.Size;
        if (!Copy && !Extension)
          return std::nullopt;
        // The input's nearest writer, past an architectural extension of
        // that same value into its container (an EAX write's RAX extension).
        int Producer = -1;
        for (int I = At - 1; I >= 0; --I) {
          if (!consumeGuardBuildWork())
            return std::nullopt;
          const LowOp &Prior = Ops[I];
          const NdVar &Out = Prior.Output;
          if (Out.Space != Input.Space || Out.Size == 0 ||
              Out.Offset >= Input.Offset + Input.Size ||
              Input.Offset >= Out.Offset + Out.Size)
            continue;
          if (Out.Offset == Input.Offset && Out.Size == Input.Size) {
            Producer = I;
            break;
          }
          const bool ExtendsInput =
              (Prior.Opcode == NdOp::INT_ZEXT ||
               Prior.Opcode == NdOp::INT_SEXT) &&
              Prior.NumInputs == 1 && Prior.Inputs[0].Space == Input.Space &&
              Prior.Inputs[0].Offset == Input.Offset &&
              Prior.Inputs[0].Size == Input.Size && Out.Offset == Input.Offset;
          if (!ExtendsInput)
            break;
        }
        // Sizes shrink only through extensions, so this value is extended.
        if (Input.Size == Lane.Size) {
          if (Producer < 0)
            return JumpTableValueOccurrence{Input, Op.Addr, Op.Seq,
                                            /*DefinedAtPoint=*/false};
          return JumpTableValueOccurrence{Input, Ops[Producer].Addr,
                                          Ops[Producer].Seq,
                                          /*DefinedAtPoint=*/true};
        }
        if (Producer < 0)
          return std::nullopt;
        At = Producer;
      }
      return std::nullopt;
    };

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
        if (!isGuardLiteral(V))
          return {};
        if (V.Offset < std::numeric_limits<uint64_t>::digits)
          ProposedJoinedBound = std::max(ProposedJoinedBound, V.Offset + 1);
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
      if (IndexAlternatives.size() == std::numeric_limits<size_t>::max() ||
          !consumeGuardBuildWork(IndexAlternatives.size() + 1)) {
        return Finish({});
      }
      Node->IndexQuery = ProofQueries.size();
      ProofQueries.push_back({V, UseAddr, UseSeq, IndexAlternatives,
                              /*AllowZeroExtension=*/true,
                              /*AllowSignExtension=*/true});
      if (UseDefinedAlternativesAsRoots)
        ProofQueries.back().UseDefinedAlternativesAsOccurrenceRoots = true;

      int DefIndex = -1;
      const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
      for (int I = std::min(Before, static_cast<int>(Ops.size()) - 1); I >= 0;
           --I) {
        if (!consumeGuardBuildWork())
          return Finish({});
        const NdVar &Output = Ops[I].Output;
        const bool Exact = Output.Space == V.Space &&
                           Output.Offset == V.Offset && Output.Size == V.Size;
        const bool DefinedLane =
            V.isReg() && Output.isReg() && V.Size < Output.Size &&
            TRI.isSubRegOf(V.Offset, V.Size, Output.Offset, Output.Size);
        // A register-wide extension emitted for an EAX/W write carries known
        // upper bits. Replacing it with an opaque full-register occurrence
        // would lose that fact and disconnect an ECX guard from an RCX index.
        // Keep the existing exact-width producer/query for this lane.
        const bool ExtendsThisLane = (Ops[I].Opcode == NdOp::INT_ZEXT ||
                                      Ops[I].Opcode == NdOp::INT_SEXT) &&
                                     Ops[I].NumInputs == 1 &&
                                     Ops[I].Inputs[0].Size == V.Size &&
                                     V.Offset == Output.Offset;
        if (Exact || (DefinedLane && !ExtendsThisLane)) {
          DefIndex = I;
          break;
        }
      }
      auto adoptStructuralIndex = [&]() {
        Node->StructuralIndex = true;
        Node->ContainsStructuralIndex = true;
        if (!ProofQueries.empty() &&
            Node->IndexQuery + 1 == ProofQueries.size()) {
          ProofQueries.pop_back();
          Node->IndexQuery = std::numeric_limits<size_t>::max();
        }
      };
      // The compared register is the index when it is that register, its
      // zero-extend, or a later AND that keeps every bit of the lane. The
      // check does not require the index register to be the compared one:
      // `and xIndex, xLane, #0xffffffff` writes a different register. A
      // miss on a live-in stays a leaf. A miss on a local producer still
      // unfolds, so a copy the identity does not cover keeps its proof.
      if (V.isReg() && Info.IndexValueAtUse.isReg() &&
          structuralRegisterIndex(V, UseAddr, UseSeq, BranchAddr)) {
        adoptStructuralIndex();
        return Finish(Node);
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
      if (!supportedGuardOpcode(Def.Opcode)) {
        // A nearby literal STORE is a proposal, not the value of this LOAD.
        // Ask the shared all-path memory/value resolver to authenticate the
        // literal at this exact use before materializing it in the guard.
        // One nearest-writer proposal bounds speculative work; an intervening
        // alias, call, partial write or independent root grants no shortcut.
        if (Def.Opcode == NdOp::LOAD &&
            Def.MemoryOrdering == NdMemoryOrdering::None &&
            Def.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
          for (int J = DefIndex - 1; J >= 0; --J) {
            if (!consumeGuardBuildWork())
              return Finish({});
            const LowOp &Writer = Ops[J];
            if (Writer.Opcode != NdOp::STORE)
              continue;
            if (Writer.NumInputs == 2 && isGuardLiteral(Writer.Inputs[1]) &&
                Writer.Inputs[1].Size == V.Size &&
                Writer.MemoryOrdering == NdMemoryOrdering::None &&
                Writer.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
              if (!consumeGuardBuildWork(2))
                return Finish({});
              Node->LiteralQuery = ProofQueries.size();
              Node->Literal = Writer.Inputs[1];
              ProofQueries.push_back({V,
                                      UseAddr,
                                      UseSeq,
                                      {{Node->Literal, InvalidVA, -1, false}}});
            }
            break;
          }
        }
        return Finish(Node);
      }
      if (!consumeGuardBuildWork(/*query + one alternative=*/2))
        return Finish({});
      const bool DefinedLane = Def.Output.Size != V.Size;
      const NdVar DefinedValue = DefinedLane ? V : Def.Output;
      Node->HasDef = !DefinedLane;
      Node->Def = Def;
      Node->DefQuery = ProofQueries.size();
      ProofQueries.push_back({
          V,
          UseAddr,
          UseSeq,
          {{DefinedValue, Def.Addr, Def.Seq, /*DefinedAtPoint=*/true}},
          /*AllowZeroExtension=*/false,
          /*AllowSignExtension=*/false,
      });
      if (UseDefinedAlternativesAsRoots)
        ProofQueries.back().UseDefinedAlternativesAsOccurrenceRoots = true;

      // Rooting a narrow lane at a complete extension writer preserves cheap
      // occurrence identity, but hides any further range known by its input
      // producer (for example, a bit count). Retain the original query as a
      // fallback, only to be evaluated if the occurrence proof cannot bind
      // the index. Expanding it eagerly would revisit unrelated deep inputs
      // even when the exact lane already proves the guard.  A masked value
      // has no occurrence identity with a mask the table use recomputes from
      // the wider register (a W compare before an X index); only the value
      // comparison sees that, so it gets the same fallback.  `ubfx xD` writes
      // that mask to the whole X register and `cmp wD` reads its low lane, so
      // the lane-sized occurrence also hides the mask.
      const bool ExtensionWriter =
          DefinedLane &&
          (Def.Opcode == NdOp::INT_ZEXT || Def.Opcode == NdOp::INT_SEXT) &&
          Def.NumInputs == 1 && Def.Inputs[0].Size > V.Size &&
          V.Offset == Def.Output.Offset;
      const bool MaskedDefinition =
          Def.Opcode == NdOp::INT_AND && Def.NumInputs == 2 &&
          Def.Inputs[0].isConst() != Def.Inputs[1].isConst();
      if (ExtensionWriter || MaskedDefinition) {
        if (!consumeGuardBuildWork(IndexAlternatives.size() + 2))
          return Finish({});
        ExtendedLaneQueries.emplace_back(Node, ProofQueries[Node->IndexQuery]);
      }

      // Prove index identity through the same immutable definition from both
      // ends.  This avoids recursively expanding unrelated wide inputs merely
      // to establish that the narrow value produced here is the table index:
      // one query proves the canonical table use reaches Def, and DefQuery
      // proves the guard use reaches Def.  The initial query already owns at
      // least one alternative, so narrowing it in place allocates nothing.
      JumpTableValueQuery &IndexQuery = ProofQueries[Node->IndexQuery];
      IndexQuery.Candidate = Info.IndexValueAtUse;
      IndexQuery.UseAddr = Info.IndexUseAddr;
      IndexQuery.UseSeq = Info.IndexUseSeq;
      IndexQuery.Alternatives.resize(1);
      IndexQuery.Alternatives.front() = {DefinedValue, Def.Addr, Def.Seq,
                                         /*DefinedAtPoint=*/true};
      IndexQuery.AllowZeroExtension = true;
      IndexQuery.AllowSignExtension = true;
      IndexQuery.UseDefinedAlternativesAsOccurrenceRoots = true;
      ProofQueries[Node->DefQuery].UseDefinedAlternativesAsOccurrenceRoots =
          true;
      Node->IndexQueryUsesDefinition = true;
      // A contained architectural lane is an identity candidate, not a
      // narrower version of the producer's arithmetic. Both reaching-value
      // queries must prove that very lane; otherwise this leaf stays unknown.
      if (DefinedLane) {
        // The complete writer's occurrence hides an extension it copies, so
        // an ECX guard is not tied to the RCX index it sign-extends.  Keep a
        // second pair of the same two queries rooted at the value the lane
        // carries unchanged; it runs only when this pair proves nothing.
        if (const auto Source = extendedLaneSource(DefIndex, V)) {
          if (!consumeGuardBuildWork(/*two queries + alternatives=*/4))
            return Finish({});
          JumpTableValueQuery IndexFallback = ProofQueries[Node->IndexQuery];
          IndexFallback.Alternatives.front() = *Source;
          JumpTableValueQuery DefFallback = ProofQueries[Node->DefQuery];
          DefFallback.Alternatives.front() = *Source;
          LaneSourceQueries.push_back(
              {Node, std::move(IndexFallback), std::move(DefFallback)});
        }
        return Finish(Node);
      }
      for (int I = 0; I < Def.NumInputs; ++I) {
        // Charge the input scan independently from recursive collection.
        if (!consumeGuardBuildWork())
          return Finish({});
        GuardSyntaxPtr Input =
            Collect(Def.Inputs[I], DefIndex - 1, Def.Addr, Def.Seq, Depth + 1);
        if (!Input) {
          if (GuardBuildExhausted)
            return Finish({});
          // The current narrow value may itself be the exact table-index
          // occurrence.  Preserve that query even when expanding an unrelated
          // upstream lane would require an unsupported wide expression; if the
          // exact query does not match, Materialize still refuses this partial
          // syntax node instead of treating the missing input as evidence.
          Node->InputsComplete = false;
          break;
        }
        if (!consumeGuardBuildWork())
          return Finish({});
        if (Input->ContainsStructuralIndex)
          Node->ContainsStructuralIndex = true;
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

  // Resolve the syntax leaves and def edges in one CFG/lane query session.
  // Its ResolverFlowGraph, memo tables, and MatchBudget are shared across the
  // entire guard DAG; the outer evidence budget bounds collection and result
  // materialisation, so no node can restart a whole-function proof.  Preserve
  // per-query completeness as well: an unrelated controlling condition may
  // exceed the recursive value depth after fixed-point graph growth, while a
  // separate guard still proves a complete (and therefore conservative)
  // selector domain.
  auto ResolveGuardQueries =
      [&](const std::vector<JumpTableValueQuery> &Queries,
          std::vector<bool> &QueryComplete, bool &Complete, size_t *Budget) {
        return tableValuesMatchAtUses(
            Queries, &Complete, &QueryComplete, InvalidVA, nullptr, Budget,
            /*LocalMatchEvidenceLimit=*/0,
            /*CandidateBranchesSharingTargets=*/nullptr,
            /*QueryUnsignedFeasibleMasks=*/nullptr, GuardValueDepth,
            CertifiedEdgeOverrides, /*UseGroupContext=*/true, &QueryScope);
      };
  // The compare that names the index lane is a structural leaf. Other
  // controlling conditions still need value queries. One session over every
  // condition can spend the candidate account before that compare is built,
  // and a short result is not a fact about the compare. Run the session
  // against a copy when the compare is already in hand. A copy that cannot
  // finish leaves the candidate balance for the compare's own queries.
  bool HasStructuralCompare = false;
  std::vector<size_t> StructuralQueryIndices;
  {
    std::vector<char> StructuralQuery(ProofQueries.size(), 0);
    std::function<void(const GuardSyntaxPtr &)> Mark =
        [&](const GuardSyntaxPtr &Node) {
          if (!Node || !Node->ContainsStructuralIndex)
            return;
          auto markQuery = [&](size_t Query) {
            if (Query < StructuralQuery.size())
              StructuralQuery[Query] = 1;
          };
          // A parent expands through the in-block def Collect already bound.
          // Its optional "is this whole value the index?" query stays with
          // the other guards. Only the structural leaf's own query, and each
          // def or literal on the path to that leaf, is the compare.
          if (Node->StructuralIndex)
            markQuery(Node->IndexQuery);
          markQuery(Node->DefQuery);
          markQuery(Node->LiteralQuery);
          for (const GuardSyntaxPtr &Input : Node->Inputs)
            Mark(Input);
        };
    for (const auto &[Root, TableCondition] : GuardRoots) {
      (void)TableCondition;
      if (!Root || !Root->ContainsStructuralIndex)
        continue;
      HasStructuralCompare = true;
      Mark(Root);
    }
    for (size_t Index = 0; Index < StructuralQuery.size(); ++Index)
      if (StructuralQuery[Index])
        StructuralQueryIndices.push_back(Index);
  }
  bool ProofComplete = false;
  std::vector<bool> ProofQueryComplete;
  std::vector<bool> ProofResults;
  bool AbandonedUnrelatedQueries = false;
  auto assignBatch = [&](std::vector<bool> Results, std::vector<bool> Complete,
                         bool BatchComplete) {
    ProofResults = std::move(Results);
    ProofQueryComplete = std::move(Complete);
    ProofComplete = BatchComplete;
  };
  if (!ProofQueries.empty() && HasStructuralCompare) {
    size_t SideBudget = *CandidateEvidenceBudget;
    bool BatchComplete = false;
    std::vector<bool> BatchQueryComplete;
    const std::vector<bool> BatchResults = ResolveGuardQueries(
        ProofQueries, BatchQueryComplete, BatchComplete, &SideBudget);
    const bool BatchUsable = BatchResults.size() == ProofQueries.size() &&
                             BatchQueryComplete.size() == ProofQueries.size() &&
                             SideBudget != 0;
    if (BatchUsable) {
      *CandidateEvidenceBudget = SideBudget;
      assignBatch(std::move(BatchResults), std::move(BatchQueryComplete),
                  BatchComplete);
    } else {
      AbandonedUnrelatedQueries = true;
      ProofResults.assign(ProofQueries.size(), false);
      ProofQueryComplete.assign(ProofQueries.size(), false);
      ProofComplete = false;
      if (!StructuralQueryIndices.empty()) {
        std::vector<JumpTableValueQuery> StructuralQueries;
        StructuralQueries.reserve(StructuralQueryIndices.size());
        for (size_t Index : StructuralQueryIndices)
          StructuralQueries.push_back(ProofQueries[Index]);
        bool StructuralComplete = false;
        std::vector<bool> StructuralQueryComplete;
        const std::vector<bool> StructuralResults =
            ResolveGuardQueries(StructuralQueries, StructuralQueryComplete,
                                StructuralComplete, CandidateEvidenceBudget);
        if (StructuralResults.size() != StructuralQueries.size() ||
            StructuralQueryComplete.size() != StructuralQueries.size()) {
          if (SawControllingGuard)
            Info.IncompleteGuardDomain = true;
          return false;
        }
        for (size_t I = 0; I < StructuralQueryIndices.size(); ++I) {
          const size_t Index = StructuralQueryIndices[I];
          ProofResults[Index] = StructuralResults[I];
          ProofQueryComplete[Index] = StructuralQueryComplete[I];
        }
        ProofComplete = StructuralComplete;
      }
    }
  } else if (!ProofQueries.empty()) {
    ProofResults = ResolveGuardQueries(ProofQueries, ProofQueryComplete,
                                       ProofComplete, CandidateEvidenceBudget);
    if (ProofResults.size() != ProofQueries.size() ||
        ProofQueryComplete.size() != ProofQueries.size()) {
      if (SawControllingGuard)
        Info.IncompleteGuardDomain = true;
      return false;
    }
  }
  // Preserve the fast exact-occurrence path for narrow guards over arbitrarily
  // deep arithmetic. Only its unmatched extension lanes need their input's
  // value facts. Batch those fallback queries as well, retaining the same
  // graph context, shared evidence budget and per-query completeness rules.
  // An abandoned unrelated session has not proved those lanes; running them
  // again would spend the account the structural compare still needs.
  std::vector<JumpTableValueQuery> FallbackQueries;
  if (!AbandonedUnrelatedQueries) {
    for (auto &[Node, Query] : ExtendedLaneQueries) {
      if (!consumeGuardBuildWork(2))
        return failIncomplete();
      if (ProofQueryComplete[Node->IndexQuery] &&
          ProofResults[Node->IndexQuery] &&
          ProofQueryComplete[Node->DefQuery] && ProofResults[Node->DefQuery])
        continue;
      Node->ExtendedLaneQuery = ProofResults.size() + FallbackQueries.size();
      FallbackQueries.push_back(std::move(Query));
    }
  }
  if (!FallbackQueries.empty()) {
    bool FallbackComplete = false;
    std::vector<bool> FallbackQueryComplete;
    const auto FallbackResults =
        ResolveGuardQueries(FallbackQueries, FallbackQueryComplete,
                            FallbackComplete, CandidateEvidenceBudget);
    ProofComplete &= FallbackComplete;
    if (FallbackResults.size() != FallbackQueries.size() ||
        FallbackQueryComplete.size() != FallbackQueries.size() ||
        !consumeGuardBuildWork(FallbackQueries.size()) ||
        !consumeGuardBuildWork(FallbackQueries.size()))
      return failIncomplete();
    ProofResults.insert(ProofResults.end(), FallbackResults.begin(),
                        FallbackResults.end());
    ProofQueryComplete.insert(ProofQueryComplete.end(),
                              FallbackQueryComplete.begin(),
                              FallbackQueryComplete.end());
  }
  // Lane-source identities only add evidence: they run for lanes no query
  // above bound, and a pair that fails or cannot complete leaves its guard
  // unbound and the proof's completeness as it was without them.  Only
  // exhausting the shared guard budget fails the proof, as everywhere else.
  std::vector<JumpTableValueQuery> LaneQueries;
  std::vector<GuardSyntaxNode *> LaneNodes;
  for (LaneSourceFallback &Fallback : LaneSourceQueries) {
    GuardSyntaxNode &Node = *Fallback.Node;
    if (!consumeGuardBuildWork(2))
      return failIncomplete();
    const bool Bound =
        (ProofQueryComplete[Node.IndexQuery] && ProofResults[Node.IndexQuery] &&
         ProofQueryComplete[Node.DefQuery] && ProofResults[Node.DefQuery]) ||
        (Node.ExtendedLaneQuery < ProofResults.size() &&
         ProofQueryComplete[Node.ExtendedLaneQuery] &&
         ProofResults[Node.ExtendedLaneQuery]);
    if (Bound)
      continue;
    LaneNodes.push_back(&Node);
    LaneQueries.push_back(std::move(Fallback.Index));
    LaneQueries.push_back(std::move(Fallback.Def));
  }
  if (!LaneQueries.empty()) {
    bool LaneComplete = false;
    std::vector<bool> LaneQueryComplete;
    const auto LaneResults = ResolveGuardQueries(
        LaneQueries, LaneQueryComplete, LaneComplete, CandidateEvidenceBudget);
    if (LaneResults.size() != LaneQueries.size() ||
        LaneQueryComplete.size() != LaneQueries.size() ||
        !consumeGuardBuildWork(LaneQueries.size()))
      return failIncomplete();
    for (size_t I = 0; I < LaneNodes.size(); ++I) {
      LaneNodes[I]->LaneSourceIndexQuery = ProofResults.size();
      ProofResults.push_back(LaneResults[2 * I]);
      ProofQueryComplete.push_back(LaneQueryComplete[2 * I]);
      LaneNodes[I]->LaneSourceDefQuery = ProofResults.size();
      ProofResults.push_back(LaneResults[2 * I + 1]);
      ProofQueryComplete.push_back(LaneQueryComplete[2 * I + 1]);
    }
  }
  std::map<const GuardSyntaxNode *, GuardExprPtr> ExprMemo;
  // The syntax leaf behind each materialized index leaf, for the narrow-lane
  // extension check after the solver.
  std::map<const GuardExpr *, const GuardSyntaxNode *> IndexLeafSources;
  std::set<const GuardSyntaxNode *> ActiveExprs;
  bool SawIncompleteProofQuery = false;
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
      // An empty result caused by a spent account is not the expression.
      // Remembering it would make a later walk of this compare fail closed.
      if (Result || !GuardBuildExhausted)
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
    const bool IndexQueryPresent = Node->IndexQuery < ProofResults.size() &&
                                   Node->IndexQuery < ProofQueryComplete.size();
    const bool IndexQueryComplete =
        IndexQueryPresent && ProofQueryComplete[Node->IndexQuery];
    const bool DefQueryPresent = Node->DefQuery < ProofResults.size() &&
                                 Node->DefQuery < ProofQueryComplete.size();
    const bool DefQueryComplete =
        DefQueryPresent && ProofQueryComplete[Node->DefQuery];
    const bool ExtendedLanePresent =
        Node->ExtendedLaneQuery < ProofResults.size();
    const bool ExtendedLaneComplete =
        ExtendedLanePresent && ProofQueryComplete[Node->ExtendedLaneQuery];
    // Both lane-source identities must hold, exactly like IndexQuery with
    // its DefQuery.
    const bool LaneSourceMatches =
        Node->LaneSourceIndexQuery < ProofResults.size() &&
        Node->LaneSourceDefQuery < ProofResults.size() &&
        ProofQueryComplete[Node->LaneSourceIndexQuery] &&
        ProofResults[Node->LaneSourceIndexQuery] &&
        ProofQueryComplete[Node->LaneSourceDefQuery] &&
        ProofResults[Node->LaneSourceDefQuery];
    const bool IndexMatches =
        Node->StructuralIndex ||
        (IndexQueryComplete && ProofResults[Node->IndexQuery] &&
         (!Node->IndexQueryUsesDefinition ||
          (DefQueryComplete && ProofResults[Node->DefQuery]))) ||
        (ExtendedLaneComplete && ProofResults[Node->ExtendedLaneQuery]) ||
        LaneSourceMatches;
    if (IndexMatches) {
      Expr->K = GuardExpr::Kind::Index;
      Expr->ContainsIndex = true;
      IndexLeafSources.emplace(Expr.get(), Node.get());
      return Finish(Expr);
    }
    const bool LiteralQueryPresent =
        Node->LiteralQuery < ProofResults.size() &&
        Node->LiteralQuery < ProofQueryComplete.size();
    const bool LiteralQueryComplete =
        LiteralQueryPresent && ProofQueryComplete[Node->LiteralQuery];
    if (LiteralQueryComplete && ProofResults[Node->LiteralQuery]) {
      Expr->K = GuardExpr::Kind::Constant;
      Expr->Constant = Node->Literal.Offset;
      return Finish(Expr);
    }
    // The lexical def is only syntax.  Its exact output must be the value
    // reaching this exact use in the shared lane-aware resolver; an EAX/W
    // write therefore blocks a stale older RAX/X definition here.  When only
    // the optional "is this whole subtree the index?" query is incomplete, a
    // complete def query still permits exact expansion into its children.  No
    // incomplete result is reinterpreted as false or as an index identity.
    if (!Node->HasDef || !Node->InputsComplete) {
      if (!IndexQueryComplete ||
          (Node->IndexQueryUsesDefinition && !DefQueryComplete) ||
          (ExtendedLanePresent && !ExtendedLaneComplete) ||
          (LiteralQueryPresent && !LiteralQueryComplete))
        SawIncompleteProofQuery = true;
      return Finish({});
    }
    if (!DefQueryPresent || !DefQueryComplete) {
      SawIncompleteProofQuery = true;
      return Finish({});
    }
    if (!ProofResults[Node->DefQuery])
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
  // The compare that names the index lane is a small tree. Other controlling
  // guards can be large enough to spend the syntax account before that
  // compare is built. Build the compare first. An unfinished later guard is
  // not a fact about the compare: keep the compare and stop the scan.
  std::stable_partition(GuardRoots.begin(), GuardRoots.end(),
                        [](const std::pair<GuardSyntaxPtr, bool> &Root) {
                          return Root.first &&
                                 Root.first->ContainsStructuralIndex;
                        });
  auto completeIndexGuard = [&]() {
    for (const MaterializedGuard &Guard : MaterializedGuards)
      if (Guard.Expr && Guard.Expr->ContainsIndex)
        return true;
    return false;
  };
  // Refill the ordinary syntax account so the compare already in hand can
  // be symbolized. This is not a higher ceiling, and the scan does not resume.
  auto abandonUnrelatedGuard = [&](const GuardSyntaxPtr &Root) {
    if (!completeIndexGuard() || !Root || Root->ContainsStructuralIndex)
      return false;
    // Syntax work and the candidate account are different ceilings. Refilling
    // syntax after the candidate account is empty would keep a guard the
    // evidence no longer supports.
    if (*CandidateEvidenceBudget == 0)
      return false;
    GuardBuildExhausted = false;
    GuardBuildWork = limits::kMaxJumpTableEvidenceWork;
    return true;
  };
  bool AbandonedUnrelatedGuards = false;
  for (const auto &[Root, TableCondition] : GuardRoots) {
    if (AbandonedUnrelatedGuards)
      break;
    if (!consumeGuardBuildWork()) {
      if (abandonUnrelatedGuard(Root)) {
        AbandonedUnrelatedGuards = true;
        break;
      }
      return failIncomplete();
    }
    // A later controlling guard that does not name the index is not a tighter
    // bound. Its syntax still counts against the ceiling the compare's solver
    // formula has to pay. Remember the ceiling and restore it when that guard
    // adds no index constraint. The candidate account keeps the walk's charge.
    const bool HaveIndexGuard = completeIndexGuard();
    const size_t SyntaxBefore = GuardBuildWork;
    const bool ExhaustedBefore = GuardBuildExhausted;
    GuardExprPtr Expr = Materialize(Root, /*Depth=*/0);
    if (HaveIndexGuard && Root && !Root->ContainsStructuralIndex &&
        (!Expr || !Expr->ContainsIndex)) {
      GuardBuildWork = SyntaxBefore;
      GuardBuildExhausted = ExhaustedBefore;
      continue;
    }
    // Other guards can spend the account while they are collected. The
    // compare is already a syntax tree. Walk that tree once on the ordinary
    // account. A second miss stays fail-closed, and the scan does not resume.
    if (GuardBuildExhausted && Root && Root->ContainsStructuralIndex &&
        !completeIndexGuard()) {
      GuardBuildExhausted = false;
      GuardBuildWork = limits::kMaxJumpTableEvidenceWork;
      Expr = Materialize(Root, /*Depth=*/0);
    }
    if (GuardBuildExhausted) {
      if (abandonUnrelatedGuard(Root)) {
        AbandonedUnrelatedGuards = true;
        break;
      }
      return failIncomplete();
    }
    // Record the exact source-lane width of this constraint.  Constraints over
    // different views cannot share one unconstrained symbolic variable; the
    // coherent view used for the proof is selected below.
    std::optional<uint16_t> Width = indexWidth(Expr);
    if (GuardBuildExhausted && Root && Root->ContainsStructuralIndex &&
        !completeIndexGuard()) {
      GuardBuildExhausted = false;
      GuardBuildWork = limits::kMaxJumpTableEvidenceWork;
      Width = indexWidth(Expr);
    }
    if (GuardBuildExhausted) {
      if (abandonUnrelatedGuard(Root)) {
        AbandonedUnrelatedGuards = true;
        break;
      }
      return failIncomplete();
    }
    if (Expr && Expr->ContainsIndex && Width && *Width != 0 &&
        *Width <= Info.IndexValueAtUse.Size) {
      if (!consumeGuardBuildWork()) {
        if (abandonUnrelatedGuard(Root)) {
          AbandonedUnrelatedGuards = true;
          break;
        }
        return failIncomplete();
      }
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

  // Literals only propose the envelope. The query proves a dense prefix on
  // every incoming path and keeps each edge constraint. A dominating
  // equality that is not itself that prefix stays one of those constraints.
  enum class JoinedPrefix : uint8_t {
    Skipped,
    Proved,
    QueryIncomplete,
    BudgetExhausted
  };
  auto tryJoinedPredecessorDomain = [&]() -> JoinedPrefix {
    if (GuardedGroupProofContext || Info.IndexValueDefinedAtUse)
      return JoinedPrefix::Skipped;
    for (const auto &[Addr, Insn] : Insns) {
      if (!consumeCandidateEvidence())
        return JoinedPrefix::BudgetExhausted;
      for (const LowOp &Op : Insn.Ops) {
        if (!consumeCandidateEvidence())
          return JoinedPrefix::BudgetExhausted;
        if (Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_EQUAL &&
            Op.Opcode != NdOp::INT_NOTEQUAL && Op.Opcode != NdOp::INT_LESS &&
            Op.Opcode != NdOp::INT_LESSEQUAL && Op.Opcode != NdOp::INT_SLESS &&
            Op.Opcode != NdOp::INT_SLESSEQUAL)
          continue;
        if (Op.NumInputs != 2) {
          SawIncompleteProofQuery = true;
          return JoinedPrefix::QueryIncomplete;
        }
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          if (!consumeCandidateEvidence())
            return JoinedPrefix::BudgetExhausted;
          const NdVar &Value = Op.Inputs[I];
          if (isGuardLiteral(Value) && Value.Size != 0 &&
              Value.Size <= sizeof(uint64_t) &&
              Value.Offset < std::numeric_limits<uint64_t>::digits)
            ProposedJoinedBound =
                std::max(ProposedJoinedBound, Value.Offset + 1);
        }
      }
    }
    // Provisional group graphs keep their separate universal guard proof;
    // their hypothesized edges cannot seed an existential finite domain.
    if (ProposedJoinedBound < limits::kMinJumpTableEntries)
      return JoinedPrefix::Skipped;
    if (!consumeGuardBuildWork(12))
      return JoinedPrefix::BudgetExhausted;
    JumpTableValueQuery Joined;
    Joined.Candidate = Info.IndexValueAtUse;
    Joined.UseAddr = Info.IndexUseAddr;
    Joined.UseSeq = Info.IndexUseSeq;
    Joined.Relation = JumpTableValueRelation::JoinedUnsignedFeasibleSet;
    Joined.UnsignedUpperBound = ProposedJoinedBound;
    std::vector<bool> Complete;
    std::vector<uint64_t> Masks;
    const auto Results = tableValuesMatchAtUses(
        {Joined}, nullptr, &Complete, InvalidVA, nullptr,
        CandidateEvidenceBudget, 0, nullptr, &Masks, GuardValueDepth,
        CertifiedEdgeOverrides, /*UseGroupContext=*/true, &QueryScope);
    // The ordinary table representation requires a dense zero-based
    // prefix. A two-level index table permits holes: its unreached index
    // bytes are unused, so its last reaching value bounds that table.
    // The proved mask, not the literal envelope, is the bound.
    if (Results.size() == 1 && Complete.size() == 1 && Complete[0] &&
        Masks.size() == 1 && Results[0] && Masks[0] &&
        ((Masks[0] & (Masks[0] + 1)) == 0 || AllowSparseDomain)) {
      const uint32_t Bound = std::bit_width(Masks[0]);
      // A prefix bounds this use but does not shrink a relocation-owned
      // object whose remaining slots may serve another dispatch.
      const bool PrefixOfOwnedObject =
          Info.RelocAbsolute && Bound < Info.PhysicalCapacity;
      if (Bound >= limits::kMinJumpTableEntries && !PrefixOfOwnedObject) {
        if (Info.MaxEntries == 0 || Bound < Info.MaxEntries)
          Info.MaxEntries = Bound;
        Info.IncompleteGuardDomain = false;
        Info.SemanticGuardDomainAmbiguous = false;
        return JoinedPrefix::Proved;
      }
    }
    if (Complete.size() != 1 || !Complete[0]) {
      SawIncompleteProofQuery = true;
      return JoinedPrefix::QueryIncomplete;
    }
    return JoinedPrefix::Skipped;
  };

  if (IndexGuards.empty()) {
    // Separate predecessor arms can each guard the same selector without a
    // single condition dominating the LOAD. Their literals only propose an
    // envelope; the joined query below must still prove it on every path.
    // A loop may check its initial and recurrent index separately. No one
    // dominating comparison then names the complete merged value. Reuse the
    // shared finite-domain proof over that exact use, including every current
    // predecessor and root. The normal candidate replay still authenticates
    // later graph growth before publishing targets. Never infer this envelope
    // from table capacity or omit an unguarded incoming path.
    switch (tryJoinedPredecessorDomain()) {
    case JoinedPrefix::Proved:
      return true;
    case JoinedPrefix::BudgetExhausted:
      return failIncomplete();
    case JoinedPrefix::QueryIncomplete:
    case JoinedPrefix::Skipped:
      break;
    }
    // A bounded lookup loop can read its selector from immutable records
    // without comparing the selector itself. Explore exact scalar states on
    // the same rooted graph; neither adjacent bytes nor a physical table run
    // supplies its bound. Normal candidate replay rechecks newly found edges.
    if (!GuardedGroupProofContext && !Info.IndexValueDefinedAtUse &&
        CurrentImg && supportsImmutableImageReads(*CurrentImg)) {
      JumpTableValueQuery Lookup;
      Lookup.Candidate = Info.IndexValueAtUse;
      Lookup.UseAddr = Info.IndexUseAddr;
      Lookup.UseSeq = Info.IndexUseSeq;
      Lookup.Relation = JumpTableValueRelation::ReadonlyUnsignedEnvelope;
      Lookup.UnsignedUpperBound = 64;
      std::vector<bool> Complete;
      std::vector<uint64_t> Masks;
      const auto Results = tableValuesMatchAtUses(
          {Lookup}, nullptr, &Complete, InvalidVA, nullptr,
          CandidateEvidenceBudget, 0, nullptr, &Masks, GuardValueDepth,
          CertifiedEdgeOverrides, /*UseGroupContext=*/true, &QueryScope);
      if (Results.size() == 1 && Complete.size() == 1 && Complete[0] &&
          Masks.size() == 1 && Results[0] && Masks[0] &&
          (Masks[0] & (Masks[0] + 1)) == 0) {
        const uint32_t Bound = std::bit_width(Masks[0]);
        if (Bound >= limits::kMinJumpTableEntries) {
          if (Info.MaxEntries == 0 || Bound < Info.MaxEntries)
            Info.MaxEntries = Bound;
          Info.IncompleteGuardDomain = false;
          Info.SemanticGuardDomainAmbiguous = false;
          return true;
        }
      }
      if (Complete.size() != 1 || !Complete[0])
        SawIncompleteProofQuery = true;
    }
    // Dropping an unrelated guard only enlarges the candidate domain and is
    // conservative when another complete guard was materialized.  With no
    // complete index guard, however, an incomplete leaf/def query must remain
    // a resource/analysis failure rather than a semantic rejection that could
    // later be lowered to a callback tail call.
    if (SawControllingGuard && (SawIncompleteProofQuery || !ProofComplete))
      Info.IncompleteGuardDomain = true;
    else if (SawControllingGuard)
      Info.SemanticGuardDomainAmbiguous = true;
    return false;
  }

  // All dominating table-reaching guards constrain the same runtime path.
  // First prove their conjunction: a signed
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
    if (Expr->Inputs.size() > (std::numeric_limits<size_t>::max() - Work) / 4)
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
    if (!Condition)
      return failSemantic();
    if (!consumeGuardBuildWork(/*zero + ne + optional not + retention=*/4))
      return failIncomplete();
    SymRef NonZero = Symbolic.Ctx.mkNe(
        *Condition, Symbolic.Ctx.mkZero(Symbolic.Ctx.width(*Condition)));
    SymbolicConstraints.push_back(TableCondition ? NonZero
                                                 : Symbolic.Ctx.mkNot(NonZero));
  }
  if (!Symbolic.Index || Symbolic.IndexSize == 0 ||
      Symbolic.IndexSize > sizeof(uint64_t))
    return failSemantic();
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
    if (Expr->Inputs.size() > (std::numeric_limits<size_t>::max() - Work) / 3)
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
        *GuardWork > std::numeric_limits<size_t>::max() - ConcreteWorkPerValue)
      return failIncomplete();
    ConcreteWorkPerValue += *GuardWork;
  }

  // `(zext(load or register) >> S) ^ C` is exactly 0..2^R-1 when the shift
  // is logical, the input was not already narrowed by a mask or an earlier
  // shift, and C stays inside that power of two. The compare still decides
  // which of those values reach. Capping the sample without that fact would
  // turn an interior hole into a suffix. An unfinished walk leaves the
  // width window unchanged and does not withdraw the compare.
  uint32_t DefinedPrefix = 0;
  {
    auto blockOf = [&](va_t Addr) -> std::optional<va_t> {
      auto It = BlockStarts.upper_bound(Addr);
      if (It == BlockStarts.begin())
        return std::nullopt;
      --It;
      return *It;
    };
    const auto IndexBlock = blockOf(Info.IndexUseAddr);
    if (IndexBlock && CurrentImg) {
      const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
      auto sameVar = [](const NdVar &A, const NdVar &B) {
        return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
      };
      auto overlaps = [&](const NdVar &A, const NdVar &B) {
        if (A.Size == 0 || B.Size == 0)
          return false;
        if (A.isReg() && B.isReg()) {
          if (A.Offset == B.Offset)
            return true;
          return TRI.isSubRegOf(A.Offset, A.Size, B.Offset, B.Size) ||
                 TRI.isSubRegOf(B.Offset, B.Size, A.Offset, A.Size);
        }
        if (!A.isTemp() || !B.isTemp() || A.Space != B.Space)
          return false;
        if (A.Offset > InvalidVA - A.Size || B.Offset > InvalidVA - B.Size)
          return true;
        return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
      };
      auto uniquePred = [&](va_t Block) -> std::optional<va_t> {
        std::set<va_t> Preds;
        const InsnRecord *Pending = nullptr;
        va_t PendingBlock = 0;
        bool Failed = false;
        auto consider = [&](const InsnRecord &Term, va_t From) {
          if (Failed)
            return;
          const bool Sized =
              Term.Size != 0 && Term.Addr <= InvalidVA - Term.Size;
          const va_t Fall = Sized ? Term.Addr + Term.Size : InvalidVA;
          bool Hits = false;
          bool Falls = false;
          if (Term.IsRet && Term.IsCond && Term.IsBranch) {
            Hits = Term.BranchTarget == Block;
          } else if (Term.IsRet) {
            return;
          } else if (Term.IsBranch && Term.IsIndirect) {
            Falls = Term.IsCond && Sized;
            for (va_t Target : Term.JumpTableTargets) {
              if (!tryCandidateEvidence()) {
                Failed = true;
                return;
              }
              Hits = Hits || Target == Block;
            }
          } else if (Term.IsBranch && !Term.IsIndirect && !Term.IsCall) {
            Falls = Term.IsCond && Sized;
            Hits = Term.BranchTarget == Block;
          } else if ((!Term.IsBranch || Term.IsCall) &&
                     (!Term.IsNoReturnCall || Term.IsCond)) {
            Falls = Sized;
          }
          if ((Falls && Fall == Block) || Hits)
            Preds.insert(From);
        };
        for (const auto &[Addr, Insn] : Insns) {
          if (!tryCandidateEvidence())
            return std::nullopt;
          const auto At = blockOf(Addr);
          if (!At)
            return std::nullopt;
          if (Pending && *At != PendingBlock)
            consider(*Pending, PendingBlock);
          if (Failed)
            return std::nullopt;
          Pending = &Insn;
          PendingBlock = *At;
        }
        if (Pending)
          consider(*Pending, PendingBlock);
        if (Failed || Preds.size() != 1)
          return std::nullopt;
        return *Preds.begin();
      };
      struct DefHit {
        const LowOp *Op = nullptr;
        const InsnRecord *Insn = nullptr;
        bool Clobber = false;
        bool CallBefore = false;
        bool Budget = false;
      };
      auto findDef = [&](va_t Block, const NdVar &Want, va_t BeforeAddr,
                         int BeforeSeq, bool Entire) -> DefHit {
        DefHit Hit;
        bool CallAfter = false;
        for (auto It = Insns.lower_bound(Block); It != Insns.end(); ++It) {
          if (!tryCandidateEvidence()) {
            Hit.Budget = true;
            return Hit;
          }
          const auto At = blockOf(It->first);
          if (!At || *At != Block)
            break;
          const bool InsnBefore = Entire || It->first < BeforeAddr ||
                                  (It->first == BeforeAddr && BeforeSeq > 0);
          if (!Entire && It->first > BeforeAddr)
            break;
          if (It->second.IsCall && InsnBefore && Hit.Op)
            CallAfter = true;
          if (It->second.IsCall && InsnBefore && !Hit.Op)
            Hit.CallBefore = true;
          for (const LowOp &Op : It->second.Ops) {
            if (!tryCandidateEvidence()) {
              Hit.Budget = true;
              return Hit;
            }
            if (!Entire && (Op.Addr > BeforeAddr ||
                            (Op.Addr == BeforeAddr && Op.Seq >= BeforeSeq)))
              continue;
            const bool Same = sameVar(Op.Output, Want);
            const bool LaneExtend =
                Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
                sameVar(Op.Inputs[0], Want) && Op.Output.isReg() &&
                Op.Output.Offset == Want.Offset && Op.Output.Size > Want.Size;
            if (Same) {
              Hit.Op = &Op;
              Hit.Insn = &It->second;
              Hit.Clobber = false;
              Hit.CallBefore = false;
              CallAfter = false;
              continue;
            }
            if (overlaps(Op.Output, Want) && !LaneExtend) {
              Hit.Op = nullptr;
              Hit.Insn = nullptr;
              Hit.Clobber = true;
            }
          }
        }
        if (CallAfter)
          Hit.Clobber = true;
        return Hit;
      };
      NdVar Value = Info.IndexValueAtUse;
      va_t BeforeAddr = Info.IndexUseAddr;
      int BeforeSeq = Info.IndexUseSeq;
      va_t Block = *IndexBlock;
      bool Entire = false;
      bool SawXor = false;
      uint64_t XorBits = 0;
      bool SawShift = false;
      uint64_t Shift = 0;
      uint16_t Width = Value.Size;
      bool Abandoned = false;
      bool Finished = false;
      int Hops = 0;
      for (int Step = 0;
           Step < limits::kMaxSliceDepth && !Abandoned && !Finished; ++Step) {
        const DefHit Hit = findDef(Block, Value, BeforeAddr, BeforeSeq, Entire);
        if (Hit.Budget || Hit.Clobber) {
          Abandoned = true;
          break;
        }
        if (!Hit.Op) {
          // A call with no later def in this block sits between the value
          // and its use. Do not treat the register as a live-in, and do not
          // follow the predecessor across that call.
          if (Hit.CallBefore) {
            Abandoned = true;
            break;
          }
          if (SawShift) {
            Finished = true;
            break;
          }
          if (Hops >= 2) {
            Abandoned = true;
            break;
          }
          const std::optional<va_t> Pred = uniquePred(Block);
          if (!Pred) {
            Abandoned = true;
            break;
          }
          Block = *Pred;
          Entire = true;
          ++Hops;
          continue;
        }
        if (Hit.Insn && Hit.Insn->IsInstructionGuard) {
          Abandoned = true;
          break;
        }
        const LowOp &Op = *Hit.Op;
        if (Hit.Insn &&
            std::any_of(
                Hit.Insn->Ops.begin(), Hit.Insn->Ops.end(),
                [&](const LowOp &Merge) {
                  if (Merge.Opcode != NdOp::SELECT || Merge.NumInputs < 3)
                    return false;
                  const int Arms = std::min<int>(Merge.NumInputs, 3);
                  return std::any_of(Merge.Inputs + 1, Merge.Inputs + Arms,
                                     [&](const NdVar &Arm) {
                                       return sameVar(Arm, Op.Output);
                                     });
                })) {
          Abandoned = true;
          break;
        }
        auto take = [&](const NdVar &Input) {
          Value = Input;
          BeforeAddr = Op.Addr;
          BeforeSeq = Op.Seq;
          Entire = false;
          if (const auto At = blockOf(Op.Addr))
            Block = *At;
        };
        const bool Copy = Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
                          !Op.Inputs[0].isConst() &&
                          Op.Inputs[0].Size == Op.Output.Size;
        const bool Extend = Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
                            !Op.Inputs[0].isConst() && Op.Inputs[0].Size != 0 &&
                            Op.Inputs[0].Size < Op.Output.Size &&
                            Op.Inputs[0].Offset == Op.Output.Offset;
        if (!SawShift) {
          if (Copy || Extend) {
            take(Op.Inputs[0]);
            continue;
          }
          if (!SawXor && Op.Opcode == NdOp::INT_XOR && Op.NumInputs == 2) {
            const bool Left = Op.Inputs[0].isConst();
            const bool Right = Op.Inputs[1].isConst();
            if (Left == Right) {
              Abandoned = true;
              break;
            }
            const std::optional<uint64_t> Bits =
                scalarConstantBits(Op.Inputs[Left ? 0 : 1]);
            const NdVar &Source = Op.Inputs[Left ? 1 : 0];
            if (!Bits || Source.isConst()) {
              Abandoned = true;
              break;
            }
            SawXor = true;
            XorBits = *Bits;
            take(Source);
            continue;
          }
          if (Op.Opcode == NdOp::INT_RIGHT && Op.NumInputs >= 2 &&
              !Op.Inputs[0].isConst()) {
            const std::optional<uint64_t> Bits =
                scalarConstantBits(Op.Inputs[1]);
            const unsigned OutBits =
                Op.Output.Size == 0 ? 0u : unsigned(Op.Output.Size) * 8u;
            if (!Bits || *Bits == 0 || *Bits >= 64 ||
                (OutBits != 0 && *Bits >= OutBits) || Op.Inputs[0].Size == 0) {
              Abandoned = true;
              break;
            }
            SawShift = true;
            Shift = *Bits;
            Width = Op.Inputs[0].Size;
            take(Op.Inputs[0]);
            continue;
          }
          Abandoned = true;
          break;
        }
        if (Copy) {
          take(Op.Inputs[0]);
          continue;
        }
        if (Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
            Op.Inputs[0].Size != 0 && Op.Inputs[0].Size < Op.Output.Size) {
          Width = Op.Inputs[0].Size;
          take(Op.Inputs[0]);
          continue;
        }
        if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::CALL ||
            Op.Opcode == NdOp::INDIR_CALL) {
          if (Op.Output.Size != 0 && Op.Output.Size < Width)
            Width = Op.Output.Size;
          Finished = true;
          break;
        }
        Abandoned = true;
        break;
      }
      if (!Abandoned && Finished && SawShift && Width != 0 &&
          Width <= sizeof(uint64_t)) {
        const unsigned Bits = unsigned(Width) * 8u;
        if (Shift > 0 && Shift < Bits && Shift < 64 && Bits - Shift < 32) {
          const unsigned Remaining = Bits - static_cast<unsigned>(Shift);
          const uint32_t Count = uint32_t{1} << Remaining;
          if (Count >= limits::kMinJumpTableEntries &&
              Count <= limits::kMaxJumpTableEntries &&
              (!SawXor || XorBits < Count))
            DefinedPrefix = Count;
        }
      }
    }
  }
  bool DefinedPrefixCapped = false;
  if (DefinedPrefix != 0 && uint64_t(DefinedPrefix) - 1 < LastSample) {
    LastSample = uint64_t(DefinedPrefix) - 1;
    DefinedPrefixCapped = true;
  }
  if (!consumeCandidateProduct(ConcreteWorkPerValue,
                               static_cast<size_t>(LastSample) + 1))
    return failIncomplete();

  // The definitional prefix, when one was proved, replaced the width window
  // before the charge above. Partitioning still does not change that charge.
  std::vector<uint64_t> ChangePoints{0, LastSample + 1};
  bool Partitioned = true;
  for (const auto &[Expr, TableCondition] : IndexGuards) {
    (void)TableCondition;
    if (!guardChangePoints(Expr, LastSample, ChangePoints)) {
      Partitioned = false;
      break;
    }
  }
  if (Partitioned) {
    std::sort(ChangePoints.begin(), ChangePoints.end());
    ChangePoints.erase(std::unique(ChangePoints.begin(), ChangePoints.end()),
                       ChangePoints.end());
  }
  size_t Interval = 0;
  uint32_t FirstRejected = 0;
  bool SawRejected = false;
  bool IsDensePrefix = true;
  // A sparse domain (a two-level index table, whose unreached index bytes are
  // simply unused) is bounded by its last reaching value.
  std::optional<uint64_t> LastReaching;
  // Length of the open non-reaching run. A later reaching value closes it.
  uint64_t OpenGap = 0;
  // One excluded coordinate inside an unsigned range is the compiler's
  // earlier equality exit. The table still contains that slot. A longer
  // interior gap is a different selector and stays unresolved.
  bool IsolatedExclusion = false;
  for (uint64_t Value = 0; Value <= LastSample;) {
    const uint64_t Next = Partitioned ? ChangePoints[++Interval] : Value + 1;
    bool Reaches = true;
    for (const auto &[Expr, TableCondition] : IndexGuards) {
      std::optional<uint64_t> Condition = evaluateGuardExpr(Expr, Value);
      if (!Condition)
        return failSemantic();
      Reaches &= ((*Condition != 0) == TableCondition);
    }
    const uint64_t Span = Next - Value;
    if (Reaches) {
      if (OpenGap > 1 && !AllowSparseDomain)
        IsDensePrefix = false;
      if (OpenGap == 1 && !AllowSparseDomain)
        IsolatedExclusion = true;
      OpenGap = 0;
      LastReaching = Next - 1;
    } else {
      if (OpenGap > std::numeric_limits<uint64_t>::max() - Span)
        return failSemantic();
      OpenGap += Span;
      if (!SawRejected) {
        FirstRejected = static_cast<uint32_t>(Value);
        SawRejected = true;
      }
    }
    Value = Next;
  }
  // A stricter dominating predicate can cut holes in an otherwise dense
  // bound. Such holes do not invalidate a different dominating predicate's
  // independently proved prefix. Select one complete predicate and replay its
  // exact equivalence below; dropping the others only enlarges the domain.
  if (!AllowSparseDomain && !IsDensePrefix) {
    if (!consumeCandidateProduct(ConcreteWorkPerValue,
                                 static_cast<size_t>(LastSample) + 1) ||
        !consumeGuardBuildWork(IndexGuards.size()))
      return failIncomplete();
    std::optional<size_t> SelectedGuard;
    uint32_t SelectedBound = 0;
    for (size_t I = 0; I < IndexGuards.size(); ++I) {
      const auto &[Expr, TableCondition] = IndexGuards[I];
      size_t GuardInterval = 0;
      uint32_t Bound = 0;
      bool Rejected = false;
      bool Prefix = true;
      for (uint64_t Value = 0; Value <= LastSample;) {
        const uint64_t Next =
            Partitioned ? ChangePoints[++GuardInterval] : Value + 1;
        const std::optional<uint64_t> Condition =
            evaluateGuardExpr(Expr, Value);
        if (!Condition)
          return failSemantic();
        const bool Reaches = (*Condition != 0) == TableCondition;
        if (!Rejected && !Reaches) {
          Bound = static_cast<uint32_t>(Value);
          Rejected = true;
        } else if (Rejected && Reaches) {
          Prefix = false;
          break;
        }
        Value = Next;
      }
      if (Prefix && Rejected && Bound >= limits::kMinJumpTableEntries &&
          Bound <= limits::kMaxJumpTableEntries &&
          (!SelectedGuard || Bound < SelectedBound)) {
        SelectedGuard = I;
        SelectedBound = Bound;
      }
    }
    if (!SelectedGuard)
      return failSemantic();
    ReachesTable = SymbolicConstraints[*SelectedGuard];
    FirstRejected = SelectedBound;
    IsolatedExclusion = false;
  }
  // Under the definitional cap, `!= N` with N at the top of the prefix is a
  // suffix. The same comparison without the cap is an interior hole, and
  // EqualityExclusion would then include that hole. A hole that is still
  // interior after the cap stays unresolved. A prefix whose every sampled
  // value reaches is bounded by the prefix itself: the rejection sits just
  // past the last sample, as with `cmp #0xf; b.hi` on a 16-value shift.
  if (DefinedPrefixCapped && IsolatedExclusion)
    return failSemantic();
  if (DefinedPrefixCapped && !SawRejected) {
    FirstRejected = DefinedPrefix;
    SawRejected = true;
  }
  const bool SparseDomain = AllowSparseDomain && SawRejected && LastReaching &&
                            *LastReaching >= FirstRejected &&
                            *LastReaching < LastSample;
  const bool EqualityExclusion =
      !SparseDomain && IsolatedExclusion && LastReaching &&
      *LastReaching >= FirstRejected && *LastReaching < LastSample &&
      *LastReaching + 1 >= limits::kMinJumpTableEntries &&
      *LastReaching + 1 <= limits::kMaxJumpTableEntries;
  if (SparseDomain || EqualityExclusion)
    FirstRejected = static_cast<uint32_t>(*LastReaching + 1);
  if (!SawRejected || FirstRejected < limits::kMinJumpTableEntries ||
      FirstRejected > limits::kMaxJumpTableEntries) {
    if (SawControllingGuard)
      Info.SemanticGuardDomainAmbiguous = true;
    return false;
  }
  // Past the narrow lane's sign bit, sign- and zero-extension of that lane no
  // longer agree. The bound still names the index when every index leaf of
  // the selected guards reaches the table index zero-extended: a structural
  // leaf (the lane itself, its zero-extend, or an AND keeping the whole lane),
  // or a leaf whose identity query also holds with sign extension excluded.
  // Those queries run on a copy of the account, charged only when they hold,
  // so a lane they cannot prove fails exactly as before.
  auto selectedLaneZeroExtended = [&]() {
    std::vector<JumpTableValueQuery> ZeroQueries;
    std::vector<const GuardExpr *> Work;
    std::set<const GuardExpr *> Seen;
    for (const auto &[Guard, TableCondition] : IndexGuards) {
      (void)TableCondition;
      if (Guard)
        Work.push_back(Guard.get());
    }
    size_t Visits = 0;
    while (!Work.empty()) {
      if (++Visits > limits::kMaxJumpTableEvidenceWork)
        return false;
      const GuardExpr *Expr = Work.back();
      Work.pop_back();
      if (!Seen.insert(Expr).second)
        continue;
      if (Expr->K != GuardExpr::Kind::Index) {
        for (const GuardExprPtr &Input : Expr->Inputs)
          if (Input)
            Work.push_back(Input.get());
        continue;
      }
      const auto Source = IndexLeafSources.find(Expr);
      if (Source == IndexLeafSources.end())
        return false;
      const GuardSyntaxNode &Leaf = *Source->second;
      if (Leaf.StructuralIndex)
        continue;
      auto holds = [&](size_t Query) {
        return Query < ProofQueries.size() && Query < ProofResults.size() &&
               Query < ProofQueryComplete.size() && ProofQueryComplete[Query] &&
               ProofResults[Query];
      };
      if (!holds(Leaf.IndexQuery) ||
          (Leaf.IndexQueryUsesDefinition && !holds(Leaf.DefQuery)))
        return false;
      JumpTableValueQuery Query = ProofQueries[Leaf.IndexQuery];
      Query.AllowSignExtension = false;
      ZeroQueries.push_back(std::move(Query));
    }
    if (ZeroQueries.empty())
      return true;
    size_t ZeroBudget = *CandidateEvidenceBudget;
    bool BatchComplete = false;
    std::vector<bool> QueryComplete;
    const std::vector<bool> Results = ResolveGuardQueries(
        ZeroQueries, QueryComplete, BatchComplete, &ZeroBudget);
    if (Results.size() != ZeroQueries.size() ||
        QueryComplete.size() != ZeroQueries.size())
      return false;
    for (size_t I = 0; I < Results.size(); ++I)
      if (!QueryComplete[I] || !Results[I])
        return false;
    *CandidateEvidenceBudget = ZeroBudget;
    return true;
  };
  if (SelectedIndexWidth < Info.IndexValueAtUse.Size) {
    const unsigned SourceBits = unsigned(SelectedIndexWidth) * 8u;
    if (SourceBits == 0 ||
        (SourceBits < 64 && FirstRejected > (uint64_t(1) << (SourceBits - 1)) &&
         (FirstRejected > (uint64_t(1) << SourceBits) ||
          !selectedLaneZeroExtended()))) {
      return failSemantic();
    }
  }
  constexpr size_t SolverProofWork =
      size_t(limits::kMaxJumpTableGuardSolverConflicts) +
      size_t(limits::kMaxJumpTableGuardSolverPropagations) +
      size_t(limits::kMaxJumpTableGuardSolverWatchVisits) +
      limits::kMaxJumpTableGuardSolverGates;
  if (DefinedPrefixCapped) {
    if (!consumeGuardBuildWork(/*ult + constant + and=*/3))
      return failIncomplete();
    const uint32_t IndexWidth = Symbolic.Ctx.width(Symbolic.Index);
    if (IndexWidth == 0 || IndexWidth > 64)
      return failSemantic();
    // `index != Top` is not `index < Top` until values past the prefix are
    // excluded. The concrete sample never sees those values; the solver does.
    SymRef WithinPrefix = Symbolic.Ctx.mkUlt(
        Symbolic.Index,
        Symbolic.Ctx.mkConst(IndexWidth, uint64_t(DefinedPrefix)));
    ReachesTable = Symbolic.Ctx.mkAnd(ReachesTable, WithinPrefix);
  }
  if (!consumeGuardBuildWork(/*constant + ult + xor=*/3) ||
      !consumeCandidateEvidence(SolverProofWork))
    return failIncomplete();
  switch (proveDenseGuardPrefix(Symbolic, ReachesTable, FirstRejected,
                                SparseDomain || EqualityExclusion)) {
  case DenseGuardPrefixProof::Proven:
    break;
  case DenseGuardPrefixProof::Counterexample:
    // The dominating conjunction is not a dense prefix. Predecessor ranges
    // that do not dominate the load can still prove one, with the dominating
    // edge constraints kept. A prefix already proved above is not replaced,
    // and a wider literal remains only the query envelope.
    switch (tryJoinedPredecessorDomain()) {
    case JoinedPrefix::Proved:
      return true;
    case JoinedPrefix::BudgetExhausted:
    case JoinedPrefix::QueryIncomplete:
      return failIncomplete();
    case JoinedPrefix::Skipped:
      return failSemantic();
    }
    return failSemantic();
  case DenseGuardPrefixProof::Incomplete:
    return failIncomplete();
  }

  if (Info.MaxEntries == 0 || FirstRejected < Info.MaxEntries)
    Info.MaxEntries = FirstRejected;
  Info.IncompleteGuardDomain = false;
  Info.SemanticGuardDomainAmbiguous = false;
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
