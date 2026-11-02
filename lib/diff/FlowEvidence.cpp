//===- FlowEvidence.cpp - SSA collection identity and admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep bounded dependency observations attached to their decoded input and
/// reject edited dependency facts when restoring a saved comparison.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowEvidence.h"

#include "FlowBinding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <set>

namespace neverd::diff {
namespace limits = neverd::limits::diff;

std::optional<std::string> DataFlowSettings::validate() const {
  if (Profile != DataFlowProfile && !isLLVMDataFlowProfile(Profile))
    return "unsupported dataflow profile";
  if (!MaxFunctions || MaxFunctions > limits::kMaxFlowFunctions ||
      !MaxNativeInstructions ||
      MaxNativeInstructions > limits::kMaxFlowNativeInstructions ||
      !MaxTotalNodes || MaxTotalNodes > limits::kMaxFlowTotalNodes)
    return "dataflow collection allowance is outside the admitted range";
  return Graph.validate();
}
std::string DataFlowSettings::describe() const {
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  Out << "enabled=" << Enabled << ",functions=" << MaxFunctions
      << ",native_instructions=" << MaxNativeInstructions
      << ",total_nodes=" << MaxTotalNodes << ",nodes=" << Graph.MaxNodes
      << ",blocks=" << Graph.MaxBlocks << ",work=" << Graph.MaxWork
      << ",rounds=" << Graph.Rounds;
  if (Profile != DataFlowProfile)
    Out << ",profile=" << Profile;
  return Text;
}
llvm::StringRef availabilityName(FlowAvailability State) {
  switch (State) {
  case FlowAvailability::Available:
    return "available";
  case FlowAvailability::Unsupported:
    return "unsupported";
  case FlowAvailability::ResourceLimited:
    return "resource_limited";
  case FlowAvailability::Failed:
    return "failed";
  }
  return "invalid";
}
std::optional<FlowAvailability> availabilityNamed(llvm::StringRef Name) {
  for (auto State :
       {FlowAvailability::Available, FlowAvailability::Unsupported,
        FlowAvailability::ResourceLimited, FlowAvailability::Failed})
    if (Name == availabilityName(State))
      return State;
  return {};
}
uint64_t DataFlowEvidence::nodes() const {
  uint64_t Total = 0;
  for (const auto &E : Entries)
    if (E.Graph)
      Total += E.Graph->Nodes.size();
  return Total;
}
uint64_t DataFlowEvidence::retainedItems() const {
  uint64_t Total = 0;
  for (const auto &E : Entries)
    if (E.Graph)
      Total += E.Graph->retainedItems();
  return Total;
}
const DataFlowEntry *DataFlowEvidence::find(uint64_t Function) const {
  const auto I = std::lower_bound(
      Entries.begin(), Entries.end(), Function,
      [](const auto &E, uint64_t A) { return E.Function < A; });
  return I != Entries.end() && I->Function == Function ? &*I : nullptr;
}
std::optional<std::string>
validateDataFlowEvidence(const DataFlowEvidence *E, const DataFlowSettings &S,
                         const Program &P, const ComparisonControl &Control) {
  Control.check();
  if (auto Error = S.validate())
    return Error;
  if (bool(E) != S.Enabled)
    return "dataflow collection disagrees with requested settings";
  if (!E)
    return {};
  if (!stateNamed(stateName(E->State)) || E->Recipe.empty() ||
      E->Recipe.size() > limits::kMaxFlowIdentityBytes ||
      E->Entries.size() > S.MaxFunctions ||
      E->retainedItems() > S.MaxTotalNodes)
    return "dataflow collection exceeds its bounds or has no recipe";
  std::vector<uint64_t> Eligible;
  for (const auto &F : P.Functions) {
    Control.check();
    if (P.instructionCount(F))
      Eligible.push_back(F.Address);
  }
  if (E->EligibleFunctions != Eligible.size() ||
      E->Entries.size() > Eligible.size() ||
      (E->State == ComparisonState::Complete &&
       E->Entries.size() != Eligible.size()))
    return "dataflow coverage disagrees with the decoded function inventory";
  for (size_t I = 0; I < E->Entries.size(); ++I) {
    Control.check();
    const auto &Row = E->Entries[I];
    if (Row.Function != Eligible[I] ||
        !availabilityNamed(availabilityName(Row.Availability)) ||
        (bool(Row.Graph) !=
         (Row.Availability == FlowAvailability::Available)) ||
        Row.Reason.size() > limits::kMaxFlowReasonBytes ||
        (Row.Graph ? !Row.Reason.empty() : Row.Reason.empty()))
      return "dataflow entry has an invalid scope or availability record";
    if (!Row.Graph)
      if (Row.Availability == FlowAvailability::ResourceLimited &&
          E->State == ComparisonState::Complete)
        return "dataflow collection hides a function resource limit";
    if (!Row.Graph)
      continue;
    const auto &G = *Row.Graph;
    if (auto Error = G.validate(S.Graph, Control))
      return Error;
    if (G.Profile != S.Profile || G.Function != Row.Function ||
        G.Revision != P.Identity.Revision || G.ABI != P.Identity.ABI)
      return "dataflow graph belongs to a different native snapshot";
    const auto &F = P.Functions[*P.functionAt(Row.Function)];
    if (P.instructionCount(F) > S.MaxNativeInstructions)
      return "dataflow graph exceeds the native instruction admission bound";
    std::set<uint64_t> Addresses;
    for (const auto &B : P.blocks(F))
      for (const auto &Insn : P.instructions(B)) {
        Control.check();
        Addresses.insert(Insn.Address);
      }
    for (const auto &N : G.Nodes) {
      Control.check();
      std::optional<std::string> Error;
      N.forEachOrigin([&](const FlowOrigin &Origin) {
        if (!Addresses.contains(Origin.Address))
          Error = "dataflow operation " + N.Opcode + " at 0x" +
                  llvm::utohexstr(Origin.Address) + " (sequence " +
                  std::to_string(Origin.Sequence) + ") in function 0x" +
                  llvm::utohexstr(Row.Function) +
                  " has no corresponding original instruction";
      });
      if (Error)
        return Error;
    }
  }
  return {};
}
std::optional<std::string>
bindDataFlowEvidence(const DataFlowEvidence *Saved,
                     const DataFlowEvidence *Fresh,
                     const DataFlowSettings &Settings, const Program &Input,
                     const ComparisonControl &Control) {
  if (auto Error = validateDataFlowEvidence(Saved, Settings, Input, Control))
    return Error;
  if (bool(Saved) != bool(Fresh))
    return "saved dataflow requires fresh extraction with the same settings";
  if (!Saved)
    return {};
  const auto Disagrees = [] {
    return std::optional<std::string>(
        "saved dataflow facts disagree with the current native SSA projection");
  };
  if (Saved->Recipe != Fresh->Recipe ||
      Saved->EligibleFunctions != Fresh->EligibleFunctions ||
      Saved->Entries.size() > Fresh->Entries.size())
    return Disagrees();
  for (size_t I = 0; I < Saved->Entries.size(); ++I) {
    Control.check();
    const auto &A = Saved->Entries[I], &B = Fresh->Entries[I];
    if (A == B)
      continue;
    if (A.Function != B.Function || A.Availability != B.Availability ||
        A.Reason != B.Reason || !A.Graph || !B.Graph)
      return Disagrees();
    uint64_t Work = 0;
    try {
      if (!sameLLVMGraphAfterPhiRenumbering(
              *A.Graph, *B.Graph, Settings.Graph,
              [&](uint64_t Amount) {
                Control.check();
                if (Amount > Settings.Graph.MaxWork - Work)
                  throw DataFlowInterrupted(ComparisonState::ResourceLimited);
                Work += Amount;
              },
              Control))
        return Disagrees();
    } catch (const DataFlowInterrupted &) {
      return "saved dataflow PHI binding exceeds its work allowance";
    }
  }
  return {};
}
} // namespace neverd::diff
