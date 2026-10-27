//===- Comparison.cpp - Function correspondence between programs ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The function matcher: unique exact features first, drilled down within
/// the groups a feature cannot split, then the callers and callees of every
/// pair, then similarity for what remains; and the editor that applies an
/// analyst's overrides to a finished comparison.
///
//===----------------------------------------------------------------------===//

#include "neverd/diff/Comparison.h"

#include "CallAlignment.h"
#include "Features.h"
#include "Matching.h"

#include "neverd/diff/FunctionDelta.h"
#include "neverd/diff/StableHash.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {

namespace limits = neverd::limits::diff;

std::string ComparisonSettings::describe() const {
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "names=" << (UseNames ? 1 : 0)
     << ";approximate=" << (Approximate ? 1 : 0)
     << ";min_similarity=" << llvm::format("%.17g", MinimumSimilarity)
     << ";min_context_similarity="
     << llvm::format("%.17g", MinimumContextSimilarity)
     << ";ambiguity_margin=" << llvm::format("%.17g", AmbiguityMargin)
     << ";max_candidates=" << MaxCandidates << ";regions=" << Regions.describe()
     << ";dataflow=" << DataFlow.describe()
     << ";flow_regions=" << FlowRegions.describe();
  return Out;
}

std::optional<std::string> ComparisonSettings::validate() const {
  const auto Unit = [](double V) {
    return std::isfinite(V) && V >= 0 && V <= 1;
  };
  if (!Unit(MinimumSimilarity))
    return std::string("minimum similarity must lie between 0 and 1");
  if (!Unit(MinimumContextSimilarity))
    return std::string("minimum context similarity must lie between 0 and 1");
  if (!Unit(AmbiguityMargin))
    return std::string("ambiguity margin must lie between 0 and 1");
  if (MaxCandidates == 0 || MaxCandidates > limits::kMaxCandidates)
    return "candidates per function must lie between 1 and " +
           std::to_string(limits::kMaxCandidates);
  if (auto Error = Regions.validate())
    return Error;
  if (auto Error = FlowRegions.validate())
    return Error;
  if (FlowRegions.Enabled && !DataFlow.Enabled)
    return "SSA regions require dependency evidence";
  return DataFlow.validate();
}

uint32_t ComparisonResult::secondaryOf(uint32_t Primary) const {
  const FunctionMatch *M = matchOfPrimary(Primary);
  return M ? M->Secondary : NoIndex;
}
uint32_t ComparisonResult::primaryOf(uint32_t Secondary) const {
  const FunctionMatch *M = matchOfSecondary(Secondary);
  return M ? M->Primary : NoIndex;
}
const FunctionMatch *ComparisonResult::matchOfPrimary(uint32_t Primary) const {
  if (Primary >= PrimaryMatch.size() || PrimaryMatch[Primary] == NoIndex)
    return nullptr;
  return &Matches[PrimaryMatch[Primary]];
}
const FunctionMatch *
ComparisonResult::matchOfSecondary(uint32_t Secondary) const {
  if (Secondary >= SecondaryMatch.size() ||
      SecondaryMatch[Secondary] == NoIndex)
    return nullptr;
  return &Matches[SecondaryMatch[Secondary]];
}

void measure(FunctionMatch &M, const FunctionDelta &D) {
  if (!D.ComputationComplete)
    return;
  M.Measured = true;
  M.IdentityOnly = D.IdentityOnly;
  M.AnalysisComplete = D.AnalysisComplete;
  M.Similarity = D.Similarity;
  M.Changes = D.Changes;
  M.MatchedBlocks = static_cast<uint32_t>(D.Blocks.size());
  M.MatchedEdges = D.MatchedEdges;
  M.EqualInstructions = D.EqualInstructions;
}

namespace {

template <typename Set, typename Key>
void recordRejection(Set &Rejected, const Key &Pair, OverrideKind Kind) {
  switch (Kind) {
  case OverrideKind::Match:
    Rejected.erase(Pair);
    break;
  case OverrideKind::Unmatch:
    Rejected.insert(Pair);
    break;
  case OverrideKind::Confirm:
    break;
  }
}

using Clock = std::chrono::steady_clock;

/// The exact features the global passes try, strongest first.
constexpr FunctionStep GlobalSteps[] = {
    FunctionStep::SymbolName,        FunctionStep::ExactBytes,
    FunctionStep::NormalizedBody,    FunctionStep::MnemonicsAndGraph,
    FunctionStep::GraphNeighborhood, FunctionStep::GraphTopDown,
    FunctionStep::GraphBottomUp,     FunctionStep::PrimeProduct,
    FunctionStep::StringReferences,  FunctionStep::ImportCalls,
    FunctionStep::Constants,         FunctionStep::CallGraphPosition,
};

/// The key of \p F for \p Step, zero when F is not eligible.
uint64_t stepKey(FunctionStep Step, const FunctionFeatures &F,
                 const Function &Fn, bool UseNames) {
  if (!Fn.hasBody())
    return 0;
  const auto Pair = [](uint64_t A, uint64_t B, uint64_t C = 0) {
    return StableHash().add(A).add(B).add(C).value();
  };
  switch (Step) {
  case FunctionStep::SymbolName:
    return UseNames ? F.Name : 0;
  case FunctionStep::ExactBytes:
    return F.Bytes;
  case FunctionStep::NormalizedBody:
    return F.Shape;
  case FunctionStep::MnemonicsAndGraph:
    return F.Edges >= limits::kMinMDIndexEdges
               ? Pair(F.Mnemonics, F.GraphTopDown)
               : 0;
  case FunctionStep::GraphNeighborhood:
    return F.Edges >= limits::kMinMDIndexEdges ? F.Neighborhood : 0;
  case FunctionStep::GraphTopDown:
    return F.Edges >= limits::kMinMDIndexEdges
               ? Pair(F.GraphTopDown, F.Blocks, F.Edges)
               : 0;
  case FunctionStep::GraphBottomUp:
    return F.Edges >= limits::kMinMDIndexEdges
               ? Pair(F.GraphBottomUp, F.Blocks, F.Edges)
               : 0;
  case FunctionStep::PrimeProduct:
    return F.Instructions >= limits::kMinPrimeProductInstructions
               ? Pair(F.Prime, F.Instructions)
               : 0;
  case FunctionStep::StringReferences:
    return F.Strings;
  case FunctionStep::ImportCalls:
    return F.ImportCalls;
  case FunctionStep::Constants:
    return F.Constants;
  case FunctionStep::CallGraphPosition:
    return F.CallGraph ? Pair(F.CallGraph, F.Blocks, F.Edges) : 0;
  default:
    return 0;
  }
}

/// A group of functions sharing one step's key that no feature split.
struct Tie {
  FunctionStep Step;
  std::vector<uint32_t> Primary;
  std::vector<uint32_t> Secondary;
};

class Matcher {
public:
  Matcher(const Program &P, const Program &S,
          const ComparisonSettings &Settings, const ComparisonControl &Control,
          ComparisonResult &R)
      : P(P), S(S), Settings(Settings), Control(Control), R(R) {}

  void run(llvm::ArrayRef<Override> Overrides);

private:
  const Program &P;
  const Program &S;
  const ComparisonSettings &Settings;
  const ComparisonControl &Control;
  ComparisonResult &R;
  ProgramFeatures PF;
  ProgramFeatures SF;
  std::vector<uint32_t> PMatch;
  std::vector<uint32_t> SMatch;
  /// Pairs in the order they formed, with how.
  struct Formed {
    uint32_t Primary;
    uint32_t Secondary;
    FunctionStep Step;
    bool FromContext;
  };
  std::vector<Formed> Order;
  llvm::DenseSet<uint64_t> Rejected;
  llvm::DenseSet<uint64_t> Confirmed;
  /// The names each program states.
  llvm::DenseSet<uint64_t> PrimaryNames;
  llvm::DenseSet<uint64_t> SecondaryNames;
  std::vector<Tie> NearTies;
  Clock::time_point PhaseStart;
  std::string Phase;
  bool Stopped = false;

  static uint64_t pairKey(uint32_t A, uint32_t B) {
    return (uint64_t(A) << 32) | B;
  }
  bool stopNow() {
    if (Stopped)
      return true;
    if (const auto Why = Control.stop()) {
      R.State = *Why;
      R.StoppedPhase = Phase;
      Stopped = true;
    }
    return Stopped;
  }
  void beginPhase(std::string Name) {
    endPhase();
    Phase = std::move(Name);
    PhaseStart = Clock::now();
  }
  void endPhase() {
    if (Phase.empty())
      return;
    R.Timings.push_back({Phase, std::chrono::duration<double, std::milli>(
                                    Clock::now() - PhaseStart)
                                    .count()});
    Phase.clear();
  }
  void progress(uint64_t Done, uint64_t Total) {
    if (Control.Progress)
      Control.Progress(Phase, Done, Total);
  }

  bool matched(uint32_t Side, uint32_t F) const {
    return Side == 0 ? PMatch[F] != NoIndex : SMatch[F] != NoIndex;
  }
  /// Whether a name one function states names another function of the
  /// other program: then the two are different functions.
  bool namesConflict(uint32_t A, uint32_t B) const {
    if (!Settings.UseNames)
      return false;
    const uint64_t X = PF.Functions[A].Name, Y = SF.Functions[B].Name;
    if (!X || !Y || X == Y)
      return false;
    return SecondaryNames.contains(X) || PrimaryNames.contains(Y);
  }
  bool pair(uint32_t A, uint32_t B, FunctionStep Step, bool FromContext) {
    if (PMatch[A] != NoIndex || SMatch[B] != NoIndex ||
        Rejected.contains(pairKey(A, B)) ||
        (Step != FunctionStep::Manual && namesConflict(A, B)))
      return false;
    PMatch[A] = B;
    SMatch[B] = A;
    Order.push_back({A, B, Step, FromContext});
    return true;
  }

  unsigned drill(llvm::ArrayRef<uint32_t> Ps, llvm::ArrayRef<uint32_t> Ss,
                 size_t FirstStep, bool FromContext, unsigned Depth);
  unsigned globalPass();
  unsigned matchImports();
  void propagate(size_t From, bool Loose);
  void settle(size_t From);
  void context(uint32_t A, uint32_t B, bool Loose);
  void callSites(uint32_t A, uint32_t B);
  unsigned approximate();
  double contextAgreement(uint32_t A, uint32_t B) const;
  double quickScore(uint32_t A, uint32_t B) const;
  FunctionDelta delta(uint32_t A, uint32_t B) const;
  std::vector<uint32_t> unmatchedNeighbors(const ProgramFeatures &F,
                                           const Adjacency &Graph, uint32_t Of,
                                           uint32_t Side) const;
  void collectAmbiguities();
  void finish();
};

unsigned Matcher::drill(llvm::ArrayRef<uint32_t> Ps,
                        llvm::ArrayRef<uint32_t> Ss, size_t FirstStep,
                        bool FromContext, unsigned Depth) {
  unsigned Paired = 0;
  struct Entry {
    uint64_t Key;
    uint8_t Side;
    uint32_t Function;
  };
  std::vector<Entry> Entries;
  for (size_t K = FirstStep; K < std::size(GlobalSteps); ++K) {
    if (stopNow())
      return Paired;
    const FunctionStep Step = GlobalSteps[K];
    Entries.clear();
    for (uint32_t A : Ps)
      if (PMatch[A] == NoIndex)
        if (const uint64_t Key = stepKey(Step, PF.Functions[A], P.Functions[A],
                                         Settings.UseNames))
          Entries.push_back({Key, 0, A});
    for (uint32_t B : Ss)
      if (SMatch[B] == NoIndex)
        if (const uint64_t Key = stepKey(Step, SF.Functions[B], S.Functions[B],
                                         Settings.UseNames))
          Entries.push_back({Key, 1, B});
    if (Entries.empty())
      continue;
    llvm::sort(Entries, [](const Entry &L, const Entry &R) {
      return std::tie(L.Key, L.Side, L.Function) <
             std::tie(R.Key, R.Side, R.Function);
    });
    std::vector<uint32_t> GroupP, GroupS;
    for (size_t I = 0; I < Entries.size();) {
      size_t J = I;
      GroupP.clear();
      GroupS.clear();
      for (; J < Entries.size() && Entries[J].Key == Entries[I].Key; ++J)
        (Entries[J].Side == 0 ? GroupP : GroupS).push_back(Entries[J].Function);
      I = J;
      if (GroupP.empty() || GroupS.empty())
        continue;
      if (GroupP.size() == 1 && GroupS.size() == 1) {
        Paired += pair(GroupP[0], GroupS[0], Step, FromContext);
        continue;
      }
      // Several functions share this feature: the next features may tell
      // them apart among just themselves.
      if (GroupP.size() + GroupS.size() <= limits::kMaxDrillDownGroup &&
          Depth < std::size(GlobalSteps))
        Paired += drill(GroupP, GroupS, K + 1, FromContext, Depth + 1);
    }
  }
  return Paired;
}

unsigned Matcher::matchImports() {
  // Imports pair by symbol, then by symbol and library for a symbol two
  // libraries provide.
  unsigned Paired = 0;
  for (bool WithModule : {false, true}) {
    struct Entry {
      uint64_t Key;
      uint8_t Side;
      uint32_t Function;
    };
    std::vector<Entry> Entries;
    const auto Add = [&](const Program &Prog, const ProgramFeatures &F,
                         const std::vector<uint32_t> &Match, uint8_t Side) {
      for (uint32_t I = 0; I < Prog.Functions.size(); ++I)
        if (Match[I] == NoIndex && F.Functions[I].Import)
          Entries.push_back(
              {WithModule
                   ? StableHash(F.Functions[I].Import)
                         .add(llvm::StringRef(Prog.Functions[I].Module).lower())
                         .value()
                   : F.Functions[I].Import,
               Side, I});
    };
    Add(P, PF, PMatch, 0);
    Add(S, SF, SMatch, 1);
    llvm::sort(Entries, [](const Entry &L, const Entry &R) {
      return std::tie(L.Key, L.Side, L.Function) <
             std::tie(R.Key, R.Side, R.Function);
    });
    for (size_t I = 0; I < Entries.size();) {
      size_t J = I;
      while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
        ++J;
      if (J - I == 2 && Entries[I].Side == 0 && Entries[I + 1].Side == 1)
        Paired += pair(Entries[I].Function, Entries[I + 1].Function,
                       FunctionStep::ImportName, false);
      I = J;
    }
  }
  return Paired;
}

unsigned Matcher::globalPass() {
  std::vector<uint32_t> Ps, Ss;
  for (uint32_t I = 0; I < P.Functions.size(); ++I)
    if (PMatch[I] == NoIndex && P.Functions[I].hasBody())
      Ps.push_back(I);
  for (uint32_t I = 0; I < S.Functions.size(); ++I)
    if (SMatch[I] == NoIndex && S.Functions[I].hasBody())
      Ss.push_back(I);
  return drill(Ps, Ss, 0, false, 0);
}

std::vector<uint32_t> Matcher::unmatchedNeighbors(const ProgramFeatures &F,
                                                  const Adjacency &Graph,
                                                  uint32_t Of,
                                                  uint32_t Side) const {
  (void)F;
  std::vector<uint32_t> Out;
  const Program &Prog = Side == 0 ? P : S;
  for (uint32_t N : Graph.of(Of))
    if (!matched(Side, N) && Prog.Functions[N].hasBody())
      Out.push_back(N);
  return Out;
}

double Matcher::contextAgreement(uint32_t A, uint32_t B) const {
  unsigned MatchedA = 0, MatchedB = 0, Agree = 0;
  for (bool Calls : {true, false}) {
    const auto AN = Calls ? PF.Callees.of(A) : PF.Callers.of(A);
    const auto BN = Calls ? SF.Callees.of(B) : SF.Callers.of(B);
    for (uint32_t N : AN)
      if (PMatch[N] != NoIndex) {
        ++MatchedA;
        Agree += std::binary_search(BN.begin(), BN.end(), PMatch[N]);
      }
    for (uint32_t N : BN)
      MatchedB += SMatch[N] != NoIndex;
  }
  if (MatchedA + MatchedB == 0)
    return -1;
  return 2.0 * Agree / (MatchedA + MatchedB);
}

double Matcher::quickScore(uint32_t A, uint32_t B) const {
  const FunctionFeatures &X = PF.Functions[A], &Y = SF.Functions[B];
  const auto Close = [](double L, double R) {
    if (L == 0 && R == 0)
      return 1.0;
    return std::min(L, R) / std::max(L, R);
  };
  const double Size = (Close(X.Instructions, Y.Instructions) +
                       Close(X.Blocks, Y.Blocks) + Close(X.Edges, Y.Edges)) /
                      3;
  const double Sketch = sketchSimilarity(X, Y);
  const double Context = contextAgreement(A, B);
  if (Context < 0)
    return 0.7 * Sketch + 0.3 * Size;
  return 0.5 * Sketch + 0.2 * Size + 0.3 * Context;
}

FunctionDelta Matcher::delta(uint32_t A, uint32_t B) const {
  return compareFunctionsWithKeys(P, A, S, B, {PMatch, SMatch}, PF.MnemonicKeys,
                                  SF.MnemonicKeys, Control);
}

void Matcher::callSites(uint32_t A, uint32_t B) {
  // Only anchors shared by every optimal alignment separate call intervals.
  // A repeated callee must not select one arbitrary alignment for new pairs.
  const auto X = PF.CallSites.of(A), Y = SF.CallSites.of(B);
  if (X.empty() || Y.empty())
    return;
  std::vector<uint32_t> XP, YP;
  for (uint32_t C : X) {
    if (stopNow())
      return;
    XP.push_back(C == NoIndex ? NoIndex : PMatch[C]);
  }
  for (uint32_t D : Y) {
    if (stopNow())
      return;
    YP.push_back(D == NoIndex || SMatch[D] == NoIndex ? NoIndex : D);
  }
  auto Anchors = uniqueCallAnchors(XP, YP, Control);
  if (!Anchors) {
    stopNow();
    return;
  }
  Anchors->emplace_back(X.size(), Y.size());
  std::vector<std::pair<uint32_t, uint32_t>> Proposals;
  size_t I = 0, J = 0;
  for (const auto &[AI, AJ] : *Anchors) {
    if (stopNow())
      return;
    if (AI - I == AJ - J) {
      bool Clear = true;
      for (size_t K = 0; K < AI - I; ++K) {
        if (stopNow())
          return;
        const uint32_t C = X[I + K], D = Y[J + K];
        // An unresolved call or an already matched call without a mandatory
        // anchor leaves this interval's positional correspondence uncertain.
        if (C == NoIndex || D == NoIndex || PMatch[C] != NoIndex ||
            SMatch[D] != NoIndex) {
          Clear = false;
          break;
        }
      }
      if (Clear)
        for (size_t K = 0; K < AI - I; ++K) {
          if (stopNow())
            return;
          Proposals.emplace_back(X[I + K], Y[J + K]);
        }
    }
    I = AI + 1;
    J = AJ + 1;
  }
  // Check every occurrence before committing anything: a->x, b->x, a->y
  // cannot become a->x followed by a fabricated lone-neighbor b->y match.
  llvm::sort(Proposals);
  Proposals.erase(std::unique(Proposals.begin(), Proposals.end()),
                  Proposals.end());
  llvm::DenseMap<uint32_t, size_t> ByPrimary, BySecondary;
  for (const auto &[C, D] : Proposals) {
    if (stopNow())
      return;
    ++ByPrimary[C];
    ++BySecondary[D];
  }
  for (const auto &[C, D] : Proposals) {
    if (stopNow())
      return;
    if (ByPrimary[C] != 1 || BySecondary[D] != 1 || !P.Functions[C].hasBody() ||
        !S.Functions[D].hasBody() || Rejected.contains(pairKey(C, D)) ||
        namesConflict(C, D))
      continue;
    const auto Delta = delta(C, D);
    if (stopNow())
      return;
    if (Delta.ComputationComplete &&
        Delta.Similarity >= Settings.MinimumContextSimilarity)
      pair(C, D, FunctionStep::CallSite, true);
  }
}

void Matcher::context(uint32_t A, uint32_t B, bool Loose) {
  for (bool Calls : {true, false}) {
    const Adjacency &AG = Calls ? PF.Callees : PF.Callers;
    const Adjacency &BG = Calls ? SF.Callees : SF.Callers;
    auto As = unmatchedNeighbors(PF, AG, A, 0);
    auto Bs = unmatchedNeighbors(SF, BG, B, 1);
    if (As.empty() || Bs.empty())
      continue;
    drill(As, Bs, 0, true, 0);
    if (!Loose)
      continue;
    // Weaker context once no exact feature is left to pair by: the place
    // of a call, and a single remaining neighbor on both sides.
    if (Calls)
      callSites(A, B);
    if (stopNow())
      return;
    As = unmatchedNeighbors(PF, AG, A, 0);
    Bs = unmatchedNeighbors(SF, BG, B, 1);
    if (As.size() == 1 && Bs.size() == 1 &&
        !Rejected.contains(pairKey(As[0], Bs[0])) &&
        !namesConflict(As[0], Bs[0])) {
      const auto Delta = delta(As[0], Bs[0]);
      if (stopNow())
        return;
      if (Delta.ComputationComplete &&
          Delta.Similarity >= Settings.MinimumContextSimilarity)
        pair(As[0], Bs[0], FunctionStep::Neighborhood, true);
    }
  }
}

void Matcher::propagate(size_t From, bool Loose) {
  for (size_t I = From; I < Order.size(); ++I) {
    if (stopNow())
      return;
    if ((I & 255) == 0)
      progress(I, Order.size());
    context(Order[I].Primary, Order[I].Secondary, Loose);
  }
}

void Matcher::settle(size_t From) {
  propagate(From, false);
  for (unsigned Round = 0; Round < limits::kMaxPropagationRounds && !stopNow();
       ++Round) {
    const size_t Before = Order.size();
    globalPass();
    propagate(Before, false);
    if (Order.size() == Before)
      break;
  }
}

unsigned Matcher::approximate() {
  // Candidates: functions sharing a sketch band, and the unmatched
  // neighbors of corresponding functions.
  std::vector<uint32_t> Ps, Ss;
  for (uint32_t I = 0; I < P.Functions.size(); ++I)
    if (PMatch[I] == NoIndex && P.Functions[I].hasBody())
      Ps.push_back(I);
  for (uint32_t I = 0; I < S.Functions.size(); ++I)
    if (SMatch[I] == NoIndex && S.Functions[I].hasBody())
      Ss.push_back(I);
  if (Ps.empty() || Ss.empty())
    return 0;
  std::vector<uint64_t> Candidates;
  {
    struct Entry {
      uint64_t Key;
      uint8_t Side;
      uint32_t Function;
    };
    std::vector<Entry> Entries;
    for (unsigned Band = 0; Band < SketchBands; ++Band) {
      Entries.clear();
      for (uint32_t A : Ps)
        if (PF.Functions[A].Sketched)
          Entries.push_back({sketchBand(PF.Functions[A], Band), 0, A});
      for (uint32_t B : Ss)
        if (SF.Functions[B].Sketched)
          Entries.push_back({sketchBand(SF.Functions[B], Band), 1, B});
      llvm::sort(Entries, [](const Entry &L, const Entry &R) {
        return std::tie(L.Key, L.Side, L.Function) <
               std::tie(R.Key, R.Side, R.Function);
      });
      for (size_t I = 0; I < Entries.size();) {
        size_t J = I;
        while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
          ++J;
        if (J - I <= limits::kMaxSketchBucket)
          for (size_t X = I; X < J; ++X)
            for (size_t Y = I; Y < J; ++Y)
              if (Entries[X].Side == 0 && Entries[Y].Side == 1)
                Candidates.push_back(
                    pairKey(Entries[X].Function, Entries[Y].Function));
        I = J;
      }
    }
    for (uint32_t A : Ps)
      for (bool Calls : {true, false}) {
        const Adjacency &AG = Calls ? PF.Callers : PF.Callees;
        const Adjacency &BG = Calls ? SF.Callees : SF.Callers;
        for (uint32_t N : AG.of(A)) {
          const uint32_t Counterpart = PMatch[N];
          if (Counterpart == NoIndex)
            continue;
          for (uint32_t B : BG.of(Counterpart))
            if (SMatch[B] == NoIndex && S.Functions[B].hasBody())
              Candidates.push_back(pairKey(A, B));
        }
      }
  }
  if (uint64_t(Ps.size()) * Ss.size() <= limits::kMaxExhaustiveCandidatePairs)
    for (uint32_t A : Ps)
      for (uint32_t B : Ss)
        Candidates.push_back(pairKey(A, B));
  llvm::sort(Candidates);
  Candidates.erase(std::unique(Candidates.begin(), Candidates.end()),
                   Candidates.end());
  if (stopNow())
    return 0;

  // The best few candidates of each function on either side, by a quick
  // score, are measured exactly.
  struct Scored {
    uint32_t A;
    uint32_t B;
    double Quick;
    double Exact = -1;
  };
  std::vector<Scored> Pairs;
  Pairs.reserve(Candidates.size());
  for (uint64_t K : Candidates) {
    const uint32_t A = uint32_t(K >> 32), B = uint32_t(K);
    if (!Rejected.contains(K) && !namesConflict(A, B))
      Pairs.push_back({A, B, quickScore(A, B)});
  }
  const unsigned Keep = Settings.MaxCandidates;
  std::vector<uint8_t> Measure(Pairs.size(), 0);
  for (bool BySecondary : {false, true}) {
    std::vector<uint32_t> Index(Pairs.size());
    std::iota(Index.begin(), Index.end(), 0);
    llvm::sort(Index, [&](uint32_t L, uint32_t R) {
      const Scored &X = Pairs[L], &Y = Pairs[R];
      const uint32_t XK = BySecondary ? X.B : X.A, YK = BySecondary ? Y.B : Y.A;
      return std::make_tuple(XK, -X.Quick, X.A, X.B) <
             std::make_tuple(YK, -Y.Quick, Y.A, Y.B);
    });
    for (size_t I = 0; I < Index.size();) {
      size_t J = I;
      const auto Owner = [&](uint32_t At) {
        return BySecondary ? Pairs[Index[At]].B : Pairs[Index[At]].A;
      };
      while (J < Index.size() && Owner(J) == Owner(I))
        ++J;
      for (size_t K = I; K < J && K < I + Keep; ++K)
        Measure[Index[K]] = 1;
      I = J;
    }
  }
  std::vector<uint32_t> ToMeasure;
  for (uint32_t I = 0; I < Pairs.size(); ++I)
    if (Measure[I])
      ToMeasure.push_back(I);
  parallelForEach(ToMeasure.size(), [&](auto Claim, size_t Total) {
    for (size_t I = Claim(); I < Total; I = Claim()) {
      Scored &Pair = Pairs[ToMeasure[I]];
      Pair.Exact = delta(Pair.A, Pair.B).Similarity;
    }
  });
  if (stopNow())
    return 0;

  // Strongest first; a pair whose functions have another candidate nearly
  // as similar stays undecided.
  std::vector<uint32_t> Ranked = ToMeasure;
  llvm::sort(Ranked, [&](uint32_t L, uint32_t R) {
    const Scored &X = Pairs[L], &Y = Pairs[R];
    return std::make_tuple(-X.Exact, -X.Quick, X.A, X.B) <
           std::make_tuple(-Y.Exact, -Y.Quick, Y.A, Y.B);
  });
  llvm::DenseMap<uint32_t, std::vector<uint32_t>> ByA, ByB;
  for (uint32_t I : Ranked) {
    ByA[Pairs[I].A].push_back(I);
    ByB[Pairs[I].B].push_back(I);
  }
  llvm::DenseSet<uint32_t> UndecidedA, UndecidedB;
  unsigned Paired = 0;
  for (uint32_t I : Ranked) {
    const Scored &Pair = Pairs[I];
    if (Pair.Exact < Settings.MinimumSimilarity)
      break;
    if (PMatch[Pair.A] != NoIndex || SMatch[Pair.B] != NoIndex ||
        UndecidedA.contains(Pair.A) || UndecidedB.contains(Pair.B))
      continue;
    // Another open candidate of either function nearly as similar leaves
    // the choice to the analyst.
    std::vector<uint32_t> RivalsOfA, RivalsOfB;
    for (uint32_t Other : ByA[Pair.A]) {
      const Scored &O = Pairs[Other];
      if (Other != I && SMatch[O.B] == NoIndex &&
          (O.Exact >= Pair.Exact ||
           Pair.Exact - O.Exact < Settings.AmbiguityMargin))
        RivalsOfA.push_back(O.B);
    }
    for (uint32_t Other : ByB[Pair.B]) {
      const Scored &O = Pairs[Other];
      if (Other != I && PMatch[O.A] == NoIndex &&
          (O.Exact >= Pair.Exact ||
           Pair.Exact - O.Exact < Settings.AmbiguityMargin))
        RivalsOfB.push_back(O.A);
    }
    if (!RivalsOfA.empty() || !RivalsOfB.empty()) {
      Tie T{FunctionStep::Similarity, {Pair.A}, {Pair.B}};
      T.Primary.insert(T.Primary.end(), RivalsOfB.begin(), RivalsOfB.end());
      T.Secondary.insert(T.Secondary.end(), RivalsOfA.begin(), RivalsOfA.end());
      for (uint32_t A : T.Primary)
        UndecidedA.insert(A);
      for (uint32_t B : T.Secondary)
        UndecidedB.insert(B);
      NearTies.push_back(std::move(T));
      continue;
    }
    Paired += pair(Pair.A, Pair.B, FunctionStep::Similarity, false);
  }
  return Paired;
}

void Matcher::collectAmbiguities() {
  std::vector<uint8_t> InPrimary(P.Functions.size(), 0),
      InSecondary(S.Functions.size(), 0);
  const auto Record = [&](Tie T) {
    llvm::erase_if(T.Primary, [&](uint32_t A) {
      return PMatch[A] != NoIndex || InPrimary[A];
    });
    llvm::erase_if(T.Secondary, [&](uint32_t B) {
      return SMatch[B] != NoIndex || InSecondary[B];
    });
    llvm::sort(T.Primary);
    T.Primary.erase(std::unique(T.Primary.begin(), T.Primary.end()),
                    T.Primary.end());
    llvm::sort(T.Secondary);
    T.Secondary.erase(std::unique(T.Secondary.begin(), T.Secondary.end()),
                      T.Secondary.end());
    if (T.Primary.empty() || T.Secondary.empty())
      return;
    for (uint32_t A : T.Primary)
      InPrimary[A] = 1;
    for (uint32_t B : T.Secondary)
      InSecondary[B] = 1;
    R.Ambiguities.push_back(
        {T.Step, std::move(T.Primary), std::move(T.Secondary)});
  };
  // What the strongest feature still cannot split.
  struct Entry {
    uint64_t Key;
    uint8_t Side;
    uint32_t Function;
  };
  for (FunctionStep Step : GlobalSteps) {
    std::vector<Entry> Entries;
    for (uint32_t A = 0; A < P.Functions.size(); ++A)
      if (PMatch[A] == NoIndex && !InPrimary[A])
        if (const uint64_t K = stepKey(Step, PF.Functions[A], P.Functions[A],
                                       Settings.UseNames))
          Entries.push_back({K, 0, A});
    for (uint32_t B = 0; B < S.Functions.size(); ++B)
      if (SMatch[B] == NoIndex && !InSecondary[B])
        if (const uint64_t K = stepKey(Step, SF.Functions[B], S.Functions[B],
                                       Settings.UseNames))
          Entries.push_back({K, 1, B});
    llvm::sort(Entries, [](const Entry &L, const Entry &R) {
      return std::tie(L.Key, L.Side, L.Function) <
             std::tie(R.Key, R.Side, R.Function);
    });
    for (size_t I = 0; I < Entries.size();) {
      size_t J = I;
      Tie T{Step, {}, {}};
      for (; J < Entries.size() && Entries[J].Key == Entries[I].Key; ++J)
        (Entries[J].Side == 0 ? T.Primary : T.Secondary)
            .push_back(Entries[J].Function);
      I = J;
      // A lone pair left here was refused by an override.
      if (!T.Primary.empty() && !T.Secondary.empty() &&
          T.Primary.size() + T.Secondary.size() > 2)
        Record(std::move(T));
    }
  }
  for (Tie &T : NearTies)
    Record(std::move(T));
}

/// The confidence of a measured pair.
double confidenceOf(const FunctionMatch &M, const FunctionFeatures &A,
                    double Context) {
  if (M.Step == FunctionStep::Manual)
    return 1;
  double Base = stepConfidence(M.Step);
  if (M.Step == FunctionStep::Similarity || M.Step == FunctionStep::CallSite ||
      M.Step == FunctionStep::Neighborhood)
    Base *= M.Similarity;
  if (M.FromContext)
    Base *= 0.97;
  if (A.Instructions && A.Instructions < limits::kMinShapeInstructions &&
      M.Step != FunctionStep::ImportName && M.Step != FunctionStep::SymbolName)
    Base *= 0.85;
  if (Context >= 0)
    Base *= 1 - limits::kContextConfidenceWeight +
            limits::kContextConfidenceWeight * Context;
  return std::clamp(Base, 0.0, 1.0);
}

void Matcher::finish() {
  // Matches in primary address order.
  llvm::sort(Order, [](const Formed &L, const Formed &R) {
    return L.Primary < R.Primary;
  });
  R.Matches.clear();
  R.Matches.reserve(Order.size());
  for (const Formed &F : Order) {
    FunctionMatch M;
    M.Primary = F.Primary;
    M.Secondary = F.Secondary;
    M.Step = F.Step;
    M.FromContext = F.FromContext;
    M.Confirmed = F.Step == FunctionStep::Manual ||
                  Confirmed.contains(pairKey(F.Primary, F.Secondary));
    R.Matches.push_back(M);
  }
  R.PrimaryMatch.assign(P.Functions.size(), NoIndex);
  R.SecondaryMatch.assign(S.Functions.size(), NoIndex);
  for (uint32_t I = 0; I < R.Matches.size(); ++I) {
    R.PrimaryMatch[R.Matches[I].Primary] = I;
    R.SecondaryMatch[R.Matches[I].Secondary] = I;
  }
  R.PrimaryAmbiguity.assign(P.Functions.size(), NoIndex);
  R.SecondaryAmbiguity.assign(S.Functions.size(), NoIndex);
  for (uint32_t I = 0; I < R.Ambiguities.size(); ++I) {
    for (uint32_t A : R.Ambiguities[I].Primary)
      R.PrimaryAmbiguity[A] = I;
    for (uint32_t B : R.Ambiguities[I].Secondary)
      R.SecondaryAmbiguity[B] = I;
  }

  // Every pair's delta, under the final correspondence.  A stopped
  // comparison measures what it can and leaves the rest unmeasured.
  beginPhase("measure");
  std::atomic<size_t> Done{0};
  const size_t Total = R.Matches.size();
  parallelForEach(Total, [&](auto Claim, size_t Count) {
    for (size_t I = Claim(); I < Count; I = Claim()) {
      if (Control.stop())
        return;
      FunctionMatch &M = R.Matches[I];
      measure(M, delta(M.Primary, M.Secondary));
      if (M.Measured)
        Done.fetch_add(1, std::memory_order_relaxed);
    }
  });
  progress(Done.load(), Total);
  if (Done.load() != Total)
    stopNow();
  for (FunctionMatch &M : R.Matches)
    if (M.Measured)
      M.Confidence = confidenceOf(M, PF.Functions[M.Primary],
                                  contextAgreement(M.Primary, M.Secondary));
  endPhase();
}

void Matcher::run(llvm::ArrayRef<Override> Overrides) {
  beginPhase("features");
  PMatch.assign(P.Functions.size(), NoIndex);
  SMatch.assign(S.Functions.size(), NoIndex);
  {
    // Both programs at once; each computes its functions in parallel.
    PF = computeFeatures(P, Control);
    if (stopNow()) {
      finish();
      return;
    }
    progress(1, 2);
    SF = computeFeatures(S, Control);
    if (stopNow()) {
      finish();
      return;
    }
    progress(2, 2);
  }

  // The analyst's decisions come first and bind every later step.
  llvm::DenseMap<uint32_t, uint32_t> ForcedByPrimary;
  for (const Override &O : Overrides) {
    const auto A = P.functionAt(O.Primary);
    const auto B = S.functionAt(O.Secondary);
    if (!A || !B) {
      R.Warnings.push_back(
          "an override names no function: " + llvm::utohexstr(O.Primary, true) +
          " <-> " + llvm::utohexstr(O.Secondary, true));
      continue;
    }
    const uint64_t K = pairKey(*A, *B);
    recordRejection(Rejected, K, O.Kind);
    switch (O.Kind) {
    case OverrideKind::Match:
      // A new pairing replaces whatever either function was forced into.
      ForcedByPrimary.erase(*A);
      for (auto It = ForcedByPrimary.begin(); It != ForcedByPrimary.end();)
        if (It->second == *B)
          ForcedByPrimary.erase(It++);
        else
          ++It;
      ForcedByPrimary[*A] = *B;
      break;
    case OverrideKind::Unmatch:
      if (auto It = ForcedByPrimary.find(*A);
          It != ForcedByPrimary.end() && It->second == *B)
        ForcedByPrimary.erase(It);
      Confirmed.erase(K);
      break;
    case OverrideKind::Confirm:
      Confirmed.insert(K);
      break;
    }
    R.Overrides.push_back(O);
  }
  std::vector<std::pair<uint32_t, uint32_t>> Seeds(ForcedByPrimary.begin(),
                                                   ForcedByPrimary.end());
  llvm::sort(Seeds);
  for (const auto &[A, B] : Seeds)
    pair(A, B, FunctionStep::Manual, false);

  for (uint32_t I = 0; I < PF.Functions.size(); ++I)
    if (PF.Functions[I].Name)
      PrimaryNames.insert(PF.Functions[I].Name);
  for (uint32_t I = 0; I < SF.Functions.size(); ++I)
    if (SF.Functions[I].Name)
      SecondaryNames.insert(SF.Functions[I].Name);

  // Unique features over all functions and among the callers and callees
  // of every pair, until neither pairs anything more; only then the weaker
  // context of call sites and lone neighbors, and similarity last.
  beginPhase("exact");
  matchImports();
  settle(0);
  beginPhase("context");
  for (unsigned Round = 0; Round < limits::kMaxPropagationRounds && !stopNow();
       ++Round) {
    const size_t Before = Order.size();
    propagate(0, true);
    if (Order.size() == Before)
      break;
    settle(Before);
  }
  if (Settings.Approximate && !stopNow()) {
    beginPhase("approximate");
    for (unsigned Round = 0;
         Round < limits::kMaxPropagationRounds && !stopNow(); ++Round) {
      const size_t Before = Order.size();
      if (!approximate())
        break;
      settle(Before);
      propagate(Before, true);
      settle(Before);
    }
  }
  beginPhase("ambiguities");
  collectAmbiguities();
  endPhase();
  finish();
  for (uint64_t K : Confirmed)
    if (!llvm::any_of(R.Matches, [&](const FunctionMatch &M) {
          return pairKey(M.Primary, M.Secondary) == K;
        }))
      R.Warnings.push_back(
          "a confirmed pair no longer forms: " +
          llvm::utohexstr(P.Functions[K >> 32].Address, true) + " <-> " +
          llvm::utohexstr(S.Functions[uint32_t(K)].Address, true));
}

} // namespace

std::vector<std::pair<uint64_t, uint64_t>>
rejectedPairs(llvm::ArrayRef<Override> Overrides,
              const ComparisonControl &Control) {
  std::set<std::pair<uint64_t, uint64_t>> Rejected;
  for (const auto &O : Overrides) {
    Control.check();
    recordRejection(Rejected, std::pair{O.Primary, O.Secondary}, O.Kind);
  }
  Control.check();
  return {Rejected.begin(), Rejected.end()};
}

ComparisonResult compare(const Program &Primary, const Program &Secondary,
                         const ComparisonSettings &Settings,
                         llvm::ArrayRef<Override> Overrides,
                         const ComparisonControl &Control) {
  for (const Program *P : {&Primary, &Secondary})
    if (auto Error = P->validate())
      throw std::invalid_argument(*Error);
  if (auto Error = Settings.validate())
    throw std::invalid_argument(*Error);
  for (const Program *P : {&Primary, &Secondary})
    if (auto Error =
            validateDataFlowEvidence(P->DataFlow.get(), Settings.DataFlow, *P))
      throw std::invalid_argument(*Error);
  if (Primary.Identity.Architecture != Secondary.Identity.Architecture ||
      Primary.Identity.Bits != Secondary.Identity.Bits ||
      Primary.Identity.Endian != Secondary.Identity.Endian ||
      Primary.Identity.ABI != Secondary.Identity.ABI ||
      Primary.Identity.Normalization != Secondary.Identity.Normalization)
    throw std::invalid_argument("unsupported comparison profile pair");
  ComparisonResult R;
  R.Settings = Settings;
  R.FlowRegions.Profile = flowRegionProfile(Settings.DataFlow.Profile,
                                            Settings.FlowRegions.Fragments)
                              .str();
  R.PrimaryDataFlow = Primary.DataFlow;
  R.SecondaryDataFlow = Secondary.DataFlow;
  Matcher(Primary, Secondary, Settings, Control, R).run(Overrides);
  if (Settings.Regions.Enabled && R.State == ComparisonState::Complete) {
    const auto Begin = std::chrono::steady_clock::now();
    R.Regions = findRegions(Primary, Secondary, Settings.Regions, Control);
    R.Timings.push_back(
        {"regions", std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - Begin)
                        .count()});
    if (R.Regions.State && *R.Regions.State != ComparisonState::Complete) {
      R.State = *R.Regions.State;
      R.StoppedPhase = R.Regions.StoppedPhase;
    }
  }
  if (Settings.FlowRegions.Enabled && R.State == ComparisonState::Complete) {
    const auto Begin = std::chrono::steady_clock::now();
    R.FlowRegions = findFlowRegions(Primary, Secondary, Settings.DataFlow,
                                    Settings.FlowRegions, Control);
    R.Timings.push_back(
        {"flow_regions", std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - Begin)
                             .count()});
    if (R.FlowRegions.State &&
        *R.FlowRegions.State != ComparisonState::Complete) {
      R.State = *R.FlowRegions.State;
      R.StoppedPhase = R.FlowRegions.StoppedPhase;
    }
  }
  for (const auto *E : {R.PrimaryDataFlow.get(), R.SecondaryDataFlow.get()})
    if (E && E->State != ComparisonState::Complete &&
        R.State == ComparisonState::Complete) {
      R.State = E->State;
      R.StoppedPhase = "dataflow";
    }
  for (const std::string &W : Primary.Warnings)
    R.Warnings.push_back("primary: " + W);
  for (const std::string &W : Secondary.Warnings)
    R.Warnings.push_back("secondary: " + W);
  summarize(R, Primary, Secondary);
  return R;
}

void summarize(ComparisonResult &R, const Program &Primary,
               const Program &Secondary) {
  const auto Side = [&](const Program &P, const std::vector<uint32_t> &Match,
                        const std::vector<uint32_t> &Ambiguous,
                        SideCoverage &C) {
    C = SideCoverage();
    C.DiscoveryComplete = P.Identity.DiscoveryComplete;
    for (uint32_t I = 0; I < P.Functions.size(); ++I) {
      const Function &F = P.Functions[I];
      ++C.Functions;
      C.Imports += F.has(TraitImport);
      C.Thunks += F.has(TraitThunk);
      C.Library += F.has(TraitLibrary);
      C.Incomplete += F.has(TraitIncomplete);
      const uint64_t Count = P.instructionCount(F);
      C.Instructions += Count;
      if (F.hasBody())
        ++C.WithBody;
      if (I < Match.size() && Match[I] != NoIndex) {
        ++C.Matched;
        const FunctionMatch &M = R.Matches[Match[I]];
        C.MatchedInstructions += M.Measured ? M.EqualInstructions : 0;
      } else if (I < Ambiguous.size() && Ambiguous[I] != NoIndex) {
        ++C.Ambiguous;
      } else {
        ++C.Unmatched;
      }
    }
  };
  Side(Primary, R.PrimaryMatch, R.PrimaryAmbiguity, R.PrimaryCoverage);
  Side(Secondary, R.SecondaryMatch, R.SecondaryAmbiguity, R.SecondaryCoverage);
  // Library code, imports and thunks would let shared runtime code decide
  // the program's score.
  const auto Counts = [](const Program &P, const Function &F) {
    return !F.hasBody() || F.has(TraitLibrary) || F.has(TraitThunk) ||
                   F.has(TraitImport)
               ? 0.0
               : double(P.instructionCount(F));
  };
  double Total = 0, Similar = 0, Sure = 0, Measured = 0;
  for (const Function &F : Primary.Functions)
    Total += Counts(Primary, F);
  for (const Function &F : Secondary.Functions)
    Total += Counts(Secondary, F);
  for (const FunctionMatch &M : R.Matches) {
    if (!M.Measured)
      continue;
    const double W = Counts(Primary, Primary.Functions[M.Primary]) +
                     Counts(Secondary, Secondary.Functions[M.Secondary]);
    Similar += W * M.Similarity;
    Sure += W * M.Confidence;
    Measured += W;
  }
  R.Similarity = Total ? Similar / Total : 1;
  R.Confidence = Measured ? Sure / Measured : 0;
}

namespace {

/// Rebuilds a result's lookups after its matches or ambiguities change.
void reindex(ComparisonResult &R, size_t PrimaryCount, size_t SecondaryCount) {
  llvm::sort(R.Matches, [](const FunctionMatch &L, const FunctionMatch &M) {
    return L.Primary < M.Primary;
  });
  R.PrimaryMatch.assign(PrimaryCount, NoIndex);
  R.SecondaryMatch.assign(SecondaryCount, NoIndex);
  for (uint32_t I = 0; I < R.Matches.size(); ++I) {
    R.PrimaryMatch[R.Matches[I].Primary] = I;
    R.SecondaryMatch[R.Matches[I].Secondary] = I;
  }
  R.PrimaryAmbiguity.assign(PrimaryCount, NoIndex);
  R.SecondaryAmbiguity.assign(SecondaryCount, NoIndex);
  for (uint32_t I = 0; I < R.Ambiguities.size(); ++I) {
    for (uint32_t A : R.Ambiguities[I].Primary)
      R.PrimaryAmbiguity[A] = I;
    for (uint32_t B : R.Ambiguities[I].Secondary)
      R.SecondaryAmbiguity[B] = I;
  }
}

/// The functions of \p P with a call to one of \p Callees.
std::vector<uint32_t> callersOf(const Program &P,
                                const llvm::DenseSet<uint32_t> &Callees) {
  std::vector<uint32_t> Out;
  for (uint32_t F = 0; F < P.Functions.size(); ++F) {
    bool Calls = false;
    for (const Block &B : P.blocks(P.Functions[F])) {
      for (const Instruction &I : P.instructions(B))
        if (I.Callee != NoIndex && Callees.contains(I.Callee)) {
          Calls = true;
          break;
        }
      if (Calls)
        break;
    }
    if (Calls)
      Out.push_back(F);
  }
  return Out;
}

} // namespace

std::optional<std::string> applyOverride(ComparisonResult &R,
                                         const Program &Primary,
                                         const Program &Secondary,
                                         const Override &O,
                                         OverrideEffect *Effect) {
  const auto A = Primary.functionAt(O.Primary);
  const auto B = Secondary.functionAt(O.Secondary);
  if (!A)
    return "no primary function begins at " + llvm::utohexstr(O.Primary, true);
  if (!B)
    return "no secondary function begins at " +
           llvm::utohexstr(O.Secondary, true);
  if (R.PrimaryMatch.size() != Primary.Functions.size() ||
      R.SecondaryMatch.size() != Secondary.Functions.size())
    return std::string("the comparison belongs to other programs");
  const uint32_t Existing = R.PrimaryMatch[*A];
  const bool Paired =
      Existing != NoIndex && R.Matches[Existing].Secondary == *B;
  if ((O.Kind == OverrideKind::Unmatch || O.Kind == OverrideKind::Confirm) &&
      !Paired)
    return std::string("the two functions are not paired");

  OverrideEffect Local;
  OverrideEffect &E = Effect ? *Effect : Local;
  E = OverrideEffect();
  E.Applied = O;
  llvm::DenseSet<uint32_t> ChangedP, ChangedS;
  const auto Dissolve = [&](uint32_t Index) {
    const FunctionMatch M = R.Matches[Index];
    E.Removed.push_back(M);
    ChangedP.insert(M.Primary);
    ChangedS.insert(M.Secondary);
  };
  switch (O.Kind) {
  case OverrideKind::Confirm: {
    FunctionMatch &M = R.Matches[Existing];
    E.Removed.push_back(M);
    M.Confirmed = true;
    E.Added = M;
    break;
  }
  case OverrideKind::Unmatch:
    Dissolve(Existing);
    break;
  case OverrideKind::Match: {
    if (Paired) {
      FunctionMatch &M = R.Matches[Existing];
      E.Removed.push_back(M);
      M.Step = FunctionStep::Manual;
      M.Confirmed = true;
      M.FromContext = false;
      M.Confidence = 1;
      E.Added = M;
      break;
    }
    if (Existing != NoIndex)
      Dissolve(Existing);
    if (const uint32_t Other = R.SecondaryMatch[*B]; Other != NoIndex)
      Dissolve(Other);
    FunctionMatch M;
    M.Primary = *A;
    M.Secondary = *B;
    M.Step = FunctionStep::Manual;
    M.Confirmed = true;
    M.Confidence = 1;
    ChangedP.insert(*A);
    ChangedS.insert(*B);
    E.Added = M;
    break;
  }
  }
  if (O.Kind != OverrideKind::Confirm) {
    // Remove dissolved matches, add the new one.
    std::vector<uint8_t> Drop(R.Matches.size(), 0);
    for (const FunctionMatch &M : E.Removed)
      Drop[R.PrimaryMatch[M.Primary]] = 1;
    std::vector<FunctionMatch> Kept;
    Kept.reserve(R.Matches.size() + 1);
    for (size_t I = 0; I < R.Matches.size(); ++I)
      if (!Drop[I])
        Kept.push_back(R.Matches[I]);
    if (E.Added)
      Kept.push_back(*E.Added);
    R.Matches = std::move(Kept);
    // Paired functions leave their ambiguities.
    if (E.Added) {
      for (uint32_t I = 0; I < R.Ambiguities.size(); ++I) {
        Ambiguity &Amb = R.Ambiguities[I];
        const bool HasA = llvm::is_contained(Amb.Primary, *A);
        const bool HasB = llvm::is_contained(Amb.Secondary, *B);
        if (!HasA && !HasB)
          continue;
        E.Ambiguities.emplace_back(I, Amb);
        llvm::erase(Amb.Primary, *A);
        llvm::erase(Amb.Secondary, *B);
      }
      // A remaining function without any opposite-side candidates is
      // unmatched. Keep the original groups in the effect for undo, rather
      // than retaining one-sided or empty groups in the live result.
      llvm::erase_if(R.Ambiguities, [](const Ambiguity &Amb) {
        return Amb.Primary.empty() || Amb.Secondary.empty();
      });
    }
    reindex(R, Primary.Functions.size(), Secondary.Functions.size());
    // The new pair, and every pair calling a function whose counterpart
    // changed, measure again.
    const std::vector<uint64_t> PK = mnemonicKeys(Primary),
                                SK = mnemonicKeys(Secondary);
    std::vector<uint32_t> PMap(Primary.Functions.size(), NoIndex),
        SMap(Secondary.Functions.size(), NoIndex);
    for (const FunctionMatch &M : R.Matches) {
      PMap[M.Primary] = M.Secondary;
      SMap[M.Secondary] = M.Primary;
    }
    llvm::DenseSet<uint32_t> Again;
    for (uint32_t F : callersOf(Primary, ChangedP))
      if (R.PrimaryMatch[F] != NoIndex)
        Again.insert(R.PrimaryMatch[F]);
    for (uint32_t F : callersOf(Secondary, ChangedS))
      if (R.SecondaryMatch[F] != NoIndex)
        Again.insert(R.SecondaryMatch[F]);
    if (E.Added)
      Again.insert(R.PrimaryMatch[*A]);
    std::vector<uint32_t> Ordered(Again.begin(), Again.end());
    llvm::sort(Ordered);
    for (uint32_t I : Ordered) {
      FunctionMatch &M = R.Matches[I];
      const bool IsNew = E.Added && M.Primary == *A;
      if (!IsNew)
        E.Remeasured.push_back(M);
      measure(M, compareFunctionsWithKeys(Primary, M.Primary, Secondary,
                                          M.Secondary, {PMap, SMap}, PK, SK));
      if (IsNew)
        E.Added = M;
    }
  }
  R.Overrides.push_back(O);
  summarize(R, Primary, Secondary);
  return std::nullopt;
}

void revertOverride(ComparisonResult &R, const Program &Primary,
                    const Program &Secondary, const OverrideEffect &E) {
  if (E.Applied.Kind == OverrideKind::Confirm ||
      (E.Applied.Kind == OverrideKind::Match && E.Added &&
       E.Removed.size() == 1 && E.Removed[0].Primary == E.Added->Primary &&
       E.Removed[0].Secondary == E.Added->Secondary)) {
    // Only the pair's review state or step changed.
    const FunctionMatch &Before = E.Removed[0];
    if (const uint32_t I = R.PrimaryMatch[Before.Primary]; I != NoIndex)
      R.Matches[I] = Before;
  } else {
    std::vector<FunctionMatch> Kept;
    for (const FunctionMatch &M : R.Matches)
      if (!(E.Added && M.Primary == E.Added->Primary &&
            M.Secondary == E.Added->Secondary))
        Kept.push_back(M);
    for (const FunctionMatch &M : E.Removed)
      Kept.push_back(M);
    R.Matches = std::move(Kept);
    if (!E.Ambiguities.empty()) {
      // Effects retain original indices in increasing order. Merge changed
      // and removed groups back into the surviving sequence in linear time.
      std::vector<Ambiguity> Restored;
      Restored.reserve(R.Ambiguities.size() + E.Ambiguities.size());
      size_t Current = 0;
      for (const auto &[Index, Before] : E.Ambiguities) {
        while (Restored.size() < Index)
          Restored.push_back(std::move(R.Ambiguities[Current++]));
        Restored.push_back(Before);
        const bool Retained =
            llvm::any_of(Before.Primary,
                         [&](uint32_t A) { return A != E.Added->Primary; }) &&
            llvm::any_of(Before.Secondary,
                         [&](uint32_t B) { return B != E.Added->Secondary; });
        if (Retained)
          ++Current;
      }
      while (Current < R.Ambiguities.size())
        Restored.push_back(std::move(R.Ambiguities[Current++]));
      R.Ambiguities = std::move(Restored);
    }
    reindex(R, Primary.Functions.size(), Secondary.Functions.size());
    for (const FunctionMatch &Before : E.Remeasured)
      if (const uint32_t I = R.PrimaryMatch[Before.Primary]; I != NoIndex)
        R.Matches[I] = Before;
  }
  if (!R.Overrides.empty() && R.Overrides.back() == E.Applied)
    R.Overrides.pop_back();
  summarize(R, Primary, Secondary);
}

} // namespace neverd::diff
