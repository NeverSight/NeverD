//===- CallImpact.cpp - Bounded native caller/callee witnesses ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index observed transfers once, retain unresolved boundaries and traverse
/// in breadth-first order without enumerating exponentially many paths.
//===----------------------------------------------------------------------===//

#include "neverd/diff/CallImpact.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <stdexcept>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
struct Interrupted {};
class Budget {
  CallImpact &Result;
  const ImpactOptions &Options;
  const ComparisonControl &Control;

public:
  Budget(CallImpact &R, const ImpactOptions &O, const ComparisonControl &C)
      : Result(R), Options(O), Control(C) {}
  [[noreturn]] void stop(ComparisonState State) {
    Result.State = State;
    throw Interrupted{};
  }
  void poll() {
    if (auto State = Control.stop())
      stop(*State);
  }
  void charge(uint64_t Count = 1) {
    if (Count > Options.MaxWork - Result.Work)
      stop(ComparisonState::ResourceLimited);
    const auto Before = Result.Work;
    Result.Work += Count;
    if (Before / limits::kImpactPollWork !=
        Result.Work / limits::kImpactPollWork)
      poll();
  }
  void progress(llvm::StringRef Phase, uint64_t Done, uint64_t Total) {
    if (Control.Progress)
      Control.Progress(Phase, Done, Total);
    poll();
  }
};
struct Links {
  uint32_t Incoming = NoIndex, Outgoing = NoIndex;
};
struct FunctionIndex {
  Links Head;
  uint32_t Gaps = 0;
  bool Complete = false;
};
} // namespace

std::vector<llvm::StringRef> impactGapNames(uint32_t Mask) {
  std::vector<llvm::StringRef> R;
  for (auto [Bit, Name] :
       {std::pair{ImpactIndirectCall, "unresolved_indirect_call"},
        {ImpactIndirectTransfer, "unresolved_indirect_transfer"},
        {ImpactUnresolvedCall, "unresolved_direct_call"},
        {ImpactNativeCoverage, "native_coverage"},
        {ImpactExceptionalControl, "exceptional_control"},
        {ImpactExternalBody, "external_body"},
        {ImpactDiscovery, "incomplete_discovery"}})
    if (Mask & Bit)
      R.push_back(Name);
  return R;
}
std::optional<std::string> ImpactOptions::validate() const {
  if (!MaxFunctions || MaxFunctions > limits::kMaxImpactFunctions)
    return "impact function allowance is outside its bounds";
  if (MaxDepth > limits::kMaxImpactDepth)
    return "impact depth allowance is outside its bounds";
  if (!MaxSites || MaxSites > limits::kMaxImpactSites)
    return "impact site allowance is outside its bounds";
  if (!MaxWork || MaxWork > limits::kMaxImpactWork)
    return "impact work allowance is outside its bounds";
  return std::nullopt;
}

CallImpact analyzeCallImpact(const Program &P, llvm::ArrayRef<uint32_t> Seeds,
                             ImpactDirection Direction, const ImpactOptions &O,
                             const ComparisonControl &Control) {
  if (auto Error = O.validate())
    throw std::invalid_argument(*Error);
  if (Direction != ImpactDirection::Callers &&
      Direction != ImpactDirection::Callees)
    throw std::invalid_argument("invalid impact direction");
  if (Seeds.empty() ||
      Seeds.size() > std::min(O.MaxFunctions, limits::kMaxImpactSeeds))
    throw std::invalid_argument("impact requires a bounded nonempty seed set");
  for (auto F : Seeds)
    if (F >= P.Functions.size())
      throw std::invalid_argument("impact seed is outside the program");
  CallImpact R;
  if (!P.Identity.DiscoveryComplete)
    R.Gaps |= ImpactDiscovery;
  Budget Work(R, O, Control);
  std::vector<FunctionIndex> Index;
  std::vector<Links> Next;
  const auto Site = [&](uint32_t Caller, uint32_t Callee, uint32_t Insn,
                        uint32_t Gaps) {
    if (R.Sites.size() == O.MaxSites)
      Work.stop(ComparisonState::ResourceLimited);
    const auto SI = uint32_t(R.Sites.size());
    R.Sites.push_back({Caller, Callee, Insn, Gaps});
    Next.emplace_back();
    if (Callee == NoIndex) {
      R.BoundarySites.push_back(SI);
      Index[Caller].Gaps |= Gaps;
      R.Gaps |= Gaps;
    } else {
      Next.back().Incoming = Index[Callee].Head.Incoming;
      Next.back().Outgoing = Index[Caller].Head.Outgoing;
      Index[Callee].Head.Incoming = SI;
      Index[Caller].Head.Outgoing = SI;
    }
  };
  try {
    Work.progress("impact:index", 0, P.Functions.size());
    if (P.Functions.size() > limits::kMaxImpactIndexedFunctions)
      Work.stop(ComparisonState::ResourceLimited);
    Work.charge(P.Functions.size() + P.Blocks.size() + P.Edges.size() +
                P.References.size() + P.Instructions.size());
    if (auto Error = P.validate())
      throw std::invalid_argument(*Error);
    Index.resize(P.Functions.size());
    for (uint32_t FI = 0; FI < P.Functions.size(); ++FI) {
      Work.charge();
      const auto &F = P.Functions[FI];
      uint32_t Gaps = F.has(TraitIncomplete) ? ImpactNativeCoverage : 0;
      if (!F.hasBody())
        Gaps |= F.has(TraitImport) ? ImpactExternalBody : ImpactNativeCoverage;
      for (const auto &E : P.edges(F)) {
        Work.charge();
        if (E.Kind == EdgeKind::Exception)
          Gaps |= ImpactExceptionalControl;
      }
      if (Gaps)
        Site(FI, NoIndex, NoIndex, Gaps);
      for (const auto &B : P.blocks(F)) {
        Work.charge();
        for (uint32_t II = B.FirstInstruction;
             II < uint64_t(B.FirstInstruction) + B.InstructionCount; ++II) {
          Work.charge();
          const auto &I = P.Instructions[II];
          if (I.Callee != NoIndex)
            Site(FI, I.Callee, II, 0);
          else {
            uint32_t Gap =
                I.Flow == InstructionFlow::IndirectCall ? ImpactIndirectCall
                : I.Flow == InstructionFlow::IndirectJump
                    ? ImpactIndirectTransfer
                : I.UnresolvedCallee || I.Flow == InstructionFlow::Call
                    ? ImpactUnresolvedCall
                : I.Flow == InstructionFlow::Unknown ? ImpactNativeCoverage
                                                     : 0;
            if (Gap)
              Site(FI, NoIndex, II, Gap);
          }
          ++R.IndexedInstructions;
        }
      }
      Index[FI].Complete = true;
      ++R.IndexedFunctions;
      if (R.IndexedFunctions % limits::kImpactPollWork == 0)
        Work.progress("impact:index", R.IndexedFunctions, P.Functions.size());
    }
    Work.progress("impact:index", R.IndexedFunctions, P.Functions.size());
    R.IndexComplete = true;
  } catch (const Interrupted &) {
  }
  R.IndexState = R.State;
  if (R.State == ComparisonState::Cancelled ||
      R.State == ComparisonState::TimedOut || Index.empty())
    return R;
  R.State = ComparisonState::Complete;
  try {
    Work.progress("impact:traverse", 0, P.Functions.size());
    std::vector<uint32_t> Sorted(Seeds.begin(), Seeds.end());
    llvm::sort(Sorted);
    Sorted.erase(std::unique(Sorted.begin(), Sorted.end()), Sorted.end());
    std::vector<uint32_t> Row(P.Functions.size(), NoIndex);
    const auto Add = [&](uint32_t FI, uint32_t Depth, uint32_t Prev,
                         uint32_t SI) {
      Row[FI] = R.Functions.size();
      R.Functions.push_back(
          {FI, Depth, Prev, SI, Index[FI].Gaps, Index[FI].Complete});
    };
    for (auto FI : Sorted) {
      Work.charge();
      Add(FI, 0, NoIndex, NoIndex);
    }
    bool Limited = false;
    for (uint32_t RI = 0; RI < R.Functions.size(); ++RI) {
      Work.charge();
      const auto F = R.Functions[RI];
      const auto &Head = Index[F.Function].Head;
      for (auto SI = Direction == ImpactDirection::Callers ? Head.Incoming
                                                           : Head.Outgoing;
           SI != NoIndex;
           SI = Direction == ImpactDirection::Callers ? Next[SI].Incoming
                                                      : Next[SI].Outgoing) {
        Work.charge();
        const auto &S = R.Sites[SI];
        const auto Other =
            Direction == ImpactDirection::Callers ? S.Caller : S.Callee;
        if (Row[Other] == NoIndex) {
          if (F.Depth == O.MaxDepth || R.Functions.size() == O.MaxFunctions) {
            Limited = true;
            R.FrontierSites.push_back(SI);
            continue;
          }
          Add(Other, F.Depth + 1, RI, SI);
        }
        R.TraversedSites.push_back(SI);
      }
      if ((RI + 1) % limits::kImpactPollWork == 0)
        Work.progress("impact:traverse", RI + 1, R.Functions.size());
    }
    Work.progress("impact:traverse", R.Functions.size(), R.Functions.size());
    if (Limited)
      Work.stop(ComparisonState::ResourceLimited);
    R.TraversalComplete = true;
  } catch (const Interrupted &) {
  }
  R.TraversalState = R.State;
  if (R.State == ComparisonState::Complete)
    R.State = R.IndexState;
  return R;
}
} // namespace neverd::diff
