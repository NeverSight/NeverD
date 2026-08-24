//===- EVMHighAnalysisValues.cpp - EVM MedIR value classification -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace neverd::evm::detail {
namespace {

bool sameExpression(const SemanticValue &LHS, const SemanticValue &RHS) {
  if (LHS.Kind != RHS.Kind)
    return false;
  switch (LHS.Kind) {
  case SemanticKind::Constant:
    return LHS.Word == RHS.Word;
  case SemanticKind::SelectorXor:
  case SemanticKind::SelectorEquality:
    return LHS.Selector == RHS.Selector;
  case SemanticKind::ExternalOutcome:
  case SemanticKind::IsZeroExternalOutcome:
    return LHS.OriginPCs == RHS.OriginPCs;
  case SemanticKind::Unknown:
    return false;
  case SemanticKind::CalldataWordZero:
  case SemanticKind::SelectorWord:
  case SemanticKind::CallValue:
  case SemanticKind::IsZeroCallValue:
  case SemanticKind::CalldataSize:
  case SemanticKind::IsZeroCalldataSize:
  case SemanticKind::CalldataSizeBelowSelector:
  case SemanticKind::CalldataSizeAtLeastSelector:
    return true;
  }
  return false;
}

void mergeOrigins(std::vector<uint64_t> &Destination,
                  llvm::ArrayRef<uint64_t> Source) {
  Destination.insert(Destination.end(), Source.begin(), Source.end());
  llvm::sort(Destination);
  Destination.erase(std::unique(Destination.begin(), Destination.end()),
                    Destination.end());
}

llvm::Expected<bool>
containsLane(llvm::ArrayRef<MedStateLaneID> Lanes, MedStateLaneID Target,
             uint64_t PC,
             llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) {
  size_t First = 0;
  size_t Count = Lanes.size();
  while (Count != 0) {
    const size_t Step = Count / 2;
    const size_t Index = First + Step;
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return std::move(Error);
    if (Lanes[Index] < Target) {
      First = Index + 1;
      Count -= Step + 1;
    } else {
      Count = Step;
    }
  }
  if (First == Lanes.size())
    return false;
  if (llvm::Error Error = NoteReferenceVisit(PC))
    return std::move(Error);
  return Lanes[First] == Target;
}

} // namespace

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  std::string Result = Digits.str().str();
  if (Result.size() < MinDigits)
    Result.insert(Result.begin(), MinDigits - Result.size(), '0');
  return Result;
}

std::string selectorHex(uint32_t Selector) {
  return wordHexDigits(llvm::APInt(kSelectorBits, Selector),
                       kSelectorHexDigits);
}

std::optional<uint64_t> constantWord(const MedValue *Value) {
  if (!Value || !Value->Constant ||
      Value->Constant->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Value->Constant->getZExtValue();
}

DefiniteExecutionIndex::DefiniteExecutionIndex(const EVMLowIR &Low,
                                               const EVMMedIR &Med)
    : Low(Low), Med(Med) {
  for (const MedStateLane &Lane : Med.StateLanes) {
    MedByLowLane.emplace(Lane.LowLane, Lane.ID);
    if (Lane.Evidence == Reachability::Reachable)
      ReachableByBlock[Lane.LowLane.BlockPC].push_back(Lane.ID);
  }
  for (auto &[BlockPC, Lanes] : ReachableByBlock) {
    (void)BlockPC;
    llvm::sort(Lanes);
    Lanes.erase(std::unique(Lanes.begin(), Lanes.end()), Lanes.end());
  }
  for (const LowLaneTransition &Transition : Low.LaneTransitions) {
    const auto Source = medLane(Transition.Source);
    if (!Source)
      continue;
    if (Transition.Evidence == Reachability::MayReachable) {
      MayTransitionSources.insert(*Source);
      continue;
    }
    if (!Transition.Target || !isReachable(*Source))
      continue;
    const auto Target = medLane(*Transition.Target);
    if (Target && isReachable(*Target))
      Outgoing[*Source].push_back({*Target, Transition.Kind});
  }
  for (auto &[Source, Transitions] : Outgoing) {
    (void)Source;
    llvm::sort(Transitions);
    Transitions.erase(std::unique(Transitions.begin(), Transitions.end()),
                      Transitions.end());
    for (const IndexedTransition &Transition : Transitions)
      Incoming[Transition.Target].push_back(Source);
  }
  for (auto &[Target, Sources] : Incoming) {
    (void)Target;
    llvm::sort(Sources);
    Sources.erase(std::unique(Sources.begin(), Sources.end()), Sources.end());
  }
}

const MedStateLane *DefiniteExecutionIndex::lane(MedStateLaneID ID) const {
  const MedStateLane *Lane = Med.findStateLane(ID);
  return Lane && Lane->ID == ID ? Lane : nullptr;
}

std::optional<MedStateLaneID>
DefiniteExecutionIndex::medLane(LowStateLaneID ID) const {
  const auto It = MedByLowLane.find(ID);
  return It == MedByLowLane.end() ? std::nullopt
                                  : std::optional<MedStateLaneID>(It->second);
}

bool DefiniteExecutionIndex::isReachable(MedStateLaneID ID) const {
  const MedStateLane *StateLane = lane(ID);
  return StateLane && StateLane->Evidence == Reachability::Reachable;
}

bool DefiniteExecutionIndex::isEligible(const MedOperation &Operation) const {
  return llvm::any_of(Operation.ExecutingLanes,
                      [&](MedStateLaneID ID) { return isReachable(ID); });
}

llvm::Expected<bool> DefiniteExecutionIndex::isEligible(
    const MedOperation &Operation,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const {
  for (MedStateLaneID ID : Operation.ExecutingLanes) {
    if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
      return std::move(Error);
    if (isReachable(ID))
      return true;
  }
  return false;
}

bool DefiniteExecutionIndex::executesIn(const MedOperation &Operation,
                                        MedStateLaneID Lane) const {
  return isReachable(Lane) &&
         std::binary_search(Operation.ExecutingLanes.begin(),
                            Operation.ExecutingLanes.end(), Lane);
}

llvm::Expected<bool> DefiniteExecutionIndex::executesIn(
    const MedOperation &Operation, MedStateLaneID Lane,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const {
  if (!isReachable(Lane))
    return false;
  return containsLane(Operation.ExecutingLanes, Lane, Operation.PC,
                      NoteReferenceVisit);
}

bool DefiniteExecutionIndex::faultsIn(const MedOperation &Operation,
                                      MedStateLaneID Lane) const {
  return isReachable(Lane) &&
         std::binary_search(Operation.FaultingLanes.begin(),
                            Operation.FaultingLanes.end(), Lane);
}

llvm::Expected<bool> DefiniteExecutionIndex::faultsIn(
    const MedOperation &Operation, MedStateLaneID Lane,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const {
  if (!isReachable(Lane))
    return false;
  return containsLane(Operation.FaultingLanes, Lane, Operation.PC,
                      NoteReferenceVisit);
}

bool DefiniteExecutionIndex::executesInAny(
    const MedOperation &Operation,
    const std::set<MedStateLaneID> &Lanes) const {
  return llvm::any_of(Operation.ExecutingLanes, [&](MedStateLaneID Lane) {
    return isReachable(Lane) && Lanes.contains(Lane);
  });
}

llvm::Expected<bool> DefiniteExecutionIndex::executesInAny(
    const MedOperation &Operation, const std::set<MedStateLaneID> &Lanes,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const {
  for (MedStateLaneID Lane : Operation.ExecutingLanes) {
    if (llvm::Error Error = NoteReferenceVisit(Operation.PC))
      return std::move(Error);
    if (isReachable(Lane) && Lanes.contains(Lane))
      return true;
  }
  return false;
}

llvm::SmallVector<MedStateLaneID, 4>
DefiniteExecutionIndex::reachableLanesAt(uint64_t BlockPC) const {
  const auto It = ReachableByBlock.find(BlockPC);
  if (It == ReachableByBlock.end())
    return {};
  return {It->second.begin(), It->second.end()};
}

llvm::SmallVector<MedStateLaneID, 4>
DefiniteExecutionIndex::successors(MedStateLaneID Source,
                                   std::optional<EdgeKind> Kind,
                                   std::optional<uint64_t> TargetPC) const {
  const MedStateLane *SourceLane = lane(Source);
  if (!SourceLane || SourceLane->Evidence != Reachability::Reachable)
    return {};

  llvm::SmallVector<MedStateLaneID, 4> Result;
  const auto It = Outgoing.find(Source);
  if (It == Outgoing.end())
    return Result;
  for (const IndexedTransition &Transition : It->second) {
    if (Kind && Transition.Kind != *Kind)
      continue;
    const MedStateLane *Target = lane(Transition.Target);
    if (TargetPC && (!Target || Target->LowLane.BlockPC != *TargetPC))
      continue;
    Result.push_back(Transition.Target);
  }
  return Result;
}

llvm::Expected<llvm::SmallVector<MedStateLaneID, 4>>
DefiniteExecutionIndex::successors(
    MedStateLaneID Source, std::optional<EdgeKind> Kind,
    std::optional<uint64_t> TargetPC,
    llvm::function_ref<llvm::Error(uint64_t)> NoteReferenceVisit) const {
  const MedStateLane *SourceLane = lane(Source);
  if (!SourceLane || SourceLane->Evidence != Reachability::Reachable)
    return llvm::SmallVector<MedStateLaneID, 4>{};

  llvm::SmallVector<MedStateLaneID, 4> Result;
  const auto It = Outgoing.find(Source);
  if (It == Outgoing.end())
    return Result;
  for (const IndexedTransition &Transition : It->second) {
    if (llvm::Error Error = NoteReferenceVisit(SourceLane->LowLane.BlockPC))
      return std::move(Error);
    if (Kind && Transition.Kind != *Kind)
      continue;
    const MedStateLane *Target = lane(Transition.Target);
    if (TargetPC && (!Target || Target->LowLane.BlockPC != *TargetPC))
      continue;
    Result.push_back(Transition.Target);
  }
  return Result;
}

llvm::SmallVector<MedStateLaneID, 4> DefiniteExecutionIndex::operationTargets(
    const MedOperation &Operation, EdgeKind Kind,
    std::optional<uint64_t> TargetPC) const {
  llvm::SmallVector<MedStateLaneID, 4> Result;
  for (MedStateLaneID Lane : Operation.ExecutingLanes) {
    if (!isReachable(Lane))
      continue;
    llvm::append_range(Result, successors(Lane, Kind, TargetPC));
  }
  llvm::sort(Result);
  Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
  return Result;
}

llvm::SmallVector<MedStateLaneID, 4>
DefiniteExecutionIndex::predecessors(MedStateLaneID Target) const {
  const auto It = Incoming.find(Target);
  if (It == Incoming.end())
    return {};
  return {It->second.begin(), It->second.end()};
}

bool DefiniteExecutionIndex::hasMayTransitionFrom(MedStateLaneID Source) const {
  return MayTransitionSources.contains(Source);
}

ProducerIndex::ProducerIndex(const EVMMedIR &Med)
    : Producers(Med.Values.size(), nullptr) {
  build(Med);
}

void ProducerIndex::fail(uint64_t PC) {
  if (!Valid)
    return;
  Valid = false;
  ErrorPC = PC;
}

void ProducerIndex::build(const EVMMedIR &Med) {
  for (size_t I = 0; I < Med.Values.size(); ++I) {
    const MedValue &Value = Med.Values[I];
    if (Value.ID != I ||
        (Value.Constant && Value.Constant->getBitWidth() != kWordBits)) {
      fail(Value.PC);
      return;
    }
    for (ValueID Input : Value.Inputs)
      if (Input >= Med.Values.size()) {
        fail(Value.PC);
        return;
      }
  }

  for (const MedBlock &Block : Med.Blocks) {
    if (!Blocks.emplace(Block.StartPC, &Block).second) {
      fail(Block.StartPC);
      return;
    }
    for (const MedOperation &Operation : Block.Operations) {
      if (!Operations.emplace(Operation.PC, std::make_pair(&Block, &Operation))
               .second) {
        fail(Operation.PC);
        return;
      }
      for (ValueID Input : Operation.Inputs)
        if (Input >= Med.Values.size()) {
          fail(Operation.PC);
          return;
        }
      for (ValueID Output : Operation.Outputs) {
        if (Output >= Med.Values.size() || Producers[Output] ||
            Med.Values[Output].Kind == ValueKind::Phi ||
            Med.Values[Output].Inputs != Operation.Inputs) {
          fail(Operation.PC);
          return;
        }
        Producers[Output] = &Operation;
      }
    }
  }

  for (const MedValue &Value : Med.Values)
    if (Value.Kind == ValueKind::Instruction && !Producers[Value.ID]) {
      fail(Value.PC);
      return;
    }
}

llvm::Expected<SemanticValue> SemanticClassifier::classify(ValueID Root) {
  if (Root >= Med.Values.size())
    return SemanticValue{};
  if (States[Root] == VisitState::Complete)
    return Results[Root];

  struct Frame {
    ValueID ID = 0;
    llvm::SmallVector<ValueID, kMaxALUStackPops> Dependencies;
    size_t NextDependency = 0;
    bool HasCycle = false;
  };

  std::vector<Frame> Stack;
  const auto Push = [&](ValueID ID) -> llvm::Error {
    Frame Next;
    Next.ID = ID;
    auto Dependencies = dependencies(ID);
    if (!Dependencies)
      return Dependencies.takeError();
    Next.Dependencies = std::move(*Dependencies);
    States[ID] = VisitState::Active;
    Stack.push_back(std::move(Next));
    return llvm::Error::success();
  };
  if (llvm::Error Error = Push(Root))
    return std::move(Error);

  while (!Stack.empty()) {
    Frame &Current = Stack.back();
    if (Current.NextDependency < Current.Dependencies.size()) {
      const ValueID Dependency = Current.Dependencies[Current.NextDependency++];
      if (States[Dependency] == VisitState::Unseen) {
        if (llvm::Error Error = Push(Dependency))
          return std::move(Error);
        continue;
      }
      if (States[Dependency] == VisitState::Active)
        Current.HasCycle = true;
      continue;
    }

    if (Current.HasCycle) {
      Results[Current.ID] = SemanticValue{};
    } else {
      auto Evaluated = evaluate(Current.ID);
      if (!Evaluated)
        return Evaluated.takeError();
      Results[Current.ID] = std::move(*Evaluated);
    }
    States[Current.ID] = VisitState::Complete;
    Stack.pop_back();
  }
  return Results[Root];
}

llvm::Expected<llvm::SmallVector<ValueID, kMaxALUStackPops>>
SemanticClassifier::dependencies(ValueID ID) const {
  if (llvm::Error Error = NoteReferenceVisit(Med.Values[ID].PC))
    return std::move(Error);
  const MedValue &Value = Med.Values[ID];
  if (Value.Kind == ValueKind::Instruction) {
    const MedOperation *Operation = Index.producer(ID);
    if (!Operation)
      return llvm::SmallVector<ValueID, kMaxALUStackPops>{};
    auto Eligible = Execution.isEligible(*Operation, NoteReferenceVisit);
    if (!Eligible)
      return Eligible.takeError();
    if (!*Eligible)
      return llvm::SmallVector<ValueID, kMaxALUStackPops>{};
  }
  if (Value.Constant || Value.Kind == ValueKind::Unknown)
    return llvm::SmallVector<ValueID, kMaxALUStackPops>{};
  if (Value.Kind == ValueKind::Phi) {
    if (llvm::Error Error = noteReferences(Value.Inputs.size(), Value.PC))
      return std::move(Error);
    llvm::SmallVector<ValueID, kMaxALUStackPops> Result;
    llvm::append_range(Result, Value.Inputs);
    return Result;
  }
  const MedOperation *Operation = Index.producer(ID);
  if (!Operation)
    return llvm::SmallVector<ValueID, kMaxALUStackPops>{};
  if (llvm::Error Error =
          noteReferences(Operation->Inputs.size(), Operation->PC))
    return std::move(Error);
  llvm::SmallVector<ValueID, kMaxALUStackPops> Result;
  llvm::append_range(Result, Operation->Inputs);
  return Result;
}

llvm::Error SemanticClassifier::noteReferences(size_t Count,
                                               uint64_t PC) const {
  for (size_t Index = 0; Index < Count; ++Index)
    if (llvm::Error Error = NoteReferenceVisit(PC))
      return Error;
  return llvm::Error::success();
}

const SemanticValue &SemanticClassifier::input(const MedOperation &Operation,
                                               size_t Index) const {
  return Results[Operation.Inputs[Index]];
}

llvm::Expected<SemanticValue>
SemanticClassifier::evaluatePhi(const MedValue &Phi) const {
  if (Phi.PhiIncomings.empty())
    return SemanticValue{};
  std::optional<SemanticValue> Common;
  for (const MedPhiIncoming &Incoming : Phi.PhiIncomings) {
    if (llvm::Error Error = NoteReferenceVisit(Phi.PC))
      return std::move(Error);
    // Conservative lanes are useful to the CFG but cannot prove a HighIR
    // expression. Downgrading the whole merge also avoids selecting a
    // convenient exact incoming from a path-correlated phi.
    if (!Execution.isReachable(Incoming.SourceLane))
      return SemanticValue{};
    const SemanticValue &Candidate = Results[Incoming.Value];
    if (!Common) {
      Common = Candidate;
      if (Common->Kind == SemanticKind::Unknown)
        return SemanticValue{};
      continue;
    }
    if (!sameExpression(*Common, Candidate))
      return SemanticValue{};
    mergeOrigins(Common->OriginPCs, Candidate.OriginPCs);
  }
  return Common.value_or(SemanticValue{});
}

bool SemanticClassifier::isSelectorMask(const SemanticValue &Value) const {
  return Value.Kind == SemanticKind::Constant &&
         Value.Word == llvm::APInt::getLowBitsSet(kWordBits, kSelectorBits);
}

namespace {

std::optional<uint32_t> matchSelectorConstant(const SemanticValue &Constant,
                                              const SemanticValue &Selector) {
  if (Constant.Kind != SemanticKind::Constant ||
      Constant.Word.getActiveBits() > kSelectorBits ||
      Selector.Kind != SemanticKind::SelectorWord)
    return std::nullopt;
  return static_cast<uint32_t>(Constant.Word.getZExtValue());
}

bool isZeroConstant(const SemanticValue &Value) {
  return Value.Kind == SemanticKind::Constant && Value.Word.isZero();
}

bool isSelectorByteCount(const SemanticValue &Value) {
  return Value.Kind == SemanticKind::Constant &&
         Value.Word == llvm::APInt(kWordBits, kSelectorBytes);
}

} // namespace

llvm::Expected<SemanticValue>
SemanticClassifier::evaluateInstruction(ValueID ID) const {
  const MedOperation *Operation = Index.producer(ID);
  if (!Operation)
    return SemanticValue{};
  auto Eligible = Execution.isEligible(*Operation, NoteReferenceVisit);
  if (!Eligible)
    return Eligible.takeError();
  if (!*Eligible)
    return SemanticValue{};
  if (llvm::Error Error =
          noteReferences(Operation->Inputs.size(), Operation->PC))
    return std::move(Error);
  if ((evm::isDup(Operation->Op) || evm::isDeepDup(Operation->Op)) &&
      Operation->Inputs.size() == 1)
    return input(*Operation, 0);
  if (Operation->Op == Opcode::CALLDATALOAD && Operation->Inputs.size() == 1) {
    const SemanticValue &Offset = input(*Operation, 0);
    if (Offset.Kind == SemanticKind::Constant && Offset.Word.isZero())
      return SemanticValue::simple(SemanticKind::CalldataWordZero);
    return SemanticValue{};
  }
  if (Operation->Op == Opcode::SHR && Operation->Inputs.size() == 2) {
    const SemanticValue &Shift = input(*Operation, 0);
    const SemanticValue &Word = input(*Operation, 1);
    if (Shift.Kind == SemanticKind::Constant &&
        Shift.Word == llvm::APInt(kWordBits, kWordBits - kSelectorBits) &&
        Word.Kind == SemanticKind::CalldataWordZero)
      return SemanticValue::simple(SemanticKind::SelectorWord);
    return SemanticValue{};
  }
  if (Operation->Op == Opcode::AND && Operation->Inputs.size() == 2) {
    const SemanticValue &First = input(*Operation, 0);
    const SemanticValue &Second = input(*Operation, 1);
    if ((First.Kind == SemanticKind::SelectorWord && isSelectorMask(Second)) ||
        (Second.Kind == SemanticKind::SelectorWord && isSelectorMask(First)))
      return SemanticValue::simple(SemanticKind::SelectorWord);
    return SemanticValue{};
  }
  if (Operation->Op == Opcode::XOR && Operation->Inputs.size() == 2) {
    const SemanticValue &First = input(*Operation, 0);
    const SemanticValue &Second = input(*Operation, 1);
    if (const auto Selector = matchSelectorConstant(First, Second))
      return SemanticValue::selectorXor(*Selector);
    if (const auto Selector = matchSelectorConstant(Second, First))
      return SemanticValue::selectorXor(*Selector);
    return SemanticValue{};
  }
  if (Operation->Op == Opcode::CALLVALUE && Operation->Inputs.empty())
    return SemanticValue::callValue(SemanticKind::CallValue, Operation->PC);
  if (Operation->Op == Opcode::CALLDATASIZE && Operation->Inputs.empty())
    return SemanticValue::simple(SemanticKind::CalldataSize);
  if (const auto Info = assignedOpcodeInfo(Operation->Op);
      Info && Operation->Outputs.size() == 1 &&
      (Info->Effect == EffectKind::ExternalCall ||
       Info->Effect == EffectKind::Create))
    return SemanticValue::externalOutcome(SemanticKind::ExternalOutcome,
                                          Operation->PC);
  if (Operation->Op == Opcode::ISZERO && Operation->Inputs.size() == 1) {
    SemanticValue Operand = input(*Operation, 0);
    if (Operand.Kind == SemanticKind::CallValue) {
      Operand.Kind = SemanticKind::IsZeroCallValue;
      return Operand;
    }
    if (Operand.Kind == SemanticKind::CalldataSize)
      return SemanticValue::simple(SemanticKind::IsZeroCalldataSize);
    if (Operand.Kind == SemanticKind::ExternalOutcome) {
      Operand.Kind = SemanticKind::IsZeroExternalOutcome;
      return Operand;
    }
    if (Operand.Kind == SemanticKind::IsZeroExternalOutcome) {
      Operand.Kind = SemanticKind::ExternalOutcome;
      return Operand;
    }
    if (Operand.Kind == SemanticKind::SelectorXor)
      return SemanticValue::selectorEquality(Operand.Selector);
    if (Operand.Kind == SemanticKind::CalldataSizeBelowSelector)
      return SemanticValue::simple(SemanticKind::CalldataSizeAtLeastSelector);
    if (Operand.Kind == SemanticKind::CalldataSizeAtLeastSelector)
      return SemanticValue::simple(SemanticKind::CalldataSizeBelowSelector);
    return SemanticValue{};
  }
  if (Operation->Op == Opcode::EQ && Operation->Inputs.size() == 2) {
    const SemanticValue &First = input(*Operation, 0);
    const SemanticValue &Second = input(*Operation, 1);
    if (const auto Selector = matchSelectorConstant(First, Second))
      return SemanticValue::selectorEquality(*Selector);
    if (const auto Selector = matchSelectorConstant(Second, First))
      return SemanticValue::selectorEquality(*Selector);
    const auto MatchZero = [](const SemanticValue &Value,
                              const SemanticValue &Zero) {
      if (!isZeroConstant(Zero))
        return SemanticValue{};
      if (Value.Kind == SemanticKind::CallValue) {
        SemanticValue Result = Value;
        Result.Kind = SemanticKind::IsZeroCallValue;
        return Result;
      }
      if (Value.Kind == SemanticKind::CalldataSize)
        return SemanticValue::simple(SemanticKind::IsZeroCalldataSize);
      if (Value.Kind == SemanticKind::ExternalOutcome) {
        SemanticValue Result = Value;
        Result.Kind = SemanticKind::IsZeroExternalOutcome;
        return Result;
      }
      if (Value.Kind == SemanticKind::IsZeroExternalOutcome) {
        SemanticValue Result = Value;
        Result.Kind = SemanticKind::ExternalOutcome;
        return Result;
      }
      if (Value.Kind == SemanticKind::SelectorXor)
        return SemanticValue::selectorEquality(Value.Selector);
      return SemanticValue{};
    };
    if (SemanticValue Match = MatchZero(First, Second);
        Match.Kind != SemanticKind::Unknown)
      return Match;
    if (SemanticValue Match = MatchZero(Second, First);
        Match.Kind != SemanticKind::Unknown)
      return Match;
    return SemanticValue{};
  }
  if ((Operation->Op == Opcode::LT || Operation->Op == Opcode::GT) &&
      Operation->Inputs.size() == 2) {
    const SemanticValue &First = input(*Operation, 0);
    const SemanticValue &Second = input(*Operation, 1);
    if ((Operation->Op == Opcode::LT &&
         First.Kind == SemanticKind::CalldataSize &&
         isSelectorByteCount(Second)) ||
        (Operation->Op == Opcode::GT && isSelectorByteCount(First) &&
         Second.Kind == SemanticKind::CalldataSize))
      return SemanticValue::simple(SemanticKind::CalldataSizeBelowSelector);
    return SemanticValue{};
  }
  return SemanticValue{};
}

llvm::Expected<SemanticValue> SemanticClassifier::evaluate(ValueID ID) const {
  if (llvm::Error Error = NoteReferenceVisit(Med.Values[ID].PC))
    return std::move(Error);
  const MedValue &Value = Med.Values[ID];
  if (Value.Kind == ValueKind::Instruction) {
    const MedOperation *Operation = Index.producer(ID);
    if (!Operation)
      return SemanticValue{};
    auto Eligible = Execution.isEligible(*Operation, NoteReferenceVisit);
    if (!Eligible)
      return Eligible.takeError();
    if (!*Eligible)
      return SemanticValue{};
  }
  if (Value.Constant)
    return SemanticValue::constant(*Value.Constant);
  if (Value.Kind == ValueKind::Phi)
    return evaluatePhi(Value);
  if (Value.Kind == ValueKind::Instruction)
    return evaluateInstruction(ID);
  return SemanticValue{};
}

} // namespace neverd::evm::detail
