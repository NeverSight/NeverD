//===- LowIRLoopAlignment.cpp - Bounded search for paired loop cuts -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LowIRLoopInference.h"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <set>

namespace neverd::analysis {
namespace {
using Status = LowIRLoopAlignmentStatus;
using CutFamily = detail::LowIRLoopCutFamily;
struct Stop {};

class AlignmentSearch {
  const LowFunc &Original, &Candidate;
  llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions;
  const LowIRIndependenceContract &Contract;
  LowIRRefinementWitness Witness;
  const LowIRLoopAlignmentLimits &Limits;
  LowIRLoopAlignmentResult Result;
  bool AttemptExhausted = false;
  struct CachedPlan {
    bool Attempted = false;
    std::optional<LowIRLoopRefinementPlan> Plan;
    std::string Diagnostic;
  };
  std::array<CachedPlan, 3> OriginalPlans, CandidatePlans;
  uint64_t RemainingCachedMetadata;
  using BlockMap = std::map<int, const LowBlock *>;
  using CutPairs = std::vector<LowIRLoopCutpointPair>;

  [[noreturn]] void stop(Status S, llvm::StringRef Message) {
    Result.Status = S;
    Result.Diagnostic = Message.str();
    throw Stop{};
  }

  void charge(uint64_t Count = 1) {
    if (Count > Limits.MaxSearchWork - Result.SearchWork)
      stop(Status::BudgetExceeded, "loop alignment search work exhausted");
    Result.SearchWork += Count;
  }

  void requireQueries() {
    if (Result.SolverQueries == Limits.MaxSolverQueries)
      stop(Status::BudgetExceeded,
           "loop alignment total query budget exhausted");
  }

  uint32_t queryGrant(uint32_t StageLimit) {
    requireQueries();
    const uint64_t Remaining = Limits.MaxSolverQueries - Result.SolverQueries;
    return static_cast<uint32_t>(std::min<uint64_t>(StageLimit, Remaining));
  }

  // Validate identities before any search indexes or traverses successors.
  // Semantic graph validation remains with the existing executor.
  BlockMap index(const LowFunc &F) {
    charge(F.Blocks.size());
    BlockMap Blocks;
    std::set<va_t> Addresses;
    for (const auto &B : F.Blocks)
      if (!Blocks.emplace(B.Id, &B).second ||
          !Addresses.insert(B.StartAddr).second)
        stop(Status::Invalid, "duplicate LowIR block identity");
    if (!Addresses.count(F.Entry))
      stop(Status::Invalid, "missing LowIR entry block");
    for (const auto &B : F.Blocks) {
      charge(B.Succs.size());
      std::set<int> Unique;
      for (int S : B.Succs)
        if (!Blocks.count(S) || !Unique.insert(S).second)
          stop(Status::Invalid, "unknown or duplicate CFG successor");
    }
    return Blocks;
  }

  bool cyclic(const LowBlock &Root, const BlockMap &Blocks) {
    charge();
    std::vector<int> Pending{Root.Id};
    std::set<int> Seen{Root.Id};
    while (!Pending.empty()) {
      charge();
      const auto &B = *Blocks.at(Pending.back());
      Pending.pop_back();
      charge(B.Succs.size());
      for (int S : B.Succs) {
        if (S == Root.Id)
          return true;
        if (Seen.insert(S).second)
          Pending.push_back(S);
      }
    }
    return false;
  }

  LowIRLoopInferenceResult
  infer(const LowFunc &F, LowIRLoopInferenceLimits Stage,
        llvm::ArrayRef<va_t> Eligible = {},
        CutFamily Family = CutFamily::Default,
        llvm::ArrayRef<const LowIRLoopRefinementPlan *> Previous = {}) {
    if (Result.SearchWork == Limits.MaxSearchWork)
      stop(Status::BudgetExceeded, "loop alignment search work exhausted");
    Stage.Execution.MaxSolverQueries =
        queryGrant(Stage.Execution.MaxSolverQueries);
    Stage.MaxCutSelectionWork = std::min(
        Stage.MaxCutSelectionWork, Limits.MaxSearchWork - Result.SearchWork);
    auto R = Family == CutFamily::Default
                 ? inferLowIRLoopRefinementPlan(F, Contract, Stage, Eligible)
                 : detail::inferLowIRLoopRefinementPlanFamily(
                       F, Contract, Stage, Family, Previous);
    Result.SolverQueries += R.SolverQueries;
    charge(R.CutSelectionWork);
    AttemptExhausted |= R.Status == LowIRLoopInferenceStatus::BudgetExceeded;
    if (R.Status == LowIRLoopInferenceStatus::Invalid)
      stop(Status::Invalid, R.Diagnostic);
    return R;
  }

  // Count before allocating pairings or invoking the authoritative pairer.
  bool metadata(uint64_t Count, uint64_t &Remaining) {
    charge(Count);
    if (Count > Remaining) {
      AttemptExhausted = true;
      Result.LastCandidateDiagnostic =
          "loop alignment metadata budget exhausted";
      return false;
    }
    Remaining -= Count;
    return true;
  }

  bool planMetadata(const LowIRLoopRefinementPlan &Plan, uint64_t &Remaining) {
    if (!metadata(Plan.Cutpoints.size(), Remaining))
      return false;
    for (const auto &C : Plan.Cutpoints)
      for (size_t Count :
           {C.Inputs.size(), C.Expressions.size(), C.OriginalState.size(),
            C.CandidateState.size(), C.Rank.size(), C.OriginalGuards.size(),
            C.CandidateGuards.size()})
        if (!metadata(Count, Remaining))
          return false;
    return true;
  }

  void beginPairing() {
    if (Result.PairingAttempts >= Limits.MaxPairingAttempts)
      stop(Status::BudgetExceeded, "loop alignment pairing budget exhausted");
    ++Result.PairingAttempts;
  }

  bool framePairs(const LowIRLoopRefinementPlan &Left,
                  const LowIRLoopRefinementPlan &Right,
                  llvm::ArrayRef<size_t> Order, uint64_t &Remaining,
                  CutPairs &Pairs) {
    if (!metadata(Order.size(), Remaining))
      return false;
    for (size_t I = 0; I != Order.size(); ++I) {
      const auto &L = Left.Cutpoints[I], &R = Right.Cutpoints[Order[I]];
      LowIRLoopCutpointPair Pair;
      Pair.OriginalAddress = L.OriginalAddress;
      Pair.CandidateAddress = R.OriginalAddress;
      for (const auto &LI : L.Inputs) {
        charge();
        if (LI.Side != LowIRLoopSide::Original ||
            LI.Location.Space != LowIRLoopSpace::Frame)
          continue;
        for (const auto &RI : R.Inputs) {
          charge();
          if (RI.Side == LowIRLoopSide::Original &&
              RI.Location.Space == LowIRLoopSpace::Frame &&
              LI.Location.Offset == RI.Location.Offset &&
              LI.Location.Bytes == RI.Location.Bytes) {
            if (!metadata(1, Remaining))
              return false;
            Pair.SharedInputs.push_back({LI.Location, RI.Location});
          }
        }
      }
      Pairs.push_back(std::move(Pair));
    }
    return true;
  }

  bool checkPairs(const LowIRLoopRefinementPlan &Left,
                  const LowIRLoopRefinementPlan &Right, const CutPairs &Pairs) {
    auto Plan =
        pairLowIRLoopRefinementPlans(Left, Right, Pairs, Limits.MaxMetadata);
    if (!Plan) {
      Result.LastCandidateDiagnostic = llvm::toString(Plan.takeError());
      return false;
    }
    auto ProofLimits = Limits.Proof;
    ProofLimits.Execution.MaxSolverQueries =
        queryGrant(ProofLimits.Execution.MaxSolverQueries);
    Result.Refinement =
        checkLowIRLoopRefinement(Original, OriginalInstructions, Candidate,
                                 Contract, *Plan, Witness, ProofLimits);
    Result.SolverQueries += Result.Refinement.SolverQueries;
    if (Result.Refinement.proved()) {
      Result.Status = Status::Proved;
      return true;
    }
    Result.LastCandidateDiagnostic = Result.Refinement.Diagnostic;
    if (Result.Refinement.Status == LowIRRefinementStatus::Invalid)
      stop(Status::Invalid, Result.Refinement.Diagnostic);
    AttemptExhausted |=
        Result.Refinement.Status == LowIRRefinementStatus::BudgetExceeded;
    requireQueries();
    return false;
  }

  bool pairAndCheck(const LowIRLoopRefinementPlan &Left,
                    const LowIRLoopRefinementPlan &Right,
                    llvm::ArrayRef<size_t> Order) {
    beginPairing();
    uint64_t Remaining = Limits.MaxMetadata;
    CutPairs Pairs;
    return planMetadata(Left, Remaining) && planMetadata(Right, Remaining) &&
           framePairs(Left, Right, Order, Remaining, Pairs) &&
           checkPairs(Left, Right, Pairs);
  }

  static bool sameLocation(const LowIRLoopLocation &A,
                           const LowIRLoopLocation &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Bytes == B.Bytes;
  }

  const LowIRLoopInput *rankInput(const LowIRLoopCutpoint &Cut, NdVar Rank) {
    const LowIRLoopInput *Found = nullptr;
    for (const auto &I : Cut.Inputs) {
      charge();
      if (I.Side != LowIRLoopSide::Original || I.Temporary != Rank)
        continue;
      if (Found || !I.Location.Bytes || I.Location.Bytes != Rank.Size)
        return nullptr;
      Found = &I;
    }
    return Found;
  }

  bool rankPairs(const LowIRLoopRefinementPlan &Left,
                 const LowIRLoopRefinementPlan &Right,
                 llvm::ArrayRef<size_t> Order, uint64_t &Remaining,
                 CutPairs &Pairs) {
    if (!metadata(Order.size(), Remaining))
      return false;
    for (size_t I = 0; I != Order.size(); ++I) {
      const auto &L = Left.Cutpoints[I], &R = Right.Cutpoints[Order[I]];
      if (L.Rank.empty() || L.Rank.size() != R.Rank.size())
        return false;
      LowIRLoopCutpointPair Pair{L.OriginalAddress, R.OriginalAddress, {}};
      for (size_t J = 0; J != L.Rank.size(); ++J) {
        charge();
        const auto *LI = rankInput(L, L.Rank[J]);
        const auto *RI = rankInput(R, R.Rank[J]);
        if (!LI || !RI || LI->Location.Bytes != RI->Location.Bytes)
          return false;
        for (const auto &Previous : Pair.SharedInputs) {
          charge();
          if (sameLocation(Previous.Original, LI->Location) ||
              sameLocation(Previous.Candidate, RI->Location))
            return false;
        }
        if (!metadata(1, Remaining))
          return false;
        Pair.SharedInputs.push_back({LI->Location, RI->Location});
      }
      Pairs.push_back(std::move(Pair));
    }
    return true;
  }

  bool sameBindings(const CutPairs &A, const CutPairs &B) {
    // Both vectors follow the same cut permutation and contain distinct
    // locations. Compare sets without imposing a rank-component ordering.
    for (size_t I = 0; I != A.size(); ++I) {
      charge();
      if (A[I].SharedInputs.size() != B[I].SharedInputs.size())
        return false;
      for (const auto &P : A[I].SharedInputs) {
        bool Found = false;
        for (const auto &Q : B[I].SharedInputs) {
          charge();
          if (sameLocation(P.Original, Q.Original) &&
              sameLocation(P.Candidate, Q.Candidate)) {
            Found = true;
            break;
          }
        }
        if (!Found)
          return false;
      }
    }
    return true;
  }

  bool rankAndCheck(const LowIRLoopRefinementPlan &Left,
                    const LowIRLoopRefinementPlan &Right,
                    llvm::ArrayRef<size_t> Order, bool IncludeFrame) {
    if (!Limits.MaxRankPairingAttempts)
      return false;
    if (Result.RankPairingAttempts >= Limits.MaxRankPairingAttempts) {
      AttemptExhausted = true;
      Result.LastCandidateDiagnostic =
          "loop alignment rank pairing budget exhausted";
      return false;
    }
    beginPairing();
    ++Result.RankPairingAttempts;
    uint64_t Remaining = Limits.MaxMetadata;
    CutPairs Rank, Frame;
    if (!planMetadata(Left, Remaining) || !planMetadata(Right, Remaining))
      return false;
    Result.LastCandidateDiagnostic =
        "loop alignment ranks are not distinct direct inputs of equal width";
    if (!rankPairs(Left, Right, Order, Remaining, Rank) ||
        !framePairs(Left, Right, Order, Remaining, Frame))
      return false;
    if (IncludeFrame) {
      bool Added = false;
      for (size_t I = 0; I != Rank.size(); ++I) {
        charge();
        for (const auto &P : Frame[I].SharedInputs) {
          charge();
          bool Conflict = false;
          for (const auto &Q : Rank[I].SharedInputs) {
            charge();
            if (sameLocation(P.Original, Q.Original) ||
                sameLocation(P.Candidate, Q.Candidate)) {
              Conflict = true;
              break;
            }
          }
          if (Conflict)
            continue;
          if (!metadata(1, Remaining))
            return false;
          Rank[I].SharedInputs.push_back(P);
          Added = true;
        }
      }
      if (!Added) {
        Result.LastCandidateDiagnostic = "duplicate loop input pairing";
        return false;
      }
    }
    if (sameBindings(Rank, Frame)) {
      Result.LastCandidateDiagnostic = "duplicate loop input pairing";
      return false;
    }
    return checkPairs(Left, Right, Rank);
  }

  LowIRLoopInferenceResult inferCandidate(
      llvm::ArrayRef<va_t> Eligible = {}, CutFamily Family = CutFamily::Default,
      llvm::ArrayRef<const LowIRLoopRefinementPlan *> Previous = {}) {
    requireQueries();
    if (Result.CandidateAttempts >= Limits.MaxCandidateAttempts)
      stop(Status::BudgetExceeded, "loop alignment candidate budget exhausted");
    ++Result.CandidateAttempts;
    auto Right =
        infer(Candidate, Limits.CandidateInference, Eligible, Family, Previous);
    if (!Right.inferred()) {
      Result.LastCandidateDiagnostic = Right.Diagnostic;
      requireQueries();
    }
    return Right;
  }

  bool admissible(const LowIRLoopRefinementPlan &Plan) {
    if (Plan.Cutpoints.size() > Limits.MaxCuts) {
      AttemptExhausted = true;
      Result.LastCandidateDiagnostic = "loop alignment cut count exceeded";
      return false;
    }
    return true;
  }

  void remember(LowIRLoopInferenceResult R, CachedPlan &Cache) {
    Cache.Diagnostic = R.Diagnostic;
    if (!R.inferred())
      return;
    auto Remaining = RemainingCachedMetadata;
    if (!admissible(*R.Plan) || !planMetadata(*R.Plan, Remaining)) {
      Cache.Diagnostic = Result.LastCandidateDiagnostic;
      return;
    }
    RemainingCachedMetadata = Remaining;
    Cache.Plan = std::move(R.Plan);
  }

  std::vector<const LowIRLoopRefinementPlan *>
  previousPlans(const std::array<CachedPlan, 3> &Plans, unsigned Family) {
    std::vector<const LowIRLoopRefinementPlan *> Previous;
    if (Family == 0)
      return Previous;
    charge(Plans.size() - 1);
    for (unsigned I = 0; I != Plans.size(); ++I)
      if (I != Family && Plans[I].Plan)
        Previous.push_back(&*Plans[I].Plan);
    return Previous;
  }

  const LowIRLoopRefinementPlan *originalPlan(unsigned Family) {
    auto &Cache = OriginalPlans[Family];
    if (!Cache.Attempted) {
      Cache.Attempted = true;
      const auto Previous = previousPlans(OriginalPlans, Family);
      remember(infer(Original, Limits.OriginalInference, {},
                     static_cast<CutFamily>(Family), Previous),
               Cache);
      if (!Cache.Plan)
        requireQueries();
    }
    return Cache.Plan ? &*Cache.Plan : nullptr;
  }

  const LowIRLoopRefinementPlan *candidatePlan(unsigned Family) {
    auto &Cache = CandidatePlans[Family];
    if (!Cache.Attempted) {
      Cache.Attempted = true;
      const auto Previous = previousPlans(CandidatePlans, Family);
      remember(inferCandidate({}, static_cast<CutFamily>(Family), Previous),
               Cache);
    }
    if (!Cache.Plan)
      Result.LastCandidateDiagnostic = Cache.Diagnostic;
    return Cache.Plan ? &*Cache.Plan : nullptr;
  }

  bool tryPlans(const LowIRLoopRefinementPlan &Left,
                const LowIRLoopRefinementPlan &Right) {
    if (Right.Cutpoints.size() != Left.Cutpoints.size()) {
      Result.LastCandidateDiagnostic = "loop alignment cut counts differ";
      return false;
    }
    charge(Left.Cutpoints.size());
    std::vector<size_t> Order(Left.Cutpoints.size());
    std::iota(Order.begin(), Order.end(), 0);
    do {
      if (pairAndCheck(Left, Right, Order))
        return true;
      if (rankAndCheck(Left, Right, Order, false) ||
          rankAndCheck(Left, Right, Order, true))
        return true;
      charge(Order.size());
    } while (std::next_permutation(Order.begin(), Order.end()));
    return false;
  }

public:
  AlignmentSearch(const LowFunc &A,
                  llvm::ArrayRef<LowIRUndefinedInstruction> Records,
                  const LowFunc &B, const LowIRIndependenceContract &C,
                  LowIRRefinementWitness W, const LowIRLoopAlignmentLimits &L)
      : Original(A), Candidate(B), OriginalInstructions(Records), Contract(C),
        Witness(W), Limits(L), RemainingCachedMetadata(L.MaxMetadata) {}

  LowIRLoopAlignmentResult run() {
    try {
      index(Original);
      const auto Blocks = index(Candidate);
      // Try corresponding families before spending the remaining attempts
      // on individual candidate cuts. Failed inferences are cached as well:
      // crossing families must not replay an exhausted or duplicate proposal.
      for (unsigned Family = 0; Family != OriginalPlans.size(); ++Family)
        if (const auto *Left = originalPlan(Family))
          if (const auto *Right = candidatePlan(Family))
            if (tryPlans(*Left, *Right))
              return std::move(Result);
      if (std::none_of(OriginalPlans.begin(), OriginalPlans.end(),
                       [](const auto &P) { return P.Plan.has_value(); }))
        stop(AttemptExhausted ? Status::BudgetExceeded : Status::Unsupported,
             "original loop inference: " + OriginalPlans.back().Diagnostic);
      for (unsigned Family = 0; Family != OriginalPlans.size(); ++Family)
        if (const auto &Left = OriginalPlans[Family].Plan)
          for (unsigned Other = 0; Other != CandidatePlans.size(); ++Other)
            if (Other != Family)
              if (const auto *Right = candidatePlan(Other))
                if (tryPlans(*Left, *Right))
                  return std::move(Result);
      requireQueries();
      for (const auto &B : Candidate.Blocks) {
        charge();
        if (!cyclic(B, Blocks))
          continue;
        auto Right = inferCandidate({B.StartAddr});
        if (!Right.inferred() || !admissible(*Right.Plan))
          continue;
        for (const auto &Left : OriginalPlans)
          if (Left.Plan && tryPlans(*Left.Plan, *Right.Plan))
            return std::move(Result);
      }
      stop(AttemptExhausted ? Status::BudgetExceeded : Status::Unsupported,
           AttemptExhausted
               ? "loop alignment search incomplete within stage limits"
               : "no loop relation found in bounded alignment search");
    } catch (const Stop &) {
    }
    return std::move(Result);
  }
};
} // namespace

LowIRLoopAlignmentResult inferAndCheckLowIRLoopRefinement(
    const LowFunc &Original,
    llvm::ArrayRef<LowIRUndefinedInstruction> OriginalInstructions,
    const LowFunc &Candidate, const LowIRIndependenceContract &Contract,
    LowIRRefinementWitness Witness, const LowIRLoopAlignmentLimits &Limits) {
  return AlignmentSearch(Original, OriginalInstructions, Candidate, Contract,
                         Witness, Limits)
      .run();
}
} // namespace neverd::analysis
