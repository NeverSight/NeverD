//===- BitVectorSolver.h - Deciding bitvector questions ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The decision procedure the rest of NeverD calls.
///
/// Three questions cover what a decompiler needs to ask about the fixed-width
/// arithmetic it recovers, and all three are here:
///
///   - *Can this hold?*  A path condition collected by symbolic execution is
///     satisfiable exactly when the path is reachable, and the model is the
///     input that reaches it.  A branch whose condition is unsatisfiable is
///     dead code that the recovered source should not contain.
///
///   - *Are these the same?*  A rewrite is sound exactly when the original and
///     the replacement differ nowhere, which is the negation of their
///     difference being satisfiable.  This is what lets a simplification be
///     proved rather than sampled — an obfuscated expression and its short
///     form can agree at a million random points and still differ, so nothing
///     short of a proof is worth putting in front of a user.
///
///   - *What value does this take?*  A model gives a concrete assignment,
///     which turns "these two expressions differ" into an input that shows how
///     and makes a failing rewrite reducible to a test.
///
/// Every question is decided by lowering the expression to a boolean circuit,
/// encoding the circuit as clauses, and searching.  Complete and exact at any
/// width, with the running time that implies: bounded by budgets rather than
/// by hope, and reporting \c SatResult::Unknown when a budget runs out.  A
/// caller must treat \c Unknown as "no answer" — in particular \c proveEqual
/// returns false for it, because an unproved rewrite and a refuted one are
/// equally unusable.
///
/// The one-shot free functions cover the common case.  \c BitVectorSolver
/// itself is for a caller asking many related questions: assertions and
/// everything learned from them persist across checks, and per-check
/// assumptions switch parts of the formula on and off without re-encoding
/// anything.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_BITVECTORSOLVER_H
#define NEVERD_SOLVER_BITVECTORSOLVER_H

#include "neverd/solver/BitBlaster.h"
#include "neverd/solver/CnfEncoder.h"
#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::solver {

/// Concrete values for the free variables of a satisfied formula.
///
/// Only variables the formula actually reached appear.  A variable it never
/// mentioned is unconstrained rather than zero, and the distinction is worth
/// keeping: a caller reporting a counterexample should say which inputs it
/// depends on.
class BitVectorModel {
public:
  bool empty() const { return VarIds.empty(); }
  size_t size() const { return VarIds.size(); }

  /// The variables this model constrains, ascending.
  llvm::ArrayRef<uint32_t> vars() const { return VarIds; }

  bool contains(uint32_t VarId) const { return indexOf(VarId) < VarIds.size(); }

  /// The value found for \p VarId, or nothing when the formula never
  /// mentioned it.
  std::optional<llvm::APInt> value(uint32_t VarId) const;

  /// The value found for the variable node \p Var.
  std::optional<llvm::APInt> value(const symbolic::SymContext &Ctx,
                                   symbolic::SymRef Var) const;

  /// One value per variable id in \p Ctx, with unconstrained variables reading
  /// zero.  This is the shape \c SymContext::eval takes, so a caller checks a
  /// model by evaluating the original expression under it — which is how the
  /// tests hold the encoding to the expression semantics rather than to
  /// themselves.
  std::vector<llvm::APInt> asVarValues(const symbolic::SymContext &Ctx) const;

  /// Record a value, replacing any previous one for the same variable.
  void set(uint32_t VarId, llvm::APInt Value);
  void clear();

private:
  /// Position of \p VarId, or \c VarIds.size() when absent.
  size_t indexOf(uint32_t VarId) const;

  /// Kept ascending by variable id: lookup is a binary search, and iteration
  /// order does not depend on the order the search happened to assign in.
  std::vector<uint32_t> VarIds;
  std::vector<llvm::APInt> Values;
};

/// Tuning for a bitvector query.
struct SolverOptions {
  SatOptions Sat;
  BlastLimits Blast;

  /// Read a model back when the answer is satisfiable.  A caller that only
  /// wants the answer turns it off and skips a walk of the variable table.
  bool BuildModel = true;

  /// Remove caller-imposed SAT search and bit-blasting ceilings.
  ///
  /// Zero has that meaning for both constituent policies.  This does not
  /// promise infinite physical resources: expression widths and SAT literals
  /// retain their representational bounds, and allocation failure remains a
  /// host resource failure.
  static SolverOptions unlimited() {
    SolverOptions Options;
    Options.Sat.MaxConflicts = 0;
    Options.Sat.MaxPropagations = 0;
    Options.Sat.MaxWatchVisits = 0;
    Options.Blast = BlastLimits::unlimited();
    return Options;
  }
};

/// The answer to "are these two expressions the same value everywhere".
enum class EquivResult : uint8_t {
  /// No assignment makes them differ.
  Equal = 0,
  /// An assignment makes them differ, and it is in the counterexample model.
  Different = 1,
  /// A budget ran out.
  Unknown = 2,
  /// One of the expressions was invalid or the two widths did not match.
  Invalid = 3,
};

const char *equivResultName(EquivResult R);

/// An incremental bitvector solver over one expression context.
///
/// Assertions accumulate and everything learned from them is kept, so a caller
/// exploring a path tree asserts each new constraint as it descends and pays
/// only for what changed.  Assumptions go the other way: they hold for a
/// single check and cost nothing to retract, which is what makes a query like
/// "and what if this branch went the other way" cheap.
class BitVectorSolver {
public:
  explicit BitVectorSolver(symbolic::SymContext &Ctx,
                           const SolverOptions &Opts = SolverOptions());
  ~BitVectorSolver();

  BitVectorSolver(const BitVectorSolver &) = delete;
  BitVectorSolver &operator=(const BitVectorSolver &) = delete;

  //===--------------------------------------------------------------------===//
  // Constraints
  //===--------------------------------------------------------------------===//

  /// Constrain \p Pred to hold.  A one-bit expression is read as the predicate
  /// it is; a wider one holds when it is nonzero, matching how a lifted
  /// machine condition reaches this interface.
  ///
  /// Returns false when the expression is malformed or could not be encoded
  /// within the configured limits.  The solver is then unusable.  Every later
  /// check reports \c SatResult::Invalid for malformed input and
  /// \c SatResult::Unknown for an exhausted encoding budget; a partially
  /// encoded formula is never queried.
  bool assertTrue(symbolic::SymRef Pred);
  bool assertFalse(symbolic::SymRef Pred);
  bool assertEqual(symbolic::SymRef A, symbolic::SymRef B);
  bool assertDistinct(symbolic::SymRef A, symbolic::SymRef B);

  //===--------------------------------------------------------------------===//
  // Asking
  //===--------------------------------------------------------------------===//

  SatResult check();

  /// Check with each expression in \p Assumptions additionally held true, for
  /// this call only.
  SatResult check(llvm::ArrayRef<symbolic::SymRef> Assumptions);

  //===--------------------------------------------------------------------===//
  // Answers
  //===--------------------------------------------------------------------===//

  /// Values found by the last check.  Empty unless it returned
  /// \c SatResult::Sat with model building enabled.
  const BitVectorModel &model() const { return Model; }

  /// The assumptions of the last check that were enough to make it
  /// unsatisfiable, as the expressions that were passed in.
  llvm::ArrayRef<symbolic::SymRef> failedAssumptions() const {
    return FailedAssumptions;
  }

  /// False once an expression could not be encoded.
  bool ok() const { return Error == BlastError::None; }
  BlastError encodeError() const { return Error; }

  const SatStats &stats() const { return Sat.stats(); }

  symbolic::SymContext &context() { return Ctx; }
  SatSolver &sat() { return Sat; }
  BitBlaster &blaster() { return Blaster; }

private:
  /// Encode \p Pred as the single literal that holds exactly when it does.
  std::optional<SatLit> literalFor(symbolic::SymRef Pred);
  SatResult finish(SatResult R, llvm::ArrayRef<symbolic::SymRef> Assumptions,
                   llvm::ArrayRef<SatLit> AssumptionLits);
  void extractModel();

  symbolic::SymContext &Ctx;
  SolverOptions Opts;
  SatSolver Sat;
  CnfEncoder Enc;
  BitBlaster Blaster;
  BlastError Error = BlastError::None;
  BitVectorModel Model;
  std::vector<symbolic::SymRef> FailedAssumptions;
  /// Scratch for the literals of one check's assumptions.
  std::vector<SatLit> AssumptionLits;
};

//===----------------------------------------------------------------------===//
// One-shot queries
//===----------------------------------------------------------------------===//

/// Decide whether \p Pred can hold.
///
/// A one-bit expression is read as a predicate; a wider one is satisfied when
/// it is nonzero.  When \p Model is given and the answer is \c SatResult::Sat
/// it receives an assignment that satisfies \p Pred.  Malformed references
/// return \c SatResult::Invalid; resource limits return \c SatResult::Unknown.
SatResult checkSat(symbolic::SymContext &Ctx, symbolic::SymRef Pred,
                   BitVectorModel *Model = nullptr,
                   const SolverOptions &Opts = SolverOptions());

/// Decide whether \p A and \p B are the same value under every assignment, by
/// searching for an assignment where they differ.
///
/// The two must be the same width.  When \p Counterexample is given and the
/// answer is \c EquivResult::Different it receives an assignment that tells
/// them apart.  Invalid references and width mismatches return
/// \c EquivResult::Invalid; resource limits return \c EquivResult::Unknown.
EquivResult checkEqual(symbolic::SymContext &Ctx, symbolic::SymRef A,
                       symbolic::SymRef B,
                       BitVectorModel *Counterexample = nullptr,
                       const SolverOptions &Opts = SolverOptions());

/// True when \p A and \p B are proved equal everywhere.
///
/// False covers "they differ", "no answer within budget", and an invalid
/// query, because a caller about to rewrite \p A into \p B must do nothing in
/// every case without a proof.  A caller that needs the reason — to report a
/// malformed query, return a counterexample, or retry with a larger budget —
/// asks \c checkEqual instead.
bool proveEqual(symbolic::SymContext &Ctx, symbolic::SymRef A,
                symbolic::SymRef B,
                const SolverOptions &Opts = SolverOptions());

} // namespace neverd::solver

#endif // NEVERD_SOLVER_BITVECTORSOLVER_H
