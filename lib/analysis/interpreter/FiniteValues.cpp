//===- FiniteValues.cpp - Bounded exhaustive bitvector projection ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "FiniteValues.h"

#include "neverd/solver/BitVectorSolver.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>

namespace neverd::analysis::detail {

using namespace symbolic;
using namespace solver;

bool hasUnconstrainedProjectionInput(const SymContext &Ctx, SymRef Predicate,
                                     SymRef Value, uint32_t Limit,
                                     uint64_t MaxVisited,
                                     llvm::ArrayRef<SymRef> RelatedValues) {
  const auto Valid = [&](SymRef Ref) {
    return Ref && Ref.index() < Ctx.numNodes() && Ctx.width(Ref);
  };
  if (!Valid(Predicate) || Ctx.width(Predicate) != 1 || !Valid(Value) ||
      Ctx.width(Value) > 64 || !Limit || !MaxVisited)
    return false;
  // N independent source bits yield 2^N distinct values on every reachable
  // path. Limit is uint32_t, so at most 32 independent bits suffice.
  const unsigned RequiredBits = 32 - llvm::countl_zero(Limit);
  if (Ctx.width(Value) < RequiredBits)
    return false;

  uint64_t Remaining = MaxVisited;
  bool Exhausted = false;
  const auto Charge = [&](uint64_t Count = 1) {
    if (Count > Remaining) {
      Exhausted = true;
      return false;
    }
    Remaining -= Count;
    return true;
  };

  llvm::DenseSet<uint32_t> ExcludedNodes;
  llvm::SmallVector<SymRef, 16> Pending;
  const auto AddRoot = [&](SymRef Root) {
    // Charge repeated roots too, so a long caller-provided list cannot hide
    // unbounded work behind a small shared DAG.
    if (!Charge() || !Valid(Root))
      return false;
    if (ExcludedNodes.insert(Root.index()).second)
      Pending.push_back(Root);
    return true;
  };
  if (!AddRoot(Predicate))
    return false;
  for (SymRef Related : RelatedValues)
    if (!AddRoot(Related))
      return false;
  while (!Pending.empty()) {
    const SymRef Current = Pending.pop_back_val();
    for (SymRef Operand : Ctx.operands(Current)) {
      // Count operand inspections as well as newly reached nodes: a shared
      // DAG may still have a very large number of incoming edges.
      if (!Charge() || !Valid(Operand))
        return false;
      if (ExcludedNodes.insert(Operand.index()).second)
        Pending.push_back(Operand);
    }
  }

  using BitKey = uint64_t;
  const auto Key = [](SymRef Ref, uint32_t Bit) -> BitKey {
    return (uint64_t{Ref.index()} << 32) | Bit;
  };
  // A known origin means this output bit equals the source bit or its
  // complement. The polarity does not affect the lower bound on cardinality.
  llvm::DenseMap<BitKey, std::optional<BitKey>> Origins;
  bool Malformed = false;
  const auto Origin = [&](SymRef Current, uint32_t Bit) {
    llvm::SmallVector<BitKey, 16> Path;
    std::optional<BitKey> Result;
    while (!Exhausted && !Malformed) {
      if (!Valid(Current) || Bit >= Ctx.width(Current)) {
        Malformed = true;
        break;
      }
      const BitKey At = Key(Current, Bit);
      if (const auto It = Origins.find(At); It != Origins.end()) {
        Result = It->second;
        break;
      }
      if (!Charge())
        break;
      Path.push_back(At);
      const auto &Node = Ctx.node(Current);
      const auto Operands = Ctx.operands(Current);
      SymRef Next;
      uint32_t NextBit = Bit;
      const auto Unary = [&]() {
        if (Operands.size() != 1 || !Valid(Operands.front())) {
          Malformed = true;
          return false;
        }
        Next = Operands.front();
        return true;
      };
      switch (Node.Op) {
      case SymOp::Var:
        if (!Operands.empty())
          Malformed = true;
        else if (!ExcludedNodes.contains(Current.index()))
          Result = At;
        break;
      case SymOp::Extract:
        if (Unary()) {
          const uint64_t Width = Ctx.width(Next);
          if (Node.Aux > Width || Node.Width > Width - Node.Aux)
            Malformed = true;
          else
            NextBit += static_cast<uint32_t>(Node.Aux);
        }
        break;
      case SymOp::Concat: {
        uint64_t Low = 0;
        for (SymRef Operand : llvm::reverse(Operands)) {
          if (!Charge() || !Valid(Operand)) {
            Malformed = !Exhausted;
            break;
          }
          const uint64_t Width = Ctx.width(Operand);
          if (Low <= Bit && Bit - Low < Width) {
            Next = Operand;
            NextBit = Bit - Low;
          }
          Low += Width;
          if (Low > Node.Width) {
            Malformed = true;
            break;
          }
        }
        if (Low != Node.Width)
          Malformed = true;
        break;
      }
      case SymOp::ZExt:
      case SymOp::SExt:
        if (Unary()) {
          const uint32_t Width = Ctx.width(Next);
          if (Width > Node.Width)
            Malformed = true;
          else if (Bit >= Width) {
            if (Node.Op == SymOp::SExt)
              NextBit = Width - 1;
            else
              Next = {};
          }
        }
        break;
      case SymOp::Not:
        if (Unary() && Ctx.width(Next) != Node.Width)
          Malformed = true;
        break;
      case SymOp::And:
      case SymOp::Or:
      case SymOp::Xor: {
        bool Killed = false;
        for (SymRef Operand : Operands) {
          if (!Charge() || !Valid(Operand)) {
            Malformed = !Exhausted;
            break;
          }
          const auto &Input = Ctx.node(Operand);
          if (Input.Width != Node.Width) {
            Malformed = true;
            break;
          }
          if (Input.Op != SymOp::Const) {
            if (Next) {
              // Combining two varying operands need not preserve either
              // source bit. Do not infer independence from mere dependency.
              Next = {};
              break;
            }
            Next = Operand;
            continue;
          }
          // Narrow constants are inline. Charge a wide constant's copy before
          // accessing it, so extracting a small view cannot hide unbounded
          // APInt work behind one expression visit.
          bool ConstantBit;
          if (Input.Width <= 64)
            ConstantBit = (Input.Aux >> Bit) & 1;
          else {
            if (!Charge((uint64_t{Input.Width} + 63) / 64))
              break;
            ConstantBit = Ctx.constValue(Operand)[Bit];
          }
          Killed |= (Node.Op == SymOp::And && !ConstantBit) ||
                    (Node.Op == SymOp::Or && ConstantBit);
        }
        if (Killed)
          Next = {};
        break;
      }
      default:
        // Arithmetic and unsupported bit mappings may have useful finite
        // domains, but cannot supply an independent source bit by this proof.
        break;
      }
      if (!Next || Exhausted || Malformed)
        break;
      Current = Next;
      Bit = NextBit;
    }
    for (BitKey At : Path)
      Origins.try_emplace(At, Result);
    return Result;
  };

  llvm::DenseSet<BitKey> IndependentBits;
  for (uint32_t Bit = 0; Bit < Ctx.width(Value); ++Bit) {
    const auto Source = Origin(Value, Bit);
    if (Exhausted || Malformed)
      return false;
    if (Source && IndependentBits.insert(*Source).second &&
        IndependentBits.size() >= RequiredBits)
      return true;
  }
  return false;
}

FiniteValues enumerateFiniteValues(SymContext &Ctx, SymRef Predicate,
                                   llvm::ArrayRef<SymRef> Values,
                                   uint32_t Limit,
                                   const SpecializationOptions &Options,
                                   uint64_t &Queries) {
  if (!Predicate || Ctx.width(Predicate) != 1 || !Limit)
    return {FiniteValueStatus::Invalid, {}};
  for (SymRef Value : Values)
    if (!Value || !Ctx.width(Value) || Ctx.width(Value) > 64)
      return {FiniteValueStatus::Invalid, {}};
  if (Ctx.isConstZero(Predicate))
    return {FiniteValueStatus::Complete, {}};
  if (Ctx.isConstOnes(Predicate)) {
    // A free wide input alone already exceeds this projection's value limit.
    // Avoid spending solver queries on ordinary unconstrained data pointers.
    for (SymRef Value : Values)
      if (Ctx.isVar(Value) &&
          (Ctx.width(Value) >= 64 || (uint64_t{1} << Ctx.width(Value)) > Limit))
        return {FiniteValueStatus::TooManyValues, {}};
    std::vector<uint64_t> Tuple;
    for (SymRef Value : Values) {
      const auto Constant = Ctx.asConst(Value);
      if (!Constant)
        break;
      Tuple.push_back(Constant->getZExtValue());
    }
    if (Tuple.size() == Values.size())
      return {FiniteValueStatus::Complete, {std::move(Tuple)}};
  }
  if (Ctx.numNodes() > Options.MaxSymbolicNodes)
    return {FiniteValueStatus::Unknown, {}};

  SolverOptions Settings;
  Settings.Blast.MaxGates = Options.MaxSolverGates;
  Settings.Sat.MaxConflicts = Options.MaxSolverConflicts;
  Settings.Sat.MaxPropagations = Options.MaxSolverPropagations;
  Settings.Sat.MaxWatchVisits = Options.MaxSolverWatchVisits;
  BitVectorSolver Solver(Ctx, Settings);
  const auto EncodingFailure = [&] {
    return FiniteValues{Solver.encodeError() == BlastError::Malformed
                            ? FiniteValueStatus::Invalid
                            : FiniteValueStatus::Unknown,
                        {}};
  };
  if (!Solver.assertTrue(Predicate))
    return EncodingFailure();

  // Explicit query variables force every requested value into the model.
  // Never fill a missing model value with zero or enumerate one operand at a
  // time: the joint model preserves correlations among all requested values.
  llvm::SmallVector<SymRef, 8> QueryValues;
  for (SymRef Value : Values) {
    SymRef Query = Ctx.mkFreshVar(Ctx.width(Value), "finite_value");
    QueryValues.push_back(Query);
    if (!Solver.assertEqual(Query, Value))
      return EncodingFailure();
  }
  FiniteValues Result;
  while (true) {
    if (Queries >= Options.MaxSolverQueries)
      return {FiniteValueStatus::QueryBudgetExceeded, {}};
    if (Ctx.numNodes() > Options.MaxSymbolicNodes)
      return {FiniteValueStatus::Unknown, {}};
    ++Queries;
    switch (Solver.check()) {
    case SatResult::Unsat:
      Result.Status = FiniteValueStatus::Complete;
      std::sort(Result.Tuples.begin(), Result.Tuples.end());
      return Result;
    case SatResult::Unknown:
      return {FiniteValueStatus::Unknown, {}};
    case SatResult::Invalid:
      return {FiniteValueStatus::Invalid, {}};
    case SatResult::Sat:
      break;
    }
    if (Result.Tuples.size() >= Limit)
      return {FiniteValueStatus::TooManyValues, {}};
    std::vector<uint64_t> Tuple;
    llvm::SmallVector<SymRef, 8> Different;
    for (SymRef Query : QueryValues) {
      const auto Value = Solver.model().value(Ctx, Query);
      if (!Value)
        return {FiniteValueStatus::Invalid, {}};
      Tuple.push_back(Value->getZExtValue());
      Different.push_back(Ctx.mkNot(Ctx.mkEq(Query, Ctx.mkConst(*Value))));
    }
    Result.Tuples.push_back(std::move(Tuple));
    // Empty projection has one possible tuple; blocking it proves that no
    // further tuple exists. A final UNSAT check is still required.
    if (!Solver.assertTrue(Different.empty() ? Ctx.mkFalse()
                                             : Ctx.mkOr(Different)))
      return EncodingFailure();
  }
}

} // namespace neverd::analysis::detail
