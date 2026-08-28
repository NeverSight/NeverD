//===- SymExplore.h - Walking every path a function can take ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Runs a whole function symbolically instead of one straight line of it, by
/// forking at every branch whose outcome the code does not already decide.
///
/// Three things make that finite.  A budget, for callers who want one.  Loop
/// headers, because every cycle in a graph passes through one, so bounding how
/// often a path may re-enter a header bounds how long a path can be — which is
/// what lets the other budgets be lifted entirely without the walk running
/// away.  And, before any of that, the fact that a great many branches are not
/// branches at all: a predicate that folds to a constant has one reachable
/// side, and the walk takes it without forking.
///
/// That last point is not a detail.  Opaque predicates — a condition contrived
/// to always hold, guarding code that never runs — are the standard way to
/// bloat a control-flow graph, and they disappear here as a consequence of the
/// expression builders folding, without a rule that mentions them and without
/// asking a solver anything.
///
/// A budget that is reached is reported, never absorbed.  A caller can tell a
/// walk that finished from one that stopped, which matters because the two
/// look identical in the results and mean opposite things.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SYMBOLIC_SYMEXPLORE_H
#define NEVERD_SYMBOLIC_SYMEXPLORE_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/symbolic/SymExec.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace neverd::symbolic {

/// Why a path stopped.
enum class PathOutcome : uint8_t {
  /// Reached a return.  The interesting one: the state is what the function
  /// leaves behind when it takes this path.
  Returned,
  /// Branched to an address this function does not contain — a tail call, or
  /// an edge out of what was lifted.
  LeftFunction,
  /// Reached a branch through an address that did not come out constant.
  UnresolvedBranch,
  /// A concrete branch or return target contradicted its instruction-mode
  /// contract, address width, or architectural alignment.
  InvalidControlTarget,
  /// Ran out of the allowance for re-entering one block, which is what bounds
  /// a loop nothing else bounds.
  LoopBudget,
  /// Ran out of the allowance for operations.
  StepBudget,
  /// Every way on from here contradicts something already assumed.
  Infeasible,
};

const char *pathOutcomeName(PathOutcome Outcome);

/// A bound that is not applied.
///
/// What stops the walk is then structural — a return, an edge out of the
/// function, or the loop-header bound — rather than an allowance running out,
/// so a result cannot quietly be a prefix of the real one.  Termination still
/// has to come from somewhere: leaving \c MaxSteps, \c MaxBlockVisits and
/// \c MaxLoopIterations all unbounded leaves it to the program being walked.
inline constexpr unsigned kUnbounded = 0;

/// One register of a concolic seed.
struct SymConcreteRegister {
  uint64_t Offset = 0;
  uint64_t Value = 0;
};

/// Identity of one executed call, including its zero-based repetition on this
/// path.  Address plus sequence distinguishes multiple LowIR calls emitted for
/// one machine instruction; Invocation distinguishes loop iterations.
struct SymCallOccurrence {
  va_t VA = 0;
  int Seq = -1;
  int BlockId = -1;
  size_t OpIndex = 0;
  unsigned Invocation = 0;
};

/// Supplies a complete, analysis-neutral effect for an exact call occurrence.
/// Returning no value preserves conservative call havoc.  The provider may
/// read \p State to recover arguments, but must express every state mutation in
/// the returned effect.
using SymCallEffectProvider = std::function<std::optional<SymCallEffect>(
    SymContext &, SymState &, const LowOp &, const SymCallOccurrence &)>;

/// The concrete half of a concolic walk.
///
/// Declared as an interface rather than as the emulator it will be, because
/// the concrete engine reads the loaded image and the symbolic engine must not
/// depend on the loader to keep the dependency running one way.  The adapter
/// that drives that emulator therefore ships with the emulator.
///
/// A shadow only ever has to answer two questions: carry this operation out,
/// and what does this operand hold.  The second is the whole point — it is
/// what turns "both sides of this branch are possible" into "this run went
/// left", and so what replaces a fork with a decision.
class ConcreteShadow {
public:
  virtual ~ConcreteShadow() = default;

  /// Start from nothing.  Called once, before the walk.
  virtual void reset() = 0;

  /// Give the register at \p Offset the value \p Value.
  virtual void setRegister(uint64_t Offset, uint64_t Value) = 0;

  /// Carry \p Op out concretely.  A false result means the shadow could not,
  /// and the walk stops consulting it from there on rather than following a
  /// concrete state that has drifted from the code.
  virtual bool step(const LowOp &Op) = 0;

  /// What \p V concretely holds, or nothing when the shadow has no value for
  /// it — which likewise ends the concrete half of the walk.
  virtual std::optional<uint64_t> value(const NdVar &V) const = 0;
};

struct ExploreOptions {
  /// How many reachable paths to finish before giving up on the rest.
  /// Infeasible diagnostics do not consume this allowance.
  unsigned MaxPaths = 64;
  /// Operations allowed along one path.
  unsigned MaxSteps = 1u << 16;
  /// How often one path may enter the same block, whichever block it is.
  unsigned MaxBlockVisits = 3;
  /// How often one path may enter a *loop header* — a block some edge returns
  /// to.  A block is bounded by whichever of this and \c MaxBlockVisits is
  /// tighter, so the two agree at their defaults and part company when the
  /// general bound is lifted: unrolling a loop a set number of times while
  /// leaving straight-line depth alone is what a deeper walk usually wants.
  unsigned MaxLoopIterations = 3;
  /// Continue two paths that arrive at one block holding the same values in
  /// every location as a single path whose condition is their disjunction.
  ///
  /// Re-convergent control flow is what makes path counts exponential: a
  /// function with n independent two-way choices in a row has 2^n paths and
  /// frequently far fewer distinct outcomes.  Off by default because it costs
  /// a comparison at every join and because a merged path reports one of the
  /// routes it stands for rather than all of them.
  bool MergeEquivalentPaths = false;
  /// Target byte order used for register and memory split/join operations.
  llvm::endianness ByteOrder = llvm::endianness::little;
  /// Register byte ranges preserved by the target's calling convention.
  std::vector<SymRegisterRange> CallPreservedRegisters;
  /// Optional ABI return register to snapshot immediately after every call.
  std::optional<SymRegisterRange> TrackedCallResultRegister;
  /// Optional complete call contracts.  This callback lives in Symbolic's
  /// neutral vocabulary so higher-level analyses can adapt their own catalogs
  /// without creating a dependency from Symbolic back to them.
  SymCallEffectProvider CallEffects;
  /// The concrete engine of a concolic walk, or null for a purely symbolic
  /// one.
  ///
  /// With one set, the walk stops forking: at a branch both of whose sides are
  /// live it asks the shadow which way this concrete run goes, takes that side
  /// and records the symbolic condition for having gone that way.  What comes
  /// back is one path and the predicate every input that follows it satisfies
  /// — the question a fuzzer's next input is the answer to, and one no amount
  /// of forking answers as directly.
  ///
  /// Not owned, and not reset between calls beyond the one reset the walk
  /// performs at its start.
  ConcreteShadow *Concolic = nullptr;
  /// Register values handed to \c Concolic once the walk has reset it.  This
  /// is the concrete input the walk follows; the symbolic side is left free,
  /// which is what makes the recorded condition worth having.
  std::vector<SymConcreteRegister> ConcolicSeed;
};

struct SymCallResult {
  va_t CallVA = 0;
  int BlockId = -1;
  size_t OpIndex = 0;
  SymRef Value;
};

/// One LowIR operation actually executed on a symbolic path.
///
/// Keeping this separate from \c SymPath::Blocks matters when exploration
/// stops in the middle of a block or a control transfer skips the remaining
/// operations in that block.  Consumers must not infer execution merely from
/// block membership.
struct SymExecutedOp {
  int BlockId = -1;
  size_t OpIndex = 0;
  va_t VA = 0;
  int Seq = -1;
  NdOp Opcode = NdOp::_COUNT;
};

/// One finished path.
struct SymPath {
  PathOutcome Outcome = PathOutcome::StepBudget;
  /// The block it stopped in.
  int BlockId = -1;
  /// Everything assumed along the way, in the order it was assumed.  A branch
  /// the code already decided contributes nothing, so this lists the genuine
  /// choices and only those.
  std::vector<SymRef> Constraints;
  /// The blocks entered, in order.  What the path *is* — or, when
  /// \c MergedPaths is not zero, one of the routes it stands for.
  std::vector<int> Blocks;
  /// Operations actually executed, in order.
  std::vector<SymExecutedOp> ExecutedOps;
  /// The machine state where it stopped.
  SymState State;
  /// Call results captured before a later call can clobber the return register.
  std::vector<SymCallResult> CallResults;
  /// For a branch that left the function or could not be resolved, its target;
  /// for an explicitly targeted return, the canonical return address.  A
  /// concrete interworking target is stored after pointer tag removal.
  SymRef Target;
  /// Decode mode of the instruction where this path stopped, when boundary
  /// metadata identifies it.
  std::optional<InstructionMode> SourceMode;
  /// Mode selected for the stopping control transfer's destination.  This is
  /// absent when the target expression itself selects the mode and remained
  /// symbolic, or when legacy LowIR carried no boundary metadata.
  std::optional<InstructionMode> DestinationMode;
  /// Operations conservatively replaced by unknown values along this path.
  unsigned UnmodelledOps = 0;
  /// Operations whose value semantics were unavailable.
  unsigned OpaqueOps = 0;
  /// Calls whose effects were conservatively havoced.
  unsigned CallHavocs = 0;
  /// Writes that required conservative may-alias memory havoc.
  unsigned MemoryHavocs = 0;
  /// Branches a concrete shadow decided rather than the walk forking at.  Zero
  /// for a purely symbolic walk.
  unsigned ConcreteBranches = 0;
  /// Routes continued as this one because they reached a join holding the same
  /// values.  This path's condition is the disjunction over all of them, and
  /// \c Blocks is whichever of them arrived first.
  unsigned MergedPaths = 0;

  /// The conjunction of the constraints, or true when there are none.
  SymRef predicate(SymContext &Ctx) const;
};

/// Paths and completeness information from one function walk.
struct SymExploration {
  std::vector<SymPath> Paths;
  /// True only when every reachable frontier ended without a path or loop
  /// budget and without an unresolved or invalid control target.
  bool Complete = false;
  unsigned ReachablePaths = 0;
  size_t ExecutedSteps = 0;
  unsigned UnmodelledOps = 0;
  /// Routes that were continued as some other path rather than reported on
  /// their own.  Reported paths plus this is what an unmerged walk would have
  /// come back with.
  unsigned MergedPaths = 0;
};

SymExploration explorePathsDetailed(SymContext &Ctx, const LowFunc &Func,
                                    const ExploreOptions &Opts = {});

/// Walk \p Func from its entry block.
///
/// Paths come back in the order they finished, which for the depth-first walk
/// used here means the first is the one that kept taking the branch.
std::vector<SymPath> explorePaths(SymContext &Ctx, const LowFunc &Func,
                                  const ExploreOptions &Opts = {});

} // namespace neverd::symbolic

#endif // NEVERD_SYMBOLIC_SYMEXPLORE_H
