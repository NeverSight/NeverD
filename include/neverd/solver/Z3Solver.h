//===- Z3Solver.h - Optional native Z3 bitvector queries --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_Z3SOLVER_H
#define NEVERD_SOLVER_Z3SOLVER_H

#include "neverd/solver/BitVectorSolver.h"

#include <cstdint>
#include <memory>
#include <string>

namespace neverd::solver {

struct Z3SolverOptions {
  /// Milliseconds allowed for each Z3 check, excluding DAG translation.
  /// Zero removes the time limit. A timeout returns Unknown, never a proof.
  uint32_t TimeoutMs = 1000;
  /// Z3's deterministic per-check resource limit; zero leaves it unbounded.
  uint32_t ResourceLimit = 0;
  bool BuildModel = true;
};

/// An incremental QF_BV session over one Symbolic context. Translation reads
/// the original DAG without invoking Symbolic's rewrite builders. Scaled
/// products may use an exact low-bit quotient, then restore the full word.
/// Assertions persist; assumptions apply to one check. Malformed expressions
/// invalidate the session, while a timed-out check may be retried.
///
/// This API remains linkable when NEVERD_ENABLE_Z3 is OFF. Such a session
/// reports Unknown with an unavailable reason and never changes backends.
class Z3Solver {
public:
  explicit Z3Solver(symbolic::SymContext &Ctx,
                    const Z3SolverOptions &Options = {});
  ~Z3Solver();

  Z3Solver(const Z3Solver &) = delete;
  Z3Solver &operator=(const Z3Solver &) = delete;

  static bool available();
  static std::string version();

  /// Predicates of any width hold exactly when their value is nonzero.
  bool assertTrue(symbolic::SymRef Pred);
  bool assertFalse(symbolic::SymRef Pred);
  bool assertEqual(symbolic::SymRef A, symbolic::SymRef B);
  bool assertDistinct(symbolic::SymRef A, symbolic::SymRef B);

  SatResult check();
  SatResult check(llvm::ArrayRef<symbolic::SymRef> Assumptions);
  /// Ask whether A differs from B under the persistent assertions, without
  /// changing those assertions or constructing a new Symbolic expression.
  SatResult checkDistinct(symbolic::SymRef A, symbolic::SymRef B);

  /// Empty unless the last check returned Sat and model building is enabled.
  const BitVectorModel &model() const;
  llvm::ArrayRef<symbolic::SymRef> failedAssumptions() const;
  bool ok() const;
  /// Z3's reason for an inconclusive check, or the availability diagnostic.
  const std::string &reasonUnknown() const;

  /// Current assertions plus the last check's assumptions as replayable
  /// SMT-LIB. Adding an assertion discards that previous assumption list.
  /// Empty when unavailable or the session is invalid.
  std::string dumpSMT2() const;

private:
  class Impl;
  std::unique_ptr<Impl> P;
};

SatResult z3CheckSat(symbolic::SymContext &Ctx, symbolic::SymRef Pred,
                     BitVectorModel *Model = nullptr,
                     const Z3SolverOptions &Options = {});

EquivResult z3CheckEqual(symbolic::SymContext &Ctx, symbolic::SymRef A,
                         symbolic::SymRef B,
                         BitVectorModel *Counterexample = nullptr,
                         const Z3SolverOptions &Options = {});

} // namespace neverd::solver

#endif // NEVERD_SOLVER_Z3SOLVER_H
