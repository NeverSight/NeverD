//===- EVMMedAnalysis.cpp - EVM stack SSA and value analysis ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMMedAnalysis.h"

#include "EVMMedConstantAnalysis.h"
#include "EVMMedIRVerifier.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::evm {
namespace {

bool faultConsumesInstructionInputs(LowFaultKind Kind) {
  switch (Kind) {
#define EVM_LOW_FAULT_KIND(ID, CONSUMES_INPUTS)                                \
  case LowFaultKind::ID:                                                       \
    return CONSUMES_INPUTS;
#include "neverd/evm/analysis/EVMLowFaultKinds.def"
  }
  llvm_unreachable("validated LowIR fault kind");
}

using Constant = std::optional<llvm::APInt>;
using MedStack = std::vector<ValueID>;

llvm::Error medAnalysisError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

llvm::Error medOptionError(llvm::Twine Name) {
  return llvm::make_error<llvm::StringError>("evm: " + Name +
                                                 " must be greater than zero",
                                             llvm::inconvertibleErrorCode());
}

llvm::Error validateMedAnalysisOptionsImpl(const AnalyzeOptions &Options) {
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)                      \
  if (Options.NAME == 0)                                                       \
    return medOptionError(#NAME);
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
  return llvm::Error::success();
}

Constant evaluatePure(Opcode Op, llvm::ArrayRef<Constant> Inputs, uint64_t PC,
                      size_t CodeSize) {
  if (Op == Opcode::PC)
    return llvm::APInt(kWordBits, PC);
  if (Op == Opcode::CODESIZE)
    return llvm::APInt(kWordBits, CodeSize);
  const auto Info = assignedOpcodeInfo(Op);
  if (!Info || !isALU(*Info))
    return std::nullopt;
  llvm::SmallVector<llvm::APInt, kMaxALUStackPops> ConcreteInputs;
  ConcreteInputs.reserve(Inputs.size());
  for (const Constant &Input : Inputs) {
    if (!Input)
      return std::nullopt;
    ConcreteInputs.push_back(*Input);
  }
  return evaluateALU(Op, ConcreteInputs);
}

enum class DefinitionKind : uint8_t { None, Phi, Operation };

struct Definition {
  DefinitionKind Kind = DefinitionKind::None;
  const MedOperation *Operation = nullptr;
};

struct User {
  ValueID Result = 0;
  DefinitionKind Kind = DefinitionKind::None;

  friend bool operator<(const User &Left, const User &Right) {
    return std::tie(Left.Result, Left.Kind) <
           std::tie(Right.Result, Right.Kind);
  }

  friend bool operator==(const User &, const User &) = default;
};

struct DataflowGraph {
  std::vector<Definition> Definitions;
  std::vector<std::vector<User>> Users;
};

llvm::Error invalidValueID(ValueID ID) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "MedIR value ID %u is out of range", ID);
}

llvm::Expected<DataflowGraph> buildDataflowGraph(const EVMMedIR &Med) {
  DataflowGraph Graph;
  Graph.Definitions.resize(Med.Values.size());
  Graph.Users.resize(Med.Values.size());

  const auto IsValid = [&](ValueID ID) { return ID < Med.Values.size(); };
  for (size_t Index = 0; Index < Med.Values.size(); ++Index) {
    const MedValue &Value = Med.Values[Index];
    if (Value.ID != Index)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR value ID %u does not match its table index %zu", Value.ID,
          Index);
    if (Value.Constant && Value.Constant->getBitWidth() != kWordBits)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR value %u has a non-EVM constant width", Value.ID);
    for (ValueID Input : Value.Inputs)
      if (!IsValid(Input))
        return invalidValueID(Input);
    if (Value.Kind == ValueKind::Phi) {
      if (Value.Inputs.size() != Value.PhiIncomings.size())
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "MedIR phi value %u has incomplete lane provenance", Value.ID);
      Graph.Definitions[Index].Kind = DefinitionKind::Phi;
    } else if (!Value.PhiIncomings.empty()) {
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR non-phi value %u has phi lane provenance", Value.ID);
    }
  }

  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOperation &Operation : Block.Operations) {
      for (ValueID Input : Operation.Inputs)
        if (!IsValid(Input))
          return invalidValueID(Input);
      for (ValueID Output : Operation.Outputs) {
        if (!IsValid(Output))
          return invalidValueID(Output);
        Definition &Def = Graph.Definitions[Output];
        if (Def.Kind != DefinitionKind::None)
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              "MedIR value %u has multiple definitions", Output);
        Def = {DefinitionKind::Operation, &Operation};
        if (Med.Values[Output].Inputs != Operation.Inputs)
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              "MedIR value %u disagrees with its operation inputs", Output);
      }
    }
  }

  for (const MedValue &Value : Med.Values) {
    const Definition &Def = Graph.Definitions[Value.ID];
    if (Value.Kind == ValueKind::Instruction &&
        Def.Kind != DefinitionKind::Operation)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR instruction value %u has no defining operation", Value.ID);
    if (Value.Kind == ValueKind::Phi)
      for (ValueID Input : Value.Inputs)
        Graph.Users[Input].push_back({Value.ID, DefinitionKind::Phi});
  }
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOperation &Operation : Block.Operations)
      for (ValueID Input : Operation.Inputs)
        for (ValueID Output : Operation.Outputs)
          Graph.Users[Input].push_back({Output, DefinitionKind::Operation});

  for (std::vector<User> &Users : Graph.Users) {
    llvm::sort(Users);
    Users.erase(std::unique(Users.begin(), Users.end()), Users.end());
  }
  return Graph;
}

enum class LatticeKind : uint8_t {
  Uninitialized,
  Constant,
  Overdefined,
};

struct LatticeValue {
  LatticeKind Kind = LatticeKind::Uninitialized;
  llvm::APInt Word = llvm::APInt(kWordBits, 0);

  static LatticeValue constant(const llvm::APInt &Value) {
    return {LatticeKind::Constant, Value};
  }

  static LatticeValue overdefined() {
    return {LatticeKind::Overdefined, llvm::APInt(kWordBits, 0)};
  }
};

LatticeValue evaluatePhi(const MedValue &Phi,
                         llvm::ArrayRef<LatticeValue> States) {
  std::optional<llvm::APInt> Common;
  for (ValueID Input : Phi.Inputs) {
    const LatticeValue &State = States[Input];
    if (State.Kind == LatticeKind::Overdefined)
      return LatticeValue::overdefined();
    // This optimistic phase permits a seeded loop to initialize. After the
    // worklist stabilizes, all remaining Uninitialized values are made
    // Overdefined and their users are revisited, so no final constant can omit
    // a feasible incoming.
    if (State.Kind == LatticeKind::Uninitialized)
      continue;
    if (Common && *Common != State.Word)
      return LatticeValue::overdefined();
    Common = State.Word;
  }
  return Common ? LatticeValue::constant(*Common) : LatticeValue{};
}

LatticeValue evaluateOperation(const MedOperation &Operation,
                               llvm::ArrayRef<LatticeValue> States,
                               size_t CodeSize) {
  if (Operation.Op == Opcode::PC)
    return LatticeValue::constant(llvm::APInt(kWordBits, Operation.PC));
  if (Operation.Op == Opcode::CODESIZE)
    return LatticeValue::constant(llvm::APInt(kWordBits, CodeSize));
  if (evm::isDup(Operation.Op) || evm::isDeepDup(Operation.Op)) {
    if (Operation.Inputs.size() != 1)
      return LatticeValue::overdefined();
    return States[Operation.Inputs.front()];
  }

  const auto Info = assignedOpcodeInfo(Operation.Op);
  if (!Info || !isALU(*Info) || Operation.Inputs.size() != Info->StackPops)
    return LatticeValue::overdefined();

  llvm::SmallVector<Constant, kMaxALUStackPops> Inputs;
  Inputs.reserve(Operation.Inputs.size());
  for (ValueID Input : Operation.Inputs) {
    const LatticeValue &State = States[Input];
    if (State.Kind == LatticeKind::Overdefined)
      return LatticeValue::overdefined();
    if (State.Kind == LatticeKind::Uninitialized)
      return {};
    Inputs.emplace_back(State.Word);
  }
  if (Constant Result =
          evaluatePure(Operation.Op, Inputs, Operation.PC, CodeSize))
    return LatticeValue::constant(*Result);
  return LatticeValue::overdefined();
}

LatticeValue evaluateValue(const EVMMedIR &Med, const DataflowGraph &Graph,
                           llvm::ArrayRef<LatticeValue> States, ValueID ID,
                           size_t CodeSize) {
  const MedValue &Value = Med.Values[ID];
  if (Value.Kind == ValueKind::Constant)
    return Value.Constant ? LatticeValue::constant(*Value.Constant)
                          : LatticeValue::overdefined();
  if (Value.Kind == ValueKind::Unknown)
    return LatticeValue::overdefined();
  if (Value.Kind == ValueKind::Phi)
    return evaluatePhi(Value, States);
  const Definition &Def = Graph.Definitions[ID];
  if (Def.Kind != DefinitionKind::Operation || !Def.Operation)
    return LatticeValue::overdefined();
  return evaluateOperation(*Def.Operation, States, CodeSize);
}

bool mergeState(LatticeValue &Current, const LatticeValue &Candidate) {
  if (Current.Kind == LatticeKind::Overdefined ||
      Candidate.Kind == LatticeKind::Uninitialized)
    return false;
  if (Current.Kind == LatticeKind::Uninitialized) {
    Current = Candidate;
    return true;
  }
  if (Candidate.Kind == LatticeKind::Overdefined ||
      (Candidate.Kind == LatticeKind::Constant &&
       Current.Word != Candidate.Word)) {
    Current = LatticeValue::overdefined();
    return true;
  }
  return false;
}

llvm::Expected<std::vector<Constant>>
computeConstants(const EVMMedIR &Med, size_t CodeSize,
                 size_t MaxWorklistUpdates) {
  auto Graph = buildDataflowGraph(Med);
  if (!Graph)
    return Graph.takeError();

  std::vector<LatticeValue> States(Med.Values.size());
  std::deque<ValueID> Worklist;
  std::vector<bool> InWorklist(Med.Values.size(), false);
  size_t WorklistUpdates = 0;
  const auto Enqueue = [&](ValueID ID) -> llvm::Error {
    if (InWorklist[ID])
      return llvm::Error::success();
    if (WorklistUpdates >= MaxWorklistUpdates)
      return medAnalysisError(
          Med.Values[ID].PC, "MedIR worklist update limit " +
                                 llvm::Twine(MaxWorklistUpdates) + " exceeded");
    InWorklist[ID] = true;
    Worklist.push_back(ID);
    ++WorklistUpdates;
    return llvm::Error::success();
  };
  for (size_t Index = 0; Index < Med.Values.size(); ++Index)
    if (llvm::Error Error = Enqueue(static_cast<ValueID>(Index)))
      return Error;

  const auto Drain = [&]() -> llvm::Error {
    while (!Worklist.empty()) {
      const ValueID ID = Worklist.front();
      Worklist.pop_front();
      InWorklist[ID] = false;
      const LatticeValue Candidate =
          evaluateValue(Med, *Graph, States, ID, CodeSize);
      if (!mergeState(States[ID], Candidate))
        continue;
      for (const User &Use : Graph->Users[ID])
        if (llvm::Error Error = Enqueue(Use.Result))
          return Error;
    }
    return llvm::Error::success();
  };
  if (llvm::Error Error = Drain())
    return Error;

  // A closed cycle without a concrete seed conveys no constant. Promoting all
  // unresolved values and revisiting their users also prevents a phi from
  // retaining a provisional constant while one feasible incoming is missing.
  for (size_t Index = 0; Index < States.size(); ++Index) {
    if (States[Index].Kind != LatticeKind::Uninitialized)
      continue;
    States[Index] = LatticeValue::overdefined();
    for (const User &Use : Graph->Users[Index])
      if (llvm::Error Error = Enqueue(Use.Result))
        return Error;
  }
  if (llvm::Error Error = Drain())
    return Error;

  std::vector<Constant> Constants(Med.Values.size());
  for (size_t Index = 0; Index < Med.Values.size(); ++Index)
    if (States[Index].Kind == LatticeKind::Constant)
      Constants[Index] = States[Index].Word;
  return Constants;
}

llvm::Error propagateConstants(EVMMedIR &Med, size_t CodeSize,
                               const AnalyzeOptions &Options) {
  auto Constants = detail::computeCanonicalMedIRConstants(
      Med, CodeSize, Options.MaxMedWorklistUpdates);
  if (!Constants)
    return Constants.takeError();
  for (size_t Index = 0; Index < Med.Values.size(); ++Index)
    Med.Values[Index].Constant = std::move((*Constants)[Index]);
  return llvm::Error::success();
}

using LowLaneSet = std::set<LowStateLaneID>;
using PhiCacheKey = std::pair<uint64_t, std::vector<MedPhiIncoming>>;

struct OperationState {
  size_t BlockIndex = 0;
  size_t OperationIndex = 0;
  std::map<MedStateLaneID, std::vector<ValueID>> LaneInputs;
  llvm::SmallDenseSet<MedStateLaneID, 4> ExecutingLanes;
  llvm::SmallDenseSet<MedStateLaneID, 4> FaultingLanes;
};

class MedLowering {
public:
  MedLowering(const EVMLowIR &Low, const AnalyzeOptions &Options)
      : Low(Low), Options(Options) {}

  llvm::Expected<EVMMedIR> run() {
    if (llvm::Error Error = detail::validateMedAnalysisOptions(Options))
      return std::move(Error);
    if (llvm::Error Error = indexLowIR())
      return std::move(Error);
    if (llvm::Error Error = createStateLanes())
      return std::move(Error);
    if (llvm::Error Error = lowerStateLanes())
      return std::move(Error);
    if (llvm::Error Error = connectStateLanes())
      return std::move(Error);
    if (llvm::Error Error = finalizeOperations())
      return std::move(Error);
    if (llvm::Error Error = buildCompatibilityViews())
      return std::move(Error);
    if (llvm::Error Error = finalizeOrdering())
      return std::move(Error);
    if (llvm::Error Error = validateFinalState())
      return std::move(Error);
    if (llvm::Error Error = propagateConstants(Med, Low.Code.size(), Options))
      return std::move(Error);
    if (auto Failure = detail::verifyMedIRStructure(Low, Med))
      return medAnalysisError(
          Failure->PC, detail::medIRValidationFailureMessage(Failure->Kind));
    return std::move(Med);
  }

private:
  llvm::Error charge(size_t &Counter, size_t Amount, size_t Limit, uint64_t PC,
                     llvm::Twine Name) {
    if (Amount > Limit - Counter)
      return medAnalysisError(PC, Name + " limit " + llvm::Twine(Limit) +
                                      " exceeded");
    Counter += Amount;
    return llvm::Error::success();
  }

  llvm::Expected<ValueID> addValue(ValueKind Kind, uint64_t PC,
                                   llvm::StringRef Name,
                                   std::vector<ValueID> Inputs = {},
                                   Constant Folded = std::nullopt) {
    if (Med.Values.size() >= Options.MaxMedValues)
      return medAnalysisError(PC, "MedIR value limit " +
                                      llvm::Twine(Options.MaxMedValues) +
                                      " exceeded");
    if (Med.Values.size() >= std::numeric_limits<ValueID>::max())
      return medAnalysisError(PC, "MedIR value ID space exhausted");
    const ValueID ID = static_cast<ValueID>(Med.Values.size());
    MedValue Value;
    Value.ID = ID;
    Value.Kind = Kind;
    Value.PC = PC;
    Value.Name = Name.str();
    Value.Inputs = std::move(Inputs);
    Value.Constant = std::move(Folded);
    Med.Values.push_back(std::move(Value));
    return ID;
  }

  llvm::Expected<ValueID> addPhi(uint64_t PC) {
    return addValue(ValueKind::Phi, PC, kStackPhiValueName);
  }

  llvm::Error addPhiIncoming(ValueID PhiID, MedPhiIncoming Incoming,
                             uint64_t PC) {
    MedValue &Phi = Med.Values[PhiID];
    if (Phi.Kind != ValueKind::Phi || Incoming.Value >= Med.Values.size() ||
        !Med.findStateLane(Incoming.SourceLane))
      return medAnalysisError(PC, "invalid MedIR phi incoming");
    if (!Phi.PhiIncomings.empty() &&
        Phi.PhiIncomings.back().SourceLane >= Incoming.SourceLane)
      return medAnalysisError(PC, "non-canonical MedIR phi incoming order");
    if (llvm::Error Error =
            charge(PhiIncomingCount, 1, Options.MaxMedPhiIncomings, PC,
                   "MedIR phi incoming"))
      return Error;
    Phi.PhiIncomings.push_back(Incoming);
    Phi.Inputs.push_back(Incoming.Value);
    return llvm::Error::success();
  }

  llvm::Expected<ValueID> joinValues(uint64_t PC,
                                     std::vector<MedPhiIncoming> Incomings) {
    if (Incomings.empty())
      return medAnalysisError(PC, "MedIR join has no incoming lane");
    llvm::sort(Incomings);
    const auto Duplicate = std::adjacent_find(
        Incomings.begin(), Incomings.end(),
        [](const MedPhiIncoming &Left, const MedPhiIncoming &Right) {
          return Left.SourceLane == Right.SourceLane;
        });
    if (Duplicate != Incomings.end())
      return medAnalysisError(PC, "MedIR join has duplicate source lanes");
    const ValueID First = Incomings.front().Value;
    if (llvm::all_of(Incomings, [First](const MedPhiIncoming &Incoming) {
          return Incoming.Value == First;
        }))
      return First;
    const PhiCacheKey Key{PC, Incomings};
    if (const auto Existing = PhiCache.find(Key); Existing != PhiCache.end())
      return Existing->second;
    auto Phi = addPhi(PC);
    if (!Phi)
      return Phi.takeError();
    for (MedPhiIncoming Incoming : Incomings)
      if (llvm::Error Error = addPhiIncoming(*Phi, Incoming, PC))
        return std::move(Error);
    PhiCache.emplace(Key, *Phi);
    return *Phi;
  }

  llvm::Error chargeStackEntries(size_t Count, uint64_t PC) {
    return charge(StackEntryCount, Count, Options.MaxMedStackEntries, PC,
                  "MedIR stack entry");
  }

  llvm::Error indexLowIR() {
    if (Low.Instructions.size() > Options.MaxInstructions)
      return medAnalysisError(
          kEntryPC, "instruction limit " +
                        llvm::Twine(Options.MaxInstructions) + " exceeded");
    if (Low.Blocks.size() > Options.MaxBlocks)
      return medAnalysisError(kEntryPC, "basic block limit " +
                                            llvm::Twine(Options.MaxBlocks) +
                                            " exceeded");
    size_t AggregateEdges = 0;
    for (const LowBlock &Block : Low.Blocks) {
      if (Block.Successors.size() > Options.MaxEdges - AggregateEdges)
        return medAnalysisError(
            Block.StartPC,
            "CFG edge limit " + llvm::Twine(Options.MaxEdges) + " exceeded");
      AggregateEdges += Block.Successors.size();
    }
    if (Low.LaneTransitions.size() > Options.MaxEdges - AggregateEdges)
      return medAnalysisError(kEntryPC, "CFG edge limit " +
                                            llvm::Twine(Options.MaxEdges) +
                                            " exceeded");
    Med.Diagnostics = Low.Diagnostics;
    Med.Blocks.reserve(Low.Blocks.size());
    for (size_t Index = 0; Index < Low.Blocks.size(); ++Index) {
      const LowBlock &Block = Low.Blocks[Index];
      if (!BlockIndices.emplace(Block.StartPC, Index).second)
        return medAnalysisError(Block.StartPC,
                                "duplicate LowIR basic block address");
      MedBlock MedBlock;
      MedBlock.StartPC = Block.StartPC;
      Med.Blocks.push_back(std::move(MedBlock));
    }

    for (const LowLaneTransition &Transition : Low.LaneTransitions) {
      if (!Transition.Target)
        continue;
      IncomingLowLanes[*Transition.Target].insert(Transition.Source);
    }
    for (const LowBlock &Block : Low.Blocks)
      for (const LowFaultPrefix &Fault : Block.FaultPrefixes)
        if (!Faults.emplace(std::make_pair(Fault.Lane, Fault.PC), Fault.Kind)
                 .second)
          return medAnalysisError(Fault.PC,
                                  "duplicate LowIR lane fault prefix");
    return llvm::Error::success();
  }

  llvm::Error createStateLanes() {
    if (Low.StateLanes.size() > Options.MaxMedStateLanes)
      return medAnalysisError(
          kEntryPC, "MedIR state lane limit " +
                        llvm::Twine(Options.MaxMedStateLanes) + " exceeded");
    if (Low.StateLanes.size() > std::numeric_limits<uint32_t>::max())
      return medAnalysisError(kEntryPC, "MedIR state lane ID space exhausted");
    Med.StateLanes.reserve(Low.StateLanes.size());
    LaneCompleted.assign(Low.StateLanes.size(), false);
    for (const LowStateLane &LowLane : Low.StateLanes) {
      if (Med.StateLanes.size() >= Options.MaxMedStateLanes)
        return medAnalysisError(LowLane.ID.BlockPC,
                                "MedIR state lane limit " +
                                    llvm::Twine(Options.MaxMedStateLanes) +
                                    " exceeded");
      if (Med.StateLanes.size() >= std::numeric_limits<uint32_t>::max())
        return medAnalysisError(LowLane.ID.BlockPC,
                                "MedIR state lane ID space exhausted");
      const auto Block = BlockIndices.find(LowLane.ID.BlockPC);
      if (Block == BlockIndices.end())
        return medAnalysisError(LowLane.ID.BlockPC,
                                "LowIR lane has no basic block");
      const auto ID = static_cast<MedStateLaneID>(
          static_cast<uint32_t>(Med.StateLanes.size()));
      if (!LowToMedLane.emplace(LowLane.ID, ID).second)
        return medAnalysisError(LowLane.ID.BlockPC,
                                "duplicate LowIR state lane identity");
      Med.StateLanes.push_back({ID, LowLane.ID, LowLane.Evidence, {}, {}});
      Med.Blocks[Block->second].StateLanes.push_back(ID);
    }

    for (MedStateLane &Lane : Med.StateLanes) {
      const LowStateLane *LowLane = Low.findStateLane(Lane.LowLane);
      const LowAbstractStack *Entry =
          LowLane ? Low.findAbstractStack(LowLane->EntryState) : nullptr;
      if (!LowLane || !Entry)
        return medAnalysisError(Lane.LowLane.BlockPC,
                                "invalid LowIR state lane entry");
      if (llvm::Error Error =
              chargeStackEntries(Entry->Words.size(), Lane.LowLane.BlockPC))
        return Error;
      Lane.EntryStack.reserve(Entry->Words.size());
      for (size_t Slot = 0; Slot < Entry->Words.size(); ++Slot) {
        auto Phi = addPhi(Lane.LowLane.BlockPC);
        if (!Phi)
          return Phi.takeError();
        Lane.EntryStack.push_back(*Phi);
      }
      const bool IsRoot =
          Lane.LowLane.BlockPC == kEntryPC && Lane.LowLane.Ordinal == 0;
      if (IncomingLowLanes[Lane.LowLane].empty() && !IsRoot)
        return medAnalysisError(Lane.LowLane.BlockPC,
                                "non-root MedIR lane has no incoming edge");
      if (IsRoot && !Lane.EntryStack.empty())
        return medAnalysisError(kEntryPC,
                                "root MedIR lane must have an empty stack");
    }
    return llvm::Error::success();
  }

  llvm::Expected<OperationState *>
  getOrCreateOperation(size_t BlockIndex, const LowInstruction &Instruction) {
    if (auto Existing = Operations.find(Instruction.PC);
        Existing != Operations.end()) {
      if (Existing->second.BlockIndex != BlockIndex)
        return medAnalysisError(Instruction.PC,
                                "MedIR operation crosses basic blocks");
      return &Existing->second;
    }
    if (OperationCount >= Options.MaxMedOperations)
      return medAnalysisError(Instruction.PC,
                              "MedIR operation limit " +
                                  llvm::Twine(Options.MaxMedOperations) +
                                  " exceeded");
    MedOperation Operation;
    Operation.PC = Instruction.PC;
    Operation.Op = Instruction.opcode();
    Operation.Name = std::string(Instruction.Info.Name);
    Operation.Effect = Instruction.isExecutable() ? Instruction.Info.Effect
                                                  : EffectKind::Unknown;
    Operation.MemoryAccess = Instruction.isExecutable()
                                 ? Instruction.Info.MemoryAccess
                                 : MemoryAccessKind::Unknown;
    Operation.StateAccess = Instruction.isExecutable()
                                ? Instruction.Info.StateAccess
                                : StateAccessKind::Unknown;
    Operation.CallValueAccess = Instruction.isExecutable()
                                    ? Instruction.Info.CallValueAccess
                                    : CallValueAccessKind::Unknown;
    MedBlock &Block = Med.Blocks[BlockIndex];
    const size_t OperationIndex = Block.Operations.size();
    Block.Operations.push_back(std::move(Operation));
    ++OperationCount;
    auto [It, Inserted] = Operations.emplace(
        Instruction.PC, OperationState{BlockIndex, OperationIndex});
    if (!Inserted)
      return medAnalysisError(Instruction.PC,
                              "duplicate MedIR operation identity");
    return &It->second;
  }

  MedOperation &operation(OperationState &State) {
    return Med.Blocks[State.BlockIndex].Operations[State.OperationIndex];
  }

  llvm::Error addOperationLane(std::vector<MedStateLaneID> &Lanes,
                               llvm::SmallDenseSet<MedStateLaneID, 4> &Seen,
                               MedStateLaneID Lane, uint64_t PC) {
    if (Seen.contains(Lane))
      return medAnalysisError(PC, "duplicate MedIR operation lane");
    if (llvm::Error Error = charge(OperationLaneReferenceCount, 1,
                                   Options.MaxMedOperationLaneReferences, PC,
                                   kMaxMedOperationLaneReferencesName))
      return Error;
    Seen.insert(Lane);
    Lanes.push_back(Lane);
    return llvm::Error::success();
  }

  llvm::Error recordExecution(OperationState &State, MedStateLaneID Lane,
                              std::vector<ValueID> Inputs) {
    MedOperation &Operation = operation(State);
    if (llvm::Error Error = addOperationLane(
            Operation.ExecutingLanes, State.ExecutingLanes, Lane, Operation.PC))
      return Error;
    if (!State.LaneInputs.emplace(Lane, std::move(Inputs)).second)
      return medAnalysisError(Operation.PC, "duplicate MedIR lane execution");
    return llvm::Error::success();
  }

  llvm::Error recordFault(OperationState &State, MedStateLaneID Lane) {
    MedOperation &Operation = operation(State);
    return addOperationLane(Operation.FaultingLanes, State.FaultingLanes, Lane,
                            Operation.PC);
  }

  llvm::Error ensureOutputs(OperationState &State,
                            const LowInstruction &Instruction) {
    MedOperation &Operation = operation(State);
    if (!Operation.Outputs.empty()) {
      if (Operation.Outputs.size() != Instruction.Info.StackPushes)
        return medAnalysisError(Instruction.PC,
                                "inconsistent MedIR operation result arity");
      return llvm::Error::success();
    }
    for (uint8_t Output = 0; Output < Instruction.Info.StackPushes; ++Output) {
      llvm::Expected<ValueID> Value =
          Instruction.isPush()
              ? addValue(ValueKind::Constant, Instruction.PC,
                         kConstantValueName, {}, Instruction.Immediate)
              : addValue(ValueKind::Instruction, Instruction.PC,
                         Instruction.Info.Name);
      if (!Value)
        return Value.takeError();
      Operation.Outputs.push_back(*Value);
    }
    return llvm::Error::success();
  }

  llvm::Error completeLane(MedStateLaneID LaneID, MedStack Stack, uint64_t PC) {
    MedStateLane &Lane = *Med.findStateLane(LaneID);
    const LowStateLane *LowLane = Low.findStateLane(Lane.LowLane);
    const LowAbstractStack *LowExit =
        LowLane && LowLane->ExitState
            ? Low.findAbstractStack(*LowLane->ExitState)
            : nullptr;
    if (!LowExit || LowExit->Words.size() != Stack.size())
      return medAnalysisError(PC, "MedIR lane exit disagrees with LowIR");
    if (LaneCompleted[medStateLaneIndex(LaneID)])
      return medAnalysisError(PC, "MedIR lane completed more than once");
    if (llvm::Error Error = chargeStackEntries(Stack.size(), PC))
      return Error;
    Lane.ExitStack = std::move(Stack);
    LaneCompleted[medStateLaneIndex(LaneID)] = true;
    return llvm::Error::success();
  }

  llvm::Error lowerStateLane(MedStateLaneID LaneID) {
    MedStateLane &Lane = *Med.findStateLane(LaneID);
    const auto BlockIt = BlockIndices.find(Lane.LowLane.BlockPC);
    if (BlockIt == BlockIndices.end())
      return medAnalysisError(Lane.LowLane.BlockPC,
                              "MedIR lane has no basic block");
    const size_t BlockIndex = BlockIt->second;
    const LowBlock &Block = Low.Blocks[BlockIndex];
    MedStack Stack = Lane.EntryStack;
    const auto Pop = [&]() {
      const ValueID Value = Stack.back();
      Stack.pop_back();
      return Value;
    };

    for (size_t Index = Block.FirstInstruction;
         Index < Block.FirstInstruction + Block.InstructionCount; ++Index) {
      const LowInstruction &Instruction = Low.Instructions[Index];
      auto State = getOrCreateOperation(BlockIndex, Instruction);
      if (!State)
        return State.takeError();
      const auto Fault = Faults.find({Lane.LowLane, Instruction.PC});
      if (Fault != Faults.end()) {
        if (faultConsumesInstructionInputs(Fault->second)) {
          if (Stack.size() < Instruction.Info.StackPops)
            return medAnalysisError(
                Instruction.PC,
                "post-input LowIR fault has insufficient stack operands");
          for (uint8_t Input = 0; Input < Instruction.Info.StackPops; ++Input)
            Stack.pop_back();
        }
        if (llvm::Error Error = recordFault(**State, LaneID))
          return Error;
        return completeLane(LaneID, std::move(Stack), Instruction.PC);
      }
      if (!Instruction.isExecutable())
        return medAnalysisError(Instruction.PC,
                                "non-executable LowIR lane lacks fault prefix");

      std::vector<ValueID> Inputs;
      if (Instruction.isPush()) {
        // PUSH has no stack inputs.
      } else if (Instruction.isDup()) {
        const size_t Depth = Instruction.dupDepth();
        if (Stack.size() < Depth)
          return medAnalysisError(Instruction.PC,
                                  "MedIR DUP underflow lacks fault prefix");
        Inputs.push_back(Stack[Stack.size() - Depth]);
      } else if (Instruction.isSwap()) {
        const size_t Depth = Instruction.swapDepth();
        if (Stack.size() <= Depth)
          return medAnalysisError(Instruction.PC,
                                  "MedIR SWAP underflow lacks fault prefix");
        Inputs = {Stack.back(), Stack[Stack.size() - Depth - 1]};
      } else if (Instruction.isExchange()) {
        const auto [First, Second] = *Instruction.exchangeDepths();
        if (Stack.size() <= std::max(First, Second))
          return medAnalysisError(
              Instruction.PC, "MedIR EXCHANGE underflow lacks fault prefix");
        Inputs = {Stack[Stack.size() - First - 1],
                  Stack[Stack.size() - Second - 1]};
      } else {
        if (Stack.size() < Instruction.Info.StackPops)
          return medAnalysisError(Instruction.PC,
                                  "MedIR stack underflow lacks fault prefix");
        Inputs.reserve(Instruction.Info.StackPops);
        for (uint8_t Input = 0; Input < Instruction.Info.StackPops; ++Input)
          Inputs.push_back(Pop());
      }

      if (llvm::Error Error = recordExecution(**State, LaneID, Inputs))
        return Error;
      if (llvm::Error Error = ensureOutputs(**State, Instruction))
        return Error;
      MedOperation &Operation = operation(**State);
      if (Instruction.isDup()) {
        Stack.push_back(Operation.Outputs.front());
      } else if (Instruction.isSwap()) {
        const size_t Depth = Instruction.swapDepth();
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      } else if (Instruction.isExchange()) {
        const auto [First, Second] = *Instruction.exchangeDepths();
        std::swap(Stack[Stack.size() - First - 1],
                  Stack[Stack.size() - Second - 1]);
      } else {
        for (ValueID Output : Operation.Outputs)
          Stack.push_back(Output);
      }

      if (Instruction.isTerminator())
        return completeLane(LaneID, std::move(Stack), Instruction.PC);
    }
    return completeLane(LaneID, std::move(Stack), Block.EndPC);
  }

  llvm::Error lowerStateLanes() {
    for (const MedStateLane &Lane : Med.StateLanes)
      if (llvm::Error Error = lowerStateLane(Lane.ID))
        return Error;
    return llvm::Error::success();
  }

  llvm::Error connectStateLanes() {
    for (MedStateLane &Target : Med.StateLanes) {
      const LowLaneSet &Sources = IncomingLowLanes[Target.LowLane];
      if (Target.EntryStack.empty())
        continue;
      if (Sources.empty())
        return medAnalysisError(Target.LowLane.BlockPC,
                                "MedIR entry phi has no incoming lane");
      for (size_t Slot = 0; Slot < Target.EntryStack.size(); ++Slot) {
        const ValueID Phi = Target.EntryStack[Slot];
        for (LowStateLaneID LowSource : Sources) {
          const auto SourceID = LowToMedLane.find(LowSource);
          if (SourceID == LowToMedLane.end())
            return medAnalysisError(Target.LowLane.BlockPC,
                                    "MedIR transition source has no lane");
          const MedStateLane &Source = *Med.findStateLane(SourceID->second);
          if (Source.ExitStack.size() != Target.EntryStack.size())
            return medAnalysisError(Target.LowLane.BlockPC,
                                    "MedIR transition stack-height mismatch");
          if (llvm::Error Error =
                  addPhiIncoming(Phi, {Source.ID, Source.ExitStack[Slot]},
                                 Target.LowLane.BlockPC))
            return Error;
        }
      }
    }
    return llvm::Error::success();
  }

  llvm::Error finalizeOperations() {
    for (auto &[PC, State] : Operations) {
      MedOperation &Operation = operation(State);
      llvm::sort(Operation.ExecutingLanes);
      llvm::sort(Operation.FaultingLanes);
      if (Operation.ExecutingLanes.empty())
        continue;
      if (State.LaneInputs.size() != Operation.ExecutingLanes.size())
        return medAnalysisError(PC, "MedIR operation lacks lane operands");
      const size_t Arity = State.LaneInputs.begin()->second.size();
      if (llvm::any_of(State.LaneInputs, [Arity](const auto &Entry) {
            return Entry.second.size() != Arity;
          }))
        return medAnalysisError(PC,
                                "inconsistent MedIR operation operand arity");
      Operation.Inputs.reserve(Arity);
      for (size_t Operand = 0; Operand < Arity; ++Operand) {
        std::vector<MedPhiIncoming> Incomings;
        Incomings.reserve(State.LaneInputs.size());
        for (const auto &[Lane, Inputs] : State.LaneInputs)
          Incomings.push_back({Lane, Inputs[Operand]});
        auto Joined = joinValues(PC, std::move(Incomings));
        if (!Joined)
          return Joined.takeError();
        Operation.Inputs.push_back(*Joined);
      }
      for (ValueID Output : Operation.Outputs)
        Med.Values[Output].Inputs = Operation.Inputs;
    }
    return llvm::Error::success();
  }

  llvm::Expected<std::vector<ValueID>>
  joinLaneStacks(uint64_t PC, llvm::ArrayRef<MedStateLaneID> Lanes,
                 bool Entry) {
    if (Lanes.empty())
      return std::vector<ValueID>{};
    const MedStateLane &First = *Med.findStateLane(Lanes.front());
    const size_t Height =
        Entry ? First.EntryStack.size() : First.ExitStack.size();
    for (MedStateLaneID LaneID : Lanes) {
      const MedStateLane &Lane = *Med.findStateLane(LaneID);
      const size_t LaneHeight =
          Entry ? Lane.EntryStack.size() : Lane.ExitStack.size();
      if (LaneHeight != Height)
        return std::vector<ValueID>{};
    }
    if (llvm::Error Error = chargeStackEntries(Height, PC))
      return std::move(Error);
    std::vector<ValueID> Joined;
    Joined.reserve(Height);
    for (size_t Slot = 0; Slot < Height; ++Slot) {
      std::vector<MedPhiIncoming> Incomings;
      Incomings.reserve(Lanes.size());
      for (MedStateLaneID LaneID : Lanes) {
        const MedStateLane &Lane = *Med.findStateLane(LaneID);
        const MedStack &Stack = Entry ? Lane.EntryStack : Lane.ExitStack;
        Incomings.push_back({LaneID, Stack[Slot]});
      }
      auto Value = joinValues(PC, std::move(Incomings));
      if (!Value)
        return Value.takeError();
      Joined.push_back(*Value);
    }
    return Joined;
  }

  llvm::Error buildCompatibilityViews() {
    for (MedBlock &Block : Med.Blocks) {
      std::vector<MedStateLaneID> ReachableLanes;
      for (MedStateLaneID LaneID : Block.StateLanes)
        if (Med.findStateLane(LaneID)->Evidence == Reachability::Reachable)
          ReachableLanes.push_back(LaneID);
      if (ReachableLanes.empty())
        continue;
      const size_t EntryHeight =
          Med.findStateLane(ReachableLanes.front())->EntryStack.size();
      const bool PolymorphicEntry =
          llvm::any_of(ReachableLanes, [&](MedStateLaneID LaneID) {
            return Med.findStateLane(LaneID)->EntryStack.size() != EntryHeight;
          });
      if (PolymorphicEntry) {
        Med.Diagnostics.push_back(
            {Block.StartPC, kPolymorphicStackDiagnostic.str()});
      } else {
        auto Entry = joinLaneStacks(Block.StartPC, ReachableLanes, true);
        if (!Entry)
          return Entry.takeError();
        Block.EntryStack = std::move(*Entry);
        for (ValueID Value : Block.EntryStack)
          if (Med.Values[Value].Kind == ValueKind::Phi)
            Block.PhiValues.push_back(Value);
      }

      auto Exit = joinLaneStacks(Block.StartPC, ReachableLanes, false);
      if (!Exit)
        return Exit.takeError();
      Block.ExitStack = std::move(*Exit);
    }
    return llvm::Error::success();
  }

  llvm::Error finalizeOrdering() {
    for (MedBlock &Block : Med.Blocks) {
      llvm::sort(Block.StateLanes);
      llvm::sort(Block.Operations,
                 [](const MedOperation &Left, const MedOperation &Right) {
                   return Left.PC < Right.PC;
                 });
      for (size_t Index = 0; Index < Block.Operations.size(); ++Index) {
        const auto State = Operations.find(Block.Operations[Index].PC);
        if (State == Operations.end())
          return medAnalysisError(Block.Operations[Index].PC,
                                  "MedIR operation lacks lowering state");
        State->second.BlockIndex =
            static_cast<size_t>(&Block - Med.Blocks.data());
        State->second.OperationIndex = Index;
      }
      llvm::sort(Block.PhiValues);
      Block.PhiValues.erase(
          std::unique(Block.PhiValues.begin(), Block.PhiValues.end()),
          Block.PhiValues.end());
    }
    llvm::sort(Med.Diagnostics,
               [](const Diagnostic &Left, const Diagnostic &Right) {
                 return std::tie(Left.PC, Left.Message) <
                        std::tie(Right.PC, Right.Message);
               });
    Med.Diagnostics.erase(
        std::unique(Med.Diagnostics.begin(), Med.Diagnostics.end(),
                    [](const Diagnostic &Left, const Diagnostic &Right) {
                      return Left.PC == Right.PC &&
                             Left.Message == Right.Message;
                    }),
        Med.Diagnostics.end());
    return llvm::Error::success();
  }

  llvm::Error validateFinalState() const {
    const auto ValidValue = [&](ValueID ID) { return ID < Med.Values.size(); };
    for (size_t Index = 0; Index < Med.Values.size(); ++Index) {
      const MedValue &Value = Med.Values[Index];
      if (Value.ID != Index ||
          (Value.Constant && Value.Constant->getBitWidth() != kWordBits))
        return medAnalysisError(Value.PC, "invalid MedIR value record");
      for (ValueID Input : Value.Inputs)
        if (!ValidValue(Input))
          return medAnalysisError(Value.PC,
                                  "MedIR value references an invalid input");
      if (Value.Kind != ValueKind::Phi) {
        if (!Value.PhiIncomings.empty())
          return medAnalysisError(Value.PC,
                                  "non-phi MedIR value has lane incomings");
        continue;
      }
      if (Value.PhiIncomings.empty() ||
          Value.PhiIncomings.size() != Value.Inputs.size() ||
          !std::is_sorted(Value.PhiIncomings.begin(), Value.PhiIncomings.end()))
        return medAnalysisError(Value.PC, "invalid MedIR phi incoming table");
      for (size_t Incoming = 0; Incoming < Value.PhiIncomings.size();
           ++Incoming) {
        const MedPhiIncoming &Edge = Value.PhiIncomings[Incoming];
        if (!Med.findStateLane(Edge.SourceLane) || !ValidValue(Edge.Value) ||
            Edge.Value != Value.Inputs[Incoming] ||
            (Incoming != 0 &&
             Value.PhiIncomings[Incoming - 1].SourceLane == Edge.SourceLane))
          return medAnalysisError(Value.PC, "invalid MedIR phi lane reference");
      }
    }

    if (Med.Blocks.size() != Low.Blocks.size() ||
        Med.StateLanes.size() != Low.StateLanes.size())
      return medAnalysisError(kEntryPC, "MedIR table cardinality mismatch");
    for (size_t Index = 0; Index < Med.StateLanes.size(); ++Index) {
      const MedStateLane &Lane = Med.StateLanes[Index];
      const auto Expected =
          static_cast<MedStateLaneID>(static_cast<uint32_t>(Index));
      const LowStateLane *LowLane = Low.findStateLane(Lane.LowLane);
      const LowAbstractStack *LowEntry =
          LowLane ? Low.findAbstractStack(LowLane->EntryState) : nullptr;
      const LowAbstractStack *LowExit =
          LowLane && LowLane->ExitState
              ? Low.findAbstractStack(*LowLane->ExitState)
              : nullptr;
      const auto Block = BlockIndices.find(Lane.LowLane.BlockPC);
      if (Lane.ID != Expected || !isValidMedStateLaneID(Lane.ID) || !LowLane ||
          !LowEntry || !LowExit || Lane.Evidence != LowLane->Evidence ||
          Lane.EntryStack.size() != LowEntry->Words.size() ||
          Lane.ExitStack.size() != LowExit->Words.size() ||
          Block == BlockIndices.end() ||
          !llvm::is_contained(Med.Blocks[Block->second].StateLanes, Lane.ID) ||
          !LaneCompleted[Index])
        return medAnalysisError(Lane.LowLane.BlockPC,
                                "invalid MedIR state lane record");
      if (!llvm::all_of(Lane.EntryStack, ValidValue) ||
          !llvm::all_of(Lane.ExitStack, ValidValue))
        return medAnalysisError(Lane.LowLane.BlockPC,
                                "MedIR state lane has invalid stack values");

      const auto ExpectedIt = IncomingLowLanes.find(Lane.LowLane);
      const LowLaneSet &ExpectedSources = ExpectedIt == IncomingLowLanes.end()
                                              ? EmptyLowLaneSet
                                              : ExpectedIt->second;
      const bool IsRoot =
          Lane.LowLane.BlockPC == kEntryPC && Lane.LowLane.Ordinal == 0;
      if (!IsRoot && ExpectedSources.empty())
        return medAnalysisError(Lane.LowLane.BlockPC,
                                "MedIR lane has incomplete predecessors");
      for (size_t Slot = 0; Slot < Lane.EntryStack.size(); ++Slot) {
        const MedValue &Phi = Med.Values[Lane.EntryStack[Slot]];
        if (Phi.Kind != ValueKind::Phi ||
            Phi.PhiIncomings.size() != ExpectedSources.size())
          return medAnalysisError(Lane.LowLane.BlockPC,
                                  "MedIR lane entry is not an exact phi");
        size_t IncomingIndex = 0;
        for (LowStateLaneID LowSource : ExpectedSources) {
          const auto SourceID = LowToMedLane.find(LowSource);
          if (SourceID == LowToMedLane.end() ||
              IncomingIndex >= Phi.PhiIncomings.size())
            return medAnalysisError(Lane.LowLane.BlockPC,
                                    "MedIR lane phi misses a predecessor");
          const MedStateLane &Source = *Med.findStateLane(SourceID->second);
          const MedPhiIncoming ExpectedIncoming{Source.ID,
                                                Source.ExitStack[Slot]};
          if (Phi.PhiIncomings[IncomingIndex] != ExpectedIncoming)
            return medAnalysisError(Lane.LowLane.BlockPC,
                                    "MedIR lane phi disagrees with transition");
          ++IncomingIndex;
        }
      }
    }

    std::set<std::pair<MedStateLaneID, uint64_t>> SeenFaults;
    for (size_t BlockIndex = 0; BlockIndex < Med.Blocks.size(); ++BlockIndex) {
      const MedBlock &Block = Med.Blocks[BlockIndex];
      const LowBlock &LowBlock = Low.Blocks[BlockIndex];
      if (Block.StartPC != LowBlock.StartPC ||
          !std::is_sorted(Block.StateLanes.begin(), Block.StateLanes.end()) ||
          std::adjacent_find(Block.StateLanes.begin(),
                             Block.StateLanes.end()) != Block.StateLanes.end())
        return medAnalysisError(Block.StartPC,
                                "invalid MedIR basic block record");
      uint64_t PreviousPC = 0;
      bool FirstOperation = true;
      for (const MedOperation &Operation : Block.Operations) {
        if ((!FirstOperation && Operation.PC <= PreviousPC) ||
            (Operation.ExecutingLanes.empty() &&
             Operation.FaultingLanes.empty()) ||
            !std::is_sorted(Operation.ExecutingLanes.begin(),
                            Operation.ExecutingLanes.end()) ||
            !std::is_sorted(Operation.FaultingLanes.begin(),
                            Operation.FaultingLanes.end()) ||
            std::adjacent_find(Operation.ExecutingLanes.begin(),
                               Operation.ExecutingLanes.end()) !=
                Operation.ExecutingLanes.end() ||
            std::adjacent_find(Operation.FaultingLanes.begin(),
                               Operation.FaultingLanes.end()) !=
                Operation.FaultingLanes.end())
          return medAnalysisError(Operation.PC,
                                  "invalid canonical MedIR operation");
        FirstOperation = false;
        PreviousPC = Operation.PC;
        const auto State = Operations.find(Operation.PC);
        if (State == Operations.end())
          return medAnalysisError(Operation.PC,
                                  "MedIR operation lacks lowering state");
        const llvm::ArrayRef<LowInstruction> Instructions =
            llvm::ArrayRef(Low.Instructions)
                .slice(LowBlock.FirstInstruction, LowBlock.InstructionCount);
        const auto Instruction =
            llvm::find_if(Instructions, [&](const LowInstruction &Candidate) {
              return Candidate.PC == Operation.PC;
            });
        if (Instruction == Instructions.end() ||
            Instruction->opcode() != Operation.Op)
          return medAnalysisError(Operation.PC,
                                  "MedIR operation has no LowIR instruction");
        if (Operation.ExecutingLanes.empty() &&
            (!Operation.Inputs.empty() || !Operation.Outputs.empty()))
          return medAnalysisError(Operation.PC,
                                  "fault-only MedIR operation defines values");
        if (!Operation.ExecutingLanes.empty() && !Instruction->isExecutable())
          return medAnalysisError(
              Operation.PC, "non-executable MedIR operation has execution");
        for (MedStateLaneID LaneID : Operation.ExecutingLanes) {
          const MedStateLane *Lane = Med.findStateLane(LaneID);
          if (!Lane || Lane->LowLane.BlockPC != Block.StartPC ||
              std::binary_search(Operation.FaultingLanes.begin(),
                                 Operation.FaultingLanes.end(), LaneID) ||
              Faults.contains({Lane->LowLane, Operation.PC}) ||
              !State->second.LaneInputs.contains(LaneID))
            return medAnalysisError(Operation.PC,
                                    "invalid executing MedIR lane");
        }
        for (MedStateLaneID LaneID : Operation.FaultingLanes) {
          const MedStateLane *Lane = Med.findStateLane(LaneID);
          if (!Lane || Lane->LowLane.BlockPC != Block.StartPC ||
              !Faults.contains({Lane->LowLane, Operation.PC}) ||
              !SeenFaults.emplace(LaneID, Operation.PC).second)
            return medAnalysisError(Operation.PC,
                                    "invalid faulting MedIR lane");
        }
        for (ValueID Input : Operation.Inputs)
          if (!ValidValue(Input))
            return medAnalysisError(Operation.PC,
                                    "MedIR operation has invalid input");
        for (ValueID Output : Operation.Outputs)
          if (!ValidValue(Output) || Med.Values[Output].PC != Operation.PC ||
              Med.Values[Output].Inputs != Operation.Inputs)
            return medAnalysisError(Operation.PC,
                                    "MedIR operation has invalid output");
      }
      for (ValueID Phi : Block.PhiValues)
        if (!ValidValue(Phi) || Med.Values[Phi].Kind != ValueKind::Phi ||
            !llvm::is_contained(Block.EntryStack, Phi))
          return medAnalysisError(Block.StartPC,
                                  "invalid MedIR block phi compatibility view");
    }
    if (SeenFaults.size() != Faults.size())
      return medAnalysisError(kEntryPC,
                              "MedIR omits a LowIR lane fault prefix");
    return llvm::Error::success();
  }

  const EVMLowIR &Low;
  const AnalyzeOptions &Options;
  EVMMedIR Med;
  std::map<uint64_t, size_t> BlockIndices;
  std::map<LowStateLaneID, MedStateLaneID> LowToMedLane;
  std::map<LowStateLaneID, LowLaneSet> IncomingLowLanes;
  std::map<std::pair<LowStateLaneID, uint64_t>, LowFaultKind> Faults;
  std::map<uint64_t, OperationState> Operations;
  std::map<PhiCacheKey, ValueID> PhiCache;
  std::vector<bool> LaneCompleted;
  LowLaneSet EmptyLowLaneSet;
  size_t StackEntryCount = 0;
  size_t OperationCount = 0;
  size_t OperationLaneReferenceCount = 0;
  size_t PhiIncomingCount = 0;
};

} // namespace

llvm::Expected<std::vector<std::optional<llvm::APInt>>>
detail::computeCanonicalMedIRConstants(const EVMMedIR &Med, size_t CodeSize,
                                       size_t MaxWorklistUpdates) {
  return computeConstants(Med, CodeSize, MaxWorklistUpdates);
}

llvm::Error detail::validateMedAnalysisOptions(const AnalyzeOptions &Options) {
  return validateMedAnalysisOptionsImpl(Options);
}

llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low,
                                      AnalyzeOptions Options) {
  if (llvm::Error Error =
          detail::verifyCanonicalLowIRForMedLowering(Low, Options))
    return std::move(Error);
  return detail::lowerCanonicalLowToMedIR(Low, Options);
}

llvm::Expected<EVMMedIR>
detail::lowerCanonicalLowToMedIR(const EVMLowIR &Low, AnalyzeOptions Options) {
  return MedLowering(Low, Options).run();
}

} // namespace neverd::evm
