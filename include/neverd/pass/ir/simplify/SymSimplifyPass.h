//===- SymSimplifyPass.h - Semantic simplification of LLVM IR ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Collapses mixed boolean-arithmetic and related integer obfuscation on LLVM
/// IR by measuring what a computation does rather than matching its shape.
///
/// This is the counterpart to the HighIR simplifier: the same engine, run one
/// layer earlier, so the recovered form is what the rest of the optimizer and
/// every downstream IR sees.  It works over integer expression trees and the
/// branch conditions built from them; it does not touch memory or anything it
/// cannot read exactly, so it composes with the ordinary optimization pipeline
/// instead of standing apart from it.
///
/// Before general MBA measurement, exact two-valued integer slices can reduce
/// encoded conditions to a comparison and encoded values to a select. Opaque
/// predicates whose conditions are constant can also be folded. Removing the
/// unreachable side remains the following SimplifyCFG pass's responsibility.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PASS_IR_SIMPLIFY_SYMSIMPLIFYPASS_H
#define NEVERD_PASS_IR_SIMPLIFY_SYMSIMPLIFYPASS_H

#include "neverd/solver/SymSynthVerifier.h"
#include "neverd/symbolic/SymMBA.h"
#include "neverd/symbolic/SymSynth.h"

#include "llvm/ADT/APInt.h"
#include "llvm/IR/PassManager.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Function;
class Value;
} // namespace llvm

namespace neverd {

/// Function attribute NeverD's L1 obfuscation passes stamp on every definition
/// they rewrite.  SymSimplifyPass skips any function carrying it: this pass
/// measures away exactly the mixed boolean-arithmetic that obfuscation injects,
/// so running it on obfuscated IR would take the obfuscation straight back off.
/// The stamp is what lets an obfuscate-then-patch pipeline reuse one module
/// without the simplifier and the obfuscator undoing each other.
inline constexpr char kObfuscatedFnAttr[] = "neverd-obfuscated";

/// The proof implementation used to decide a discovered synthesis candidate.
enum class ProofProvider : uint8_t {
  BuiltInSolver = 0,
  Disabled = 1,
  Callback = 2,
};

/// Why a semantic simplification attempt stopped.
///
/// These values are part of the public result contract.  New outcomes may be
/// appended, but existing values must not be renumbered.
enum class SymSimplifyOutcome : uint8_t {
  NotApplicable = 0,
  AlreadyShortest = 1,
  TooManyInputs = 2,
  SearchBudgetExhausted = 3,
  Counterexample = 4,
  ProofIncomplete = 5,
  Rewritten = 6,
};
static_assert(static_cast<uint8_t>(SymSimplifyOutcome::NotApplicable) == 0 &&
              static_cast<uint8_t>(SymSimplifyOutcome::AlreadyShortest) == 1 &&
              static_cast<uint8_t>(SymSimplifyOutcome::TooManyInputs) == 2 &&
              static_cast<uint8_t>(SymSimplifyOutcome::SearchBudgetExhausted) ==
                  3 &&
              static_cast<uint8_t>(SymSimplifyOutcome::Counterexample) == 4 &&
              static_cast<uint8_t>(SymSimplifyOutcome::ProofIncomplete) == 5 &&
              static_cast<uint8_t>(SymSimplifyOutcome::Rewritten) == 6);

const char *symSimplifyOutcomeName(SymSimplifyOutcome Outcome);

/// One concrete variable assignment from a failed equivalence proof.
struct SymSimplifyCounterexampleValue {
  uint32_t Id = 0;
  uint32_t Width = 0;
  std::string Name;
  std::string HexValue;
};

/// The rejected candidate and a concrete assignment that distinguishes it.
struct SymSimplifyCounterexample {
  std::string Candidate;
  std::vector<SymSimplifyCounterexampleValue> Variables;

  /// Serialize in the stable public JSON form, ordered by variable id.
  std::string toJson() const;
};

/// Typed telemetry from one function simplification.
///
/// Search work and proof work deliberately remain separate: they use
/// different units and adding them would make neither number meaningful.
/// Across multiple roots, Outcome, Proof, and Counterexample are one coherent
/// disposition from the last actual proof attempt.  Without a proof attempt,
/// Outcome is the most informative derivational disposition.
struct SymSimplifyResult {
  uint64_t Rewrites = 0;
  uint64_t SearchWork = 0;
  SymSimplifyOutcome Outcome = SymSimplifyOutcome::NotApplicable;
  solver::ProofStatus Proof = solver::ProofStatus::NotRun;
  solver::ProofStats ProofWork;
  std::optional<SymSimplifyCounterexample> Counterexample;
};

/// Work used by the exact modular predicate phase alone. This is derivation
/// work (including width-weighted algebra and use traversal), not solver work.
struct SymPredicateSimplifyResult {
  unsigned Rewrites = 0;
  size_t Work = 0;
  /// A charge exceeded MaxPredicateWork; an exact completed limit is allowed.
  bool WorkLimitExceeded = false;
};

/// Derivation work for exact two-valued integer slices alone. Completed
/// rewrites retain their anchor and all of its original definedness behavior.
struct SymFiniteValueSimplifyResult {
  unsigned Rewrites = 0;
  size_t Work = 0;
  bool WorkLimitExceeded = false;
};

/// What a rewrite has to be worth before it is made.
///
/// The defaults are the policy the optimization pipeline wants: an expression
/// too small to be hiding anything is not measured, and a measured form that
/// does not shorten the function is not written back.  Together they keep the
/// pass from churning IR the rest of the pipeline is built on, which is a
/// different goal from finding every identity there is.
///
/// A caller whose goal *is* finding every identity — a report on how far
/// measurement reaches, a fixture pinning one law, an analysis reading the
/// result rather than compiling it — asks for \c aggressive() instead.
struct SymSimplifyOptions {
  /// Smallest expression worth handing to the engine, in interned nodes.
  ///
  /// The count is of the shared graph, so a subterm an obfuscator repeated is
  /// one node however many times it appears.  Four is the first size that can
  /// improve: `~x + 1` is four nodes and is `-x`.
  /// When translation already returns an operand, the original region's
  /// distinct LLVM values determine whether that fold meets the threshold.
  size_t MinMeasuredNodes = 4;

  /// How many instructions a rewrite has to remove before it is made.
  ///
  /// One is enough because the InstCombine that follows cleans up any residue.
  /// Zero still refuses a rewrite that would hand back more instructions than
  /// it was given; there is no setting that lets this pass grow a function.
  size_t MinInstructionsSaved = 1;

  /// Policy for the exact derivational MBA engine.
  symbolic::MBAOptions MBA;

  /// Search policy used only after derivation fails to improve a root.
  symbolic::SynthOptions Synthesis;

  /// Resource policy for the built-in proof provider.
  solver::SolverOptions Solver;

  /// Optional proof budget for heuristic synthesis alone. Without an override,
  /// synthesis uses Solver too. Source views bound their new search without
  /// reducing the existing split-word arithmetic recovery proof allowance.
  std::optional<solver::SolverOptions> SynthesisSolver;

  /// Permit heuristic candidate discovery.  Off by default so existing pass
  /// pipelines remain derivational until they opt into solver-backed search.
  bool EnableSynthesis = false;

  ProofProvider Provider = ProofProvider::BuiltInSolver;

  /// Used only when \c Provider is \c Callback.  An empty callback is an
  /// incomplete proof and therefore cannot authorize a rewrite.
  std::function<symbolic::SynthVerification(symbolic::SymContext &,
                                            symbolic::SymRef, symbolic::SymRef)>
      ProofCallback;

  /// Cumulative work for exact two-valued integer slices, including dependency
  /// edges and profitability checks. Zero disables this derivational phase.
  /// Exhaustion keeps any unfinished slice unchanged; it never supplies facts.
  /// This phase requires a strict instruction-count improvement even when
  /// MinInstructionsSaved is zero.
  size_t MaxFiniteValueWork = 262144;

  /// Cumulative work for exact modular truth sets over one retained SSA value.
  /// Includes width-weighted algebra, dependencies, uses and rewrite work.
  /// Zero disables this phase; incomplete or disconnected sets stay unchanged.
  /// Requires strict instruction savings, including under aggressive().
  size_t MaxPredicateWork = 262144;

  /// Take every rewrite the engine can derive, however small the expression
  /// and however little the result saves.
  static SymSimplifyOptions aggressive() {
    SymSimplifyOptions Opts;
    Opts.MinMeasuredNodes = 1;
    Opts.MinInstructionsSaved = 0;
    return Opts;
  }
};

/// Rewrites integer expressions into the shortest sequence computing the same
/// value, using measurement rather than pattern matching.
///
/// Belongs in the same function pass manager as InstCombine, and immediately
/// after it: the canonical form InstCombine leaves is the cleanest thing to
/// measure, and the compact result of a measurement is the cleanest thing for
/// another InstCombine to finish.  Neither reaches the other's fixed point
/// alone -- an expression mixing `+ - *` with `& | ^ ~` blocks every rule
/// InstCombine can state, and the arithmetic residue this pass leaves is what
/// InstCombine folds best.
struct SymSimplifyPass : public llvm::PassInfoMixin<SymSimplifyPass> {
  SymSimplifyPass() = default;
  explicit SymSimplifyPass(SymSimplifyOptions Opts) : Opts(Opts) {}

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);

  /// Standalone entry point that instantiates no PassManager template, so a
  /// caller in a different image than libneverd can apply the transform without
  /// tripping an AnalysisKey ODR violation (mirrors the L1 obfuscation passes).
  /// Returns the number of expression roots rewritten.
  static unsigned simplify(llvm::Function &F, SymSimplifyOptions Opts = {});

  /// Apply the same transform as \c simplify and return its search and proof
  /// disposition without combining unlike work counters.
  static SymSimplifyResult simplifyWithResult(llvm::Function &F,
                                              SymSimplifyOptions Opts = {});

  /// Run only the exact two-valued phase, with the same input, poison,
  /// profitability and obfuscation-stamp policy as the full pass. No MBA
  /// synthesis or solver queries run. MaxFiniteValueWork == 0 disables it;
  /// incomplete slices stay unchanged and only completed rewrites count.
  static SymFiniteValueSimplifyResult
  simplifyFiniteValues(llvm::Function &F, SymSimplifyOptions Opts = {});

  /// Run only the existing exact modular predicate phase. This entry shares
  /// its poison/input, profitability and obfuscation-stamp policy with the
  /// full pass, without MBA synthesis or solver queries. Completed rewrites
  /// are retained; budget exhaustion leaves unfinished roots unchanged.
  /// MaxPredicateWork == 0 disables this phase without spending work.
  static SymPredicateSimplifyResult
  simplifyPredicates(llvm::Function &F, SymSimplifyOptions Opts = {});

  /// The value \p V always holds, when measuring it says it holds one.
  ///
  /// Offered on its own because deciding a branch is not the only place a value
  /// that cannot vary is worth spotting.  A flattened function computes the
  /// number of the block to run next, and an obfuscator hides that number the
  /// same way it hides everything else -- as arithmetic that no amount of
  /// constant folding sees through, but that measuring collapses. Answering
  /// with the number is what lets the control-flow recovery thread the jump.
  ///
  /// No threshold applies: a value that cannot vary is worth the same whether
  /// it took four nodes to say so or forty.
  static std::optional<llvm::APInt> constantValueOf(llvm::Value *V);

private:
  SymSimplifyOptions Opts;
};

} // namespace neverd

#endif // NEVERD_PASS_IR_SIMPLIFY_SYMSIMPLIFYPASS_H
