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
#include "ReferenceContext.h"

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
  M.ReferenceComparisonComplete = D.ReferenceComparisonComplete;
  M.Similarity = D.Similarity;
  M.Changes = D.Changes;
  M.MatchedBlocks = static_cast<uint32_t>(D.Blocks.size());
  M.MatchedEdges = D.MatchedEdges;
  M.EqualInstructions = D.EqualInstructions;
}

llvm::StringRef changeState(const FunctionMatch &M) {
  if (!M.Measured)
    return "unmeasured";
  if (M.IdentityOnly)
    return "identity_only";
  if (M.Changes & ~MovementChanges)
    return "changed";
  if (!M.AnalysisComplete)
    return "incomplete";
  if (!M.ReferenceComparisonComplete)
    return "uncertain";
  return "unchanged";
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
  struct CandidateEvidence {
    FunctionStep Step;
    bool FromContext = false;
    bool ContextEligible = false;
    bool CallSite = false;
    bool DataReference = false;
    bool AddressSite = false;
  };
  llvm::DenseMap<uint64_t, CandidateEvidence> FeatureCandidates;
  Clock::time_point PhaseStart;
  std::string Phase;
  bool Stopped = false;

  static uint64_t pairKey(uint32_t A, uint32_t B) {
    return (uint64_t(A) << 32) | B;
  }
  void propose(uint32_t A, uint32_t B, FunctionStep Step, bool FromContext) {
    auto [It, Inserted] =
        FeatureCandidates.try_emplace(pairKey(A, B), CandidateEvidence{Step});
    (void)Inserted;
    It->second.FromContext |= FromContext;
    It->second.ContextEligible |= Step == FunctionStep::CallSite ||
                                  Step == FunctionStep::Neighborhood ||
                                  Step == FunctionStep::AddressSite;
    It->second.CallSite |= Step == FunctionStep::CallSite;
    It->second.AddressSite |= Step == FunctionStep::AddressSite;
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
  unsigned approximate(bool ReferenceOnly = false);
  uint64_t featureKey(FunctionStep Step, uint32_t Function,
                      bool Secondary) const;
  double contextAgreement(uint32_t A, uint32_t B) const;
  double quickScore(uint32_t A, uint32_t B) const;
  bool consistentContext(uint32_t A, uint32_t B) const;
  FunctionDelta delta(uint32_t A, uint32_t B) const;
  std::vector<uint32_t> inlinedTailCallers(uint32_t A, uint32_t B,
                                           const FunctionDelta &D,
                                           bool Secondary) const;
  std::vector<uint32_t> unmatchedNeighbors(const ProgramFeatures &F,
                                           const Adjacency &Graph, uint32_t Of,
                                           uint32_t Side) const;
  void collectAmbiguities();
  void retainEntryAlternatives();
  void finish();
};

uint64_t Matcher::featureKey(FunctionStep Step, uint32_t Function,
                             bool Secondary) const {
  const auto &Features = Secondary ? SF : PF;
  const auto &Program = Secondary ? S : P;
  const auto &F = Features.Functions[Function];
  const uint64_t Key =
      stepKey(Step, F, Program.Functions[Function], Settings.UseNames);
  if (!Key || (Step != FunctionStep::ExactBytes &&
               Step != FunctionStep::NormalizedBody))
    return Key;
  if (F.UnresolvedCallee || (Step == FunctionStep::ExactBytes &&
                             (F.UnresolvedData || F.AddressEncodings)))
    return 0;
  const auto Targets = Features.Referents.of(Function);
  if (Targets.size() != F.FunctionReferences)
    return 0; // A filtered snapshot omitted at least one target.
  if (F.AddressSites.empty())
    return F.FunctionReferences ? 0 : Key;
  StableHash References;
  for (const auto &Site : F.AddressSites) {
    if (Site.Target == NoIndex)
      return 0;
    References.add(Site.Instruction).add(Site.Operand);
    // A reference to the function being compared is intrinsic. Other targets
    // need independent correspondence before a body key is strong evidence.
    if (Site.Target == Function) {
      References.add("self");
      continue;
    }
    const uint32_t Mapped =
        Secondary ? SMatch[Site.Target] : PMatch[Site.Target];
    if (Mapped == NoIndex)
      return 0;
    References.add("mapped").add(Secondary ? Site.Target : Mapped);
  }
  return StableHash()
      .add(Key)
      .add(References.value())
      .add(F.AddressSites.size())
      .value();
}

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
        if (const uint64_t Key = featureKey(Step, A, false))
          Entries.push_back({Key, 0, A});
    for (uint32_t B : Ss)
      if (SMatch[B] == NoIndex)
        if (const uint64_t Key = featureKey(Step, B, true))
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
        // Reference sets and graph degree propose candidates. Code moved into
        // a new helper can inherit the old strings/imports; compare competing
        // bodies before consuming either endpoint, even when this key is
        // unique.
        if (Step != FunctionStep::SymbolName &&
            Step != FunctionStep::ExactBytes &&
            Step != FunctionStep::NormalizedBody) {
          const uint32_t A = GroupP[0], B = GroupS[0];
          if (Step == FunctionStep::CallGraphPosition &&
              contextAgreement(A, B) == 0)
            continue;
          propose(A, B, Step, FromContext);
          continue;
        }
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
  for (const auto &[AG, BG] : {std::pair{&PF.Callees, &SF.Callees},
                               {&PF.Callers, &SF.Callers},
                               {&PF.Referents, &SF.Referents},
                               {&PF.Referrers, &SF.Referrers}}) {
    const auto AN = AG->of(A), BN = BG->of(B);
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
    propose(C, D, FunctionStep::CallSite, true);
  }
}

void Matcher::context(uint32_t A, uint32_t B, bool Loose) {
  for (const auto &[AG, BG] : {std::pair{&PF.Callees, &SF.Callees},
                               {&PF.Callers, &SF.Callers},
                               {&PF.Referents, &SF.Referents},
                               {&PF.Referrers, &SF.Referrers}}) {
    auto As = unmatchedNeighbors(PF, *AG, A, 0);
    auto Bs = unmatchedNeighbors(SF, *BG, B, 1);
    if (As.empty() || Bs.empty())
      continue;
    drill(As, Bs, 0, true, 0);
    if (!Loose)
      continue;
    // Weaker context once no exact feature is left to pair by: the place
    // of a call, and a single remaining neighbor on both sides.
    if (AG == &PF.Callees)
      callSites(A, B);
    if (stopNow())
      return;
    As = unmatchedNeighbors(PF, *AG, A, 0);
    Bs = unmatchedNeighbors(SF, *BG, B, 1);
    if (As.size() == 1 && Bs.size() == 1 &&
        !Rejected.contains(pairKey(As[0], Bs[0])) &&
        !namesConflict(As[0], Bs[0])) {
      propose(As[0], Bs[0], FunctionStep::Neighborhood, true);
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

std::vector<uint32_t> Matcher::inlinedTailCallers(uint32_t A, uint32_t B,
                                                  const FunctionDelta &D,
                                                  bool Secondary) const {
  const auto &Source = Secondary ? S : P;
  const auto &Target = Secondary ? P : S;
  const auto &Features = Secondary ? SF : PF;
  const auto &Map = Secondary ? SMatch : PMatch;
  const uint32_t Helper = Secondary ? B : A, Merged = Secondary ? A : B;
  const auto Callers = Features.TailCallers.of(Helper);
  std::vector<uint32_t> Candidates;
  if (Callers.empty() ||
      D.EqualInstructions < limits::kMinTailPreludeContribution)
    return Candidates;
  const auto &MergedFunction = Target.Functions[Merged];
  const auto &Entry = Target.blocks(MergedFunction)[MergedFunction.EntryBlock];
  llvm::DenseSet<uint32_t> Added;
  for (const DeltaRow &Row : D.Rows)
    if (Secondary ? Row.Kind == RowKind::Deleted &&
                        Row.PrimaryBlock == MergedFunction.EntryBlock
                  : Row.Kind == RowKind::Inserted &&
                        Row.SecondaryBlock == MergedFunction.EntryBlock)
      Added.insert(Secondary ? Row.Primary : Row.Secondary);
  if (Added.size() < limits::kMinTailPreludeContribution)
    return Candidates;
  // Each tail edge is indexed once. Only callers that can fit in the merged
  // entry are measured, in the existing parallel candidate batch. Reuse the
  // delta's reference-aware classification instead of inventing an operand
  // equality rule here. Uncertain rows can justify deferral, never identity.
  for (uint32_t Caller : Callers) {
    if (Control.stop())
      return {};
    const uint32_t X = Secondary ? A : Caller, Y = Secondary ? Caller : B;
    if (Map[Caller] != NoIndex || Rejected.contains(pairKey(X, Y)) ||
        namesConflict(X, Y))
      continue;
    const auto &Prefix = Source.blocks(Source.Functions[Caller]).front();
    const uint32_t Count = Prefix.InstructionCount - 1;
    if (Count > Entry.InstructionCount)
      continue;
    const auto Prelude = delta(X, Y);
    if (!Prelude.ComputationComplete)
      return {};
    uint32_t Aligned = 0, Contributed = 0;
    for (const DeltaRow &Row : Prelude.Rows) {
      const uint32_t From = Secondary ? Row.Secondary : Row.Primary;
      const uint32_t To = Secondary ? Row.Primary : Row.Secondary;
      if (From < Prefix.FirstInstruction ||
          From - Prefix.FirstInstruction >= Count)
        continue;
      if (To < Entry.FirstInstruction ||
          To - Entry.FirstInstruction >= Entry.InstructionCount ||
          (Row.Kind != RowKind::Identical && Row.Kind != RowKind::Moved &&
           Row.Kind != RowKind::Uncertain))
        break;
      ++Aligned;
      Contributed += Added.contains(To);
    }
    if (Aligned == Count && Contributed >= limits::kMinTailPreludeContribution)
      Candidates.push_back(Caller);
  }
  return Candidates;
}

bool Matcher::consistentContext(uint32_t A, uint32_t B) const {
  const auto It = FeatureCandidates.find(pairKey(A, B));
  if (It == FeatureCandidates.end())
    return false;
  const double Agreement = contextAgreement(A, B);
  return ((It->second.CallSite || It->second.AddressSite) && Agreement == 1) ||
         (It->second.DataReference && (Agreement < 0 || Agreement == 1));
}

unsigned Matcher::approximate(bool ReferenceOnly) {
  if (!Settings.Approximate && FeatureCandidates.empty())
    return 0;
  // Candidates: bounded sketch and mnemonic/CFG groups, and the unmatched
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
  std::vector<uint8_t> ReferenceA, ReferenceB;
  if (ReferenceOnly) {
    ReferenceA.resize(P.Functions.size());
    ReferenceB.resize(S.Functions.size());
  }
  for (const auto &[Key, Evidence] : FeatureCandidates) {
    const uint32_t A = uint32_t(Key >> 32), B = uint32_t(Key);
    if (PMatch[A] == NoIndex && SMatch[B] == NoIndex) {
      Candidates.push_back(Key);
      if (ReferenceOnly && (Evidence.DataReference || Evidence.AddressSite))
        ReferenceA[A] = ReferenceB[B] = 1;
    }
  }
  {
    struct Entry {
      uint64_t Key;
      uint8_t Side;
      uint32_t Function;
    };
    std::vector<Entry> Entries;
    Entries.reserve(Ps.size() + Ss.size());
    // A common instruction sketch can still contain small, discriminating
    // mnemonic/CFG groups. Admit those alternatives before choosing a weak
    // feature proposal; shared graph keys never establish a match by
    // themselves.
    for (unsigned Bucket = 0; Bucket <= SketchBands + 1; ++Bucket) {
      if (stopNow())
        return 0;
      Entries.clear();
      const auto Append = [&](uint32_t Function, bool Secondary) {
        const auto &F = (Secondary ? SF : PF).Functions[Function];
        if (Bucket < SketchBands) {
          if (F.Sketched)
            Entries.push_back({sketchBand(F, Bucket), Secondary, Function});
        } else if (Bucket == SketchBands) {
          // Complete instruction bodies survive landing/padding edits that
          // break short run sketches. This key only retrieves candidates;
          // original instructions and references still decide the delta.
          if (F.BodyWithoutPadding)
            Entries.push_back({F.BodyWithoutPadding, Secondary, Function});
        } else if (const uint64_t K = featureKey(
                       FunctionStep::MnemonicsAndGraph, Function, Secondary))
          Entries.push_back({K, Secondary, Function});
      };
      for (uint32_t A : Ps)
        Append(A, false);
      for (uint32_t B : Ss)
        Append(B, true);
      llvm::sort(Entries, [](const Entry &L, const Entry &R) {
        return std::tie(L.Key, L.Side, L.Function) <
               std::tie(R.Key, R.Side, R.Function);
      });
      for (size_t I = 0; I < Entries.size();) {
        if (stopNow())
          return 0;
        size_t J = I;
        while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
          ++J;
        if (J - I <= limits::kMaxSketchBucket)
          for (size_t X = I; X < J; ++X)
            for (size_t Y = I; Y < J; ++Y)
              if (Entries[X].Side == 0 && Entries[Y].Side == 1 &&
                  (Bucket != SketchBands ||
                   PF.Functions[Entries[X].Function].BodyWithPadding !=
                       SF.Functions[Entries[Y].Function].BodyWithPadding))
                Candidates.push_back(
                    pairKey(Entries[X].Function, Entries[Y].Function));
        I = J;
      }
    }
    // A widely shared callee (for example malloc) is no more discriminating
    // than a common sketch bucket. Expand each matched neighborhood once and
    // apply the same admission bound before its Cartesian product exists.
    for (const auto &Matched : Order)
      for (const auto &[AG, BG] : {std::pair{&PF.Callees, &SF.Callees},
                                   {&PF.Callers, &SF.Callers},
                                   {&PF.Referents, &SF.Referents},
                                   {&PF.Referrers, &SF.Referrers}}) {
        if (stopNow())
          return 0;
        const auto As = unmatchedNeighbors(PF, *AG, Matched.Primary, 0);
        const auto Bs = unmatchedNeighbors(SF, *BG, Matched.Secondary, 1);
        if (As.size() + Bs.size() <= limits::kMaxSketchBucket)
          for (uint32_t A : As)
            for (uint32_t B : Bs)
              if (P.Functions[A].hasBody() && S.Functions[B].hasBody())
                Candidates.push_back(pairKey(A, B));
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
    bool ContextHint = false;
    std::vector<uint32_t> PrimaryTailCallers;
    std::vector<uint32_t> SecondaryTailCallers;
  };
  std::vector<Scored> Pairs;
  Pairs.reserve(Candidates.size());
  for (uint64_t K : Candidates) {
    if (stopNow())
      return 0;
    const uint32_t A = uint32_t(K >> 32), B = uint32_t(K);
    // After entry review, measure only reference proposals and their rivals.
    // An unrelated candidate has no new evidence and must stay deferred.
    if (ReferenceOnly && !ReferenceA[A] && !ReferenceB[B])
      continue;
    if (!Rejected.contains(K) && !namesConflict(A, B)) {
      Pairs.push_back(
          {A, B, quickScore(A, B), -1, consistentContext(A, B), {}, {}});
    }
  }
  const unsigned Keep = Settings.MaxCandidates;
  std::vector<uint8_t> Measure(Pairs.size(), 0);
  for (size_t I = 0; I < Pairs.size(); ++I)
    Measure[I] = FeatureCandidates.contains(pairKey(Pairs[I].A, Pairs[I].B));
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
      const auto D = delta(Pair.A, Pair.B);
      Pair.Exact = D.Similarity;
      if (D.ComputationComplete &&
          Pair.Exact >= std::min(Settings.MinimumSimilarity,
                                 Settings.MinimumContextSimilarity)) {
        Pair.PrimaryTailCallers = inlinedTailCallers(Pair.A, Pair.B, D, false);
        Pair.SecondaryTailCallers = inlinedTailCallers(Pair.A, Pair.B, D, true);
      }
    }
  });
  if (stopNow())
    return 0;

  // Strongest first; a pair whose functions have another candidate nearly
  // as similar stays undecided.
  std::vector<uint32_t> Ranked = ToMeasure;
  llvm::sort(Ranked, [&](uint32_t L, uint32_t R) {
    const Scored &X = Pairs[L], &Y = Pairs[R];
    return std::make_tuple(-X.Exact, !X.ContextHint, -X.Quick, X.A, X.B) <
           std::make_tuple(-Y.Exact, !Y.ContextHint, -Y.Quick, Y.A, Y.B);
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
    const auto Feature = FeatureCandidates.find(pairKey(Pair.A, Pair.B));
    if (!Settings.Approximate && Feature == FeatureCandidates.end())
      continue;
    // Body alternatives still compete through ByA/ByB, but only a proposal
    // supported by the new frozen reference evidence can be admitted here.
    if (ReferenceOnly &&
        (Feature == FeatureCandidates.end() ||
         (!Feature->second.DataReference && !Feature->second.AddressSite)))
      continue;
    // A positional or lone-neighbor proposal earns the lower context
    // threshold only when every already mapped neighbor agrees. A local
    // position cannot overrule contradicting context elsewhere in the graph.
    const bool Context =
        Feature != FeatureCandidates.end() && Feature->second.ContextEligible;
    const double Minimum = Context && contextAgreement(Pair.A, Pair.B) == 1
                               ? Settings.MinimumContextSimilarity
                               : Settings.MinimumSimilarity;
    if (Pair.Exact < Minimum)
      continue;
    if (PMatch[Pair.A] != NoIndex || SMatch[Pair.B] != NoIndex)
      continue;
    bool BoundaryAmbiguous = false;
    for (bool Secondary : {false, true}) {
      const auto &Map = Secondary ? SMatch : PMatch;
      std::vector<uint32_t> Callers =
          Secondary ? Pair.SecondaryTailCallers : Pair.PrimaryTailCallers;
      llvm::erase_if(Callers, [&](uint32_t F) { return Map[F] != NoIndex; });
      if (Callers.empty())
        continue;
      Callers.push_back(Secondary ? Pair.B : Pair.A);
      auto &Undecided = Secondary ? UndecidedB : UndecidedA;
      for (uint32_t F : Callers)
        Undecided.insert(F);
      NearTies.push_back(
          Secondary
              ? Tie{FunctionStep::InlinedTailCall, {Pair.A}, std::move(Callers)}
              : Tie{FunctionStep::InlinedTailCall,
                    std::move(Callers),
                    {Pair.B}});
      BoundaryAmbiguous = true;
    }
    if (BoundaryAmbiguous) {
      UndecidedA.insert(Pair.A);
      UndecidedB.insert(Pair.B);
      continue;
    }
    if (UndecidedA.contains(Pair.A) || UndecidedB.contains(Pair.B))
      continue;
    // Consistent call positions or independent data-reference roles can
    // distinguish equal or weaker bodies. Neither overrules a stronger body
    // or a rival with its own consistent context.
    // Earlier admissions in this round may change the mapped neighborhood.
    // The sorting hint must not preserve a now-contradictory call position.
    const bool Positioned = consistentContext(Pair.A, Pair.B);
    const auto Rival = [&](const Scored &Other) {
      if (Other.Exact <= Pair.Exact && Positioned &&
          !consistentContext(Other.A, Other.B))
        return false;
      return Other.Exact >= Pair.Exact ||
             Pair.Exact - Other.Exact < Settings.AmbiguityMargin;
    };
    std::vector<uint32_t> RivalsOfA, RivalsOfB;
    for (uint32_t Other : ByA[Pair.A]) {
      const Scored &O = Pairs[Other];
      if (Other != I && SMatch[O.B] == NoIndex && Rival(O))
        RivalsOfA.push_back(O.B);
    }
    for (uint32_t Other : ByB[Pair.B]) {
      const Scored &O = Pairs[Other];
      if (Other != I && PMatch[O.A] == NoIndex && Rival(O))
        RivalsOfB.push_back(O.A);
    }
    if (!RivalsOfA.empty() || !RivalsOfB.empty()) {
      UndecidedA.insert(Pair.A);
      UndecidedB.insert(Pair.B);
      for (uint32_t A : RivalsOfB)
        UndecidedA.insert(A);
      for (uint32_t B : RivalsOfA)
        UndecidedB.insert(B);
      continue;
    }
    Paired +=
        pair(Pair.A, Pair.B,
             Feature == FeatureCandidates.end() ? FunctionStep::Similarity
                                                : Feature->second.Step,
             Feature != FeatureCandidates.end() && Feature->second.FromContext);
  }
  // Retain every endpoint's near-best remaining alternatives, including
  // endpoints already deferred by another tie. Each group is a star: combining
  // the two endpoints' rivals would invent unmeasured cross-pairs.
  const auto Retain = [&](bool Secondary, uint32_t F) {
    if (Secondary ? SMatch[F] != NoIndex : PMatch[F] != NoIndex)
      return;
    const auto &ByFunction = Secondary ? ByB : ByA;
    const auto It = ByFunction.find(F);
    if (It == ByFunction.end())
      return;
    std::vector<uint32_t> Others;
    double Best = -1;
    for (uint32_t I : It->second) {
      const Scored &Pair = Pairs[I];
      if (Secondary ? PMatch[Pair.A] != NoIndex : SMatch[Pair.B] != NoIndex)
        continue;
      if (Best < 0)
        Best = Pair.Exact;
      if (Best < Settings.MinimumSimilarity ||
          (Pair.Exact < Best && Best - Pair.Exact >= Settings.AmbiguityMargin))
        break;
      Others.push_back(Secondary ? Pair.A : Pair.B);
    }
    if (Others.size() > 1)
      NearTies.push_back(
          Secondary ? Tie{FunctionStep::Similarity, std::move(Others), {F}}
                    : Tie{FunctionStep::Similarity, {F}, std::move(Others)});
  };
  for (uint32_t A : Ps)
    Retain(false, A);
  for (uint32_t B : Ss)
    Retain(true, B);
  return Paired;
}

void Matcher::retainEntryAlternatives() {
  if (!Settings.Approximate || stopNow())
    return;
  // Review against one settled correspondence. New guard candidates must not
  // change propagation order or establish identities from a shared prefix.
  std::vector<uint32_t> WeakA(P.Functions.size(), NoIndex),
      WeakB(S.Functions.size(), NoIndex);
  for (uint32_t I = 0; I < Order.size(); ++I) {
    const auto &F = Order[I];
    if (F.Step == FunctionStep::Manual || F.Step == FunctionStep::ImportName ||
        F.Step == FunctionStep::SymbolName ||
        F.Step == FunctionStep::ExactBytes ||
        F.Step == FunctionStep::NormalizedBody || F.FromContext ||
        Confirmed.contains(pairKey(F.Primary, F.Secondary)) ||
        contextAgreement(F.Primary, F.Secondary) > 0)
      continue;
    WeakA[F.Primary] = WeakB[F.Secondary] = I;
  }
  struct Entry {
    uint64_t Key;
    uint8_t Side;
    uint32_t Function;
  };
  std::vector<Entry> Entries;
  Entries.reserve(P.Functions.size() + S.Functions.size());
  for (bool Secondary : {false, true}) {
    const auto &Features = Secondary ? SF : PF;
    const auto &Map = Secondary ? SMatch : PMatch;
    const auto &Weak = Secondary ? WeakB : WeakA;
    for (uint32_t I = 0; I < Features.Functions.size(); ++I)
      if ((Map[I] == NoIndex || Weak[I] != NoIndex) &&
          Features.Functions[I].EntryBody)
        Entries.push_back({Features.Functions[I].EntryBody, Secondary, I});
  }
  llvm::sort(Entries, [](const Entry &A, const Entry &B) {
    return std::tie(A.Key, A.Side, A.Function) <
           std::tie(B.Key, B.Side, B.Function);
  });
  struct Candidate {
    uint32_t A, B, Prior;
    double Quick, Exact = -1;
  };
  std::vector<Candidate> Candidates;
  for (size_t I = 0; I < Entries.size();) {
    if (stopNow())
      return;
    size_t J = I + 1;
    while (J < Entries.size() && Entries[J].Key == Entries[I].Key)
      ++J;
    if (J - I <= limits::kMaxSketchBucket)
      for (size_t X = I; X < J; ++X)
        for (size_t Y = I; Y < J; ++Y) {
          if (Entries[X].Side != 0 || Entries[Y].Side != 1)
            continue;
          const uint32_t A = Entries[X].Function, B = Entries[Y].Function;
          if ((PMatch[A] != NoIndex && SMatch[B] != NoIndex) ||
              Rejected.contains(pairKey(A, B)) || namesConflict(A, B))
            continue;
          const auto &AF = PF.Functions[A], &BF = SF.Functions[B];
          if (AF.BodyWithPadding == BF.BodyWithPadding ||
              std::tie(AF.Blocks, AF.Edges, AF.GraphTopDown,
                       AF.GraphBottomUp) == std::tie(BF.Blocks, BF.Edges,
                                                     BF.GraphTopDown,
                                                     BF.GraphBottomUp))
            continue;
          Candidates.push_back({A, B,
                                PMatch[A] == NoIndex ? WeakB[B] : WeakA[A],
                                quickScore(A, B)});
        }
    I = J;
  }
  llvm::sort(Candidates, [](const Candidate &A, const Candidate &B) {
    return std::make_tuple(-A.Quick, A.A, A.B) <
           std::make_tuple(-B.Quick, B.A, B.B);
  });
  llvm::DenseMap<uint32_t, unsigned> CountA, CountB;
  std::vector<uint32_t> ToMeasure, Priors;
  for (uint32_t I = 0; I < Candidates.size(); ++I) {
    const auto &C = Candidates[I];
    const unsigned A = CountA[C.A]++, B = CountB[C.B]++;
    if (A >= Settings.MaxCandidates && B >= Settings.MaxCandidates)
      continue;
    ToMeasure.push_back(I);
    if (C.Prior != NoIndex)
      Priors.push_back(C.Prior);
  }
  llvm::sort(Priors);
  Priors.erase(std::unique(Priors.begin(), Priors.end()), Priors.end());
  std::vector<double> PriorScores(Order.size(), -1);
  parallelForEach(ToMeasure.size() + Priors.size(),
                  [&](auto Claim, size_t Total) {
                    for (size_t I = Claim(); I < Total; I = Claim()) {
                      if (Control.stop())
                        return;
                      if (I < ToMeasure.size()) {
                        auto &C = Candidates[ToMeasure[I]];
                        const auto D = delta(C.A, C.B);
                        if (D.ComputationComplete)
                          C.Exact = D.Similarity;
                      } else {
                        const uint32_t Prior = Priors[I - ToMeasure.size()];
                        const auto &F = Order[Prior];
                        const auto D = delta(F.Primary, F.Secondary);
                        if (D.ComputationComplete)
                          PriorScores[Prior] = D.Similarity;
                      }
                    }
                  });
  if (stopNow())
    return;
  std::vector<uint8_t> Deferred(Order.size(), 0);
  for (uint32_t I : ToMeasure) {
    const auto &C = Candidates[I];
    if (C.Exact < Settings.MinimumSimilarity)
      continue;
    if (C.Prior != NoIndex) {
      const double Prior = PriorScores[C.Prior];
      if (Prior < 0 || C.Exact <= Prior ||
          C.Exact - Prior < Settings.AmbiguityMargin)
        continue;
      Deferred[C.Prior] = 1;
    }
    NearTies.push_back({FunctionStep::Similarity, {C.A}, {C.B}});
  }
  for (uint32_t I = 0; I < Order.size(); ++I)
    if (Deferred[I]) {
      const auto &F = Order[I];
      PMatch[F.Primary] = SMatch[F.Secondary] = NoIndex;
      NearTies.push_back({F.Step, {F.Primary}, {F.Secondary}});
    }
  llvm::erase_if(Order,
                 [&](const Formed &F) { return PMatch[F.Primary] == NoIndex; });
}

void Matcher::collectAmbiguities() {
  std::vector<uint32_t> InPrimary(P.Functions.size(), NoIndex),
      InSecondary(S.Functions.size(), NoIndex);
  const auto Record = [&](Tie T, bool Exact) {
    llvm::erase_if(T.Primary, [&](uint32_t A) {
      return PMatch[A] != NoIndex || (Exact && InPrimary[A] != NoIndex);
    });
    llvm::erase_if(T.Secondary, [&](uint32_t B) {
      return SMatch[B] != NoIndex || (Exact && InSecondary[B] != NoIndex);
    });
    if (!Exact && T.Step != FunctionStep::InlinedTailCall) {
      // Exact groups already retain their full cross-product compactly.
      // Remove only those covered edges from a similarity star; sharing an
      // endpoint alone must not discard another measured alternative.
      const auto Covered = [&](uint32_t A, uint32_t B) {
        return InPrimary[A] != NoIndex && InPrimary[A] == InSecondary[B];
      };
      if (T.Primary.size() == 1)
        llvm::erase_if(T.Secondary,
                       [&](uint32_t B) { return Covered(T.Primary[0], B); });
      else if (T.Secondary.size() == 1)
        llvm::erase_if(T.Primary,
                       [&](uint32_t A) { return Covered(A, T.Secondary[0]); });
    }
    llvm::sort(T.Primary);
    T.Primary.erase(std::unique(T.Primary.begin(), T.Primary.end()),
                    T.Primary.end());
    llvm::sort(T.Secondary);
    T.Secondary.erase(std::unique(T.Secondary.begin(), T.Secondary.end()),
                      T.Secondary.end());
    if (T.Primary.empty() || T.Secondary.empty())
      return;
    if (Exact) {
      for (uint32_t A : T.Primary)
        InPrimary[A] = R.Ambiguities.size();
      for (uint32_t B : T.Secondary)
        InSecondary[B] = R.Ambiguities.size();
    }
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
      if (PMatch[A] == NoIndex && InPrimary[A] == NoIndex)
        if (const uint64_t K = featureKey(Step, A, false))
          Entries.push_back({K, 0, A});
    for (uint32_t B = 0; B < S.Functions.size(); ++B)
      if (SMatch[B] == NoIndex && InSecondary[B] == NoIndex)
        if (const uint64_t K = featureKey(Step, B, true))
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
      // Retain a deferred singleton's evidence as well as shared-key groups.
      // Rejected overrides must not reappear as suggestions.
      const bool Deferred =
          T.Primary.size() == 1 && T.Secondary.size() == 1 &&
          FeatureCandidates.contains(pairKey(T.Primary[0], T.Secondary[0])) &&
          !Rejected.contains(pairKey(T.Primary[0], T.Secondary[0]));
      if (!T.Primary.empty() && !T.Secondary.empty() &&
          (T.Primary.size() + T.Secondary.size() > 2 || Deferred))
        Record(std::move(T), true);
    }
  }
  for (Tie &T : NearTies)
    Record(std::move(T), false);
  for (const auto &[Key, Feature] : FeatureCandidates)
    if (!Rejected.contains(Key))
      Record({Feature.Step, {uint32_t(Key >> 32)}, {uint32_t(Key)}}, false);
  const auto Key = [](const Ambiguity &A) {
    return std::tie(A.Primary, A.Secondary, A.Step);
  };
  llvm::sort(R.Ambiguities, [&](const Ambiguity &A, const Ambiguity &B) {
    return Key(A) < Key(B);
  });
  R.Ambiguities.erase(std::unique(R.Ambiguities.begin(), R.Ambiguities.end(),
                                  [&](const Ambiguity &A, const Ambiguity &B) {
                                    return Key(A) == Key(B);
                                  }),
                      R.Ambiguities.end());
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

  // Unique features over all functions and among the call/address neighbors
  // of every pair, until neither pairs anything more. Resolve measured body
  // candidates before weaker lone-neighbor context can consume their endpoints.
  beginPhase("exact");
  matchImports();
  settle(0);
  beginPhase("approximate");
  for (unsigned Round = 0; Round < limits::kMaxPropagationRounds && !stopNow();
       ++Round) {
    const size_t Before = Order.size();
    // Context only proposes; every proposal competes with measured body
    // candidates before any endpoint is consumed. This also preserves exact
    // feature matching when sketch-only approximation is disabled.
    propagate(0, true);
    if (!approximate() && Order.size() == Before)
      break;
    settle(Before);
  }
  beginPhase("ambiguities");
  retainEntryAlternatives();
  // Freeze all witnesses before adding data-reference proposals. A candidate
  // cannot establish the data relation that would justify its own admission.
  beginPhase("data-reference-context");
  progress(0, 1);
  const auto Candidates =
      referenceCandidates(P, PF, PMatch, S, SF, SMatch, Control);
  for (const auto &[A, B] : Candidates.Data) {
    propose(A, B, FunctionStep::DataReference, true);
    auto &Evidence = FeatureCandidates.find(pairKey(A, B))->second;
    Evidence.Step = FunctionStep::DataReference;
    Evidence.DataReference = true;
  }
  for (const auto &[A, B] : Candidates.Functions) {
    propose(A, B, FunctionStep::AddressSite, true);
    FeatureCandidates.find(pairKey(A, B))->second.Step =
        FunctionStep::AddressSite;
  }
  if ((!Candidates.Data.empty() || !Candidates.Functions.empty()) && !stopNow())
    approximate(true);
  if (!stopNow())
    progress(1, 1);
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
  R.Regions.Calls = Settings.Regions.Calls;
  R.FlowRegions.Profile = flowRegionProfile(Settings.DataFlow.Profile,
                                            Settings.FlowRegions.Fragments)
                              .str();
  R.PrimaryDataFlow = Primary.DataFlow;
  R.SecondaryDataFlow = Secondary.DataFlow;
  Matcher(Primary, Secondary, Settings, Control, R).run(Overrides);
  if (Settings.Regions.Enabled && R.State == ComparisonState::Complete) {
    const auto Begin = std::chrono::steady_clock::now();
    R.Regions =
        findRegions(Primary, Secondary, Settings.Regions, Control, R.Matches);
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
void reindex(ComparisonResult &R, size_t PrimaryCount, size_t SecondaryCount,
             const ComparisonControl &Control) {
  Control.check();
  llvm::sort(R.Matches, [](const FunctionMatch &L, const FunctionMatch &M) {
    return L.Primary < M.Primary;
  });
  R.PrimaryMatch.assign(PrimaryCount, NoIndex);
  R.SecondaryMatch.assign(SecondaryCount, NoIndex);
  for (uint32_t I = 0; I < R.Matches.size(); ++I) {
    Control.check();
    R.PrimaryMatch[R.Matches[I].Primary] = I;
    R.SecondaryMatch[R.Matches[I].Secondary] = I;
  }
  R.PrimaryAmbiguity.assign(PrimaryCount, NoIndex);
  R.SecondaryAmbiguity.assign(SecondaryCount, NoIndex);
  for (uint32_t I = 0; I < R.Ambiguities.size(); ++I) {
    Control.check();
    for (uint32_t A : R.Ambiguities[I].Primary)
      R.PrimaryAmbiguity[A] = I;
    for (uint32_t B : R.Ambiguities[I].Secondary)
      R.SecondaryAmbiguity[B] = I;
  }
}

/// Functions whose call or address evidence depends on one of \p Callees.
std::vector<uint32_t> dependentsOf(const Program &P,
                                   const llvm::DenseSet<uint32_t> &Callees,
                                   const ComparisonControl &Control) {
  std::vector<uint32_t> Out;
  for (uint32_t F = 0; F < P.Functions.size(); ++F) {
    Control.check();
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
    if (!Calls)
      for (const auto &Ref : P.references(P.Functions[F])) {
        Control.check();
        if (Ref.Kind == ReferenceKind::Function)
          if (const auto Target = P.functionAt(Ref.Key);
              Target && Callees.contains(*Target)) {
            Calls = true;
            break;
          }
      }
    if (Calls)
      Out.push_back(F);
  }
  return Out;
}

void refreshCallRegions(ComparisonResult &R, const Program &Primary,
                        const Program &Secondary,
                        const ComparisonControl &Control) {
  if (!R.Settings.Regions.Enabled || !R.Settings.Regions.Calls)
    return;
  auto Regions =
      findRegions(Primary, Secondary, R.Settings.Regions, Control, R.Matches);
  if (Regions.State == ComparisonState::Cancelled ||
      Regions.State == ComparisonState::TimedOut)
    throw ComparisonInterrupted(*Regions.State);
  if (R.State == ComparisonState::Complete ||
      llvm::StringRef(R.StoppedPhase).starts_with("regions:")) {
    R.State = *Regions.State;
    R.StoppedPhase = Regions.StoppedPhase;
  }
  R.Regions = std::move(Regions);
}

std::optional<std::string>
applyOverrideInPlace(ComparisonResult &R, const Program &Primary,
                     const Program &Secondary, const Override &O,
                     OverrideEffect &E, const ComparisonControl &Control) {
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

  Control.report("mapping");
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
        Control.check();
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
    reindex(R, Primary.Functions.size(), Secondary.Functions.size(), Control);
    // The new pair, and every pair referencing a function whose counterpart
    // changed, measure again.
    const std::vector<uint64_t> PK = mnemonicKeys(Primary, Control),
                                SK = mnemonicKeys(Secondary, Control);
    std::vector<uint32_t> PMap(Primary.Functions.size(), NoIndex),
        SMap(Secondary.Functions.size(), NoIndex);
    for (const FunctionMatch &M : R.Matches) {
      Control.check();
      PMap[M.Primary] = M.Secondary;
      SMap[M.Secondary] = M.Primary;
    }
    llvm::DenseSet<uint32_t> Again;
    for (uint32_t F : dependentsOf(Primary, ChangedP, Control))
      if (R.PrimaryMatch[F] != NoIndex)
        Again.insert(R.PrimaryMatch[F]);
    for (uint32_t F : dependentsOf(Secondary, ChangedS, Control))
      if (R.SecondaryMatch[F] != NoIndex)
        Again.insert(R.SecondaryMatch[F]);
    if (E.Added)
      Again.insert(R.PrimaryMatch[*A]);
    std::vector<uint32_t> Ordered(Again.begin(), Again.end());
    llvm::sort(Ordered);
    for (size_t N = 0; N < Ordered.size(); ++N) {
      Control.report("measure", N, Ordered.size());
      const uint32_t I = Ordered[N];
      FunctionMatch &M = R.Matches[I];
      const bool IsNew = E.Added && M.Primary == *A;
      if (!IsNew)
        E.Remeasured.push_back(M);
      measure(M, compareFunctionsWithKeys(Primary, M.Primary, Secondary,
                                          M.Secondary, {PMap, SMap}, PK, SK,
                                          Control));
      Control.check();
      if (IsNew)
        E.Added = M;
    }
    Control.report("measure", Ordered.size(), Ordered.size());
    if (!ChangedP.empty())
      refreshCallRegions(R, Primary, Secondary, Control);
  }
  R.Overrides.push_back(O);
  summarize(R, Primary, Secondary);
  return std::nullopt;
}

void revertOverrideInPlace(ComparisonResult &R, const Program &Primary,
                           const Program &Secondary, const OverrideEffect &E,
                           const ComparisonControl &Control) {
  Control.report("mapping");
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
        Control.check();
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
    reindex(R, Primary.Functions.size(), Secondary.Functions.size(), Control);
    for (const FunctionMatch &Before : E.Remeasured)
      if (const uint32_t I = R.PrimaryMatch[Before.Primary]; I != NoIndex)
        R.Matches[I] = Before;
    refreshCallRegions(R, Primary, Secondary, Control);
  }
  if (!R.Overrides.empty() && R.Overrides.back() == E.Applied)
    R.Overrides.pop_back();
  summarize(R, Primary, Secondary);
}

} // namespace

std::optional<std::string> applyOverride(ComparisonResult &R,
                                         const Program &Primary,
                                         const Program &Secondary,
                                         const Override &O,
                                         OverrideEffect *Effect) {
  return applyOverride(R, Primary, Secondary, O, Effect, {});
}

std::optional<std::string>
applyOverride(ComparisonResult &R, const Program &Primary,
              const Program &Secondary, const Override &O,
              OverrideEffect *Effect, const ComparisonControl &Control) {
  Control.report("prepare");
  ComparisonResult Next = R;
  OverrideEffect NextEffect;
  if (auto Error = applyOverrideInPlace(Next, Primary, Secondary, O, NextEffect,
                                        Control))
    return Error;
  Control.report("publish", 0, 1);
  R = std::move(Next);
  if (Effect)
    *Effect = std::move(NextEffect);
  return std::nullopt;
}

void revertOverride(ComparisonResult &R, const Program &Primary,
                    const Program &Secondary, const OverrideEffect &E) {
  revertOverride(R, Primary, Secondary, E, {});
}

void revertOverride(ComparisonResult &R, const Program &Primary,
                    const Program &Secondary, const OverrideEffect &E,
                    const ComparisonControl &Control) {
  Control.report("prepare");
  ComparisonResult Next = R;
  revertOverrideInPlace(Next, Primary, Secondary, E, Control);
  Control.report("publish", 0, 1);
  R = std::move(Next);
}

} // namespace neverd::diff
