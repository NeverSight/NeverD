//===- FlowRegions.cpp - Indexed finite SSA region hypotheses -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse bounded per-function fingerprints, verify nominated neighborhoods,
/// then group overlapping correspondences without asserting equivalence.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowRegions.h"

#include "neverd/diff/FlowFragments.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
llvm::StringRef flowRegionProfile(llvm::StringRef GraphProfile,
                                  bool Fragments) {
  if (GraphProfile == LLVMInvariantBoundProgressDataFlowProfile)
    return Fragments ? LLVMInvariantBoundProgressFlowFragmentProfile
                     : LLVMInvariantBoundProgressFlowRegionProfile;
  if (GraphProfile == LLVMParameterBoundProgressDataFlowProfile)
    return Fragments ? LLVMParameterBoundProgressFlowFragmentProfile
                     : LLVMParameterBoundProgressFlowRegionProfile;
  if (GraphProfile == LLVMModularProgressDataFlowProfile)
    return Fragments ? LLVMModularProgressFlowFragmentProfile
                     : LLVMModularProgressFlowRegionProfile;
  if (GraphProfile == LLVMBranchProgressDataFlowProfile)
    return Fragments ? LLVMBranchProgressFlowFragmentProfile
                     : LLVMBranchProgressFlowRegionProfile;
  if (GraphProfile == LLVMNestedProgressDataFlowProfile)
    return Fragments ? LLVMNestedProgressFlowFragmentProfile
                     : LLVMNestedProgressFlowRegionProfile;
  if (GraphProfile == LLVMMultiBlockProgressDataFlowProfile)
    return Fragments ? LLVMMultiBlockProgressFlowFragmentProfile
                     : LLVMMultiBlockProgressFlowRegionProfile;
  if (GraphProfile == LLVMProgressDataFlowProfile)
    return Fragments ? LLVMProgressFlowFragmentProfile
                     : LLVMProgressFlowRegionProfile;
  if (GraphProfile == LLVMParameterDataFlowProfile)
    return Fragments ? LLVMParameterFlowFragmentProfile
                     : LLVMParameterFlowRegionProfile;
  if (GraphProfile == LLVMDataFlowProfile)
    return Fragments ? LLVMFlowFragmentProfile : LLVMFlowRegionProfile;
  if (GraphProfile == DataFlowProfile)
    return Fragments ? FlowFragmentProfile : FlowRegionProfile;
  throw std::invalid_argument("unsupported SSA region graph profile");
}
namespace {
namespace limits = neverd::limits::diff;
struct Interrupted {};
class Budget {
  FlowRegionAnalysis &Result;
  const FlowRegionSettings &Settings;
  const ComparisonControl &Control;
  std::string Phase;

public:
  Budget(FlowRegionAnalysis &R, const FlowRegionSettings &S,
         const ComparisonControl &C)
      : Result(R), Settings(S), Control(C) {}
  [[noreturn]] void stop(ComparisonState State) {
    Result.State = State;
    Result.StoppedPhase = Phase;
    throw Interrupted{};
  }
  void poll() {
    if (auto State = Control.stop())
      stop(*State);
  }
  void phase(llvm::StringRef Name) {
    Phase = Name.str();
    if (Control.Progress)
      Control.Progress(Phase, 0, 0);
    poll();
  }
  uint64_t remaining() const { return Settings.MaxWork - Result.Work; }
  void charge(uint64_t Amount = 1) {
    if (Amount > remaining())
      stop(ComparisonState::ResourceLimited);
    const auto Before = Result.Work;
    Result.Work += Amount;
    if (Before / limits::kFlowPollWork != Result.Work / limits::kFlowPollWork)
      poll();
  }
  DataFlowOptions allowance(const DataFlowOptions &Base,
                            uint64_t ValidationWork) {
    // The graph routines validate before running. Account for that traversal
    // and avoid presenting a smaller-than-input allowance as malformed input.
    charge(ValidationWork);
    auto Options = Base;
    Options.MaxWork = std::min(Options.MaxWork, remaining());
    if (!Options.MaxWork || Options.MaxWork < ValidationWork)
      stop(ComparisonState::ResourceLimited);
    return Options;
  }
};

struct IndexedGraph {
  const DataFlowGraph *Graph;
  FlowFingerprints Keys;
  uint64_t ValidationWork;
  FlowFragmentFingerprints Fragments;
};
struct Seed {
  uint64_t Key;
  uint32_t Graph, Node;
  unsigned Depth = 0;
};
auto seedKey(const Seed &S) { return std::pair{-int(S.Depth), S.Key}; }
struct Index {
  std::vector<IndexedGraph> Graphs;
  std::vector<Seed> Seeds;
};
bool eligible(const FlowNode &N) {
  return N.hasNativeOrigin() && N.ResultBytes && N.Opcode != "COPY" &&
         N.Opcode != "INT_ZEXT" && N.Opcode != "INT_SEXT" &&
         N.Opcode != "SUBBYTES" && N.Opcode != "CONCAT" && N.Opcode != "zext" &&
         N.Opcode != "sext" && N.Opcode != "trunc";
}
Index index(FlowRegionInput Input, const DataFlowSettings &EvidenceSettings,
            const FlowRegionSettings &Settings, FlowRegionAnalysis &R,
            Budget &Work, const ComparisonControl &Control, uint64_t &Count,
            bool &Complete) {
  Index Out;
  const auto &P = Input.Native;
  bool Covered = P.Identity.DiscoveryComplete &&
                 Input.Evidence->State == ComparisonState::Complete;
  for (const auto &Entry : Input.Evidence->Entries) {
    Work.poll();
    Work.charge();
    if (!Entry.Graph) {
      Covered = false;
      continue;
    }
    const auto &G = *Entry.Graph;
    uint64_t ValidationWork = G.Nodes.size() + G.Blocks.size();
    for (const auto &B : G.Blocks) {
      Work.charge();
      ValidationWork += B.Successors.size();
    }
    for (const auto &N : G.Nodes) {
      const auto Extra = flowExtraWork(N);
      Work.charge(1 + Extra);
      ValidationWork += N.Inputs.size() + Extra;
      for (const auto &I : N.Inputs)
        if (I.Predecessor != NoIndex)
          ValidationWork += G.Blocks[I.Predecessor].Successors.size();
    }
    const auto Options = Work.allowance(EvidenceSettings.Graph, ValidationWork);
    FlowFingerprints Keys;
    FlowFragmentFingerprints Fragments;
    if (Settings.Fragments)
      Fragments = fingerprintFlowFragments(G, Options, Control);
    else
      Keys = fingerprintDataFlow(G, Options, Control);
    const auto State = Settings.Fragments ? Fragments.State : Keys.State;
    Work.charge(Settings.Fragments ? Fragments.Work : Keys.Work);
    if (State != ComparisonState::Complete) {
      if (State != ComparisonState::ResourceLimited ||
          Options.MaxWork < EvidenceSettings.Graph.MaxWork)
        Work.stop(State);
      ++R.LimitedGraphs;
      Covered = false;
      continue;
    }
    const auto GI = uint32_t(Out.Graphs.size());
    Out.Graphs.push_back(
        {&G, std::move(Keys), ValidationWork, std::move(Fragments)});
    if (Settings.Fragments) {
      const auto &Layers = Out.Graphs.back().Fragments.Layers;
      for (unsigned Depth = Layers.size(); Depth; --Depth)
        for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
          Work.charge();
          if (!Layers[Depth - 1].Admitted[N] || !G.Nodes[N].hasNativeOrigin())
            continue;
          if (Count == Settings.MaxSeeds)
            Work.stop(ComparisonState::ResourceLimited);
          Out.Seeds.push_back({Layers[Depth - 1].Keys[N], GI, N, Depth});
          ++Count;
        }
      continue;
    }
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Work.charge();
      if (!eligible(G.Nodes[N]))
        continue;
      if (Count == Settings.MaxSeeds)
        Work.stop(ComparisonState::ResourceLimited);
      Out.Seeds.push_back({Out.Graphs.back().Keys.Keys[N], GI, N});
      ++Count;
    }
  }
  std::sort(Out.Seeds.begin(), Out.Seeds.end(),
            [&](const auto &A, const auto &B) {
              Work.charge();
              return std::tuple{seedKey(A), A.Graph, A.Node} <
                     std::tuple{seedKey(B), B.Graph, B.Node};
            });
  Complete = Covered;
  return Out;
}

void anchors(const Index &A, const Index &B, const DataFlowSettings &Evidence,
             const FlowRegionSettings &Settings, FlowRegionAnalysis &R,
             Budget &Work, const ComparisonControl &Control) {
  std::vector<FlowMatchInput> PreparedA, PreparedB;
  std::vector<FlowFragmentInput> FragmentsA, FragmentsB;
  try {
    if (Settings.Fragments) {
      for (const auto &[From, To] :
           {std::pair{&A, &FragmentsA}, {&B, &FragmentsB}}) {
        To->reserve(From->Graphs.size());
        for (const auto &G : From->Graphs) {
          Work.charge(G.ValidationWork + G.Fragments.Layers.size());
          To->emplace_back(*G.Graph, G.Fragments, Evidence.Graph, Control);
        }
      }
    } else {
      for (const auto &[From, To] :
           {std::pair{&A, &PreparedA}, {&B, &PreparedB}}) {
        To->reserve(From->Graphs.size());
        for (const auto &G : From->Graphs) {
          Work.charge(G.ValidationWork);
          To->emplace_back(*G.Graph, G.Keys, Evidence.Graph, Control);
        }
      }
    }
  } catch (const ComparisonInterrupted &Stopped) {
    Work.stop(Stopped.State);
  }
  std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> Accepted;
  size_t I = 0, J = 0;
  uint64_t StoredPairs = 0;
  while (I < A.Seeds.size() && J < B.Seeds.size()) {
    Work.charge();
    if (seedKey(A.Seeds[I]) < seedKey(B.Seeds[J])) {
      ++I;
      continue;
    }
    if (seedKey(B.Seeds[J]) < seedKey(A.Seeds[I])) {
      ++J;
      continue;
    }
    auto IE = I + 1, JE = J + 1;
    while (IE < A.Seeds.size() && seedKey(A.Seeds[IE]) == seedKey(A.Seeds[I])) {
      Work.charge();
      ++IE;
    }
    while (JE < B.Seeds.size() && seedKey(B.Seeds[JE]) == seedKey(B.Seeds[J])) {
      Work.charge();
      ++JE;
    }
    if (IE - I > Settings.MaxOccurrences || JE - J > Settings.MaxOccurrences) {
      ++R.FrequentSeedGroups;
      I = IE;
      J = JE;
      continue;
    }
    for (size_t X = I; X < IE; ++X)
      for (size_t Y = J; Y < JE; ++Y) {
        const auto &AS = A.Seeds[X], &BS = B.Seeds[Y];
        const auto &AG = A.Graphs[AS.Graph], &BG = B.Graphs[BS.Graph];
        const auto RootPair = std::tuple{AS.Graph, AS.Node, BS.Graph, BS.Node};
        if (Settings.Fragments) {
          Work.charge();
          if (Accepted.contains(RootPair))
            continue;
        }
        auto Match =
            Settings.Fragments
                ? matchFlowFragment(
                      FragmentsA[AS.Graph], AS.Node, FragmentsB[BS.Graph],
                      BS.Node, AS.Depth,
                      std::min(Evidence.Graph.MaxWork, Work.remaining()),
                      Control)
                : matchDataFlowNeighborhood(
                      PreparedA[AS.Graph], AS.Node, PreparedB[BS.Graph],
                      BS.Node,
                      std::min(Evidence.Graph.MaxWork, Work.remaining()),
                      Control);
        Work.charge(Match.Work);
        if (Match.State != ComparisonState::Complete)
          Work.stop(Match.State);
        if (Match.Outcome == FlowMatchOutcome::Unsupported)
          throw std::invalid_argument(Match.Reason);
        if (Match.Outcome != FlowMatchOutcome::Matched) {
          if (Match.Outcome == FlowMatchOutcome::Ambiguous)
            ++R.AmbiguousCandidates;
          else
            ++R.DifferentCandidates;
          continue;
        }
        std::set<uint64_t> OriginsA, OriginsB;
        for (const auto &[NA, NB] : Match.Nodes) {
          Work.charge();
          const auto &AN = AG.Graph->Nodes[NA], &BN = BG.Graph->Nodes[NB];
          Work.charge(AN.Origins.size() + BN.Origins.size());
          AN.forEachOrigin(
              [&](const FlowOrigin &O) { OriginsA.insert(O.Address); });
          BN.forEachOrigin(
              [&](const FlowOrigin &O) { OriginsB.insert(O.Address); });
        }
        if (Match.Nodes.size() < Settings.MinimumNodes ||
            OriginsA.size() < Settings.MinimumOrigins ||
            OriginsB.size() < Settings.MinimumOrigins) {
          ++R.WeakCandidates;
          continue;
        }
        const uint64_t Pairs =
            Match.Nodes.size() + Match.PhiBlocks.size() + Match.Frontier.size();
        if (R.Anchors.size() == Settings.MaxAnchors ||
            Pairs > Settings.MaxNodePairs - StoredPairs)
          Work.stop(ComparisonState::ResourceLimited);
        FlowRegionAnchor Anchor;
        Anchor.PrimaryFunction = AG.Graph->Function;
        Anchor.SecondaryFunction = BG.Graph->Function;
        Anchor.PrimaryRoot = AS.Node;
        Anchor.SecondaryRoot = BS.Node;
        Anchor.Id = "ssa-" + llvm::utohexstr(Anchor.PrimaryFunction, true) +
                    "-" + std::to_string(AS.Node) + "-" +
                    llvm::utohexstr(Anchor.SecondaryFunction, true) + "-" +
                    std::to_string(BS.Node);
        Anchor.Nodes = std::move(Match.Nodes);
        Anchor.PhiBlocks = std::move(Match.PhiBlocks);
        if (Settings.Fragments) {
          Work.charge();
          Anchor.Depth = AS.Depth;
          Anchor.Frontier = std::move(Match.Frontier);
          Anchor.Id = "fragment-" + std::to_string(AS.Depth) + "-" + Anchor.Id;
          Accepted.insert(RootPair);
        }
        Anchor.Gaps = Match.Gaps;
        Anchor.OpenDataFrontier = Match.OpenDataFrontier;
        Anchor.OpenControlFrontier = Match.OpenControlFrontier;
        Anchor.UniqueSeed = IE - I == 1 && JE - J == 1;
        Anchor.PrimaryOrigins = OriginsA.size();
        Anchor.SecondaryOrigins = OriginsB.size();
        R.Anchors.push_back(std::move(Anchor));
        StoredPairs += Pairs;
      }
    I = IE;
    J = JE;
  }
}

bool internalCall(const Program &P, const std::vector<uint64_t> &Functions,
                  Budget &Work) {
  if (Functions.size() < 2)
    return false;
  for (auto Address : Functions)
    for (const auto &B : P.blocks(P.Functions[*P.functionAt(Address)]))
      for (const auto &I : P.instructions(B)) {
        Work.charge();
        if (I.Callee != NoIndex && P.Functions[I.Callee].Address != Address &&
            std::binary_search(Functions.begin(), Functions.end(),
                               P.Functions[I.Callee].Address))
          return true;
      }
  return false;
}
FlowRegionHypothesis summarize(FlowRegionInput Primary,
                               FlowRegionInput Secondary,
                               const FlowRegionAnalysis &R,
                               std::vector<uint32_t> Members, Budget &Work) {
  FlowRegionHypothesis H;
  const auto &P = Primary.Native, &S = Secondary.Native;
  H.Anchors = std::move(Members);
  H.Id = "flow-region-" + R.Anchors[H.Anchors.front()].Id;
  std::set<uint64_t> PF, SF;
  using Node = std::pair<uint64_t, uint32_t>;
  std::map<Node, Node> Forward, Reverse;
  std::map<Node, Node> BoundForward, BoundReverse;
  bool Copies = false;
  bool BindingConflict = false;
  for (auto N : H.Anchors) {
    Work.charge();
    const auto &A = R.Anchors[N];
    PF.insert(A.PrimaryFunction);
    SF.insert(A.SecondaryFunction);
    H.Gaps |= A.Gaps;
    H.OpenDataFrontier |= A.OpenDataFrontier;
    H.OpenControlFrontier |= A.OpenControlFrontier;
    for (const auto &[X, Y] : A.Nodes) {
      Work.charge();
      const Node PN{A.PrimaryFunction, X}, SN{A.SecondaryFunction, Y};
      const auto [FI, NewF] = Forward.emplace(PN, SN);
      const auto [RI, NewR] = Reverse.emplace(SN, PN);
      Copies |= (!NewF && FI->second != SN) || (!NewR && RI->second != PN);
    }
    if (A.Depth)
      for (const auto *Pairs : {&A.Nodes, &A.Frontier})
        for (const auto &[X, Y] : *Pairs) {
          Work.charge();
          const Node PN{A.PrimaryFunction, X}, SN{A.SecondaryFunction, Y};
          const auto [FI, NewF] = BoundForward.emplace(PN, SN);
          const auto [RI, NewR] = BoundReverse.emplace(SN, PN);
          BindingConflict |=
              (!NewF && FI->second != SN) || (!NewR && RI->second != PN);
        }
  }
  H.PrimaryFunctions.assign(PF.begin(), PF.end());
  H.SecondaryFunctions.assign(SF.begin(), SF.end());
  H.PrimaryNodes = Forward.size();
  H.SecondaryNodes = Reverse.size();
  const auto Total = [&](FlowRegionInput Input, const auto &Functions) {
    uint64_t Count = 0;
    for (auto F : Functions) {
      Work.charge();
      Count += Input.Evidence->find(F)->Graph->Nodes.size();
    }
    return Count;
  };
  H.PrimaryCoverage = double(H.PrimaryNodes) / Total(Primary, PF);
  H.SecondaryCoverage = double(H.SecondaryNodes) / Total(Secondary, SF);
  H.Ambiguous = Copies || BindingConflict || PF.size() > 1 || SF.size() > 1;
  RegionRelationshipFacts Facts;
  Facts.PrimaryFunctions = PF.size();
  Facts.SecondaryFunctions = SF.size();
  Facts.Copies = Copies;
  Facts.CallsWithinPrimary =
      SF.size() == 1 && internalCall(P, H.PrimaryFunctions, Work);
  Facts.CallsWithinSecondary =
      PF.size() == 1 && internalCall(S, H.SecondaryFunctions, Work);
  std::set<std::string> PM, SM;
  for (auto F : PF) {
    Work.charge();
    const auto &Module = P.Functions[*P.functionAt(F)].Module;
    if (!Module.empty())
      PM.insert(Module);
  }
  for (auto F : SF) {
    Work.charge();
    const auto &Module = S.Functions[*S.functionAt(F)].Module;
    if (!Module.empty())
      SM.insert(Module);
  }
  Facts.ModuleMovement = !PM.empty() && !SM.empty() && PM != SM;
  H.Explanations = regionExplanations(Facts);
  return H;
}
void groups(FlowRegionInput P, FlowRegionInput S, FlowRegionAnalysis &R,
            Budget &Work) {
  std::map<std::pair<bool, uint64_t>, uint32_t> Vertices;
  std::vector<uint32_t> Parent;
  const auto Vertex = [&](bool Secondary, uint64_t Address) {
    Work.charge();
    const auto [At, Added] =
        Vertices.try_emplace({Secondary, Address}, Parent.size());
    if (Added)
      Parent.push_back(Parent.size());
    return At->second;
  };
  const auto Root = [&](uint32_t N) {
    while (Parent[N] != N) {
      Work.charge();
      N = Parent[N] = Parent[Parent[N]];
    }
    return N;
  };
  for (const auto &A : R.Anchors) {
    const auto X = Root(Vertex(false, A.PrimaryFunction));
    const auto Y = Root(Vertex(true, A.SecondaryFunction));
    Parent[std::max(X, Y)] = std::min(X, Y);
  }
  std::map<uint32_t, std::vector<uint32_t>> Members;
  for (uint32_t I = 0; I < R.Anchors.size(); ++I) {
    Work.charge();
    Members[Root(Vertex(false, R.Anchors[I].PrimaryFunction))].push_back(I);
  }
  for (auto &[Owner, Set] : Members) {
    (void)Owner;
    Work.poll();
    if (Set.size() > limits::kMaxFlowRegionGroupAnchors) {
      ++R.SuppressedGroups;
      continue;
    }
    R.Hypotheses.push_back(summarize(P, S, R, std::move(Set), Work));
  }
}
} // namespace

std::optional<std::string> FlowRegionSettings::validate() const {
  if (!MinimumNodes || MinimumNodes > limits::kMaxFlowRegionMinNodes ||
      !MinimumOrigins || MinimumOrigins > limits::kMaxFlowRegionMinNodes ||
      !MaxOccurrences || MaxOccurrences > limits::kMaxFlowRegionOccurrences ||
      !MaxSeeds || MaxSeeds > limits::kMaxFlowRegionSeeds || !MaxWork ||
      MaxWork > limits::kMaxFlowRegionWork || !MaxAnchors ||
      MaxAnchors > limits::kMaxFlowRegionAnchors || !MaxNodePairs ||
      MaxNodePairs > limits::kMaxFlowRegionPairs)
    return "SSA region settings are outside the admitted allowances";
  return {};
}
std::string FlowRegionSettings::describe() const {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Out << FlowRegionProfile << ":enabled=" << Enabled
      << ",nodes=" << MinimumNodes << ",origins=" << MinimumOrigins
      << ",occurrences=" << MaxOccurrences << ",seeds=" << MaxSeeds
      << ",work=" << MaxWork << ",anchors=" << MaxAnchors
      << ",pairs=" << MaxNodePairs;
  if (Fragments)
    Out << ",fragments=1";
  return Text;
}
FlowRegionAnalysis findFlowRegions(FlowRegionInput P, FlowRegionInput S,
                                   const DataFlowSettings &Evidence,
                                   const FlowRegionSettings &Settings,
                                   const ComparisonControl &Control) {
  if (auto Error = Settings.validate())
    throw std::invalid_argument(*Error);
  FlowRegionAnalysis R;
  R.Profile = flowRegionProfile(Evidence.Profile, Settings.Fragments).str();
  if (!Settings.Enabled)
    return R;
  if (!Evidence.Enabled)
    throw std::invalid_argument("SSA regions require dependency evidence");
  try {
    for (const auto Input : {P, S}) {
      if (auto Error = Input.Native.validate(Control))
        throw std::invalid_argument(*Error);
      if (auto Error = validateDataFlowEvidence(Input.Evidence, Evidence,
                                                Input.Native, Control))
        throw std::invalid_argument(*Error);
    }
  } catch (const ComparisonInterrupted &Stopped) {
    R.State = Stopped.State;
    // Preparation belongs to the first indexing phase in the persisted
    // contract. Keep its existing spelling for older report readers.
    R.StoppedPhase = "flow-regions:index-primary";
    return R;
  }
  if (P.Native.Identity.Architecture != S.Native.Identity.Architecture ||
      P.Native.Identity.Bits != S.Native.Identity.Bits ||
      P.Native.Identity.Endian != S.Native.Identity.Endian ||
      P.Native.Identity.Normalization != S.Native.Identity.Normalization ||
      P.Native.Identity.ABI != S.Native.Identity.ABI)
    throw std::invalid_argument("SSA regions require the same admitted ISA, "
                                "ABI and normalization profile");
  R.State = ComparisonState::Complete;
  Budget Work(R, Settings, Control);
  try {
    Work.phase("flow-regions:index-primary");
    const auto A = index(P, Evidence, Settings, R, Work, Control,
                         R.PrimarySeeds, R.PrimaryScopeComplete);
    Work.phase("flow-regions:index-secondary");
    const auto B = index(S, Evidence, Settings, R, Work, Control,
                         R.SecondarySeeds, R.SecondaryScopeComplete);
    Work.phase("flow-regions:anchors");
    anchors(A, B, Evidence, Settings, R, Work, Control);
    Work.phase("flow-regions:groups");
    groups(P, S, R, Work);
    if (R.LimitedGraphs || R.SuppressedGroups)
      Work.stop(ComparisonState::ResourceLimited);
    Work.poll();
  } catch (const Interrupted &) {
    // Completed anchors remain inspectable even if grouping has not finished.
  }
  return R;
}
std::optional<std::string>
validateFlowRegions(const FlowRegionAnalysis &Saved, FlowRegionInput P,
                    FlowRegionInput S, const DataFlowSettings &Evidence,
                    const FlowRegionSettings &Settings,
                    const ComparisonControl &Control) {
  Control.check();
  FlowRegionAnalysis Empty;
  Empty.Profile = flowRegionProfile(Evidence.Profile, Settings.Fragments).str();
  if (Saved.Profile != Empty.Profile)
    return "saved SSA region profile disagrees with requested evidence";
  if (!Saved.State) {
    if (Saved != Empty)
      return "unstarted SSA region analysis contains evidence";
    return {};
  }
  if (!Settings.Enabled)
    return "SSA region evidence was not requested";
  if (!stateNamed(stateName(*Saved.State)))
    return "unknown SSA region state";
  if (Saved.Work > Settings.MaxWork)
    return "saved SSA region work exceeds its allowance";
  try {
    auto Fresh = findFlowRegions(P, S, Evidence, Settings, Control);
    if (Fresh.State == ComparisonState::Cancelled ||
        Fresh.State == ComparisonState::TimedOut)
      throw ComparisonInterrupted(*Fresh.State);
    Control.check();
    // Work is historical diagnostic bookkeeping. Cache improvements and host
    // sort implementations can change it without changing any evidence.
    Fresh.Work = Saved.Work;
    if (*Saved.State == ComparisonState::Complete ||
        *Saved.State == ComparisonState::ResourceLimited) {
      if (Saved != Fresh)
        return "saved SSA regions disagree with recomputed observations";
      return {};
    }
    const auto Prefix = [](const auto &Was, const auto &Now) {
      return Was.size() <= Now.size() &&
             std::equal(Was.begin(), Was.end(), Now.begin());
    };
    if (!Prefix(Saved.Anchors, Fresh.Anchors) ||
        !Prefix(Saved.Hypotheses, Fresh.Hypotheses) ||
        Saved.PrimarySeeds > Fresh.PrimarySeeds ||
        Saved.SecondarySeeds > Fresh.SecondarySeeds ||
        Saved.FrequentSeedGroups > Fresh.FrequentSeedGroups ||
        Saved.WeakCandidates > Fresh.WeakCandidates ||
        Saved.AmbiguousCandidates > Fresh.AmbiguousCandidates ||
        Saved.DifferentCandidates > Fresh.DifferentCandidates ||
        Saved.LimitedGraphs > Fresh.LimitedGraphs ||
        Saved.SuppressedGroups > Fresh.SuppressedGroups ||
        (Saved.PrimaryScopeComplete && !Fresh.PrimaryScopeComplete) ||
        (Saved.SecondaryScopeComplete && !Fresh.SecondaryScopeComplete))
      return "stopped SSA region evidence is not a verified completed prefix";
  } catch (const ComparisonInterrupted &) {
    throw;
  } catch (const std::exception &E) {
    return E.what();
  }
  return {};
}
} // namespace neverd::diff
