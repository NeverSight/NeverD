//===- EVMMedAnalysis.cpp - EVM stack SSA and value analysis ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace neverd::evm {
namespace {

using Constant = std::optional<llvm::APInt>;

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

ValueID addValue(EVMMedIR &Med, ValueKind Kind, uint64_t PC,
                 llvm::StringRef Name, std::vector<ValueID> Inputs = {},
                 Constant Folded = std::nullopt) {
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

ValueID unknownValue(EVMMedIR &Med, uint64_t PC) {
  return addValue(Med, ValueKind::Unknown, PC, kUnknownName);
}

enum class DefinitionKind : uint8_t { None, Phi, Operation };

struct Definition {
  DefinitionKind Kind = DefinitionKind::None;
  const MedOperation *Operation = nullptr;
};

struct User {
  ValueID Result = 0;
  DefinitionKind Kind = DefinitionKind::None;

  friend bool operator<(const User &LHS, const User &RHS) {
    if (LHS.Result != RHS.Result)
      return LHS.Result < RHS.Result;
    return LHS.Kind < RHS.Kind;
  }

  friend bool operator==(const User &LHS, const User &RHS) {
    return LHS.Result == RHS.Result && LHS.Kind == RHS.Kind;
  }
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
  for (size_t I = 0; I < Med.Values.size(); ++I) {
    const MedValue &Value = Med.Values[I];
    if (Value.ID != I)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR value ID %u does not match its table index %zu", Value.ID, I);
    if (Value.Constant && Value.Constant->getBitWidth() != kWordBits)
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "MedIR value %u has a non-EVM constant width", Value.ID);
    for (ValueID Input : Value.Inputs)
      if (!IsValid(Input))
        return invalidValueID(Input);
    if (Value.Kind == ValueKind::Phi)
      Graph.Definitions[I].Kind = DefinitionKind::Phi;
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
    if (Value.Kind == ValueKind::Phi) {
      for (ValueID Input : Value.Inputs)
        Graph.Users[Input].push_back({Value.ID, DefinitionKind::Phi});
    }
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
  bool HasUninitialized = false;
  for (ValueID Input : Operation.Inputs) {
    const LatticeValue &State = States[Input];
    if (State.Kind == LatticeKind::Overdefined)
      return LatticeValue::overdefined();
    if (State.Kind == LatticeKind::Uninitialized) {
      HasUninitialized = true;
      Inputs.emplace_back();
    } else {
      Inputs.emplace_back(State.Word);
    }
  }
  if (HasUninitialized)
    return {};
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

llvm::Error propagateConstants(EVMMedIR &Med, size_t CodeSize) {
  auto Graph = buildDataflowGraph(Med);
  if (!Graph)
    return Graph.takeError();

  std::vector<LatticeValue> States(Med.Values.size());
  std::deque<ValueID> Worklist;
  std::vector<bool> InWorklist(Med.Values.size(), true);
  for (size_t I = 0; I < Med.Values.size(); ++I)
    Worklist.push_back(static_cast<ValueID>(I));

  const auto Enqueue = [&](ValueID ID) {
    if (InWorklist[ID])
      return;
    InWorklist[ID] = true;
    Worklist.push_back(ID);
  };

  while (!Worklist.empty()) {
    const ValueID ID = Worklist.front();
    Worklist.pop_front();
    InWorklist[ID] = false;
    const LatticeValue Candidate =
        evaluateValue(Med, *Graph, States, ID, CodeSize);
    if (!mergeState(States[ID], Candidate))
      continue;
    for (const User &Use : Graph->Users[ID])
      Enqueue(Use.Result);
  }

  for (size_t I = 0; I < Med.Values.size(); ++I) {
    if (States[I].Kind == LatticeKind::Constant)
      Med.Values[I].Constant = States[I].Word;
    else
      Med.Values[I].Constant.reset();
  }
  return llvm::Error::success();
}

} // namespace

llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low) {
  EVMMedIR Med;
  Med.Diagnostics = Low.Diagnostics;
  Med.Blocks.resize(Low.Blocks.size());

  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    const LowBlock &LowBlock = Low.Blocks[BI];
    MedBlock &Block = Med.Blocks[BI];
    Block.StartPC = LowBlock.StartPC;
    const auto MaximumEntryHeight = LowBlock.EntryStackHeights.maximum();
    if (!MaximumEntryHeight)
      continue;
    const size_t EntryHeight = *MaximumEntryHeight;
    const bool IsPolymorphic = !LowBlock.EntryStackHeights.singleton();
    const size_t MinimumEntryHeight =
        LowBlock.EntryStackHeights.values().front();
    const size_t FirstCommonSlot = EntryHeight - MinimumEntryHeight;
    if (IsPolymorphic)
      Med.Diagnostics.push_back(
          {LowBlock.StartPC, kPolymorphicStackDiagnostic.str()});
    for (size_t Slot = 0; Slot < EntryHeight; ++Slot) {
      const bool PresentOnEveryPath = Slot >= FirstCommonSlot;
      const ValueID Entry = addValue(
          Med, PresentOnEveryPath ? ValueKind::Phi : ValueKind::Unknown,
          LowBlock.StartPC,
          PresentOnEveryPath ? kStackPhiValueName : kUnknownStackEntryName);
      Block.EntryStack.push_back(Entry);
      if (PresentOnEveryPath)
        Block.PhiValues.push_back(Entry);
    }
  }

  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    const LowBlock &LowBlock = Low.Blocks[BI];
    MedBlock &Block = Med.Blocks[BI];
    std::vector<ValueID> Stack = Block.EntryStack;
    const auto Ensure = [&](size_t Count, uint64_t PC) {
      while (Stack.size() < Count)
        Stack.insert(Stack.begin(), unknownValue(Med, PC));
    };
    const auto Pop = [&]() {
      const ValueID Value = Stack.back();
      Stack.pop_back();
      return Value;
    };

    for (size_t II = LowBlock.FirstInstruction;
         II < LowBlock.FirstInstruction + LowBlock.InstructionCount; ++II) {
      const LowInstruction &Instruction = Low.Instructions[II];
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

      if (!Instruction.isExecutable()) {
        Block.Operations.push_back(std::move(Operation));
        continue;
      }
      if (Instruction.isPush()) {
        const ValueID Value =
            addValue(Med, ValueKind::Constant, Instruction.PC,
                     kConstantValueName, {}, Instruction.Immediate);
        Stack.push_back(Value);
        Operation.Outputs.push_back(Value);
      } else if (Instruction.isDup()) {
        const size_t Depth = Instruction.dupDepth();
        Ensure(Depth, Instruction.PC);
        const ValueID Input = Stack[Stack.size() - Depth];
        const ValueID Output =
            addValue(Med, ValueKind::Instruction, Instruction.PC,
                     Operation.Name, {Input});
        Operation.Inputs.push_back(Input);
        Operation.Outputs.push_back(Output);
        Stack.push_back(Output);
      } else if (Instruction.isSwap()) {
        const size_t Depth = Instruction.swapDepth();
        Ensure(Depth + 1, Instruction.PC);
        Operation.Inputs = {Stack.back(), Stack[Stack.size() - Depth - 1]};
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      } else if (Instruction.isExchange()) {
        const auto [First, Second] = *Instruction.exchangeDepths();
        Ensure(Second + 1, Instruction.PC);
        Operation.Inputs = {Stack[Stack.size() - First - 1],
                            Stack[Stack.size() - Second - 1]};
        std::swap(Stack[Stack.size() - First - 1],
                  Stack[Stack.size() - Second - 1]);
      } else {
        Ensure(Instruction.Info.StackPops, Instruction.PC);
        for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
          Operation.Inputs.push_back(Pop());
        for (uint8_t I = 0; I < Instruction.Info.StackPushes; ++I) {
          const ValueID Output =
              addValue(Med, ValueKind::Instruction, Instruction.PC,
                       Operation.Name, Operation.Inputs);
          Operation.Outputs.push_back(Output);
          Stack.push_back(Output);
        }
      }
      Block.Operations.push_back(std::move(Operation));
    }
    Block.ExitStack = std::move(Stack);
  }

  llvm::DenseMap<uint64_t, size_t> Indices;
  for (size_t I = 0; I < Low.Blocks.size(); ++I)
    Indices[Low.Blocks[I].StartPC] = I;
  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    const LowBlock &LowBlock = Low.Blocks[BI];
    MedBlock &Block = Med.Blocks[BI];
    for (size_t Slot = 0; Slot < Block.EntryStack.size(); ++Slot) {
      MedValue &Phi = Med.Values[Block.EntryStack[Slot]];
      if (Phi.Kind != ValueKind::Phi)
        continue;
      for (uint64_t PredPC : LowBlock.Predecessors) {
        const auto It = Indices.find(PredPC);
        if (It == Indices.end())
          continue;
        const auto &Exit = Med.Blocks[It->second].ExitStack;
        if (Exit.size() > Block.EntryStack.size())
          continue;
        const size_t Offset = Block.EntryStack.size() - Exit.size();
        if (Slot < Offset)
          continue;
        Phi.Inputs.push_back(Exit[Slot - Offset]);
        Phi.IncomingBlocks.push_back(PredPC);
      }
    }
  }
  if (llvm::Error E = propagateConstants(Med, Low.Code.size()))
    return std::move(E);
  return Med;
}

} // namespace neverd::evm
