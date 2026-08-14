//===- SatDetail.h - Internals of the CDCL search ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The state the conflict-driven search runs on: the clause arena, the watch
/// lists, the assignment trail, and the activity queue that orders decisions.
///
/// Three representation choices shape everything above them.
///
/// *Clauses live in one arena of words and are named by their offset in it.*
/// A clause reference is therefore an integer rather than a pointer, so the
/// watch lists, the reason of every assignment and the learned-clause list are
/// all arrays of integers that copy and compare trivially.
///
/// *A clause keeps its two watched literals in slots zero and one.*  The
/// invariant is what makes propagation cheap, and it carries a second meaning
/// that conflict analysis relies on: when a clause is the reason for an
/// assignment, the literal it forced is slot zero, so every walk of the reason
/// graph can skip that slot without searching for it.
///
/// *The trail is a single array with a marker per decision level.*  Undoing a
/// level is a truncation, which is why backjumping many levels at once costs
/// no more than backtracking one.
///
/// This header is an implementation detail of the solver library and should
/// not be included outside lib/solver/sat/.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SOLVER_SAT_SATDETAIL_H
#define NEVERD_SOLVER_SAT_SATDETAIL_H

#include "neverd/solver/SatSolver.h"
#include "neverd/solver/SatTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd::solver::detail {

/// A clause's offset in the arena.
using ClauseRef = uint32_t;
inline constexpr ClauseRef kNoClause = ~ClauseRef(0);

/// Largest arena a clause reference can address.  Running into it needs tens
/// of gigabytes of clauses; the engine reports it as an exhausted resource
/// rather than pretending the formula was decided.
inline constexpr size_t kMaxArenaWords = size_t(kNoClause) - 1;

/// A view of one clause in the arena.
///
/// Literals are stored as their packed words rather than as \c SatLit objects
/// so that the arena stays a plain array of integers and nothing here depends
/// on how a literal is laid out in memory.
class ClauseView {
public:
  /// Size, flags with the level span, and activity.
  static constexpr uint32_t kHeaderWords = 3;

  explicit ClauseView(uint32_t *Words) : Words(Words) {}

  /// Lay out a fresh clause.  The caller has already reserved the space.
  void initialize(uint32_t Size, bool Learned) {
    Words[0] = Size;
    Words[1] = Learned ? kLearnedFlag : 0;
    setActivity(0.0f);
  }

  uint32_t size() const { return Words[0]; }
  bool isLearned() const { return (Words[1] & kLearnedFlag) != 0; }
  bool isDeleted() const { return (Words[1] & kDeletedFlag) != 0; }
  void markDeleted() { Words[1] |= kDeletedFlag; }

  /// How many distinct decision levels the clause's literals were assigned at
  /// when it was learned.
  ///
  /// This is the best available guess at whether a learned clause will ever
  /// propagate again: a clause tying together few levels is close to a fact
  /// about the formula, while one tying together many is a record of one
  /// particular path through the search and is unlikely to be reached the same
  /// way twice.  Database reduction discards by this before it discards by
  /// activity, because activity only says a clause was useful recently.
  uint32_t levelSpan() const { return Words[1] >> kFlagBits; }
  void setLevelSpan(uint32_t Span) {
    Words[1] = (Words[1] & kFlagMask) | (std::min(Span, kMaxSpan) << kFlagBits);
  }

  /// How often the clause has taken part in a recent conflict.  Stored as a
  /// float because there is one per clause and the database is the largest
  /// thing the solver holds.
  float activity() const {
    float F;
    std::memcpy(&F, Words + 2, sizeof F);
    return F;
  }
  void setActivity(float F) { std::memcpy(Words + 2, &F, sizeof F); }

  SatLit lit(uint32_t I) const {
    return SatLit::fromIndex(Words[kHeaderWords + I]);
  }
  void setLit(uint32_t I, SatLit L) { Words[kHeaderWords + I] = L.index(); }
  void swapLits(uint32_t I, uint32_t J) {
    std::swap(Words[kHeaderWords + I], Words[kHeaderWords + J]);
  }

  /// Words this clause occupies, which is what deleting it reclaims.
  uint32_t words() const { return kHeaderWords + size(); }

private:
  static constexpr uint32_t kLearnedFlag = 1;
  static constexpr uint32_t kDeletedFlag = 2;
  static constexpr uint32_t kFlagBits = 2;
  static constexpr uint32_t kFlagMask = (1u << kFlagBits) - 1;
  static constexpr uint32_t kMaxSpan = ~0u >> kFlagBits;

  uint32_t *Words;
};

/// One entry of a watch list.
struct Watcher {
  ClauseRef Clause;
  /// Another literal of the same clause.  When it is already true the clause
  /// cannot propagate, and knowing that without reading the arena is what
  /// keeps propagation inside cache: most visits end here.
  SatLit Blocker;
};

/// A max-heap of variables ordered by branching activity.
///
/// Ties break towards the lower variable number rather than towards whichever
/// order the heap happened to be built in.  That costs nothing and is what
/// makes the whole search reproducible, including on a formula where every
/// activity is still zero because no conflict has happened yet.
class ActivityQueue {
public:
  explicit ActivityQueue(const std::vector<double> &Activity)
      : Activity(Activity) {}

  bool empty() const { return Heap.empty(); }
  size_t size() const { return Heap.size(); }

  bool contains(SatVar V) const {
    return V < Position.size() && Position[V] >= 0;
  }

  void grow(size_t NumVars) {
    if (Position.size() < NumVars)
      Position.resize(NumVars, -1);
  }

  void insert(SatVar V);
  /// Restore the heap after \p V's activity rose.
  void increased(SatVar V);
  /// The most active variable, removed from the queue.
  SatVar removeMax();
  void clear();

private:
  /// Strict priority: more active first, and on a tie the lower number.
  bool higher(SatVar A, SatVar B) const {
    if (Activity[A] != Activity[B])
      return Activity[A] > Activity[B];
    return A < B;
  }

  void siftUp(size_t I);
  void siftDown(size_t I);

  const std::vector<double> &Activity;
  std::vector<SatVar> Heap;
  /// Position of each variable in \c Heap, or -1 when it is not queued.
  std::vector<int32_t> Position;
};

/// Term \p I of the restart schedule, counting from zero: 1, 1, 2, 1, 1, 2, 4,
/// 1, 1, 2, 1, 1, 2, 4, 8, ...
///
/// The schedule matters more than the constant it scales.  Every prefix of it
/// is mostly short intervals, so a run that only needed a different first
/// decision gets many cheap chances to find one; but the intervals grow
/// without bound, so a run that is genuinely making progress is eventually
/// left alone to finish.  No fixed interval has both properties, and choosing
/// one per formula is exactly the guess this avoids.
uint64_t restartTerm(uint64_t I);

/// The conflict-driven search and everything it runs on.
///
/// This is one object rather than several because the pieces are not
/// separable: propagation writes the trail that analysis reads, analysis
/// writes the activities that decisions read, and reduction reads the reasons
/// that propagation wrote.  Splitting it into collaborators would mean handing
/// each of them references to most of the others.  It is split across
/// translation units by phase instead.
class SatEngine {
public:
  explicit SatEngine(const SatOptions &Opts);

  //===--------------------------------------------------------------------===//
  // Building the formula — SatSolver.cpp
  //===--------------------------------------------------------------------===//

  SatVar newVar(bool Decision);
  bool addClause(llvm::ArrayRef<SatLit> Lits);

  //===--------------------------------------------------------------------===//
  // Searching — SatSolver.cpp
  //===--------------------------------------------------------------------===//

  SatResult solve(llvm::ArrayRef<SatLit> Assumptions);
  SatResult search(uint64_t ConflictBudget, llvm::ArrayRef<SatLit> Assumptions);
  bool outOfPropagationBudget() const;
  bool outOfWatchVisitBudget() const;
  bool outOfBudget() const;
  void captureModel();

  //===--------------------------------------------------------------------===//
  // Propagation and the trail — SatPropagate.cpp
  //===--------------------------------------------------------------------===//

  ClauseRef allocClause(llvm::ArrayRef<SatLit> Lits, bool Learned);
  void attachClause(ClauseRef Ref);
  void detachClause(ClauseRef Ref);
  bool isLocked(ClauseRef Ref);

  /// Assign \p L, recording \p From as what forced it.  The caller has already
  /// checked that \p L is unassigned.
  void enqueue(SatLit L, ClauseRef From);
  /// Propagate every consequence of the pending assignments.  Returns the
  /// clause that was falsified, or \c kNoClause.
  ClauseRef propagate();
  void newDecisionLevel();
  void cancelUntil(uint32_t Level);

  uint32_t decisionLevel() const {
    return static_cast<uint32_t>(TrailLim.size());
  }

  /// +1 true, -1 false, 0 unassigned.  A signed value rather than an enum
  /// because negating a literal's worth is then a negation.
  int8_t varValue(SatVar V) const { return Value[V]; }
  int8_t litValue(SatLit L) const {
    int8_t V = Value[L.var()];
    return L.isNegated() ? static_cast<int8_t>(-V) : V;
  }

  ClauseView clause(ClauseRef Ref) { return ClauseView(Arena.data() + Ref); }

  //===--------------------------------------------------------------------===//
  // Conflict analysis — SatAnalyze.cpp
  //===--------------------------------------------------------------------===//

  /// Derive the clause explaining \p Conflict, together with the level to jump
  /// back to and the clause's level span.
  void analyze(ClauseRef Conflict, std::vector<SatLit> &Learnt,
               uint32_t &BacktrackLevel, uint32_t &LevelSpan);
  /// True when \p L is already implied by the other literals of the clause
  /// being derived, so dropping it loses nothing.
  bool isRedundant(SatLit L, uint32_t LevelMask);
  /// Collect the assumptions that together make \p Failed impossible.
  void analyzeFinal(SatLit Failed, std::vector<SatLit> &Out);

  //===--------------------------------------------------------------------===//
  // Heuristics — SatHeuristic.cpp
  //===--------------------------------------------------------------------===//

  SatLit pickBranchLit();
  void bumpVarActivity(SatVar V);
  void decayVarActivity() { VarInc /= Opts.VarDecay; }
  void bumpClauseActivity(ClauseRef Ref);
  void decayClauseActivity() { ClauseInc /= Opts.ClauseDecay; }
  void reduceDatabase();

  //===--------------------------------------------------------------------===//
  // State
  //===--------------------------------------------------------------------===//

  SatOptions Opts;
  SatStats Stats;

  /// Set once the clauses alone are contradictory.  Never cleared: a formula
  /// that has been shown unsatisfiable stays so however many clauses are added
  /// afterwards.
  bool Falsified = false;
  /// Set when a clause could not be stored.  The formula is then neither
  /// decided nor decidable, so every answer becomes \c SatResult::Unknown.
  bool OutOfSpace = false;

  std::vector<uint32_t> Arena;
  std::vector<ClauseRef> Clauses;
  std::vector<ClauseRef> Learnt;
  /// Words held by deleted clauses.  The arena is not compacted, so this is
  /// what a caller watching memory would look at.
  size_t WastedWords = 0;

  /// One list per literal, holding the clauses that must be visited when that
  /// literal becomes true.
  std::vector<std::vector<Watcher>> Watches;

  std::vector<int8_t> Value;
  std::vector<uint32_t> Level;
  std::vector<ClauseRef> Reason;
  /// The value each variable last held, re-used when it is decided again.
  std::vector<uint8_t> Phase;
  std::vector<uint8_t> Decidable;
  /// Scratch marks for conflict analysis, always all-zero between conflicts.
  std::vector<uint8_t> Seen;
  std::vector<double> Activity;
  ActivityQueue Order;

  std::vector<SatLit> Trail;
  /// Where each decision level starts in \c Trail.
  std::vector<uint32_t> TrailLim;
  /// First trail entry whose consequences have not been propagated.
  size_t QueueHead = 0;

  double VarInc = 1.0;
  double ClauseInc = 1.0;
  /// Learned clauses to keep, grown at every restart.
  double MaxLearnt = 0.0;

  /// Counter values at which the current call gives up, or zero for no bound.
  /// They are absolute rather than remaining because the statistics accumulate
  /// across calls while a budget is per call: a caller asking a thousand
  /// incremental questions wants each one bounded, not the thousandth to
  /// inherit the cost of the first.
  uint64_t ConflictBudgetAt = 0;
  uint64_t PropagationBudgetAt = 0;
  uint64_t WatchVisitBudgetAt = 0;

  /// The satisfying assignment, captured before the trail is unwound.
  std::vector<int8_t> Model;
  std::vector<SatLit> FailedAssumptions;

  /// Scratch buffers, kept so that a conflict does not allocate.
  std::vector<SatLit> LearntScratch;
  std::vector<SatVar> ToClear;
  std::vector<SatLit> RedundancyStack;
  std::vector<SatLit> ClauseScratch;
  std::vector<uint32_t> LevelScratch;
};

} // namespace neverd::solver::detail

#endif // NEVERD_SOLVER_SAT_SATDETAIL_H
