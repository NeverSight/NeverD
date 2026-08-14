//===- SymSynthVerifier.h - Prove synthesis candidates ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Owns the solver state produced while checking candidates from symbolic
/// expression synthesis.  The symbolic search stays independent of Solver;
/// callers that need a proof construct this adapter and pass it as the search
/// verifier.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_SYMSYNTHVERIFIER_H
#define NEVERD_SOLVER_SYMSYNTHVERIFIER_H

#include "neverd/solver/BitVectorSolver.h"
#include "neverd/symbolic/SymSynth.h"

#include <cstdint>
#include <optional>

namespace neverd::solver {

/// Last proof disposition.  Values are stable for public result adapters.
enum class ProofStatus : uint8_t {
  NotRun = 0,
  Equivalent = 1,
  Different = 2,
  /// A proof or refutation could not finish within its resource budget.
  Unknown = 3,
  /// The proof question itself was malformed.
  Invalid = 4,
};

/// Work performed by all queries made through one verifier.
struct ProofStats {
  uint64_t Queries = 0;
  uint64_t Conflicts = 0;
  uint64_t Propagations = 0;
  uint64_t WatchVisits = 0;
};

/// Proof state and any concrete refutation owned by the adapter.
struct SymSynthProofReport {
  ProofStatus Proof = ProofStatus::NotRun;
  ProofStats Stats;
  symbolic::SymRef RejectedCandidate;
  std::optional<BitVectorModel> Counterexample;
};

/// Checks synthesis candidates with the built-in bitvector solver.
class SymSynthVerifier {
public:
  explicit SymSynthVerifier(SolverOptions Options = {});

  /// Verify one candidate.  The legacy synthesis result has no invalid-query
  /// enumerator, so malformed input returns its fail-closed \c Unknown value;
  /// \c report().Proof remains \c ProofStatus::Invalid and preserves the
  /// precise disposition for callers that report or retry failures.
  symbolic::SynthVerification operator()(symbolic::SymContext &Ctx,
                                         symbolic::SymRef Original,
                                         symbolic::SymRef Candidate);

  const SymSynthProofReport &report() const { return Report; }

private:
  void addStats(const SatStats &Stats);
  void clearRefutation();

  SolverOptions Options;
  SymSynthProofReport Report;
};

} // namespace neverd::solver

#endif // NEVERD_SOLVER_SYMSYNTHVERIFIER_H
