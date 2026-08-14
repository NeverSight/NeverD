//===- BitVectorSolver.cpp - The bitvector decision procedure -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the interface the rest of NeverD asks bitvector questions
/// through: the model, the incremental solver, and the one-shot queries.
///
/// There is little arithmetic here.  The layers below already turn an
/// expression into clauses and clauses into an answer; what this file adds is
/// the translation between the two vocabularies — an expression asserted, an
/// expression assumed, a model read back as concrete values rather than as
/// bits — and the small number of decisions that only make sense at this
/// level, such as what it means to assert something that is not one bit wide,
/// and why a proof that ran out of budget is reported as a failure to prove
/// rather than as a disproof.
///
//===----------------------------------------------------------------------===//

#include "neverd/solver/BitVectorSolver.h"

#include "neverd/solver/BitBlaster.h"
#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace neverd::solver {

using symbolic::SymContext;
using symbolic::SymRef;

namespace {

bool isValidRef(const SymContext &Ctx, SymRef R) {
  return R.isValid() && R.index() < Ctx.numNodes();
}

SatResult resultForEncodingFailure(BlastError Error) {
  switch (Error) {
  case BlastError::WidthTooLarge:
  case BlastError::TooManyGates:
    return SatResult::Unknown;
  case BlastError::Malformed:
  case BlastError::None:
    return SatResult::Invalid;
  }
  return SatResult::Invalid;
}

} // namespace

const char *equivResultName(EquivResult R) {
  switch (R) {
  case EquivResult::Equal:
    return "equal";
  case EquivResult::Different:
    return "different";
  case EquivResult::Unknown:
    return "unknown";
  case EquivResult::Invalid:
    return "invalid";
  }
  return "?";
}

//===----------------------------------------------------------------------===//
// BitVectorModel
//===----------------------------------------------------------------------===//

size_t BitVectorModel::indexOf(uint32_t VarId) const {
  auto It = std::lower_bound(VarIds.begin(), VarIds.end(), VarId);
  if (It == VarIds.end() || *It != VarId)
    return VarIds.size();
  return static_cast<size_t>(It - VarIds.begin());
}

std::optional<llvm::APInt> BitVectorModel::value(uint32_t VarId) const {
  size_t I = indexOf(VarId);
  if (I >= VarIds.size())
    return std::nullopt;
  return Values[I];
}

std::optional<llvm::APInt> BitVectorModel::value(const SymContext &Ctx,
                                                 SymRef Var) const {
  if (!Var.isValid() || !Ctx.isVar(Var))
    return std::nullopt;
  return value(Ctx.varId(Var));
}

std::vector<llvm::APInt>
BitVectorModel::asVarValues(const SymContext &Ctx) const {
  std::vector<llvm::APInt> Out;
  Out.reserve(Ctx.numVars());

  for (size_t Id = 0, N = Ctx.numVars(); Id < N; ++Id) {
    uint32_t Width = Ctx.varInfo(static_cast<uint32_t>(Id)).Width;
    std::optional<llvm::APInt> V = value(static_cast<uint32_t>(Id));
    // A variable the formula never mentioned may take any value, so zero is as
    // good an answer as another and keeps the vector dense enough to evaluate
    // with.
    Out.push_back(V ? V->zextOrTrunc(Width) : llvm::APInt(Width, 0));
  }
  return Out;
}

void BitVectorModel::set(uint32_t VarId, llvm::APInt Value) {
  auto It = std::lower_bound(VarIds.begin(), VarIds.end(), VarId);
  auto Index = static_cast<size_t>(It - VarIds.begin());

  if (It != VarIds.end() && *It == VarId) {
    Values[Index] = std::move(Value);
    return;
  }
  VarIds.insert(It, VarId);
  Values.insert(Values.begin() + static_cast<ptrdiff_t>(Index),
                std::move(Value));
}

void BitVectorModel::clear() {
  VarIds.clear();
  Values.clear();
}

//===----------------------------------------------------------------------===//
// BitVectorSolver
//===----------------------------------------------------------------------===//

BitVectorSolver::BitVectorSolver(SymContext &Ctx, const SolverOptions &Opts)
    : Ctx(Ctx), Opts(Opts), Sat(Opts.Sat), Enc(Sat),
      Blaster(Ctx, Enc, Opts.Blast) {}

BitVectorSolver::~BitVectorSolver() = default;

std::optional<SatLit> BitVectorSolver::literalFor(SymRef Pred) {
  if (!ok())
    return std::nullopt;

  if (!isValidRef(Ctx, Pred)) {
    Error = BlastError::Malformed;
    return std::nullopt;
  }

  if (Ctx.width(Pred) == 1) {
    SatLit L;
    if (!Blaster.blastPredicate(Pred, L)) {
      Error = Blaster.error();
      return std::nullopt;
    }
    return L;
  }

  // A wider value stands for a condition the way a machine does: it holds when
  // it is nonzero.  Lifted code reaches this interface with the flag word or
  // the register it tested still at its natural width, and making the caller
  // compare it against zero first would only move the same disjunction one
  // layer up.
  BitLits Bits;
  if (!Blaster.blast(Pred, Bits)) {
    Error = Blaster.error();
    return std::nullopt;
  }
  return Enc.mkOr(Bits);
}

bool BitVectorSolver::assertTrue(SymRef Pred) {
  std::optional<SatLit> L = literalFor(Pred);
  if (!L)
    return false;
  Enc.assertTrue(*L);
  return true;
}

bool BitVectorSolver::assertFalse(SymRef Pred) {
  std::optional<SatLit> L = literalFor(Pred);
  if (!L)
    return false;
  Enc.assertFalse(*L);
  return true;
}

bool BitVectorSolver::assertEqual(SymRef A, SymRef B) {
  if (!ok())
    return false;
  if (!isValidRef(Ctx, A) || !isValidRef(Ctx, B) ||
      Ctx.width(A) != Ctx.width(B)) {
    Error = BlastError::Malformed;
    return false;
  }
  return assertTrue(Ctx.mkEq(A, B));
}

bool BitVectorSolver::assertDistinct(SymRef A, SymRef B) {
  if (!ok())
    return false;
  if (!isValidRef(Ctx, A) || !isValidRef(Ctx, B) ||
      Ctx.width(A) != Ctx.width(B)) {
    Error = BlastError::Malformed;
    return false;
  }
  return assertFalse(Ctx.mkEq(A, B));
}

SatResult BitVectorSolver::check() { return check(llvm::ArrayRef<SymRef>()); }

SatResult BitVectorSolver::check(llvm::ArrayRef<SymRef> Assumptions) {
  Model.clear();
  FailedAssumptions.clear();

  if (!ok())
    return resultForEncodingFailure(Error);

  AssumptionLits.clear();
  AssumptionLits.reserve(Assumptions.size());
  for (SymRef A : Assumptions) {
    std::optional<SatLit> L = literalFor(A);
    if (!L)
      return resultForEncodingFailure(Error);
    AssumptionLits.push_back(*L);
  }

  SatResult R = Sat.solve(AssumptionLits);
  return finish(R, Assumptions, AssumptionLits);
}

SatResult BitVectorSolver::finish(SatResult R,
                                  llvm::ArrayRef<SymRef> Assumptions,
                                  llvm::ArrayRef<SatLit> Lits) {
  if (R == SatResult::Sat && Opts.BuildModel)
    extractModel();

  if (R == SatResult::Unsat) {
    // Report the failure in the caller's own terms.  Several expressions can
    // share one literal once the encoder has folded them together, so the
    // first match is taken and the list stays in the order the caller gave.
    for (SatLit Failed : Sat.failedAssumptions()) {
      for (size_t I = 0, N = Lits.size(); I < N; ++I) {
        if (Lits[I] != Failed)
          continue;
        FailedAssumptions.push_back(Assumptions[I]);
        break;
      }
    }
  }

  return R;
}

void BitVectorSolver::extractModel() {
  Model.clear();

  for (uint32_t Id : Blaster.encodedVars()) {
    llvm::ArrayRef<SatLit> Bits = Blaster.variableBits(Id);
    if (Bits.empty())
      continue;

    llvm::APInt Value(static_cast<unsigned>(Bits.size()), 0);
    for (size_t I = 0, N = Bits.size(); I < N; ++I)
      if (Sat.modelValue(Bits[I]) == SatValue::True)
        Value.setBit(static_cast<unsigned>(I));
    Model.set(Id, std::move(Value));
  }
}

//===----------------------------------------------------------------------===//
// One-shot queries
//===----------------------------------------------------------------------===//

SatResult checkSat(SymContext &Ctx, SymRef Pred, BitVectorModel *Model,
                   const SolverOptions &Opts) {
  BitVectorSolver Solver(Ctx, Opts);
  if (!Solver.assertTrue(Pred))
    return Solver.check();

  SatResult R = Solver.check();
  if (R == SatResult::Sat && Model != nullptr)
    *Model = Solver.model();
  return R;
}

EquivResult checkEqual(SymContext &Ctx, SymRef A, SymRef B,
                       BitVectorModel *Counterexample,
                       const SolverOptions &Opts) {
  if (!isValidRef(Ctx, A) || !isValidRef(Ctx, B) ||
      Ctx.width(A) != Ctx.width(B))
    return EquivResult::Invalid;

  // Expressions are interned, so two references being equal already is a proof
  // of equality — and a common one, because the canonicalising builders reduce
  // a great many rewrites to the same node before anything is asked.
  if (A == B)
    return EquivResult::Equal;

  BitVectorSolver Solver(Ctx, Opts);
  if (!Solver.assertDistinct(A, B)) {
    SatResult Failure = Solver.check();
    return Failure == SatResult::Unknown ? EquivResult::Unknown
                                         : EquivResult::Invalid;
  }

  switch (Solver.check()) {
  case SatResult::Unsat:
    // Nothing tells them apart, at any width, for any input.
    return EquivResult::Equal;
  case SatResult::Sat:
    if (Counterexample != nullptr)
      *Counterexample = Solver.model();
    return EquivResult::Different;
  case SatResult::Unknown:
    return EquivResult::Unknown;
  case SatResult::Invalid:
    return EquivResult::Invalid;
  }
  return EquivResult::Invalid;
}

bool proveEqual(SymContext &Ctx, SymRef A, SymRef B,
                const SolverOptions &Opts) {
  return checkEqual(Ctx, A, B, /*Counterexample=*/nullptr, Opts) ==
         EquivResult::Equal;
}

} // namespace neverd::solver
