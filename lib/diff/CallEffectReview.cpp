//===- CallEffectReview.cpp - Bounded external-call contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Join admitted native import identities to exact SSA call occurrences, then
/// ask Safety's sole effect authority for an applicability-checked contract.
//===----------------------------------------------------------------------===//

#include "neverd/diff/CallEffectReview.h"

#include "neverd/diff/FlowEvidence.h"
#include "neverd/safety/SinkCatalog.h"

#include <map>
#include <set>
#include <stdexcept>

namespace neverd::diff {
namespace {
namespace limits = neverd::limits::diff;
struct Interrupted {};
struct CallIndex {
  const DataFlowGraph *Graph = nullptr;
  std::map<uint64_t, uint32_t> Calls;
  uint32_t Gaps = 0;
};

// This is admission of the SDK native comparison profile, not ABI inference.
// Raw images may carry an explicitly selected ABI through the same identity.
std::optional<std::pair<BinaryFormat, Arch>> environment(const Program &P) {
  const auto &I = P.Identity;
  if (!isNativeX86Normalization(I.Normalization) || I.Endian != "little")
    return {};
  const auto Architecture =
      I.Architecture == "x86-64" && I.Bits == 64 ? Arch::X64
      : I.Architecture == "x86" && I.Bits == 32  ? Arch::X86
                                                 : Arch::Unknown;
  if (Architecture == Arch::Unknown)
    return {};
  for (const auto &[Format, Name] : {std::pair{BinaryFormat::ELF, "elf"},
                                     std::pair{BinaryFormat::COFF, "coff"},
                                     std::pair{BinaryFormat::MachO, "macho"}})
    if ((I.Format == Name || I.Format == "raw") &&
        I.ABI == std::to_string(static_cast<unsigned>(Format)) + ":" +
                     I.Architecture)
      return std::pair{Format, Architecture};
  return {};
}
} // namespace

llvm::StringRef callEffectStatusName(CallEffectStatus Status) {
  switch (Status) {
  case CallEffectStatus::Qualified:
    return "qualified_contract";
  case CallEffectStatus::UnresolvedTarget:
    return "unresolved_target";
  case CallEffectStatus::InternalTarget:
    return "internal_target";
  case CallEffectStatus::UnqualifiedIdentity:
    return "unqualified_identity";
  case CallEffectStatus::UnsupportedEnvironment:
    return "unsupported_environment";
  case CallEffectStatus::SSAUnavailable:
    return "ssa_unavailable";
  case CallEffectStatus::MissingCall:
    return "missing_ssa_call";
  case CallEffectStatus::AmbiguousCall:
    return "ambiguous_ssa_call";
  case CallEffectStatus::MissingArguments:
    return "missing_arguments";
  case CallEffectStatus::UnsupportedContract:
    return "unsupported_contract";
  }
  return "invalid";
}
llvm::StringRef callEffectFamilyName(safety::CallEffectFamily Family) {
  using FamilyKind = safety::CallEffectFamily;
  switch (Family) {
  case FamilyKind::None:
    return "none";
  case FamilyKind::StringLength:
    return "string_length";
  case FamilyKind::BoundedStringLength:
    return "bounded_string_length";
  case FamilyKind::Input:
    return "input";
  case FamilyKind::FormattedInput:
    return "formatted_input";
  case FamilyKind::Copy:
    return "copy";
  case FamilyKind::Format:
    return "format";
  case FamilyKind::Allocation:
    return "allocation";
  case FamilyKind::Reallocation:
    return "reallocation";
  case FamilyKind::Release:
    return "release";
  case FamilyKind::StackAllocation:
    return "stack_allocation";
  }
  return "invalid";
}
std::vector<llvm::StringRef>
callEffectCapabilityNames(safety::CallEffectCapability Capabilities) {
  using C = safety::CallEffectCapability;
  std::vector<llvm::StringRef> Out;
  for (auto [Bit, Name] :
       {std::pair{C::Control, "control"},
        {C::Return, "return"},
        {C::ModRef, "mod_ref"},
        {C::Taint, "taint"},
        {C::StringExtent, "string_extent"},
        {C::Allocation, "allocation"},
        {C::Release, "release"},
        {C::Capture, "capture"},
        {C::MayConsumeStandardInput, "may_consume_standard_input"},
        {C::DescriptorZeroIsStandardInput, "descriptor_zero_is_standard_input"},
        {C::LiteralEnvironmentReplay, "literal_environment_replay"},
        {C::ExactStandardInputReplay, "exact_standard_input_replay"}})
    if ((Capabilities & Bit) == Bit)
      Out.push_back(Name);
  return Out;
}
std::optional<std::string> CallEffectReviewOptions::validate() const {
  if (!MaxRecords || MaxRecords > limits::kMaxEffectRecords || !MaxWork ||
      MaxWork > limits::kMaxEffectWork)
    return "call-effect review allowance is outside its bounds";
  return Graph.validate();
}

CallEffectReview reviewCallEffects(const Program &P, const CallImpact &Impact,
                                   const CallEffectReviewOptions &O,
                                   const ComparisonControl &Control) {
  if (auto Error = O.validate())
    throw std::invalid_argument(*Error);
  if (Impact.Sites.size() > limits::kMaxImpactSites ||
      Impact.Functions.size() > limits::kMaxImpactFunctions)
    throw std::invalid_argument("call-effect native scope exceeds admission");
  CallEffectReview R;
  const auto Stop = [&](ComparisonState State) {
    R.State = State;
    throw Interrupted{};
  };
  const auto Poll = [&] {
    if (auto State = Control.stop())
      Stop(*State);
  };
  const auto Charge = [&](uint64_t Count = 1) {
    if (Count > O.MaxWork - R.Work)
      Stop(ComparisonState::ResourceLimited);
    const auto Before = R.Work;
    R.Work += Count;
    if (Before / limits::kEffectPollWork != R.Work / limits::kEffectPollWork)
      Poll();
  };
  const auto Progress = [&](uint64_t Done) {
    if (Control.Progress)
      Control.Progress("impact:effects", Done, Impact.Sites.size());
    Poll();
  };
  const auto Environment = environment(P);
  std::map<uint32_t, CallIndex> Graphs;
  std::map<uint32_t, std::set<uint64_t>> NativeAddresses;
  const auto Addresses = [&](uint32_t FI) -> const std::set<uint64_t> & {
    auto [At, Added] = NativeAddresses.try_emplace(FI);
    if (Added)
      for (const auto &B : P.blocks(P.Functions[FI]))
        for (const auto &I : P.instructions(B)) {
          Charge();
          At->second.insert(I.Address);
        }
    return At->second;
  };
  const auto Index = [&](uint32_t FI) -> const CallIndex & {
    auto [At, Added] = Graphs.try_emplace(FI);
    if (!Added || !P.DataFlow)
      return At->second;
    const auto &F = P.Functions[FI];
    const auto *Entry = P.DataFlow->find(F.Address);
    if (!Entry || Entry->Availability != FlowAvailability::Available ||
        !Entry->Graph || Entry->Graph->Profile != DataFlowProfile)
      return At->second;
    const auto &G = *Entry->Graph;
    for (const auto &N : G.Nodes)
      Charge(1 + N.Inputs.size());
    for (const auto &B : G.Blocks)
      Charge(1 + B.Successors.size());
    if (auto Error = G.validate(O.Graph))
      throw std::invalid_argument(*Error);
    if (G.Function != F.Address || G.Revision != P.Identity.Revision ||
        G.ABI != P.Identity.ABI)
      throw std::invalid_argument("call-effect SSA belongs to another input");
    auto &Out = At->second;
    Out.Graph = &G;
    Out.Gaps = G.Gaps;
    for (const auto &B : G.Blocks)
      Out.Gaps |= B.Gaps;
    const auto &Native = Addresses(FI);
    for (uint32_t N = 0; N < G.Nodes.size(); ++N) {
      Charge();
      const auto &Node = G.Nodes[N];
      Out.Gaps |= Node.Gaps;
      if (Node.OriginSequence < 0)
        continue;
      if (!Native.contains(Node.Address))
        throw std::invalid_argument("call-effect SSA has a foreign origin");
      if (Node.Opcode == "CALL" || Node.Opcode == "INDIR_CALL") {
        auto [Call, Unique] = Out.Calls.emplace(Node.Address, N);
        if (!Unique)
          Call->second = NoIndex;
      }
    }
    return Out;
  };
  try {
    Progress(0);
    if (Impact.State == ComparisonState::Cancelled ||
        Impact.State == ComparisonState::TimedOut)
      Stop(Impact.State);
    std::set<uint32_t> Reached;
    for (const auto &F : Impact.Functions) {
      Charge();
      if (F.Function >= P.Functions.size() ||
          !Reached.insert(F.Function).second)
        throw std::invalid_argument("call-effect native function is invalid");
    }
    const auto Catalog = safety::SinkCatalog::defaults();
    for (uint32_t I = 0; I < Impact.Sites.size(); ++I) {
      Charge();
      const auto &S = Impact.Sites[I];
      if (S.Caller >= P.Functions.size() ||
          (S.Callee != NoIndex && S.Callee >= P.Functions.size()) ||
          (S.Instruction != NoIndex && S.Instruction >= P.Instructions.size()))
        throw std::invalid_argument("call-effect native site is invalid");
      ++R.VisitedSites;
      if (!Reached.contains(S.Caller) || S.Instruction == NoIndex)
        continue;
      const auto &Native = P.Instructions[S.Instruction];
      if (Native.Callee != S.Callee ||
          !Addresses(S.Caller).contains(Native.Address))
        throw std::invalid_argument(
            "call-effect site disagrees with native owner");
      if (R.Records.size() == O.MaxRecords)
        Stop(ComparisonState::ResourceLimited);
      CallEffectObservation Row;
      Row.Site = I;
      const auto Classify = [&] {
        if (S.Callee == NoIndex)
          return CallEffectStatus::UnresolvedTarget;
        const auto &Target = P.Functions[S.Callee];
        if (Target.hasBody())
          return CallEffectStatus::InternalTarget;
        if (!Target.has(TraitImport) || Target.Source != NameSource::Import)
          return CallEffectStatus::UnqualifiedIdentity;
        if (!Environment)
          return CallEffectStatus::UnsupportedEnvironment;
        const auto &Calls = Index(S.Caller);
        Row.SSAGaps = Calls.Gaps;
        if (!Calls.Graph)
          return CallEffectStatus::SSAUnavailable;
        const auto Found = Calls.Calls.find(Native.Address);
        if (Found == Calls.Calls.end())
          return CallEffectStatus::MissingCall;
        if (Found->second == NoIndex)
          return CallEffectStatus::AmbiguousCall;
        Row.Node = Found->second;
        const auto &Node = Calls.Graph->Nodes[Row.Node];
        if (Node.CallArgumentBegin == NoIndex)
          return CallEffectStatus::MissingArguments;
        Row.ArgumentCount = Node.CallArgumentCount;
        Charge(Target.Name.size());
        const auto Effects =
            safety::resolveCallEffects(Target.Name, Environment->first,
                                       Environment->second, Row.ArgumentCount);
        if (!Effects)
          return CallEffectStatus::UnsupportedContract;
        Row.Family = Effects.family();
        Row.Capabilities = Effects.capabilities();
        if (const auto *Sink = Catalog.matchSink(Target.Name);
            Sink && Effects.supports(*Sink)) {
          Row.Destination = Sink->DstArg;
          Row.Source = Sink->SrcArg;
          Row.Length = Sink->LenArg;
          Row.Capacity = Sink->CapArg;
          Row.Format = Sink->FmtArg;
          Row.Handle = Sink->HandleArg;
          Row.ReleaseMayFail = Sink->ReleaseMayFail;
          Row.UnboundedWrite = Sink->UnboundedWrite;
        }
        if (Effects.has(safety::CallEffectCapability::Taint))
          if (const auto *Source = Catalog.matchSource(Target.Name))
            Row.InputBuffer = Source->OutArg;
        for (int Role : {Row.Destination, Row.Source, Row.Length, Row.Capacity,
                         Row.Format, Row.Handle, Row.InputBuffer})
          if (Role < -1 || (Role >= 0 && uint32_t(Role) >= Row.ArgumentCount))
            throw std::invalid_argument(
                "call-effect catalog role exceeds arity");
        return CallEffectStatus::Qualified;
      };
      Row.Status = Classify();
      R.Qualified += Row.Status == CallEffectStatus::Qualified;
      R.Records.push_back(std::move(Row));
      if (R.VisitedSites % limits::kEffectPollWork == 0)
        Progress(R.VisitedSites);
    }
    Progress(R.VisitedSites);
    R.Complete = true;
  } catch (const Interrupted &) {
  }
  return R;
}
} // namespace neverd::diff
