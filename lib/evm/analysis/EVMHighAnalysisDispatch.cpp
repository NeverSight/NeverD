//===- EVMHighAnalysisDispatch.cpp - EVM selector dispatcher reading ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <vector>

namespace neverd::evm::detail {
namespace {

enum class SelectorDomainKind : uint8_t { Any, Exact, Excluded };

/// A compact set of selector values that can reach one dispatcher lane.
///
/// Exact holds a finite set. Excluded holds its cofinite complement. Keeping
/// both forms makes equality branches precise without enumerating the 32-bit
/// selector universe. Values are always sorted and unique. Every scan or copy
/// is charged in advance to the declarative HighIR reference-visit budget, so a
/// long dispatcher cannot turn path constraints into unbounded quadratic work.
struct SelectorDomain {
  SelectorDomainKind Kind = SelectorDomainKind::Any;
  llvm::SmallVector<uint32_t, 4> Values;

  static SelectorDomain exact(uint32_t Selector) {
    SelectorDomain Result;
    Result.Kind = SelectorDomainKind::Exact;
    Result.Values.push_back(Selector);
    return Result;
  }

  static SelectorDomain excluding(uint32_t Selector) {
    SelectorDomain Result;
    Result.Kind = SelectorDomainKind::Excluded;
    Result.Values.push_back(Selector);
    return Result;
  }
};

llvm::Error noteSelectorDomainEntries(
    size_t Count, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  for (size_t Index = 0; Index < Count; ++Index)
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
  return llvm::Error::success();
}

llvm::Expected<SelectorDomain> copySelectorDomain(
    const SelectorDomain &Domain, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (llvm::Error Error = noteSelectorDomainEntries(Domain.Values.size(), PC,
                                                    NoteReferenceVisit))
    return std::move(Error);
  return Domain;
}

llvm::Expected<bool> selectorDomainLists(
    const SelectorDomain &Domain, uint32_t Selector, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (llvm::Error Error = noteSelectorDomainEntries(Domain.Values.size(), PC,
                                                    NoteReferenceVisit))
    return std::move(Error);
  return std::binary_search(Domain.Values.begin(), Domain.Values.end(),
                            Selector);
}

llvm::Expected<bool> selectorDomainAllows(
    const SelectorDomain &Domain, uint32_t Selector, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (Domain.Kind == SelectorDomainKind::Any)
    return true;
  auto Listed = selectorDomainLists(Domain, Selector, PC, NoteReferenceVisit);
  if (!Listed)
    return Listed.takeError();
  return Domain.Kind == SelectorDomainKind::Exact ? *Listed : !*Listed;
}

llvm::Expected<std::optional<SelectorDomain>> refineSelectorEqual(
    const SelectorDomain &Domain, uint32_t Selector, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  auto Allowed = selectorDomainAllows(Domain, Selector, PC, NoteReferenceVisit);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return std::optional<SelectorDomain>{};
  if (llvm::Error Error = NoteReferenceVisit(PC))
    return std::move(Error);
  return std::optional<SelectorDomain>(SelectorDomain::exact(Selector));
}

llvm::Expected<std::optional<SelectorDomain>> refineSelectorNotEqual(
    const SelectorDomain &Domain, uint32_t Selector, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  auto Allowed = selectorDomainAllows(Domain, Selector, PC, NoteReferenceVisit);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed) {
    auto Copy = copySelectorDomain(Domain, PC, NoteReferenceVisit);
    if (!Copy)
      return Copy.takeError();
    return std::optional<SelectorDomain>(std::move(*Copy));
  }

  if (Domain.Kind == SelectorDomainKind::Any) {
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return std::move(Error);
    return std::optional<SelectorDomain>(SelectorDomain::excluding(Selector));
  }

  auto Copy = copySelectorDomain(Domain, PC, NoteReferenceVisit);
  if (!Copy)
    return Copy.takeError();
  SelectorDomain Result = std::move(*Copy);
  const auto It = llvm::lower_bound(Result.Values, Selector);
  if (Result.Kind == SelectorDomainKind::Exact) {
    Result.Values.erase(It);
    if (Result.Values.empty())
      return std::optional<SelectorDomain>{};
  } else {
    Result.Values.insert(It, Selector);
  }
  return std::optional<SelectorDomain>(std::move(Result));
}

llvm::SmallVector<uint32_t, 4>
unionSelectorSets(llvm::ArrayRef<uint32_t> Left,
                  llvm::ArrayRef<uint32_t> Right) {
  llvm::SmallVector<uint32_t, 4> Result;
  std::set_union(Left.begin(), Left.end(), Right.begin(), Right.end(),
                 std::back_inserter(Result));
  return Result;
}

llvm::SmallVector<uint32_t, 4>
intersectSelectorSets(llvm::ArrayRef<uint32_t> Left,
                      llvm::ArrayRef<uint32_t> Right) {
  llvm::SmallVector<uint32_t, 4> Result;
  std::set_intersection(Left.begin(), Left.end(), Right.begin(), Right.end(),
                        std::back_inserter(Result));
  return Result;
}

llvm::SmallVector<uint32_t, 4>
subtractSelectorSets(llvm::ArrayRef<uint32_t> Left,
                     llvm::ArrayRef<uint32_t> Right) {
  llvm::SmallVector<uint32_t, 4> Result;
  std::set_difference(Left.begin(), Left.end(), Right.begin(), Right.end(),
                      std::back_inserter(Result));
  return Result;
}

/// Join p Incoming into p Reached. The represented selector set only widens:
/// exact sets union, cofinite exclusions intersect, and exact values remove the
/// corresponding exclusions from a cofinite set.
llvm::Expected<bool> mergeSelectorDomains(
    SelectorDomain &Reached, const SelectorDomain &Incoming, uint64_t PC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (Reached.Kind == SelectorDomainKind::Any)
    return false;
  if (Incoming.Kind == SelectorDomainKind::Any) {
    Reached = SelectorDomain{};
    return true;
  }
  if (llvm::Error Error = noteSelectorDomainEntries(Reached.Values.size(), PC,
                                                    NoteReferenceVisit))
    return std::move(Error);
  if (llvm::Error Error = noteSelectorDomainEntries(Incoming.Values.size(), PC,
                                                    NoteReferenceVisit))
    return std::move(Error);

  SelectorDomain Joined;
  if (Reached.Kind == SelectorDomainKind::Exact &&
      Incoming.Kind == SelectorDomainKind::Exact) {
    Joined.Kind = SelectorDomainKind::Exact;
    Joined.Values = unionSelectorSets(Reached.Values, Incoming.Values);
  } else if (Reached.Kind == SelectorDomainKind::Excluded &&
             Incoming.Kind == SelectorDomainKind::Excluded) {
    Joined.Kind = SelectorDomainKind::Excluded;
    Joined.Values = intersectSelectorSets(Reached.Values, Incoming.Values);
  } else {
    Joined.Kind = SelectorDomainKind::Excluded;
    const SelectorDomain &Excluded =
        Reached.Kind == SelectorDomainKind::Excluded ? Reached : Incoming;
    const SelectorDomain &Exact =
        Reached.Kind == SelectorDomainKind::Exact ? Reached : Incoming;
    Joined.Values = subtractSelectorSets(Excluded.Values, Exact.Values);
  }
  if (Joined.Kind == SelectorDomainKind::Excluded && Joined.Values.empty())
    Joined.Kind = SelectorDomainKind::Any;
  if (Reached.Kind == Joined.Kind && Reached.Values == Joined.Values)
    return false;
  Reached = std::move(Joined);
  return true;
}

llvm::Expected<bool>
endsInRevert(const ProducerIndex &Index,
             const DefiniteExecutionIndex &Execution, MedStateLaneID Lane,
             llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
             llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
    return std::move(Error);
  const MedStateLane *StateLane = Execution.lane(Lane);
  if (!StateLane)
    return false;
  const MedBlock *Block = Index.block(StateLane->LowLane.BlockPC);
  if (!Block)
    return false;
  const MedOperation *Last = nullptr;
  for (const MedOperation &Operation : Block->Operations) {
    if (llvm::Error Error = NoteOperationVisit(Operation.PC))
      return std::move(Error);
    if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
      return std::move(Error);
    auto Executes = Execution.executesIn(Operation, Lane, NoteReferenceVisit);
    if (!Executes)
      return Executes.takeError();
    if (*Executes)
      Last = &Operation;
  }
  return Last && Last->Op == Opcode::REVERT;
}

enum class DispatchConstraint : uint8_t { Receive, Fallback };

bool isEmptyCalldataGuard(const SemanticValue &Condition) {
  return Condition.Kind == SemanticKind::CalldataSize ||
         Condition.Kind == SemanticKind::IsZeroCalldataSize ||
         Condition.Kind == SemanticKind::CalldataSizeBelowSelector ||
         Condition.Kind == SemanticKind::CalldataSizeAtLeastSelector;
}

std::optional<EdgeKind> constrainedEdge(const SemanticValue &Condition,
                                        DispatchConstraint Constraint) {
  std::optional<EdgeKind> Required;
  switch (Condition.Kind) {
  case SemanticKind::Constant:
    Required = Condition.Word.isZero() ? EdgeKind::ConditionalFalse
                                       : EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::CallValue:
    if (Constraint == DispatchConstraint::Receive)
      Required = EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::CalldataSize:
    Required = Constraint == DispatchConstraint::Receive
                   ? EdgeKind::ConditionalFalse
                   : EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::CalldataWordZero:
  case SemanticKind::SelectorWord:
    if (Constraint == DispatchConstraint::Receive)
      Required = EdgeKind::ConditionalFalse;
    break;
  case SemanticKind::IsZeroCallValue:
    if (Constraint == DispatchConstraint::Receive)
      Required = EdgeKind::ConditionalFalse;
    break;
  case SemanticKind::ExternalOutcome:
  case SemanticKind::IsZeroExternalOutcome:
    // Host-call and creation outcomes are modeled nondeterministically, but
    // unlike an unreadable selector expression they are not dispatcher
    // predicates. Both exact CFG edges remain feasible for the constrained
    // call.
    break;
  case SemanticKind::IsZeroCalldataSize:
    Required = Constraint == DispatchConstraint::Receive
                   ? EdgeKind::ConditionalTrue
                   : EdgeKind::ConditionalFalse;
    break;
  case SemanticKind::SelectorEquality:
    if (Constraint == DispatchConstraint::Fallback)
      Required = EdgeKind::ConditionalFalse;
    else
      Required = Condition.Selector == 0 ? EdgeKind::ConditionalTrue
                                         : EdgeKind::ConditionalFalse;
    break;
  case SemanticKind::SelectorXor:
    if (Constraint == DispatchConstraint::Fallback)
      Required = EdgeKind::ConditionalTrue;
    else
      Required = Condition.Selector == 0 ? EdgeKind::ConditionalFalse
                                         : EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::CalldataSizeBelowSelector:
    Required = Constraint == DispatchConstraint::Receive
                   ? EdgeKind::ConditionalTrue
                   : EdgeKind::ConditionalFalse;
    break;
  case SemanticKind::CalldataSizeAtLeastSelector:
    Required = Constraint == DispatchConstraint::Receive
                   ? EdgeKind::ConditionalFalse
                   : EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::Unknown:
    break;
  }

  return Required;
}

std::optional<EdgeKind> selectorConstrainedEdge(const SemanticValue &Condition,
                                                uint32_t Selector) {
  switch (Condition.Kind) {
  case SemanticKind::Constant:
    return Condition.Word.isZero() ? EdgeKind::ConditionalFalse
                                   : EdgeKind::ConditionalTrue;
  case SemanticKind::SelectorWord:
    return Selector == 0 ? EdgeKind::ConditionalFalse
                         : EdgeKind::ConditionalTrue;
  case SemanticKind::CalldataWordZero:
    // A non-zero high word proves the complete calldata word is non-zero. A
    // zero selector says nothing about the remaining calldata bytes.
    if (Selector != 0)
      return EdgeKind::ConditionalTrue;
    break;
  case SemanticKind::SelectorEquality:
    return Selector == Condition.Selector ? EdgeKind::ConditionalTrue
                                          : EdgeKind::ConditionalFalse;
  case SemanticKind::SelectorXor:
    return Selector == Condition.Selector ? EdgeKind::ConditionalFalse
                                          : EdgeKind::ConditionalTrue;
  case SemanticKind::Unknown:
  case SemanticKind::CallValue:
  case SemanticKind::IsZeroCallValue:
  case SemanticKind::ExternalOutcome:
  case SemanticKind::IsZeroExternalOutcome:
  case SemanticKind::CalldataSize:
  case SemanticKind::IsZeroCalldataSize:
  case SemanticKind::CalldataSizeBelowSelector:
  case SemanticKind::CalldataSizeAtLeastSelector:
    break;
  }
  return std::nullopt;
}

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  const auto It = llvm::lower_bound(
      Low.Instructions, PC,
      [](const LowInstruction &Instruction, uint64_t Address) {
        return Instruction.PC < Address;
      });
  return It != Low.Instructions.end() && It->PC == PC ? &*It : nullptr;
}

bool isSuccessfulTerminal(const EVMLowIR &Low, const MedOperation *LastExecuted,
                          bool Faulted, std::optional<EdgeKind> RequiredEdge) {
  if (Faulted || !LastExecuted)
    return false;
  switch (LastExecuted->Op) {
  case Opcode::STOP:
  case Opcode::RETURN:
  case Opcode::SELFDESTRUCT:
    return true;
  case Opcode::REVERT:
  case Opcode::INVALID:
    return false;
  default:
    break;
  }

  const LowInstruction *Instruction = instructionAt(Low, LastExecuted->PC);
  if (!Instruction || Instruction->NextPC != Low.Code.size())
    return false;
  if (!Instruction->isTerminator())
    return true;
  return LastExecuted->Op == Opcode::JUMPI &&
         RequiredEdge == EdgeKind::ConditionalFalse;
}

llvm::Expected<bool> analyzeConstrainedCall(
    const EVMLowIR &Low, const ProducerIndex &Index,
    const DefiniteExecutionIndex &Execution, SemanticClassifier &Classifier,
    DispatchConstraint Constraint,
    llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  if (Low.Code.empty())
    return Constraint == DispatchConstraint::Fallback;
  const auto Entry = Execution.medLane({kEntryPC, kEntryStateLaneOrdinal});
  if (!Entry || !Execution.isReachable(*Entry))
    return false;

  using WorkItem = std::pair<MedStateLaneID, bool>;
  std::set<WorkItem> Seen{{*Entry, false}};
  std::vector<WorkItem> Worklist{{*Entry, false}};
  while (!Worklist.empty()) {
    const auto [Lane, SawEmptyCalldataGuard] = Worklist.back();
    Worklist.pop_back();
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    const MedStateLane *StateLane = Execution.lane(Lane);
    const MedBlock *Block =
        StateLane ? Index.block(StateLane->LowLane.BlockPC) : nullptr;
    if (!StateLane || !Execution.isReachable(Lane) || !Block)
      continue;
    if (llvm::Error Error = NoteLaneVisit(StateLane->LowLane.BlockPC))
      return std::move(Error);

    const MedOperation *Branch = nullptr;
    SemanticValue BranchCondition;
    const MedOperation *LastExecuted = nullptr;
    bool Faulted = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Faults = Execution.faultsIn(Operation, Lane, NoteReferenceVisit);
      if (!Faults)
        return Faults.takeError();
      if (*Faults) {
        Faulted = true;
        break;
      }
      auto Executes = Execution.executesIn(Operation, Lane, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes)
        continue;
      LastExecuted = &Operation;
      if (Operation.Op == Opcode::JUMPI && Operation.Inputs.size() == 2) {
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        Branch = &Operation;
        auto Classified = Classifier.classify(Operation.Inputs[1]);
        if (!Classified)
          return Classified.takeError();
        BranchCondition = std::move(*Classified);
      }
    }

    const bool NextSawEmptyCalldataGuard =
        SawEmptyCalldataGuard || isEmptyCalldataGuard(BranchCondition);
    const std::optional<EdgeKind> RequiredEdge =
        constrainedEdge(BranchCondition, Constraint);
    llvm::SmallVector<MedStateLaneID, 4> Successors;
    if (!Branch || BranchCondition.Kind != SemanticKind::Unknown) {
      auto Next = Execution.successors(Lane, RequiredEdge, std::nullopt,
                                       NoteReferenceVisit);
      if (!Next)
        return Next.takeError();
      Successors = std::move(*Next);
    }
    if (Successors.empty() &&
        (Constraint == DispatchConstraint::Fallback ||
         NextSawEmptyCalldataGuard) &&
        isSuccessfulTerminal(Low, LastExecuted, Faulted, RequiredEdge))
      return true;
    for (MedStateLaneID Successor : Successors) {
      if (llvm::Error Error = NoteReferenceVisit(StateLane->LowLane.BlockPC))
        return std::move(Error);
      if (const WorkItem Next{Successor, NextSawEmptyCalldataGuard};
          Seen.insert(Next).second)
        Worklist.push_back(Next);
    }
  }
  return false;
}

} // namespace

llvm::Expected<llvm::SmallVector<SelectorDispatchCandidate, 8>>
discoverSelectorDispatch(
    const EVMLowIR &Low, const ProducerIndex &Index,
    const DefiniteExecutionIndex &Execution, SemanticClassifier &Classifier,
    llvm::function_ref<llvm::Error(uint64_t)> NoteDispatchCandidate,
    llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  llvm::SmallVector<SelectorDispatchCandidate, 8> Candidates;
  if (Low.Code.empty())
    return Candidates;
  const auto Entry = Execution.medLane({kEntryPC, kEntryStateLaneOrdinal});
  if (!Entry || !Execution.isReachable(*Entry))
    return Candidates;

  std::map<MedStateLaneID, SelectorDomain> ReachedDomains;
  ReachedDomains.emplace(*Entry, SelectorDomain{});
  std::set<MedStateLaneID> Queued{*Entry};
  std::set<std::pair<MedStateLaneID, uint32_t>> RecordedCandidates;
  std::vector<MedStateLaneID> Worklist{*Entry};
  while (!Worklist.empty()) {
    const MedStateLaneID Lane = Worklist.back();
    Worklist.pop_back();
    Queued.erase(Lane);
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    const MedStateLane *StateLane = Execution.lane(Lane);
    const MedBlock *Block =
        StateLane ? Index.block(StateLane->LowLane.BlockPC) : nullptr;
    if (!StateLane || !Execution.isReachable(Lane) || !Block)
      continue;
    if (llvm::Error Error = NoteLaneVisit(StateLane->LowLane.BlockPC))
      return std::move(Error);
    const auto ReachedIt = ReachedDomains.find(Lane);
    if (ReachedIt == ReachedDomains.end())
      continue;
    auto DomainCopy = copySelectorDomain(
        ReachedIt->second, StateLane->LowLane.BlockPC, NoteReferenceVisit);
    if (!DomainCopy)
      return DomainCopy.takeError();
    const SelectorDomain Domain = std::move(*DomainCopy);

    const MedOperation *Branch = nullptr;
    SemanticValue Condition;
    bool Faulted = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Faults = Execution.faultsIn(Operation, Lane, NoteReferenceVisit);
      if (!Faults)
        return Faults.takeError();
      if (*Faults) {
        Faulted = true;
        break;
      }
      auto Executes = Execution.executesIn(Operation, Lane, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes)
        continue;
      if (Operation.Op == Opcode::JUMPI && Operation.Inputs.size() == 2) {
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        Branch = &Operation;
        auto Classified = Classifier.classify(Operation.Inputs[1]);
        if (!Classified)
          return Classified.takeError();
        Condition = std::move(*Classified);
      }
    }
    if (Faulted)
      continue;

    const auto FindSuccessors = [&](std::optional<EdgeKind> Kind) {
      return Execution.successors(Lane, Kind, std::nullopt, NoteReferenceVisit);
    };
    const auto Propagate =
        [&](llvm::ArrayRef<MedStateLaneID> Successors,
            const SelectorDomain &NextDomain) -> llvm::Error {
      for (MedStateLaneID Successor : Successors) {
        if (llvm::Error Error = NoteReferenceVisit(StateLane->LowLane.BlockPC))
          return Error;
        auto Reached = ReachedDomains.find(Successor);
        bool Changed = false;
        if (Reached == ReachedDomains.end()) {
          auto Copy = copySelectorDomain(NextDomain, StateLane->LowLane.BlockPC,
                                         NoteReferenceVisit);
          if (!Copy)
            return Copy.takeError();
          ReachedDomains.emplace(Successor, std::move(*Copy));
          Changed = true;
        } else {
          auto Merged = mergeSelectorDomains(Reached->second, NextDomain,
                                             StateLane->LowLane.BlockPC,
                                             NoteReferenceVisit);
          if (!Merged)
            return Merged.takeError();
          Changed = *Merged;
        }
        if (Changed && Queued.insert(Successor).second)
          Worklist.push_back(Successor);
      }
      return llvm::Error::success();
    };
    const auto PropagateEdge = [&](std::optional<EdgeKind> Kind,
                                   const SelectorDomain &NextDomain) {
      auto Found = FindSuccessors(Kind);
      if (!Found)
        return Found.takeError();
      return Propagate(*Found, NextDomain);
    };
    const auto RecordCandidate =
        [&](uint32_t Selector, EdgeKind MatchEdge,
            bool RequiresJumpDestination) -> llvm::Error {
      auto Allowed = selectorDomainAllows(Domain, Selector, Branch->PC,
                                          NoteReferenceVisit);
      if (!Allowed)
        return Allowed.takeError();
      if (!*Allowed)
        return llvm::Error::success();
      auto FoundTargets = FindSuccessors(MatchEdge);
      if (!FoundTargets)
        return FoundTargets.takeError();
      llvm::SmallVector<MedStateLaneID, 4> Targets = std::move(*FoundTargets);
      if (Targets.empty())
        return llvm::Error::success();
      if (llvm::Error Error = NoteReferenceVisit(StateLane->LowLane.BlockPC))
        return Error;
      const MedStateLane *FirstTarget = Execution.lane(Targets.front());
      const uint64_t EntryPC =
          FirstTarget ? FirstTarget->LowLane.BlockPC : kEntryPC;
      bool SameTargetBlock = FirstTarget != nullptr;
      for (MedStateLaneID ID : Targets) {
        if (llvm::Error Error = NoteReferenceVisit(Branch->PC))
          return Error;
        const MedStateLane *Target = Execution.lane(ID);
        SameTargetBlock &= Target && Target->LowLane.BlockPC == EntryPC;
      }
      if (!SameTargetBlock ||
          (RequiresJumpDestination &&
           !Low.JumpDestinations.contains(EntryPC)) ||
          RecordedCandidates.contains({Lane, Selector}))
        return llvm::Error::success();
      if (llvm::Error Error = NoteDispatchCandidate(Branch->PC))
        return Error;
      RecordedCandidates.emplace(Lane, Selector);
      Candidates.push_back({Selector, Branch->PC, EntryPC, std::move(Targets)});
      return llvm::Error::success();
    };
    if (Branch && Condition.Kind == SemanticKind::SelectorEquality) {
      if (llvm::Error Error =
              RecordCandidate(Condition.Selector, EdgeKind::ConditionalTrue,
                              /*RequiresJumpDestination=*/true))
        return std::move(Error);
      // The true edge has matched and entered a function body. Only the false
      // edge remains in the dispatcher's unmatched state, with this selector
      // excluded from every later comparison on that path.
      auto NonMatch = refineSelectorNotEqual(Domain, Condition.Selector,
                                             Branch->PC, NoteReferenceVisit);
      if (!NonMatch)
        return NonMatch.takeError();
      if (*NonMatch)
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalFalse, **NonMatch))
          return std::move(Error);
    } else if (!Branch) {
      if (llvm::Error Error = PropagateEdge(std::nullopt, Domain))
        return std::move(Error);
    } else {
      switch (Condition.Kind) {
      case SemanticKind::Constant:
        if (llvm::Error Error = PropagateEdge(Condition.Word.isZero()
                                                  ? EdgeKind::ConditionalFalse
                                                  : EdgeKind::ConditionalTrue,
                                              Domain))
          return std::move(Error);
        break;
      case SemanticKind::CalldataSizeBelowSelector:
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalFalse, Domain))
          return std::move(Error);
        break;
      case SemanticKind::CalldataSizeAtLeastSelector:
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalTrue, Domain))
          return std::move(Error);
        break;
      case SemanticKind::SelectorXor: {
        if (llvm::Error Error =
                RecordCandidate(Condition.Selector, EdgeKind::ConditionalFalse,
                                /*RequiresJumpDestination=*/false))
          return std::move(Error);
        auto NonMatch = refineSelectorNotEqual(Domain, Condition.Selector,
                                               Branch->PC, NoteReferenceVisit);
        if (!NonMatch)
          return NonMatch.takeError();
        if (*NonMatch)
          if (llvm::Error Error =
                  PropagateEdge(EdgeKind::ConditionalTrue, **NonMatch))
            return std::move(Error);
        break;
      }
      case SemanticKind::Unknown:
        break;
      case SemanticKind::CallValue:
      case SemanticKind::IsZeroCallValue:
      case SemanticKind::ExternalOutcome:
      case SemanticKind::IsZeroExternalOutcome:
        // Call value is independent of the calldata selector. Both outcomes
        // therefore preserve the same selector domain. The same is true for
        // the nondeterministic success value produced by a host call or
        // contract creation.
        if (llvm::Error Error = PropagateEdge(std::nullopt, Domain))
          return std::move(Error);
        break;
      case SemanticKind::SelectorWord: {
        auto NonZero =
            refineSelectorNotEqual(Domain, 0, Branch->PC, NoteReferenceVisit);
        if (!NonZero)
          return NonZero.takeError();
        if (*NonZero)
          if (llvm::Error Error =
                  PropagateEdge(EdgeKind::ConditionalTrue, **NonZero))
            return std::move(Error);
        auto Zero =
            refineSelectorEqual(Domain, 0, Branch->PC, NoteReferenceVisit);
        if (!Zero)
          return Zero.takeError();
        if (*Zero)
          if (llvm::Error Error =
                  PropagateEdge(EdgeKind::ConditionalFalse, **Zero))
            return std::move(Error);
        break;
      }
      case SemanticKind::CalldataWordZero: {
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalTrue, Domain))
          return std::move(Error);
        auto Zero =
            refineSelectorEqual(Domain, 0, Branch->PC, NoteReferenceVisit);
        if (!Zero)
          return Zero.takeError();
        if (*Zero)
          if (llvm::Error Error =
                  PropagateEdge(EdgeKind::ConditionalFalse, **Zero))
            return std::move(Error);
        break;
      }
      case SemanticKind::CalldataSize:
        // The false edge is empty calldata and contains no function selector.
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalTrue, Domain))
          return std::move(Error);
        break;
      case SemanticKind::IsZeroCalldataSize:
        // The true edge is empty calldata and contains no function selector.
        if (llvm::Error Error =
                PropagateEdge(EdgeKind::ConditionalFalse, Domain))
          return std::move(Error);
        break;
      case SemanticKind::SelectorEquality:
        break;
      }
    }
  }
  return Candidates;
}

llvm::Expected<std::set<MedStateLaneID>> reachableFunctionLanes(
    const ProducerIndex &Index, const DefiniteExecutionIndex &Execution,
    SemanticClassifier &Classifier, const std::set<MedStateLaneID> &Entries,
    uint32_t Selector, llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  std::set<MedStateLaneID> Seen;
  std::vector<MedStateLaneID> Worklist;
  for (MedStateLaneID Entry : Entries) {
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    if (Execution.isReachable(Entry) && Seen.insert(Entry).second)
      Worklist.push_back(Entry);
  }

  while (!Worklist.empty()) {
    const MedStateLaneID Lane = Worklist.back();
    Worklist.pop_back();
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    const MedStateLane *StateLane = Execution.lane(Lane);
    const MedBlock *Block =
        StateLane ? Index.block(StateLane->LowLane.BlockPC) : nullptr;
    if (!StateLane || !Execution.isReachable(Lane) || !Block)
      continue;
    if (llvm::Error Error = NoteLaneVisit(StateLane->LowLane.BlockPC))
      return std::move(Error);

    const MedOperation *Branch = nullptr;
    SemanticValue Condition;
    bool Faulted = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Faults = Execution.faultsIn(Operation, Lane, NoteReferenceVisit);
      if (!Faults)
        return Faults.takeError();
      if (*Faults) {
        Faulted = true;
        break;
      }
      auto Executes = Execution.executesIn(Operation, Lane, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes)
        continue;
      if (Operation.Op == Opcode::JUMPI && Operation.Inputs.size() == 2) {
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        Branch = &Operation;
        auto Classified = Classifier.classify(Operation.Inputs[1]);
        if (!Classified)
          return Classified.takeError();
        Condition = std::move(*Classified);
      }
    }
    if (Faulted)
      continue;

    const std::optional<EdgeKind> RequiredEdge =
        Branch ? selectorConstrainedEdge(Condition, Selector) : std::nullopt;
    auto Successors = Execution.successors(Lane, RequiredEdge, std::nullopt,
                                           NoteReferenceVisit);
    if (!Successors)
      return Successors.takeError();
    for (MedStateLaneID Successor : *Successors) {
      if (llvm::Error Error = NoteReferenceVisit(StateLane->LowLane.BlockPC))
        return std::move(Error);
      if (Seen.insert(Successor).second)
        Worklist.push_back(Successor);
    }
  }
  return Seen;
}

llvm::Expected<bool>
analyzeReceive(const EVMLowIR &Low, const ProducerIndex &Index,
               const DefiniteExecutionIndex &Execution,
               SemanticClassifier &Classifier,
               llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
               llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
               llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  return analyzeConstrainedCall(Low, Index, Execution, Classifier,
                                DispatchConstraint::Receive, NoteLaneVisit,
                                NoteOperationVisit, NoteReferenceVisit);
}

llvm::Expected<bool>
analyzeFallback(const EVMLowIR &Low, const ProducerIndex &Index,
                const DefiniteExecutionIndex &Execution,
                SemanticClassifier &Classifier,
                llvm::function_ref<llvm::Error(uint64_t)> NoteLaneVisit,
                llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
                llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  return analyzeConstrainedCall(Low, Index, Execution, Classifier,
                                DispatchConstraint::Fallback, NoteLaneVisit,
                                NoteOperationVisit, NoteReferenceVisit);
}

llvm::Expected<std::set<uint64_t>> nonPayableGuardReads(
    const ProducerIndex &Index, const DefiniteExecutionIndex &Execution,
    SemanticClassifier &Classifier, const std::set<MedStateLaneID> &Lanes,
    llvm::function_ref<llvm::Error(uint64_t)> NoteOperationVisit,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  std::set<uint64_t> Reads;
  std::set<uint64_t> Blocks;
  for (MedStateLaneID Lane : Lanes) {
    if (llvm::Error Error = NoteReferenceVisit(kEntryPC))
      return std::move(Error);
    if (const MedStateLane *StateLane = Execution.lane(Lane))
      Blocks.insert(StateLane->LowLane.BlockPC);
  }
  for (uint64_t BlockPC : Blocks) {
    if (llvm::Error Error = NoteReferenceVisit(BlockPC))
      return std::move(Error);
    const MedBlock *Block = Index.block(BlockPC);
    if (!Block)
      continue;
    for (const MedOperation &Operation : Block->Operations) {
      if (llvm::Error Error = NoteOperationVisit(Operation.PC))
        return std::move(Error);
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Executes =
          Execution.executesInAny(Operation, Lanes, NoteReferenceVisit);
      if (!Executes)
        return Executes.takeError();
      if (!*Executes || Operation.Op != Opcode::JUMPI ||
          Operation.Inputs.size() != 2)
        continue;
      if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
        return std::move(Error);
      auto Classified = Classifier.classify(Operation.Inputs[1]);
      if (!Classified)
        return Classified.takeError();
      const SemanticValue Condition = std::move(*Classified);
      if (Condition.Kind != SemanticKind::IsZeroCallValue)
        continue;
      bool SawExecutingLane = false;
      bool EveryFalsePathRejects = true;
      for (MedStateLaneID Lane : Operation.ExecutingLanes) {
        if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
          return std::move(Error);
        if (!Execution.isReachable(Lane) || !Lanes.contains(Lane))
          continue;
        SawExecutingLane = true;
        auto Targets = Execution.successors(Lane, EdgeKind::ConditionalFalse,
                                            std::nullopt, NoteReferenceVisit);
        if (!Targets)
          return Targets.takeError();
        if (Targets->empty()) {
          EveryFalsePathRejects = false;
          continue;
        }
        for (MedStateLaneID Target : *Targets) {
          if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
            return std::move(Error);
          auto Rejects = endsInRevert(Index, Execution, Target,
                                      NoteOperationVisit, NoteReferenceVisit);
          if (!Rejects)
            return Rejects.takeError();
          if (!*Rejects)
            EveryFalsePathRejects = false;
        }
      }
      if (SawExecutingLane && EveryFalsePathRejects)
        for (uint64_t PC : Condition.OriginPCs) {
          if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
            return std::move(Error);
          Reads.insert(PC);
        }
    }
  }
  return Reads;
}

} // namespace neverd::evm::detail
