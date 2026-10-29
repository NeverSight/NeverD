//===- Regions.cpp - Bounded native region hypotheses --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rare exact seeds grow into normalized instruction runs. Connected
/// components retain overlapping, many-to-many evidence without forcing a
/// function bijection or interpreting shared code as a semantic proof.
//===----------------------------------------------------------------------===//

#include "neverd/diff/Regions.h"

#include "neverd/diff/Comparison.h"
#include "neverd/diff/StableHash.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
constexpr llvm::StringLiteral KindNames[] = {
    "shared_region", "split",      "merge",       "inline",        "outline",
    "clone",         "tail_merge", "module_move", "redistribution"};

struct Interrupted {};
class Budget {
  RegionAnalysis &Result;
  const RegionSettings &Settings;
  const ComparisonControl &Control;
  std::string Phase;

public:
  Budget(RegionAnalysis &R, const RegionSettings &S, const ComparisonControl &C)
      : Result(R), Settings(S), Control(C) {}
  void phase(llvm::StringRef Name) {
    Phase = Name.str();
    if (Control.Progress)
      Control.Progress(Phase, 0, 0);
    poll();
  }
  void poll() {
    if (const auto Stop = Control.stop()) {
      Result.State = *Stop;
      Result.StoppedPhase = Phase;
      throw Interrupted{};
    }
  }
  [[noreturn]] void exhausted() {
    Result.State = ComparisonState::ResourceLimited;
    Result.StoppedPhase = Phase;
    throw Interrupted{};
  }
  void charge() {
    if (Result.Work == Settings.MaxWork)
      exhausted();
    ++Result.Work;
    if (Result.Work % limits::kRegionPollWork == 0)
      poll();
  }
};

struct Seed {
  uint64_t Key;
  uint32_t Function, Block, Instruction, Begin, End;
};
/// Canonical call targets in the secondary inventory. Build once per search;
/// an unpaired target is a barrier, never a wildcard or a numeric RVA match.
struct CallContext {
  std::vector<uint32_t> Primary, Secondary;

  CallContext(const Program &P, const Program &S, bool Enabled,
              llvm::ArrayRef<FunctionMatch> Matches, Budget &Work) {
    if (!Enabled)
      return;
    Work.phase("regions:call-context");
    for (size_t I = 0; I < P.Functions.size() + S.Functions.size(); ++I)
      Work.charge();
    Primary.assign(P.Functions.size(), NoIndex);
    Secondary.assign(S.Functions.size(), NoIndex);
    for (const auto &M : Matches) {
      Work.charge();
      if (M.Primary >= Primary.size() || M.Secondary >= Secondary.size() ||
          Primary[M.Primary] != NoIndex || Secondary[M.Secondary] != NoIndex)
        throw std::invalid_argument("region call context needs a valid "
                                    "one-to-one function correspondence");
      Primary[M.Primary] = Secondary[M.Secondary] = M.Secondary;
    }
  }
};
bool eligible(const Instruction &I, llvm::ArrayRef<uint32_t> Calls) {
  return I.Flow == InstructionFlow::None ||
         (I.Flow == InstructionFlow::Call && I.Callee != NoIndex &&
          !I.UnresolvedCallee && !I.HasUnresolvedData &&
          !I.AddressReferenceCount && !Calls.empty() &&
          Calls[I.Callee] != NoIndex);
}
uint64_t instructionKey(const Instruction &I, llvm::ArrayRef<uint32_t> Calls) {
  return I.Flow == InstructionFlow::Call
             ? StableHash().add(I.Shape).add(Calls[I.Callee]).value()
             : I.Shape;
}
bool equal(const Program &P, uint32_t A, const Program &S, uint32_t B,
           const CallContext &Calls) {
  const auto &X = P.Instructions[A], &Y = S.Instructions[B];
  return eligible(X, Calls.Primary) && eligible(Y, Calls.Secondary) &&
         X.Flow == Y.Flow && X.Shape == Y.Shape &&
         P.mnemonic(X) == S.mnemonic(Y) &&
         (X.Flow != InstructionFlow::Call ||
          Calls.Primary[X.Callee] == Calls.Secondary[Y.Callee]);
}

std::vector<Seed> index(const Program &P, const RegionSettings &Settings,
                        Budget &Work, uint64_t &Count, uint64_t &Instructions,
                        std::vector<uint32_t> &FunctionInstructions,
                        llvm::ArrayRef<uint32_t> Calls) {
  FunctionInstructions.assign(P.Functions.size(), 0);
  std::vector<Seed> Seeds;
  for (uint32_t F = 0; F < P.Functions.size(); ++F) {
    Work.charge();
    const auto Blocks = P.blocks(P.Functions[F]);
    for (uint32_t B = 0; B < Blocks.size(); ++B) {
      const auto &Block = Blocks[B];
      const uint32_t End = Block.FirstInstruction + Block.InstructionCount;
      for (uint32_t I = Block.FirstInstruction; I < End;) {
        Work.charge();
        if (!eligible(P.Instructions[I], Calls)) {
          ++I;
          continue;
        }
        const auto Begin = I;
        while (I < End && eligible(P.Instructions[I], Calls)) {
          Work.charge();
          ++I;
          ++Instructions;
          ++FunctionInstructions[F];
        }
        if (I - Begin < Settings.MinimumInstructions)
          continue;
        for (uint32_t At = Begin; I - At >= limits::kRegionSeedInstructions;
             ++At) {
          Work.charge();
          if (Seeds.size() == Settings.MaxSeeds)
            Work.exhausted();
          StableHash H;
          for (unsigned K = 0; K < limits::kRegionSeedInstructions; ++K)
            H.add(instructionKey(P.Instructions[At + K], Calls));
          Seeds.push_back({H.value(), F, B, At, Begin, I});
          ++Count;
        }
      }
    }
  }
  std::sort(Seeds.begin(), Seeds.end(), [&](const auto &A, const auto &B) {
    Work.charge();
    return std::tie(A.Key, A.Function, A.Block, A.Instruction) <
           std::tie(B.Key, B.Function, B.Block, B.Instruction);
  });
  return Seeds;
}

using Alignment = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, int64_t>;
bool tail(const Program &P, const RegionSpan &Span) {
  const auto Insns =
      P.instructions(P.blocks(P.Functions[Span.Function])[Span.Block]);
  const auto End = Span.FirstInstruction + Span.InstructionCount;
  return End + 1 == Insns.size() &&
         (Insns[End].Flow == InstructionFlow::Return ||
          Insns[End].Flow == InstructionFlow::Jump);
}
Alignment alignment(const RegionAnchor &A) {
  return {A.Primary.Function, A.Primary.Block, A.Secondary.Function,
          A.Secondary.Block,
          int64_t(A.Primary.FirstInstruction) -
              int64_t(A.Secondary.FirstInstruction)};
}
void normalizeAnchors(RegionAnalysis &R, Budget &Work) {
  std::sort(R.Anchors.begin(), R.Anchors.end(),
            [&](const auto &A, const auto &B) {
              Work.charge();
              return std::pair{alignment(A), A.Primary.FirstInstruction} <
                     std::pair{alignment(B), B.Primary.FirstInstruction};
            });
  size_t Out = 0;
  for (const auto &A : R.Anchors) {
    Work.charge();
    if (Out && alignment(R.Anchors[Out - 1]) == alignment(A)) {
      auto &Last = R.Anchors[Out - 1];
      const auto End =
          Last.Primary.FirstInstruction + Last.Primary.InstructionCount;
      if (A.Primary.FirstInstruction <= End) {
        Last.Primary.InstructionCount = Last.Secondary.InstructionCount =
            std::max(End,
                     A.Primary.FirstInstruction + A.Primary.InstructionCount) -
            Last.Primary.FirstInstruction;
        Last.UniqueSeed |= A.UniqueSeed;
        continue;
      }
    }
    R.Anchors[Out++] = A;
  }
  R.Anchors.resize(Out);
}

void anchors(const Program &P, const Program &S, const std::vector<Seed> &A,
             const std::vector<Seed> &B, const RegionSettings &Settings,
             RegionAnalysis &R, Budget &Work, llvm::ArrayRef<uint32_t> PCount,
             llvm::ArrayRef<uint32_t> SCount, const CallContext &Calls) {
  // An interval index per alignment prevents every overlapping seed from
  // re-extending an already checked long run. Its size is bounded by anchors.
  std::map<Alignment, std::map<uint32_t, uint32_t>> Covered;
  size_t I = 0, J = 0;
  while (I < A.size() && J < B.size()) {
    Work.charge();
    if (A[I].Key < B[J].Key) {
      ++I;
      continue;
    }
    if (B[J].Key < A[I].Key) {
      ++J;
      continue;
    }
    size_t AE = I + 1, BE = J + 1;
    while (AE < A.size() && A[AE].Key == A[I].Key) {
      Work.charge();
      ++AE;
    }
    while (BE < B.size() && B[BE].Key == B[J].Key) {
      Work.charge();
      ++BE;
    }
    if (AE - I > Settings.MaxOccurrences || BE - J > Settings.MaxOccurrences) {
      ++R.FrequentSeedGroups;
      I = AE;
      J = BE;
      continue;
    }
    for (size_t X = I; X < AE; ++X)
      for (size_t Y = J; Y < BE; ++Y) {
        Work.charge();
        const auto &PA = A[X], &SA = B[Y];
        const Alignment Key{PA.Function, PA.Block, SA.Function, SA.Block,
                            int64_t(PA.Instruction) - int64_t(SA.Instruction)};
        auto Found = Covered.find(Key);
        if (Found != Covered.end()) {
          auto At = Found->second.upper_bound(PA.Instruction);
          if (At != Found->second.begin() &&
              std::prev(At)->second >=
                  PA.Instruction + limits::kRegionSeedInstructions)
            continue;
        }
        uint32_t Left = 0, Right = 0;
        while (PA.Instruction + Right < PA.End &&
               SA.Instruction + Right < SA.End &&
               Right < limits::kRegionExtensionInstructions) {
          Work.charge();
          if (!equal(P, PA.Instruction + Right, S, SA.Instruction + Right,
                     Calls))
            break;
          ++Right;
        }
        if (Right < limits::kRegionSeedInstructions)
          continue;
        while (Left < PA.Instruction - PA.Begin &&
               Left < SA.Instruction - SA.Begin &&
               Left + Right < limits::kRegionExtensionInstructions) {
          Work.charge();
          if (!equal(P, PA.Instruction - Left - 1, S, SA.Instruction - Left - 1,
                     Calls))
            break;
          ++Left;
        }
        if (Left + Right < Settings.MinimumInstructions)
          continue;
        const bool UniqueSeed = AE - I == 1 && BE - J == 1;
        const auto PB =
            P.blocks(P.Functions[PA.Function])[PA.Block].FirstInstruction;
        const auto SB =
            S.blocks(S.Functions[SA.Function])[SA.Block].FirstInstruction;
        const RegionAnchor Candidate{
            {PA.Function, PA.Block, PA.Instruction - Left - PB, Left + Right},
            {SA.Function, SA.Block, SA.Instruction - Left - SB, Left + Right},
            UniqueSeed};
        if (!UniqueSeed &&
            Left + Right < Settings.MinimumRepeatedInstructions &&
            Left + Right < std::min(PCount[PA.Function], SCount[SA.Function]) *
                               limits::kRegionMinShortRunFunctionCoverage) {
          // Several distinct, paired targets supply additional context for
          // short call-heavy runs. A terminal cleanup run can use one paired
          // target. Calls to one target inside a block lack that support. The
          // same occurrence bounds, exact checks and clone ambiguity apply.
          llvm::SmallVector<uint32_t, limits::kRegionMinDistinctCallTargets>
              Targets;
          if (Settings.Calls)
            for (uint32_t At = PA.Instruction - Left;
                 At < PA.Instruction + Right; ++At) {
              Work.charge();
              const auto &Insn = P.Instructions[At];
              if (Insn.Flow == InstructionFlow::Call &&
                  !llvm::is_contained(Targets, Insn.Callee))
                Targets.push_back(Insn.Callee);
              if (Targets.size() == limits::kRegionMinDistinctCallTargets)
                break;
            }
          const bool TerminalCall =
              Targets.size() >= limits::kRegionMinTerminalCallTargets &&
              tail(P, Candidate.Primary) && tail(S, Candidate.Secondary);
          if (Targets.size() < limits::kRegionMinDistinctCallTargets &&
              !TerminalCall) {
            ++R.WeakCandidates;
            continue;
          }
        }
        if (R.Anchors.size() == Settings.MaxAnchors)
          Work.exhausted();
        const uint32_t Begin = PA.Instruction - Left,
                       End = PA.Instruction + Right;
        auto &Ranges = Covered[Key];
        auto At = Ranges.lower_bound(Begin);
        uint32_t MergedBegin = Begin, MergedEnd = End;
        if (At != Ranges.begin() && std::prev(At)->second >= Begin)
          --At;
        while (At != Ranges.end() && At->first <= MergedEnd) {
          MergedBegin = std::min(MergedBegin, At->first);
          MergedEnd = std::max(MergedEnd, At->second);
          At = Ranges.erase(At);
        }
        Ranges[MergedBegin] = MergedEnd;
        R.Anchors.push_back(Candidate);
      }
    I = AE;
    J = BE;
  }
  normalizeAnchors(R, Work);
}

uint64_t covered(std::vector<std::pair<uint32_t, uint32_t>> Ranges) {
  std::sort(Ranges.begin(), Ranges.end());
  uint64_t Total = 0;
  uint32_t End = 0;
  for (const auto &[Begin, Stop] : Ranges) {
    if (Stop > End)
      Total += Stop - std::max(Begin, End);
    End = std::max(End, Stop);
  }
  return Total;
}
bool internalCall(const Program &P, const std::vector<uint32_t> &Functions,
                  Budget *Work) {
  for (const auto F : Functions)
    for (const auto &B : P.blocks(P.Functions[F]))
      for (const auto &I : P.instructions(B)) {
        if (Work)
          Work->charge();
        if (I.Callee != NoIndex && I.Callee != F &&
            std::binary_search(Functions.begin(), Functions.end(), I.Callee))
          return true;
      }
  return false;
}
RegionHypothesis summarize(const Program &P, const Program &S,
                           const RegionAnalysis &R,
                           std::vector<uint32_t> Members,
                           const CallContext &Calls, Budget *Work = nullptr) {
  RegionHypothesis H;
  H.AnalysisComplete =
      P.Identity.DiscoveryComplete && S.Identity.DiscoveryComplete;
  H.Anchors = std::move(Members);
  std::vector<std::pair<uint32_t, uint32_t>> PR, SR;
  std::set<uint32_t> PF, SF;
  uint64_t Sum = 0, Unique = 0;
  std::set<std::pair<uint32_t, uint32_t>> PrimaryTails;
  StableHash Id;
  Id.add(R.Calls ? regionProfile(true) : "native-regions-v1")
      .add(P.Identity.Revision)
      .add(S.Identity.Revision);
  for (uint32_t N : H.Anchors) {
    if (Work)
      Work->charge();
    const auto &A = R.Anchors[N];
    PF.insert(A.Primary.Function);
    SF.insert(A.Secondary.Function);
    const auto Begin = [](const Program &Input, const RegionSpan &Span) {
      return Input.blocks(Input.Functions[Span.Function])[Span.Block]
                 .FirstInstruction +
             Span.FirstInstruction;
    };
    const uint32_t PI = Begin(P, A.Primary), SI = Begin(S, A.Secondary);
    PR.emplace_back(PI, PI + A.Primary.InstructionCount);
    SR.emplace_back(SI, SI + A.Secondary.InstructionCount);
    Sum += A.Primary.InstructionCount;
    if (A.UniqueSeed)
      Unique += A.Primary.InstructionCount;
    if (tail(P, A.Primary) && tail(S, A.Secondary))
      PrimaryTails.emplace(A.Primary.Function, A.Primary.Block);
    Id.add(P.Functions[A.Primary.Function].Address)
        .add(P.Instructions[PI].Address)
        .add(S.Functions[A.Secondary.Function].Address)
        .add(S.Instructions[SI].Address)
        .add(A.Primary.InstructionCount);
  }
  H.Id = "region-" + llvm::utohexstr(Id.value(), true);
  H.PrimaryFunctions.assign(PF.begin(), PF.end());
  H.SecondaryFunctions.assign(SF.begin(), SF.end());
  H.PrimaryInstructions = covered(std::move(PR));
  H.SecondaryInstructions = covered(std::move(SR));
  const auto Eligible = [&](const Program &Input, const auto &Functions,
                            llvm::ArrayRef<uint32_t> Targets) {
    uint64_t Count = 0;
    for (auto F : Functions) {
      H.AnalysisComplete &= !Input.Functions[F].has(TraitIncomplete);
      for (const auto &B : Input.blocks(Input.Functions[F]))
        for (const auto &I : Input.instructions(B)) {
          if (Work)
            Work->charge();
          Count += eligible(I, Targets);
        }
    }
    return Count;
  };
  H.PrimaryCoverage =
      double(H.PrimaryInstructions) / Eligible(P, PF, Calls.Primary);
  H.SecondaryCoverage =
      double(H.SecondaryInstructions) / Eligible(S, SF, Calls.Secondary);
  const bool Copies =
      Sum > H.PrimaryInstructions || Sum > H.SecondaryInstructions;
  H.Ambiguous = Copies || PF.size() > 1 || SF.size() > 1;
  RegionRelationshipFacts Facts;
  Facts.PrimaryFunctions = PF.size();
  Facts.SecondaryFunctions = SF.size();
  Facts.Copies = Copies;
  Facts.CallsWithinPrimary =
      SF.size() == 1 && internalCall(P, H.PrimaryFunctions, Work);
  Facts.CallsWithinSecondary =
      PF.size() == 1 && internalCall(S, H.SecondaryFunctions, Work);
  Facts.MergedTail = PrimaryTails.size() > 1 && Sum > H.SecondaryInstructions;
  std::set<std::string> PM, SM;
  for (auto F : PF)
    if (!P.Functions[F].Module.empty())
      PM.insert(P.Functions[F].Module);
  for (auto F : SF)
    if (!S.Functions[F].Module.empty())
      SM.insert(S.Functions[F].Module);
  Facts.ModuleMovement = !PM.empty() && !SM.empty() && PM != SM;
  H.Explanations = regionExplanations(Facts);
  H.Confidence =
      std::min(limits::kRegionMaxConfidence,
               std::sqrt(H.PrimaryCoverage * H.SecondaryCoverage) *
                   (1 - limits::kRegionUniqueSeedWeight +
                    limits::kRegionUniqueSeedWeight * double(Unique) / Sum));
  if (!H.AnalysisComplete)
    H.Confidence *= limits::kRegionIncompleteConfidenceFactor;
  return H;
}

std::map<uint32_t, std::vector<uint32_t>> components(const Program &P,
                                                     const Program &S,
                                                     const RegionAnalysis &R,
                                                     Budget &Work) {
  std::vector<uint32_t> Parent(P.Functions.size() + S.Functions.size());
  std::iota(Parent.begin(), Parent.end(), 0);
  const auto Root = [&](uint32_t I) {
    while (Parent[I] != I) {
      Work.charge();
      I = Parent[I] = Parent[Parent[I]];
    }
    return I;
  };
  for (const auto &A : R.Anchors) {
    Work.charge();
    const auto X = Root(A.Primary.Function);
    const auto Y = Root(uint32_t(P.Functions.size()) + A.Secondary.Function);
    Parent[std::max(X, Y)] = std::min(X, Y);
  }
  std::map<uint32_t, std::vector<uint32_t>> Members;
  for (uint32_t I = 0; I < R.Anchors.size(); ++I) {
    Work.charge();
    Members[Root(R.Anchors[I].Primary.Function)].push_back(I);
  }
  return Members;
}
void groups(const Program &P, const Program &S, const RegionSettings &Settings,
            RegionAnalysis &R, Budget &Work, const CallContext &Calls) {
  auto Members = components(P, S, R, Work);
  for (auto &[Owner, Set] : Members) {
    (void)Owner;
    Work.poll();
    if (Set.size() > limits::kMaxRegionGroupAnchors) {
      ++R.SuppressedGroups;
      continue;
    }
    if (R.Hypotheses.size() == Settings.MaxHypotheses)
      Work.exhausted();
    R.Hypotheses.push_back(summarize(P, S, R, std::move(Set), Calls, &Work));
  }
}
} // namespace

llvm::StringRef regionKindName(RegionKind Kind) {
  return KindNames[size_t(Kind)];
}
std::vector<RegionKind> regionExplanations(const RegionRelationshipFacts &F) {
  if (!F.PrimaryFunctions || !F.SecondaryFunctions)
    throw std::invalid_argument(
        "region relationships need functions on both sides");
  std::vector<RegionKind> Out;
  Out.push_back(F.PrimaryFunctions == 1 && F.SecondaryFunctions == 1
                    ? RegionKind::Shared
                : F.PrimaryFunctions == 1   ? RegionKind::Split
                : F.SecondaryFunctions == 1 ? RegionKind::Merge
                                            : RegionKind::Redistribution);
  if (F.Copies)
    Out.push_back(RegionKind::Clone);
  if (F.SecondaryFunctions == 1 && F.CallsWithinPrimary)
    Out.push_back(RegionKind::Inline);
  if (F.PrimaryFunctions == 1 && F.CallsWithinSecondary)
    Out.push_back(RegionKind::Outline);
  if (F.MergedTail)
    Out.push_back(RegionKind::TailMerge);
  if (F.ModuleMovement)
    Out.push_back(RegionKind::ModuleMove);
  return Out;
}
std::optional<RegionKind> regionKindNamed(llvm::StringRef Name) {
  for (size_t I = 0; I < std::size(KindNames); ++I)
    if (KindNames[I] == Name)
      return RegionKind(I);
  return {};
}
std::string RegionSettings::describe() const {
  return std::to_string(Enabled) + ":" + std::to_string(MinimumInstructions) +
         ":" + std::to_string(MinimumRepeatedInstructions) + ":" +
         std::to_string(MaxOccurrences) + ":" + std::to_string(MaxSeeds) + ":" +
         std::to_string(MaxWork) + ":" + std::to_string(MaxAnchors) + ":" +
         std::to_string(MaxHypotheses) + (Calls ? ":calls" : "");
}
llvm::StringRef regionProfile(bool Calls) {
  return Calls ? "native-call-runs-v1" : "native-runs-v1";
}
std::optional<std::string> RegionSettings::validate() const {
  if (MinimumInstructions < limits::kRegionSeedInstructions ||
      MinimumInstructions > limits::kMaxRegionMinInstructions ||
      MinimumRepeatedInstructions < limits::kRegionSeedInstructions ||
      MinimumRepeatedInstructions > limits::kMaxRegionMinInstructions ||
      !MaxOccurrences || MaxOccurrences > limits::kMaxRegionOccurrences ||
      !MaxSeeds || MaxSeeds > limits::kMaxRegionSeeds || !MaxWork ||
      MaxWork > limits::kMaxRegionWork || !MaxAnchors ||
      MaxAnchors > limits::kMaxRegionAnchors || !MaxHypotheses ||
      MaxHypotheses > limits::kMaxRegionHypotheses)
    return "region settings are outside the supported work and storage "
           "allowances";
  return {};
}
RegionAnalysis findRegions(const Program &P, const Program &S,
                           const RegionSettings &Settings,
                           const ComparisonControl &Control,
                           llvm::ArrayRef<FunctionMatch> Matches) {
  if (auto Error = Settings.validate())
    throw std::invalid_argument(*Error);
  for (const auto *Input : {&P, &S})
    if (auto Error = Input->validate())
      throw std::invalid_argument(*Error);
  if (P.Identity.Architecture != S.Identity.Architecture ||
      P.Identity.Bits != S.Identity.Bits || P.Identity.ABI != S.Identity.ABI ||
      P.Identity.Endian != S.Identity.Endian ||
      P.Identity.Normalization != S.Identity.Normalization)
    throw std::invalid_argument("native regions require the same admitted ISA, "
                                "ABI and normalization profile");
  RegionAnalysis R;
  R.Calls = Settings.Calls;
  if (!Settings.Enabled)
    return R;
  R.State = ComparisonState::Complete;
  Budget Work(R, Settings, Control);
  try {
    const CallContext Calls(P, S, Settings.Calls, Matches, Work);
    std::vector<uint32_t> PCount, SCount;
    Work.phase("regions:index-primary");
    const auto A = index(P, Settings, Work, R.PrimarySeeds,
                         R.PrimaryEligibleInstructions, PCount, Calls.Primary);
    Work.phase("regions:index-secondary");
    const auto B =
        index(S, Settings, Work, R.SecondarySeeds,
              R.SecondaryEligibleInstructions, SCount, Calls.Secondary);
    Work.phase("regions:anchors");
    anchors(P, S, A, B, Settings, R, Work, PCount, SCount, Calls);
    Work.phase("regions:groups");
    groups(P, S, Settings, R, Work, Calls);
    Work.poll();
  } catch (const Interrupted &) {
    // Keep only completed anchors and hypotheses; the stop reason is separate
    // from their analysis coverage and never promotes them to proved mappings.
  }
  return R;
}

namespace {
/// Check positive uniqueness claims against the complete seed indexes. A
/// false flag is conservative: growth can absorb a unique window after its
/// first seed was found. Neither score nor saved membership proves uniqueness.
std::map<uint32_t, uint32_t> uniqueWindows(const Program &P, const Program &S,
                                           const RegionSettings &Settings,
                                           Budget &Work,
                                           const CallContext &Calls) {
  uint64_t Count = 0, Instructions = 0;
  std::vector<uint32_t> Eligible;
  const auto A =
      index(P, Settings, Work, Count, Instructions, Eligible, Calls.Primary);
  Count = Instructions = 0;
  const auto B =
      index(S, Settings, Work, Count, Instructions, Eligible, Calls.Secondary);
  std::map<uint32_t, uint32_t> Unique;
  size_t I = 0, J = 0;
  while (I < A.size() && J < B.size()) {
    Work.charge();
    if (A[I].Key < B[J].Key) {
      ++I;
      continue;
    }
    if (B[J].Key < A[I].Key) {
      ++J;
      continue;
    }
    const auto Key = A[I].Key;
    const auto BeginA = I, BeginB = J;
    do {
      Work.charge();
      ++I;
    } while (I < A.size() && A[I].Key == Key);
    do {
      Work.charge();
      ++J;
    } while (J < B.size() && B[J].Key == Key);
    if (I - BeginA == 1 && J - BeginB == 1)
      Unique.emplace(A[BeginA].Instruction, B[BeginB].Instruction);
  }
  return Unique;
}
} // namespace

std::optional<std::string>
validateRegions(const RegionAnalysis &R, const RegionSettings &Settings,
                const Program &P, const Program &S,
                const ComparisonControl &Control,
                llvm::ArrayRef<FunctionMatch> Matches) {
  Control.check();
  if (R.Calls != Settings.Calls)
    return "region profile disagrees with its settings";
  if (!R.State)
    return R.Anchors.empty() && R.Hypotheses.empty()
               ? std::optional<std::string>()
               : "unrequested regions contain evidence";
  if (!Settings.Enabled || R.Work > Settings.MaxWork ||
      R.PrimarySeeds > Settings.MaxSeeds ||
      R.SecondarySeeds > Settings.MaxSeeds ||
      R.Anchors.size() > Settings.MaxAnchors ||
      R.Hypotheses.size() > Settings.MaxHypotheses)
    return "region report exceeds its settings";
  RegionAnalysis Checked;
  Budget Work(Checked, Settings, Control);
  try {
    const CallContext Calls(P, S, Settings.Calls && !R.Anchors.empty(), Matches,
                            Work);
    std::map<uint32_t, uint32_t> Unique;
    if (llvm::any_of(R.Anchors, [](const auto &A) { return A.UniqueSeed; }))
      Unique = uniqueWindows(P, S, Settings, Work, Calls);
    const auto Span = [&](const Program &Input, const RegionSpan &A) {
      if (A.Function >= Input.Functions.size())
        return false;
      const auto &F = Input.Functions[A.Function];
      if (A.Block >= F.BlockCount)
        return false;
      const auto &B = Input.blocks(F)[A.Block];
      return A.InstructionCount >= Settings.MinimumInstructions &&
             uint64_t(A.FirstInstruction) + A.InstructionCount <=
                 B.InstructionCount;
    };
    for (const auto &A : R.Anchors) {
      Control.check();
      if (!Span(P, A.Primary) || !Span(S, A.Secondary) ||
          A.Primary.InstructionCount != A.Secondary.InstructionCount)
        return "region anchor has invalid snapshot coordinates";
      const auto PI = P.blocks(P.Functions[A.Primary.Function])[A.Primary.Block]
                          .FirstInstruction +
                      A.Primary.FirstInstruction;
      const auto SI =
          S.blocks(S.Functions[A.Secondary.Function])[A.Secondary.Block]
              .FirstInstruction +
          A.Secondary.FirstInstruction;
      bool HasUniqueWindow = false;
      for (uint32_t I = 0; I < A.Primary.InstructionCount; ++I) {
        Work.charge();
        if (!equal(P, PI + I, S, SI + I, Calls))
          return "region anchor does not have the stated normalized-run "
                 "evidence";
        if (A.Primary.InstructionCount - I >= limits::kRegionSeedInstructions) {
          const auto It = Unique.find(PI + I);
          HasUniqueWindow |= It != Unique.end() && It->second == SI + I;
        }
      }
      if (A.UniqueSeed && !HasUniqueWindow)
        return "region anchor has no unique seed in the input snapshots";
    }
    auto Components = components(P, S, R, Work);
    if (!R.Hypotheses.empty() || R.State == ComparisonState::Complete) {
      RegionAnalysis Normalized;
      Normalized.Anchors = R.Anchors;
      normalizeAnchors(Normalized, Work);
      if (Normalized.Anchors != R.Anchors)
        return "grouped region anchors are not in canonical nonoverlapping "
               "order";
    }
    std::vector<const std::vector<uint32_t> *> Membership(R.Anchors.size());
    for (const auto &[Root, Members] : Components) {
      (void)Root;
      for (auto I : Members) {
        Work.charge();
        Membership[I] = &Members;
      }
    }
    std::set<uint32_t> Used;
    for (const auto &H : R.Hypotheses) {
      Control.check();
      if (H.Anchors.empty() ||
          H.Anchors.size() > limits::kMaxRegionGroupAnchors ||
          !std::is_sorted(H.Anchors.begin(), H.Anchors.end()))
        return "region hypothesis has an invalid anchor inventory";
      for (auto N : H.Anchors)
        if (N >= R.Anchors.size() || !Used.insert(N).second)
          return "region hypothesis repeats or invents an anchor";
      if (H.Anchors != *Membership[H.Anchors.front()])
        return "region hypothesis does not describe a connected component";
      const auto Actual = summarize(P, S, R, H.Anchors, Calls, &Work);
      if (H.Id != Actual.Id || H.PrimaryFunctions != Actual.PrimaryFunctions ||
          H.SecondaryFunctions != Actual.SecondaryFunctions ||
          H.Explanations != Actual.Explanations ||
          H.PrimaryInstructions != Actual.PrimaryInstructions ||
          H.SecondaryInstructions != Actual.SecondaryInstructions ||
          H.PrimaryCoverage != Actual.PrimaryCoverage ||
          H.SecondaryCoverage != Actual.SecondaryCoverage ||
          H.Confidence != Actual.Confidence ||
          H.Ambiguous != Actual.Ambiguous ||
          H.AnalysisComplete != Actual.AnalysisComplete)
        return "region hypothesis disagrees with its snapshot evidence";
    }
    if (R.State == ComparisonState::Complete) {
      uint64_t Suppressed = 0;
      for (const auto &[Root, Members] : Components) {
        (void)Root;
        Work.charge();
        if (Members.size() > limits::kMaxRegionGroupAnchors)
          ++Suppressed;
        else if (!Used.contains(Members.front()))
          return "completed region report omits a connected component";
      }
      if (R.SuppressedGroups != Suppressed)
        return "region report has an inconsistent suppressed-group count";
    }
  } catch (const Interrupted &) {
    if (Checked.State == ComparisonState::Cancelled ||
        Checked.State == ComparisonState::TimedOut)
      throw ComparisonInterrupted(*Checked.State);
    return "region evidence validation exceeds its work allowance";
  }
  return {};
}
} // namespace neverd::diff
