//===- EVMControlFlow.cpp - Whole-program EVM control-flow analysis -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMControlFlow.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace neverd::evm {
namespace {

llvm::Error analysisError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

llvm::Error optionError(llvm::Twine Name) {
  return llvm::make_error<llvm::StringError>("evm: " + Name +
                                                 " must be greater than zero",
                                             llvm::inconvertibleErrorCode());
}

std::string wordHex(const llvm::APInt &Value) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  return "0x" + Digits.str().str();
}

bool wordLess(const llvm::APInt &Left, const llvm::APInt &Right) {
  return Left.ult(Right);
}

LowAbstractValue topValue() { return {}; }

LowAbstractValue
constantSetValue(std::vector<llvm::APInt> Constants,
                 LowAbstractExactness Exactness = LowAbstractExactness::Exact) {
  if (Constants.empty())
    return topValue();
  llvm::sort(Constants, wordLess);
  Constants.erase(std::unique(Constants.begin(), Constants.end()),
                  Constants.end());
  LowAbstractValue Value;
  Value.Kind = LowAbstractValueKind::ConstantSet;
  Value.Exactness = Exactness;
  Value.Constants = std::move(Constants);
  return Value;
}

LowAbstractValue constantValue(const llvm::APInt &Constant) {
  return constantSetValue({Constant});
}

LowAbstractValue symbolValue(LowStateLaneID Lane, uint64_t PC, Opcode Op,
                             uint8_t OutputOrdinal) {
  LowAbstractValue Value;
  Value.Kind = LowAbstractValueKind::Symbol;
  Value.Exactness = LowAbstractExactness::Exact;
  Value.Symbol = LowAbstractSymbolKey{Lane, PC, Op, OutputOrdinal};
  return Value;
}

LowAbstractValue
expressionValue(Opcode Op, llvm::ArrayRef<LowAbstractValueID> Operands,
                LowAbstractExactness Exactness = LowAbstractExactness::Exact) {
  LowAbstractValue Value;
  Value.Kind = LowAbstractValueKind::Expression;
  Value.Exactness = Exactness;
  Value.Expression =
      LowAbstractExpressionKey{Op, {Operands.begin(), Operands.end()}};
  return Value;
}

bool hasExactIdentity(const LowAbstractValue &Value) {
  return Value.Kind != LowAbstractValueKind::Top &&
         Value.Exactness == LowAbstractExactness::Exact;
}

void profileAbstractValue(llvm::FoldingSetNodeID &ID,
                          const LowAbstractValue &Value) {
  ID.AddInteger(static_cast<uint8_t>(Value.Kind));
  ID.AddInteger(static_cast<uint8_t>(Value.Exactness));
  ID.AddInteger(Value.Constants.size());
  for (const llvm::APInt &Constant : Value.Constants)
    Constant.Profile(ID);
  ID.AddBoolean(Value.Symbol.has_value());
  if (Value.Symbol) {
    ID.AddInteger(Value.Symbol->ProducerLane.BlockPC);
    ID.AddInteger(Value.Symbol->ProducerLane.Ordinal);
    ID.AddInteger(Value.Symbol->ProducerPC);
    ID.AddInteger(opcodeByte(Value.Symbol->ProducerOpcode));
    ID.AddInteger(Value.Symbol->OutputOrdinal);
  }
  ID.AddBoolean(Value.Expression.has_value());
  if (Value.Expression) {
    ID.AddInteger(opcodeByte(Value.Expression->Operator));
    ID.AddInteger(Value.Expression->Operands.size());
    for (LowAbstractValueID Operand : Value.Expression->Operands)
      ID.AddInteger(lowAbstractValueIndex(Operand));
  }
}

using AbstractStack = std::vector<LowAbstractValueID>;

class AbstractValueNode final : public llvm::FoldingSetNode {
public:
  explicit AbstractValueNode(LowAbstractValue Value)
      : Value(std::move(Value)) {}

  static void Profile(llvm::FoldingSetNodeID &ID,
                      const LowAbstractValue &Value) {
    profileAbstractValue(ID, Value);
  }

  void Profile(llvm::FoldingSetNodeID &ProfileID) const {
    Profile(ProfileID, Value);
  }

  LowAbstractValue Value;
};

class AbstractStackNode final : public llvm::FoldingSetNode {
public:
  AbstractStackNode(LowAbstractStackID ID, AbstractStack Stack)
      : ID(ID), Stack(std::move(Stack)) {}

  static void Profile(llvm::FoldingSetNodeID &ID,
                      llvm::ArrayRef<LowAbstractValueID> Stack) {
    ID.AddInteger(Stack.size());
    for (LowAbstractValueID Value : Stack)
      ID.AddInteger(lowAbstractValueIndex(Value));
  }

  void Profile(llvm::FoldingSetNodeID &ProfileID) const {
    Profile(ProfileID, Stack);
  }

  LowAbstractStackID ID;
  AbstractStack Stack;
};

struct EvidenceLanes {
  std::map<LowAbstractStackID, LowStateLaneID> ByState;
  std::set<size_t> StackHeights;
};

struct BlockState {
  EvidenceLanes ReachableLanes;
  EvidenceLanes MayReachableLanes;
};

using AggregateEdgeKey =
    std::tuple<uint64_t, EdgeKind, std::optional<uint64_t>>;
using LaneTransitionKey =
    std::tuple<LowStateLaneID, std::optional<LowStateLaneID>, EdgeKind,
               std::optional<uint64_t>, Reachability>;

EvidenceLanes &lanesFor(BlockState &State, Reachability Evidence) {
  return Evidence == Reachability::Reachable ? State.ReachableLanes
                                             : State.MayReachableLanes;
}

const EvidenceLanes &lanesFor(const BlockState &State, Reachability Evidence) {
  return Evidence == Reachability::Reachable ? State.ReachableLanes
                                             : State.MayReachableLanes;
}

StackHeightDomain &entryHeightsFor(LowBlock &Block, Reachability Evidence) {
  return Evidence == Reachability::Reachable ? Block.EntryStackHeights
                                             : Block.MayEntryStackHeights;
}

StackHeightDomain &exitHeightsFor(LowBlock &Block, Reachability Evidence) {
  return Evidence == Reachability::Reachable ? Block.ExitStackHeights
                                             : Block.MayExitStackHeights;
}

class ControlFlowAnalysis {
public:
  ControlFlowAnalysis(EVMLowIR &Low, const AnalyzeOptions &Options)
      : Low(Low), Options(Options) {}

  llvm::Error run() {
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)                         \
  if (Options.NAME == 0)                                                       \
    return optionError(#NAME);
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)                   \
  if (Options.NAME == 0)                                                       \
    return optionError(#NAME);
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
    if (llvm::Error Error = initializeDiagnosticBudget())
      return Error;
    if (Low.Instructions.size() > Options.MaxInstructions)
      return analysisError(kEntryPC, "instruction limit " +
                                         llvm::Twine(Options.MaxInstructions) +
                                         " exceeded");
    if (Low.Blocks.size() > Options.MaxBlocks)
      return analysisError(kEntryPC, "basic block limit " +
                                         llvm::Twine(Options.MaxBlocks) +
                                         " exceeded");
    if (Low.Blocks.empty())
      return llvm::Error::success();

    States.resize(Low.Blocks.size());
    for (size_t Index = 0; Index < Low.Blocks.size(); ++Index)
      BlockIndices.emplace(Low.Blocks[Index].StartPC, Index);

    auto EntryState = internStack({}, kEntryPC);
    if (!EntryState)
      return EntryState.takeError();
    auto EntryLane = getOrCreateLane(0, *EntryState, Reachability::Reachable);
    if (!EntryLane)
      return EntryLane.takeError();
    while (!Worklist.empty()) {
      const LowStateLaneID Lane = Worklist.front();
      Worklist.pop_front();
      if (llvm::Error Error = transferLane(Lane))
        return Error;
    }

    return finalize();
  }

private:
  llvm::Error initializeDiagnosticBudget() {
    if (Low.Diagnostics.size() > Options.MaxLowDiagnostics)
      return analysisError(
          kEntryPC, "LowIR diagnostic limit " +
                        llvm::Twine(Options.MaxLowDiagnostics) + " exceeded");
    DiagnosticCount = Low.Diagnostics.size();
    for (const Diagnostic &Entry : Low.Diagnostics) {
      if (DiagnosticBytes > Options.MaxLowDiagnosticBytes ||
          Entry.Message.size() >
              Options.MaxLowDiagnosticBytes - DiagnosticBytes)
        return analysisError(Entry.PC,
                             "LowIR diagnostic byte limit " +
                                 llvm::Twine(Options.MaxLowDiagnosticBytes) +
                                 " exceeded");
      DiagnosticBytes += Entry.Message.size();
    }
    return llvm::Error::success();
  }

  llvm::Expected<LowAbstractValueID> internValue(LowAbstractValue Value,
                                                 uint64_t PC) {
    Value.ID = kInvalidLowAbstractValueID;
    llvm::FoldingSetNodeID Profile;
    AbstractValueNode::Profile(Profile, Value);
    void *InsertPosition = nullptr;
    if (AbstractValueNode *Existing =
            ValueNodes.FindNodeOrInsertPos(Profile, InsertPosition))
      return Existing->Value.ID;

    if (Low.AbstractValues.size() >= Options.MaxAbstractValueNodes)
      return analysisError(PC, "abstract value node limit " +
                                   llvm::Twine(Options.MaxAbstractValueNodes) +
                                   " exceeded");
    if (Low.AbstractValues.size() >= std::numeric_limits<uint32_t>::max())
      return analysisError(PC, "abstract value ID space exhausted");

    const auto ID = static_cast<LowAbstractValueID>(
        static_cast<uint32_t>(Low.AbstractValues.size()));
    Value.ID = ID;
    auto Node = std::make_unique<AbstractValueNode>(std::move(Value));
    ValueNodes.InsertNode(Node.get(), InsertPosition);
    Low.AbstractValues.push_back(Node->Value);
    OwnedValueNodes.push_back(std::move(Node));
    return ID;
  }

  const LowAbstractValue &abstractValue(LowAbstractValueID ID) const {
    return OwnedValueNodes[lowAbstractValueIndex(ID)]->Value;
  }

  llvm::Expected<LowAbstractValueID>
  produceValue(LowStateLaneID Lane, Opcode Op,
               llvm::ArrayRef<LowAbstractValueID> Inputs, uint64_t PC,
               uint8_t OutputOrdinal) {
    if (Op == Opcode::PC)
      return internValue(constantValue(llvm::APInt(kWordBits, PC)), PC);
    if (Op == Opcode::CODESIZE)
      return internValue(constantValue(llvm::APInt(kWordBits, Low.Code.size())),
                         PC);

    if (Op == Opcode::EQ && Inputs.size() == 2 && Inputs[0] == Inputs[1] &&
        hasExactIdentity(abstractValue(Inputs[0])))
      return internValue(constantValue(llvm::APInt(kWordBits, 1)), PC);

    const auto Info = assignedOpcodeInfo(Op);
    if (OutputOrdinal != 0 || !Info || !isALU(*Info))
      return internValue(symbolValue(Lane, PC, Op, OutputOrdinal), PC);

    bool AllConstants = true;
    size_t ProductSize = 1;
    size_t VaryingInputs = 0;
    LowAbstractExactness Exactness = LowAbstractExactness::Exact;
    for (LowAbstractValueID InputID : Inputs) {
      const LowAbstractValue &Input = abstractValue(InputID);
      if (Input.Exactness == LowAbstractExactness::OverApproximation)
        Exactness = LowAbstractExactness::OverApproximation;
      if (Input.Kind != LowAbstractValueKind::ConstantSet) {
        AllConstants = false;
        continue;
      }
      VaryingInputs += Input.Constants.size() > 1 ? 1 : 0;
      if (Input.Constants.size() >
          Options.MaxAbstractValuesPerSlot / ProductSize)
        return analysisError(PC,
                             "abstract value limit " +
                                 llvm::Twine(Options.MaxAbstractValuesPerSlot) +
                                 " exceeded");
      ProductSize *= Input.Constants.size();
    }

    if (AllConstants) {
      std::vector<llvm::APInt> Results;
      Results.reserve(ProductSize);
      llvm::SmallVector<llvm::APInt, kMaxALUStackPops> ConcreteInputs;
      const auto Enumerate = [&](auto &&Self, size_t Index) -> bool {
        if (Index == Inputs.size()) {
          auto Result = evaluateALU(Op, ConcreteInputs);
          if (!Result)
            return false;
          Results.push_back(std::move(*Result));
          return true;
        }
        for (const llvm::APInt &Constant :
             abstractValue(Inputs[Index]).Constants) {
          ConcreteInputs.push_back(Constant);
          if (!Self(Self, Index + 1))
            return false;
          ConcreteInputs.pop_back();
        }
        return true;
      };
      if (Enumerate(Enumerate, 0)) {
        if (VaryingInputs > 1)
          Exactness = LowAbstractExactness::OverApproximation;
        return internValue(constantSetValue(std::move(Results), Exactness), PC);
      }
    }

    if (!llvm::all_of(Inputs, [&](LowAbstractValueID Input) {
          return hasExactIdentity(abstractValue(Input));
        }))
      Exactness = LowAbstractExactness::OverApproximation;
    return internValue(expressionValue(Op, Inputs, Exactness), PC);
  }

  llvm::Expected<LowAbstractStackID>
  internStack(llvm::ArrayRef<LowAbstractValueID> Stack, uint64_t PC) {
    llvm::FoldingSetNodeID Profile;
    AbstractStackNode::Profile(Profile, Stack);
    void *InsertPosition = nullptr;
    if (AbstractStackNode *Existing =
            StackNodes.FindNodeOrInsertPos(Profile, InsertPosition))
      return Existing->ID;

    if (Low.AbstractStacks.size() >= Options.MaxAbstractStackNodes)
      return analysisError(PC, "abstract stack node limit " +
                                   llvm::Twine(Options.MaxAbstractStackNodes) +
                                   " exceeded");
    if (Low.AbstractStacks.size() >= std::numeric_limits<uint32_t>::max())
      return analysisError(PC, "abstract stack ID space exhausted");
    if (AbstractStackEntryCount > Options.MaxAbstractStackEntries ||
        Stack.size() >
            Options.MaxAbstractStackEntries - AbstractStackEntryCount)
      return analysisError(
          PC, "abstract stack entry limit " +
                  llvm::Twine(Options.MaxAbstractStackEntries) + " exceeded");

    const auto ID = static_cast<LowAbstractStackID>(
        static_cast<uint32_t>(Low.AbstractStacks.size()));
    AbstractStackEntryCount += Stack.size();
    AbstractStack Copy(Stack.begin(), Stack.end());
    auto Node = std::make_unique<AbstractStackNode>(ID, std::move(Copy));
    LowAbstractStack PublicState;
    PublicState.ID = ID;
    PublicState.Words = Node->Stack;
    StackNodes.InsertNode(Node.get(), InsertPosition);
    Low.AbstractStacks.push_back(std::move(PublicState));
    OwnedStackNodes.push_back(std::move(Node));
    return ID;
  }

  const AbstractStackNode &stackNode(LowAbstractStackID ID) const {
    return *OwnedStackNodes[lowAbstractStackIndex(ID)];
  }

  llvm::Expected<LowStateLaneID> getOrCreateLane(size_t BlockIndex,
                                                 LowAbstractStackID State,
                                                 Reachability Evidence) {
    EvidenceLanes &Lanes = lanesFor(States[BlockIndex], Evidence);
    if (const auto Existing = Lanes.ByState.find(State);
        Existing != Lanes.ByState.end())
      return Existing->second;

    const size_t StackHeight = stackNode(State).Stack.size();
    const bool AddsStackHeight = !Lanes.StackHeights.contains(StackHeight);
    if (AddsStackHeight &&
        Lanes.StackHeights.size() >= Options.MaxStackHeightVariants)
      return analysisError(Low.Blocks[BlockIndex].StartPC,
                           "abstract stack-height variant limit " +
                               llvm::Twine(Options.MaxStackHeightVariants) +
                               " exceeded");
    if (Low.Blocks[BlockIndex].StateLanes.size() >=
        Options.MaxAbstractStateLanesPerBlock)
      return analysisError(
          Low.Blocks[BlockIndex].StartPC,
          "abstract state lane limit " +
              llvm::Twine(Options.MaxAbstractStateLanesPerBlock) + " exceeded");
    if (Low.Blocks[BlockIndex].StateLanes.size() >=
        std::numeric_limits<uint32_t>::max())
      return analysisError(Low.Blocks[BlockIndex].StartPC,
                           "block-local state lane ID space exhausted");
    if (WorklistUpdateCount >= Options.MaxWorklistUpdates)
      return analysisError(Low.Blocks[BlockIndex].StartPC,
                           "worklist update limit " +
                               llvm::Twine(Options.MaxWorklistUpdates) +
                               " exceeded");

    const LowStateLaneID ID{
        Low.Blocks[BlockIndex].StartPC,
        static_cast<uint32_t>(Low.Blocks[BlockIndex].StateLanes.size())};
    LaneIndices.emplace(ID, Low.StateLanes.size());
    Low.StateLanes.push_back({ID, Evidence, State, std::nullopt});
    Low.Blocks[BlockIndex].StateLanes.push_back(ID);
    Lanes.ByState.emplace(State, ID);
    Lanes.StackHeights.insert(StackHeight);
    entryHeightsFor(Low.Blocks[BlockIndex], Evidence).insert(StackHeight);
    Worklist.push_back(ID);
    ++WorklistUpdateCount;
    return ID;
  }

  llvm::Expected<LowStateLaneID> propagate(LowStateLaneID Source,
                                           uint64_t TargetPC,
                                           const AbstractStack &Stack,
                                           Reachability Evidence) {
    const auto Target = BlockIndices.find(TargetPC);
    if (Target == BlockIndices.end())
      return analysisError(TargetPC, "internal CFG target has no basic block");

    AbstractStack Incoming(Stack.begin(), Stack.end());
    // Every address-ordered CFG cycle contains a non-forward edge. Abstract a
    // changed word at that static recurrence boundary immediately, independent
    // of resource-budget pressure, so loop convergence is a semantic rule and
    // never an emergency widening after a limit has already been reached.
    if (TargetPC <= Source.BlockPC) {
      const EvidenceLanes &Existing =
          lanesFor(States[Target->second], Evidence);
      std::vector<bool> RecurringSlots(Incoming.size(), false);
      bool HasSameHeightEntry = false;
      for (const auto &[StateID, Lane] : Existing.ByState) {
        (void)Lane;
        const AbstractStack &Prior = stackNode(StateID).Stack;
        if (Prior.size() != Incoming.size())
          continue;
        HasSameHeightEntry = true;
        for (size_t Slot = 0; Slot < Incoming.size(); ++Slot)
          RecurringSlots[Slot] =
              RecurringSlots[Slot] || Prior[Slot] != Incoming[Slot];
      }
      if (HasSameHeightEntry && llvm::is_contained(RecurringSlots, true)) {
        auto RecurrenceTop = internValue(topValue(), TargetPC);
        if (!RecurrenceTop)
          return RecurrenceTop.takeError();
        for (size_t Slot = 0; Slot < Incoming.size(); ++Slot)
          if (RecurringSlots[Slot])
            Incoming[Slot] = *RecurrenceTop;
      }
    }

    auto State = internStack(Incoming, TargetPC);
    if (!State)
      return State.takeError();
    return getOrCreateLane(Target->second, *State, Evidence);
  }

  llvm::Error setExitState(LowStateLaneID Lane, const AbstractStack &Stack,
                           uint64_t PC) {
    auto State = internStack(Stack, PC);
    if (!State)
      return State.takeError();
    stateLane(Lane).ExitState = *State;
    return llvm::Error::success();
  }

  LowStateLane &stateLane(LowStateLaneID ID) {
    return Low.StateLanes[LaneIndices.at(ID)];
  }

  const LowStateLane &stateLane(LowStateLaneID ID) const {
    return Low.StateLanes[LaneIndices.at(ID)];
  }

  llvm::Error addTransition(LowStateLaneID Source,
                            std::optional<LowStateLaneID> Target, EdgeKind Kind,
                            std::optional<uint64_t> TargetPC,
                            Reachability Evidence) {
    const auto Key = std::make_tuple(Source, Target, Kind, TargetPC, Evidence);
    if (TransitionKeys.contains(Key))
      return llvm::Error::success();
    if (EdgeRecordCount >= Options.MaxEdges)
      return analysisError(Source.BlockPC, "CFG edge limit " +
                                               llvm::Twine(Options.MaxEdges) +
                                               " exceeded");
    Low.LaneTransitions.push_back({Source, Target, Kind, TargetPC, Evidence});
    TransitionKeys.insert(Key);
    ++EdgeRecordCount;
    return llvm::Error::success();
  }

  llvm::Error addEdge(LowBlock &Block, EdgeKind Kind,
                      std::optional<uint64_t> Target, Reachability Evidence) {
    const auto Key = std::make_tuple(Block.StartPC, Kind, Target);
    if (const auto Existing = AggregateEdgeIndices.find(Key);
        Existing != AggregateEdgeIndices.end()) {
      if (Evidence == Reachability::Reachable)
        Block.Successors[Existing->second].Evidence = Reachability::Reachable;
      return llvm::Error::success();
    }
    if (EdgeRecordCount >= Options.MaxEdges)
      return analysisError(Block.StartPC, "CFG edge limit " +
                                              llvm::Twine(Options.MaxEdges) +
                                              " exceeded");
    const size_t EdgeIndex = Block.Successors.size();
    Block.Successors.push_back({Kind, Target, Evidence});
    AggregateEdgeIndices.emplace(Key, EdgeIndex);
    ++EdgeRecordCount;
    if (Kind == EdgeKind::Indirect)
      Block.HasIndirectSuccessor = true;
    return llvm::Error::success();
  }

  llvm::Error addDiagnostic(uint64_t PC, std::string Message) {
    // Canonicalization is one sort/unique pass in finalize().  Looking for a
    // duplicate here turns a hostile collection of independent lane faults
    // into quadratic work before the configured lane bound can help us.
    if (DiagnosticCount >= Options.MaxLowDiagnostics)
      return analysisError(PC, "LowIR diagnostic limit " +
                                   llvm::Twine(Options.MaxLowDiagnostics) +
                                   " exceeded");
    if (DiagnosticBytes > Options.MaxLowDiagnosticBytes ||
        Message.size() > Options.MaxLowDiagnosticBytes - DiagnosticBytes)
      return analysisError(PC, "LowIR diagnostic byte limit " +
                                   llvm::Twine(Options.MaxLowDiagnosticBytes) +
                                   " exceeded");
    ++DiagnosticCount;
    DiagnosticBytes += Message.size();
    Low.Diagnostics.push_back({PC, std::move(Message)});
    return llvm::Error::success();
  }

  void addFaultPrefix(LowBlock &Block, LowStateLaneID Lane,
                      size_t EntryStackHeight, uint64_t PC, LowFaultKind Kind) {
    // Each transfer contributes at most one terminal fault.  Defer duplicate
    // removal to finalize() so insertion remains constant-time as lane count
    // grows.
    Block.FaultPrefixes.push_back({Lane, EntryStackHeight, PC, Kind});
  }

  llvm::Error reportFault(uint64_t PC, llvm::Twine Message) {
    if (Options.Strict)
      return analysisError(PC, Message);
    return addDiagnostic(PC, Message.str());
  }

  bool mayBeZero(LowAbstractValueID ID) const {
    const LowAbstractValue &Value = abstractValue(ID);
    return Value.Kind != LowAbstractValueKind::ConstantSet ||
           llvm::any_of(Value.Constants, [](const llvm::APInt &Constant) {
             return Constant.isZero();
           });
  }

  bool mayBeNonZero(LowAbstractValueID ID) const {
    const LowAbstractValue &Value = abstractValue(ID);
    return Value.Kind != LowAbstractValueKind::ConstantSet ||
           llvm::any_of(Value.Constants, [](const llvm::APInt &Constant) {
             return !Constant.isZero();
           });
  }

  llvm::Expected<bool> resolveJump(LowStateLaneID Source, size_t BlockIndex,
                                   EdgeKind Kind, uint64_t JumpPC,
                                   LowAbstractValueID TargetID,
                                   const AbstractStack &Stack,
                                   Reachability Evidence) {
    LowBlock &Block = Low.Blocks[BlockIndex];
    const LowAbstractValue &Target = abstractValue(TargetID);
    if (Target.Kind != LowAbstractValueKind::ConstantSet) {
      if (llvm::Error Error = addEdge(Block, EdgeKind::Indirect, std::nullopt,
                                      Reachability::MayReachable))
        return Error;
      if (llvm::Error Error =
              addTransition(Source, std::nullopt, EdgeKind::Indirect,
                            std::nullopt, Reachability::MayReachable))
        return Error;
      for (uint64_t Address : Low.JumpDestinations) {
        if (llvm::Error Error = addEdge(Block, EdgeKind::Indirect, Address,
                                        Reachability::MayReachable))
          return Error;
        auto TargetLane =
            propagate(Source, Address, Stack, Reachability::MayReachable);
        if (!TargetLane)
          return TargetLane.takeError();
        if (llvm::Error Error =
                addTransition(Source, *TargetLane, EdgeKind::Indirect, Address,
                              Reachability::MayReachable))
          return Error;
      }
      return false;
    }

    bool HasValidTarget = false;
    for (const llvm::APInt &Value : Target.Constants) {
      std::optional<uint64_t> Address;
      if (Value.getActiveBits() <= std::numeric_limits<uint64_t>::digits)
        Address = Value.getZExtValue();
      if (!Address || !Low.JumpDestinations.contains(*Address)) {
        const std::string Message =
            "jump target " + wordHex(Value) + " is not a JUMPDEST";
        if (Target.Exactness == LowAbstractExactness::Exact &&
            Evidence == Reachability::Reachable) {
          if (llvm::Error Error = reportFault(JumpPC, Message))
            return Error;
        } else {
          if (llvm::Error Error = addDiagnostic(
                  JumpPC, kOverapproximatedJumpTargetPrefix.str() +
                              wordHex(Value) + " is not a JUMPDEST"))
            return Error;
        }
        continue;
      }
      HasValidTarget = true;
      const Reachability TargetEvidence =
          Target.Exactness == LowAbstractExactness::Exact
              ? Evidence
              : Reachability::MayReachable;
      if (llvm::Error Error = addEdge(Block, Kind, Address, TargetEvidence))
        return Error;
      auto TargetLane = propagate(Source, *Address, Stack, TargetEvidence);
      if (!TargetLane)
        return TargetLane.takeError();
      if (llvm::Error Error =
              addTransition(Source, *TargetLane, Kind, Address, TargetEvidence))
        return Error;
    }
    return Target.Exactness == LowAbstractExactness::Exact &&
           !Target.Constants.empty() && !HasValidTarget;
  }

  llvm::Error transferLane(LowStateLaneID Lane) {
    const LowStateLane PublicLane = stateLane(Lane);
    const auto BlockIt = BlockIndices.find(PublicLane.ID.BlockPC);
    if (BlockIt == BlockIndices.end())
      return analysisError(PublicLane.ID.BlockPC,
                           "state lane has no basic block");
    const size_t BlockIndex = BlockIt->second;
    const Reachability Evidence = PublicLane.Evidence;
    AbstractStack Stack = stackNode(PublicLane.EntryState).Stack;
    LowBlock &Block = Low.Blocks[BlockIndex];
    const size_t EntryStackHeight = Stack.size();

    for (size_t Index = Block.FirstInstruction;
         Index < Block.FirstInstruction + Block.InstructionCount; ++Index) {
      const LowInstruction &Instruction = Low.Instructions[Index];
      if (InstructionTransferCount >= Options.MaxAbstractInstructionTransfers)
        return analysisError(
            Instruction.PC,
            "abstract instruction transfer limit " +
                llvm::Twine(Options.MaxAbstractInstructionTransfers) +
                " exceeded");
      ++InstructionTransferCount;
      if (!Instruction.isExecutable()) {
        addFaultPrefix(Block, Lane, EntryStackHeight, Instruction.PC,
                       LowFaultKind::NonExecutableInstruction);
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        if (llvm::Error Error = setExitState(Lane, Stack, Instruction.PC))
          return Error;
        // Legacy EVM bytecode has no whole-image opcode validation: an
        // unknown or not-yet-active byte faults only if execution reaches it.
        // Malformed conditional immediates remain explicit fault nodes even
        // in strict mode so their non-consumption preserves later boundaries.
        if (Options.Strict && Evidence == Reachability::Reachable &&
            Instruction.DecodeStatus != OpcodeDecodeStatus::Active)
          return analysisError(
              Instruction.PC,
              opcodeDecodeStatusName(Instruction.DecodeStatus) + " opcode " +
                  formatOpcodeByte(Instruction.Encoding.front()));
        return llvm::Error::success();
      }

      if (Stack.size() < Instruction.requiredStackHeight()) {
        addFaultPrefix(Block, Lane, EntryStackHeight, Instruction.PC,
                       LowFaultKind::StackUnderflow);
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        if (llvm::Error Error = setExitState(Lane, Stack, Instruction.PC))
          return Error;
        if (Evidence == Reachability::MayReachable)
          return llvm::Error::success();
        return reportFault(Instruction.PC,
                           "stack underflow in " +
                               llvm::Twine(Instruction.Info.Name));
      }
      const std::ptrdiff_t ResultHeight =
          static_cast<std::ptrdiff_t>(Stack.size()) + Instruction.stackDelta();
      if (ResultHeight > static_cast<std::ptrdiff_t>(kStackLimit)) {
        addFaultPrefix(Block, Lane, EntryStackHeight, Instruction.PC,
                       LowFaultKind::StackOverflow);
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        if (llvm::Error Error = setExitState(Lane, Stack, Instruction.PC))
          return Error;
        if (Evidence == Reachability::MayReachable)
          return llvm::Error::success();
        return reportFault(Instruction.PC,
                           "stack limit exceeds " + llvm::Twine(kStackLimit));
      }

      if (Instruction.isPush()) {
        auto Value =
            internValue(constantValue(Instruction.Immediate), Instruction.PC);
        if (!Value)
          return Value.takeError();
        Stack.push_back(*Value);
        continue;
      }
      if (Instruction.isDup()) {
        const size_t Depth = Instruction.dupDepth();
        Stack.push_back(Stack[Stack.size() - Depth]);
        continue;
      }
      if (Instruction.isSwap()) {
        const size_t Depth = Instruction.swapDepth();
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
        continue;
      }
      if (Instruction.isExchange()) {
        const auto [First, Second] = *Instruction.exchangeDepths();
        std::swap(Stack[Stack.size() - First - 1],
                  Stack[Stack.size() - Second - 1]);
        continue;
      }

      llvm::SmallVector<LowAbstractValueID, kMaxOpcodeStackPops> Inputs;
      Inputs.reserve(Instruction.Info.StackPops);
      for (uint8_t Input = 0; Input < Instruction.Info.StackPops; ++Input) {
        Inputs.push_back(std::move(Stack.back()));
        Stack.pop_back();
      }

      if (Instruction.is(Opcode::JUMP)) {
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        if (llvm::Error Error = setExitState(Lane, Stack, Instruction.PC))
          return Error;
        auto Faults =
            resolveJump(Lane, BlockIndex, EdgeKind::Jump, Instruction.PC,
                        Inputs.front(), Stack, Evidence);
        if (!Faults)
          return Faults.takeError();
        if (*Faults)
          addFaultPrefix(Block, Lane, EntryStackHeight, Instruction.PC,
                         LowFaultKind::InvalidJumpDestination);
        return llvm::Error::success();
      }
      if (Instruction.is(Opcode::JUMPI)) {
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        if (llvm::Error Error = setExitState(Lane, Stack, Instruction.PC))
          return Error;
        const bool MaybeTrue = mayBeNonZero(Inputs[1]);
        const bool MaybeFalse = mayBeZero(Inputs[1]);
        bool TrueBranchFaults = false;
        if (MaybeTrue) {
          auto Faults =
              resolveJump(Lane, BlockIndex, EdgeKind::ConditionalTrue,
                          Instruction.PC, Inputs.front(), Stack, Evidence);
          if (!Faults)
            return Faults.takeError();
          TrueBranchFaults = *Faults;
        }
        if (TrueBranchFaults && !MaybeFalse)
          addFaultPrefix(Block, Lane, EntryStackHeight, Instruction.PC,
                         LowFaultKind::InvalidJumpDestination);
        if (MaybeFalse && BlockIndex + 1 < Low.Blocks.size()) {
          const uint64_t Fallthrough = Low.Blocks[BlockIndex + 1].StartPC;
          if (llvm::Error Error = addEdge(Block, EdgeKind::ConditionalFalse,
                                          Fallthrough, Evidence))
            return Error;
          auto TargetLane = propagate(Lane, Fallthrough, Stack, Evidence);
          if (!TargetLane)
            return TargetLane.takeError();
          if (llvm::Error Error =
                  addTransition(Lane, *TargetLane, EdgeKind::ConditionalFalse,
                                Fallthrough, Evidence))
            return Error;
        }
        return llvm::Error::success();
      }

      for (uint8_t Output = 0; Output < Instruction.Info.StackPushes;
           ++Output) {
        auto Value = produceValue(Lane, Instruction.opcode(), Inputs,
                                  Instruction.PC, Output);
        if (!Value)
          return Value.takeError();
        Stack.push_back(*Value);
      }

      if (Instruction.Info.IsTerminator) {
        exitHeightsFor(Block, Evidence).insert(Stack.size());
        return setExitState(Lane, Stack, Instruction.PC);
      }
    }

    exitHeightsFor(Block, Evidence).insert(Stack.size());
    if (llvm::Error Error = setExitState(Lane, Stack, Block.EndPC))
      return Error;
    if (BlockIndex + 1 >= Low.Blocks.size())
      return llvm::Error::success();
    const uint64_t Fallthrough = Low.Blocks[BlockIndex + 1].StartPC;
    if (llvm::Error Error =
            addEdge(Block, EdgeKind::Fallthrough, Fallthrough, Evidence))
      return Error;
    auto TargetLane = propagate(Lane, Fallthrough, Stack, Evidence);
    if (!TargetLane)
      return TargetLane.takeError();
    if (llvm::Error Error = addTransition(
            Lane, *TargetLane, EdgeKind::Fallthrough, Fallthrough, Evidence))
      return Error;
    return llvm::Error::success();
  }

  llvm::Error finalize() {
    const auto EdgeLess = [](const LowEdge &Left, const LowEdge &Right) {
      if (Left.Kind != Right.Kind)
        return Left.Kind < Right.Kind;
      return Left.Target < Right.Target;
    };
    for (size_t Index = 0; Index < Low.Blocks.size(); ++Index) {
      LowBlock &Block = Low.Blocks[Index];
      Block.Reachable = !States[Index].ReachableLanes.ByState.empty();
      Block.MayReachable = !States[Index].MayReachableLanes.ByState.empty();
      llvm::sort(Block.StateLanes);
      llvm::sort(Block.Successors, EdgeLess);
      Block.Successors.erase(
          std::unique(Block.Successors.begin(), Block.Successors.end(),
                      [](const LowEdge &Left, const LowEdge &Right) {
                        return Left.Kind == Right.Kind &&
                               Left.Target == Right.Target;
                      }),
          Block.Successors.end());
      llvm::sort(Block.FaultPrefixes,
                 [](const LowFaultPrefix &Left, const LowFaultPrefix &Right) {
                   return std::tie(Left.Lane, Left.PC, Left.Kind) <
                          std::tie(Right.Lane, Right.PC, Right.Kind);
                 });
      Block.FaultPrefixes.erase(
          std::unique(
              Block.FaultPrefixes.begin(), Block.FaultPrefixes.end(),
              [](const LowFaultPrefix &Left, const LowFaultPrefix &Right) {
                return Left.Lane == Right.Lane && Left.PC == Right.PC &&
                       Left.Kind == Right.Kind;
              }),
          Block.FaultPrefixes.end());
      Block.Predecessors.clear();
      Block.MayPredecessors.clear();
    }

    for (const LowBlock &Block : Low.Blocks)
      for (const LowEdge &Edge : Block.Successors)
        if (Edge.Target)
          if (LowBlock *Target = Low.findBlock(*Edge.Target)) {
            std::vector<uint64_t> &Predecessors =
                Edge.Evidence == Reachability::Reachable
                    ? Target->Predecessors
                    : Target->MayPredecessors;
            Predecessors.push_back(Block.StartPC);
          }
    for (LowBlock &Block : Low.Blocks) {
      const auto SortAndUnique = [](std::vector<uint64_t> &Predecessors) {
        llvm::sort(Predecessors);
        Predecessors.erase(
            std::unique(Predecessors.begin(), Predecessors.end()),
            Predecessors.end());
      };
      SortAndUnique(Block.Predecessors);
      SortAndUnique(Block.MayPredecessors);
    }

    llvm::sort(Low.LaneTransitions, [](const LowLaneTransition &Left,
                                       const LowLaneTransition &Right) {
      return std::tie(Left.Source, Left.Kind, Left.TargetPC, Left.Target,
                      Left.Evidence) < std::tie(Right.Source, Right.Kind,
                                                Right.TargetPC, Right.Target,
                                                Right.Evidence);
    });
    Low.LaneTransitions.erase(
        std::unique(Low.LaneTransitions.begin(), Low.LaneTransitions.end()),
        Low.LaneTransitions.end());

    llvm::sort(Low.StateLanes,
               [](const LowStateLane &Left, const LowStateLane &Right) {
                 return Left.ID < Right.ID;
               });

    llvm::sort(Low.Diagnostics,
               [](const Diagnostic &Left, const Diagnostic &Right) {
                 return std::tie(Left.PC, Left.Message) <
                        std::tie(Right.PC, Right.Message);
               });
    Low.Diagnostics.erase(
        std::unique(Low.Diagnostics.begin(), Low.Diagnostics.end(),
                    [](const Diagnostic &Left, const Diagnostic &Right) {
                      return Left.PC == Right.PC &&
                             Left.Message == Right.Message;
                    }),
        Low.Diagnostics.end());
    return validateFinalState();
  }

  llvm::Error validateFinalState() const {
    for (size_t Index = 0; Index < Low.AbstractValues.size(); ++Index) {
      const LowAbstractValue &Value = Low.AbstractValues[Index];
      const auto ExpectedID =
          static_cast<LowAbstractValueID>(static_cast<uint32_t>(Index));
      if (Value.ID != ExpectedID)
        return analysisError(kEntryPC,
                             "abstract value table identity mismatch");
      const bool HasConstants = !Value.Constants.empty();
      const bool HasSymbol = Value.Symbol.has_value();
      const bool HasExpression = Value.Expression.has_value();
      switch (Value.Kind) {
      case LowAbstractValueKind::Top:
        if (Value.Exactness != LowAbstractExactness::OverApproximation ||
            HasConstants || HasSymbol || HasExpression)
          return analysisError(kEntryPC, "invalid Top abstract value");
        break;
      case LowAbstractValueKind::ConstantSet:
        if (!HasConstants || HasSymbol || HasExpression ||
            !std::is_sorted(Value.Constants.begin(), Value.Constants.end(),
                            wordLess) ||
            std::adjacent_find(Value.Constants.begin(),
                               Value.Constants.end()) != Value.Constants.end())
          return analysisError(kEntryPC, "invalid constant-set abstract value");
        break;
      case LowAbstractValueKind::Symbol:
        if (HasConstants || !HasSymbol || HasExpression ||
            Value.Exactness != LowAbstractExactness::Exact ||
            !Low.findStateLane(Value.Symbol->ProducerLane))
          return analysisError(kEntryPC, "invalid symbolic abstract value");
        break;
      case LowAbstractValueKind::Expression:
        if (HasConstants || HasSymbol || !HasExpression)
          return analysisError(kEntryPC, "invalid expression abstract value");
        for (LowAbstractValueID Operand : Value.Expression->Operands)
          if (!Low.findAbstractValue(Operand))
            return analysisError(
                kEntryPC, "expression references an invalid abstract value");
        break;
      }
    }
    for (size_t Index = 0; Index < Low.AbstractStacks.size(); ++Index) {
      const LowAbstractStack &Stack = Low.AbstractStacks[Index];
      const auto ExpectedID =
          static_cast<LowAbstractStackID>(static_cast<uint32_t>(Index));
      if (Stack.ID != ExpectedID)
        return analysisError(kEntryPC,
                             "abstract stack table identity mismatch");
      for (LowAbstractValueID Value : Stack.Words)
        if (!Low.findAbstractValue(Value))
          return analysisError(kEntryPC,
                               "abstract stack references an invalid value");
    }

    for (const LowBlock &Block : Low.Blocks) {
      for (size_t Ordinal = 0; Ordinal < Block.StateLanes.size(); ++Ordinal) {
        if (Ordinal >= std::numeric_limits<uint32_t>::max())
          return analysisError(Block.StartPC,
                               "block-local state lane ID space exhausted");
        const LowStateLaneID Expected{Block.StartPC,
                                      static_cast<uint32_t>(Ordinal)};
        if (Block.StateLanes[Ordinal] != Expected)
          return analysisError(Block.StartPC,
                               "block-local state lane ordinal mismatch");
      }
      for (const LowFaultPrefix &Fault : Block.FaultPrefixes) {
        const LowStateLane *Lane = Low.findStateLane(Fault.Lane);
        const LowAbstractStack *Entry =
            Lane ? Low.findAbstractStack(Lane->EntryState) : nullptr;
        if (!Lane || !Entry || Fault.Lane.BlockPC != Block.StartPC ||
            Fault.EntryStackHeight != Entry->Words.size())
          return analysisError(Block.StartPC,
                               "fault prefix references an invalid lane");
      }
    }

    for (const LowStateLane &Lane : Low.StateLanes) {
      const LowBlock *Block = Low.findBlock(Lane.ID.BlockPC);
      if (!Lane.ID.isValid() || !Block ||
          Lane.ID.Ordinal >= Block->StateLanes.size() ||
          Block->StateLanes[Lane.ID.Ordinal] != Lane.ID ||
          !Low.findAbstractStack(Lane.EntryState) || !Lane.ExitState ||
          !Low.findAbstractStack(*Lane.ExitState))
        return analysisError(Lane.ID.BlockPC, "invalid state lane record");
    }

    for (const LowLaneTransition &Transition : Low.LaneTransitions) {
      const LowStateLane *Source = Low.findStateLane(Transition.Source);
      const LowStateLane *Target =
          Transition.Target ? Low.findStateLane(*Transition.Target) : nullptr;
      const LowAbstractStack *SourceExit =
          Source && Source->ExitState
              ? Low.findAbstractStack(*Source->ExitState)
              : nullptr;
      const LowAbstractStack *TargetEntry =
          Target ? Low.findAbstractStack(Target->EntryState) : nullptr;
      if (!Source || !SourceExit ||
          (Transition.Target.has_value() && !Target) ||
          Transition.Target.has_value() != Transition.TargetPC.has_value() ||
          (Transition.Target && Transition.TargetPC &&
           Transition.Target->BlockPC != *Transition.TargetPC) ||
          (Target &&
           (!TargetEntry || Target->Evidence != Transition.Evidence)) ||
          (TargetEntry &&
           SourceExit->Words.size() != TargetEntry->Words.size()))
        return analysisError(Transition.Source.BlockPC,
                             "lane transition references an invalid lane");
    }
    return llvm::Error::success();
  }

  EVMLowIR &Low;
  const AnalyzeOptions &Options;
  std::vector<BlockState> States;
  std::map<uint64_t, size_t> BlockIndices;
  std::map<LowStateLaneID, size_t> LaneIndices;
  std::map<AggregateEdgeKey, size_t> AggregateEdgeIndices;
  std::set<LaneTransitionKey> TransitionKeys;
  std::vector<std::unique_ptr<AbstractValueNode>> OwnedValueNodes;
  llvm::FoldingSet<AbstractValueNode> ValueNodes;
  std::vector<std::unique_ptr<AbstractStackNode>> OwnedStackNodes;
  llvm::FoldingSet<AbstractStackNode> StackNodes;
  std::deque<LowStateLaneID> Worklist;
  size_t EdgeRecordCount = 0;
  size_t WorklistUpdateCount = 0;
  size_t InstructionTransferCount = 0;
  size_t AbstractStackEntryCount = 0;
  size_t DiagnosticCount = 0;
  size_t DiagnosticBytes = 0;
};

} // namespace

llvm::Error analyzeControlFlow(EVMLowIR &Low, const AnalyzeOptions &Options) {
  return ControlFlowAnalysis(Low, Options).run();
}

} // namespace neverd::evm
