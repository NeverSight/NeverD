//===- HighAnalysis.cpp - EVM source-level fact recovery ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Analyzer.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <vector>

namespace neverd::evm {
namespace {

inline constexpr size_t kErrorSelectorSearchWindow = 10;

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits = 1) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  std::string Result = Digits.str().str();
  if (Result.size() < MinDigits)
    Result.insert(Result.begin(), MinDigits - Result.size(), '0');
  return Result;
}

std::optional<uint64_t> asAddress(const MedValue *Value) {
  if (!Value || !Value->Constant ||
      Value->Constant->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Value->Constant->getZExtValue();
}

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  const auto It =
      std::lower_bound(Low.Instructions.begin(), Low.Instructions.end(), PC,
                       [](const LowInstruction &Instruction, uint64_t Address) {
                         return Instruction.PC < Address;
                       });
  return It != Low.Instructions.end() && It->PC == PC ? &*It : nullptr;
}

std::set<uint64_t> reachableFrom(const EVMLowIR &Low, uint64_t Entry) {
  std::set<uint64_t> Seen;
  std::queue<uint64_t> Queue;
  if (!Low.findBlock(Entry))
    return Seen;
  Seen.insert(Entry);
  Queue.push(Entry);
  while (!Queue.empty()) {
    const uint64_t PC = Queue.front();
    Queue.pop();
    const LowBlock *Block = Low.findBlock(PC);
    for (const LowEdge &Edge : Block->Successors)
      if (Edge.Target && Seen.insert(*Edge.Target).second)
        Queue.push(*Edge.Target);
  }
  return Seen;
}

Mutability recoveredMutability(StateAccessKind Access, bool ReadsCallValue) {
  if (Access == StateAccessKind::Unknown)
    return Mutability::NonPayable;
  if (ReadsCallValue)
    return Mutability::Payable;
  switch (Access) {
  case StateAccessKind::None:
    return Mutability::Pure;
  case StateAccessKind::Read:
    return Mutability::View;
  case StateAccessKind::Write:
  case StateAccessKind::Unknown:
    return Mutability::NonPayable;
  }
  return Mutability::NonPayable;
}

std::optional<llvm::APInt> precedingPush(const EVMLowIR &Low, size_t Index,
                                         size_t Window) {
  const size_t Begin = Index > Window ? Index - Window : 0;
  for (size_t I = Index; I-- > Begin;) {
    if (Low.Instructions[I].isPush())
      return Low.Instructions[I].Immediate;
    if (!Low.Instructions[I].isExecutable() ||
        Low.Instructions[I].Info.Class == OpcodeClass::Control)
      break;
  }
  return std::nullopt;
}

bool endsInRevert(const EVMLowIR &Low, uint64_t BlockPC) {
  const LowBlock *Block = Low.findBlock(BlockPC);
  if (!Block || Block->InstructionCount == 0)
    return false;
  return Low.Instructions[Block->FirstInstruction + Block->InstructionCount - 1]
      .is(Opcode::REVERT);
}

class ProducerIndex {
public:
  explicit ProducerIndex(const EVMMedIR &Med)
      : Producers(Med.Values.size(), nullptr) {
    build(Med);
  }

  [[nodiscard]] bool valid() const { return Valid; }
  [[nodiscard]] uint64_t errorPC() const { return ErrorPC; }

  [[nodiscard]] const MedOperation *producer(ValueID ID) const {
    return ID < Producers.size() ? Producers[ID] : nullptr;
  }

  [[nodiscard]] const MedBlock *block(uint64_t StartPC) const {
    const auto It = Blocks.find(StartPC);
    return It == Blocks.end() ? nullptr : It->second;
  }

private:
  void fail(uint64_t PC) {
    if (!Valid)
      return;
    Valid = false;
    ErrorPC = PC;
  }

  void build(const EVMMedIR &Med) {
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
        if (!Operations.emplace(Operation.PC, &Operation).second) {
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

  bool Valid = true;
  uint64_t ErrorPC = kEntryPC;
  std::vector<const MedOperation *> Producers;
  std::map<uint64_t, const MedOperation *> Operations;
  std::map<uint64_t, const MedBlock *> Blocks;
};

enum class SemanticKind : uint8_t {
  Unknown,
  Constant,
  CalldataWordZero,
  SelectorWord,
  SelectorEquality,
  CallValue,
  IsZeroCallValue,
  CalldataSize,
  IsZeroCalldataSize,
};

struct SemanticValue {
  SemanticKind Kind = SemanticKind::Unknown;
  llvm::APInt Word = llvm::APInt(kWordBits, 0);
  uint32_t Selector = 0;
  std::vector<uint64_t> OriginPCs;

  static SemanticValue constant(const llvm::APInt &Value) {
    SemanticValue Result;
    Result.Kind = SemanticKind::Constant;
    Result.Word = Value;
    return Result;
  }

  static SemanticValue simple(SemanticKind Kind) {
    SemanticValue Result;
    Result.Kind = Kind;
    return Result;
  }

  static SemanticValue callValue(SemanticKind Kind, uint64_t PC) {
    SemanticValue Result = simple(Kind);
    Result.OriginPCs.push_back(PC);
    return Result;
  }

  static SemanticValue selectorEquality(uint32_t Selector) {
    SemanticValue Result = simple(SemanticKind::SelectorEquality);
    Result.Selector = Selector;
    return Result;
  }
};

bool sameExpression(const SemanticValue &LHS, const SemanticValue &RHS) {
  if (LHS.Kind != RHS.Kind)
    return false;
  switch (LHS.Kind) {
  case SemanticKind::Constant:
    return LHS.Word == RHS.Word;
  case SemanticKind::SelectorEquality:
    return LHS.Selector == RHS.Selector;
  case SemanticKind::Unknown:
    return false;
  case SemanticKind::CalldataWordZero:
  case SemanticKind::SelectorWord:
  case SemanticKind::CallValue:
  case SemanticKind::IsZeroCallValue:
  case SemanticKind::CalldataSize:
  case SemanticKind::IsZeroCalldataSize:
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

enum class VisitState : uint8_t { Unseen, Active, Complete };

class SemanticClassifier {
public:
  SemanticClassifier(const EVMMedIR &Med, const ProducerIndex &Index)
      : Med(Med), Index(Index), States(Med.Values.size()),
        Results(Med.Values.size()) {}

  SemanticValue classify(ValueID Root) {
    if (Root >= Med.Values.size())
      return {};
    if (States[Root] == VisitState::Complete)
      return Results[Root];

    struct Frame {
      ValueID ID = 0;
      llvm::SmallVector<ValueID, kMaxALUStackPops> Dependencies;
      size_t NextDependency = 0;
      bool HasCycle = false;
    };

    std::vector<Frame> Stack;
    const auto Push = [&](ValueID ID) {
      Frame Next;
      Next.ID = ID;
      Next.Dependencies = dependencies(ID);
      States[ID] = VisitState::Active;
      Stack.push_back(std::move(Next));
    };
    Push(Root);

    while (!Stack.empty()) {
      Frame &Current = Stack.back();
      if (Current.NextDependency < Current.Dependencies.size()) {
        const ValueID Dependency =
            Current.Dependencies[Current.NextDependency++];
        if (States[Dependency] == VisitState::Unseen) {
          Push(Dependency);
          continue;
        }
        if (States[Dependency] == VisitState::Active)
          Current.HasCycle = true;
        continue;
      }

      Results[Current.ID] =
          Current.HasCycle ? SemanticValue{} : evaluate(Current.ID);
      States[Current.ID] = VisitState::Complete;
      Stack.pop_back();
    }
    return Results[Root];
  }

private:
  llvm::SmallVector<ValueID, kMaxALUStackPops> dependencies(ValueID ID) const {
    const MedValue &Value = Med.Values[ID];
    if (Value.Constant || Value.Kind == ValueKind::Unknown)
      return {};
    if (Value.Kind == ValueKind::Phi)
      return {Value.Inputs.begin(), Value.Inputs.end()};
    const MedOperation *Operation = Index.producer(ID);
    if (!Operation)
      return {};
    return {Operation->Inputs.begin(), Operation->Inputs.end()};
  }

  const SemanticValue &input(const MedOperation &Operation,
                             size_t Index) const {
    return Results[Operation.Inputs[Index]];
  }

  SemanticValue evaluatePhi(const MedValue &Phi) const {
    if (Phi.Inputs.empty())
      return {};
    SemanticValue Common = Results[Phi.Inputs.front()];
    if (Common.Kind == SemanticKind::Unknown)
      return {};
    for (ValueID Input : llvm::ArrayRef(Phi.Inputs).drop_front()) {
      const SemanticValue &Candidate = Results[Input];
      if (!sameExpression(Common, Candidate))
        return {};
      mergeOrigins(Common.OriginPCs, Candidate.OriginPCs);
    }
    return Common;
  }

  bool isSelectorMask(const SemanticValue &Value) const {
    return Value.Kind == SemanticKind::Constant &&
           Value.Word == llvm::APInt::getLowBitsSet(kWordBits, kSelectorBits);
  }

  SemanticValue evaluateInstruction(ValueID ID) const {
    const MedOperation *Operation = Index.producer(ID);
    if (!Operation)
      return {};
    if ((evm::isDup(Operation->Op) || evm::isDeepDup(Operation->Op)) &&
        Operation->Inputs.size() == 1)
      return input(*Operation, 0);
    if (Operation->Op == Opcode::CALLDATALOAD &&
        Operation->Inputs.size() == 1) {
      const SemanticValue &Offset = input(*Operation, 0);
      if (Offset.Kind == SemanticKind::Constant && Offset.Word.isZero())
        return SemanticValue::simple(SemanticKind::CalldataWordZero);
      return {};
    }
    if (Operation->Op == Opcode::SHR && Operation->Inputs.size() == 2) {
      const SemanticValue &Shift = input(*Operation, 0);
      const SemanticValue &Word = input(*Operation, 1);
      if (Shift.Kind == SemanticKind::Constant &&
          Shift.Word == llvm::APInt(kWordBits, kWordBits - kSelectorBits) &&
          Word.Kind == SemanticKind::CalldataWordZero)
        return SemanticValue::simple(SemanticKind::SelectorWord);
      return {};
    }
    if (Operation->Op == Opcode::AND && Operation->Inputs.size() == 2) {
      const SemanticValue &First = input(*Operation, 0);
      const SemanticValue &Second = input(*Operation, 1);
      if ((First.Kind == SemanticKind::SelectorWord &&
           isSelectorMask(Second)) ||
          (Second.Kind == SemanticKind::SelectorWord && isSelectorMask(First)))
        return SemanticValue::simple(SemanticKind::SelectorWord);
      return {};
    }
    if (Operation->Op == Opcode::CALLVALUE && Operation->Inputs.empty())
      return SemanticValue::callValue(SemanticKind::CallValue, Operation->PC);
    if (Operation->Op == Opcode::CALLDATASIZE && Operation->Inputs.empty())
      return SemanticValue::simple(SemanticKind::CalldataSize);
    if (Operation->Op == Opcode::ISZERO && Operation->Inputs.size() == 1) {
      SemanticValue Operand = input(*Operation, 0);
      if (Operand.Kind == SemanticKind::CallValue) {
        Operand.Kind = SemanticKind::IsZeroCallValue;
        return Operand;
      }
      if (Operand.Kind == SemanticKind::CalldataSize)
        return SemanticValue::simple(SemanticKind::IsZeroCalldataSize);
      return {};
    }
    if (Operation->Op == Opcode::EQ && Operation->Inputs.size() == 2) {
      const SemanticValue &First = input(*Operation, 0);
      const SemanticValue &Second = input(*Operation, 1);
      const auto Match =
          [](const SemanticValue &Constant,
             const SemanticValue &Selector) -> std::optional<uint32_t> {
        if (Constant.Kind != SemanticKind::Constant ||
            Constant.Word.getActiveBits() > kSelectorBits ||
            Selector.Kind != SemanticKind::SelectorWord)
          return std::nullopt;
        return static_cast<uint32_t>(Constant.Word.getZExtValue());
      };
      if (const auto Selector = Match(First, Second))
        return SemanticValue::selectorEquality(*Selector);
      if (const auto Selector = Match(Second, First))
        return SemanticValue::selectorEquality(*Selector);
      return {};
    }
    return {};
  }

  SemanticValue evaluate(ValueID ID) const {
    const MedValue &Value = Med.Values[ID];
    if (Value.Constant)
      return SemanticValue::constant(*Value.Constant);
    if (Value.Kind == ValueKind::Phi)
      return evaluatePhi(Value);
    if (Value.Kind == ValueKind::Instruction)
      return evaluateInstruction(ID);
    return {};
  }

  const EVMMedIR &Med;
  const ProducerIndex &Index;
  std::vector<VisitState> States;
  std::vector<SemanticValue> Results;
};

std::optional<uint64_t> jumpDestination(const EVMMedIR &Med,
                                        const MedOperation &Jump) {
  if (Jump.Inputs.size() != 2)
    return std::nullopt;
  return asAddress(Med.findValue(Jump.Inputs[0]));
}

bool hasConcreteTrueEdge(const EVMLowIR &Low, uint64_t BlockPC,
                         uint64_t Destination) {
  return Low.JumpDestinations.contains(Destination) &&
         Low.hasEdge(BlockPC, Destination, EdgeKind::ConditionalTrue);
}

std::set<uint64_t> nonPayableGuardReads(const EVMLowIR &Low,
                                        const ProducerIndex &Index,
                                        SemanticClassifier &Classifier,
                                        const std::set<uint64_t> &Blocks) {
  std::set<uint64_t> Reads;
  for (uint64_t BlockPC : Blocks) {
    const MedBlock *Block = Index.block(BlockPC);
    const LowBlock *LowBlock = Low.findBlock(BlockPC);
    if (!Block || !LowBlock)
      continue;
    for (const MedOperation &Operation : Block->Operations) {
      if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
        continue;
      const SemanticValue Condition = Classifier.classify(Operation.Inputs[1]);
      if (Condition.Kind != SemanticKind::IsZeroCallValue)
        continue;
      for (const LowEdge &Edge : LowBlock->Successors)
        if (Edge.Kind == EdgeKind::ConditionalFalse && Edge.Target &&
            endsInRevert(Low, *Edge.Target))
          Reads.insert(Condition.OriginPCs.begin(), Condition.OriginPCs.end());
    }
  }
  return Reads;
}

} // namespace

EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &Med) {
  EVMHighIR High;
  High.Diagnostics = Med.Diagnostics;
  const ProducerIndex Index(Med);
  if (!Index.valid())
    High.Diagnostics.push_back(
        {Index.errorPC(), kMalformedMedIRDiagnostic.str()});
  SemanticClassifier Classifier(Med, Index);

  std::map<uint32_t, RecoveredFunction> Functions;
  std::set<uint32_t> AmbiguousSelectors;
  if (Index.valid()) {
    for (const MedBlock &Block : Med.Blocks) {
      const LowBlock *LowBlock = Low.findBlock(Block.StartPC);
      if (!LowBlock || !LowBlock->Reachable)
        continue;
      for (const MedOperation &Operation : Block.Operations) {
        if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
          continue;
        const SemanticValue Condition =
            Classifier.classify(Operation.Inputs[1]);
        if (Condition.Kind != SemanticKind::SelectorEquality)
          continue;
        const auto Entry = jumpDestination(Med, Operation);
        if (!Entry || !hasConcreteTrueEdge(Low, Block.StartPC, *Entry))
          continue;
        const uint32_t Selector = Condition.Selector;
        if (AmbiguousSelectors.contains(Selector))
          continue;
        auto [FunctionIt, Inserted] = Functions.try_emplace(Selector);
        if (!Inserted) {
          if (FunctionIt->second.EntryPC == *Entry)
            continue;
          High.Diagnostics.push_back(
              {Operation.PC,
               "duplicate selector 0x" +
                   wordHexDigits(llvm::APInt(kSelectorBits, Selector),
                                 kSelectorHexDigits) +
                   " maps to multiple entry points"});
          Functions.erase(FunctionIt);
          AmbiguousSelectors.insert(Selector);
          continue;
        }
        RecoveredFunction &Function = FunctionIt->second;
        Function.Selector = Selector;
        Function.EntryPC = *Entry;
        Function.Name = kRecoveredFunctionPrefix.str() +
                        wordHexDigits(llvm::APInt(kSelectorBits, Selector),
                                      kSelectorHexDigits);
      }
    }
  }

  for (auto &[Selector, Function] : Functions) {
    (void)Selector;
    const std::set<uint64_t> FunctionBlocks =
        reachableFrom(Low, Function.EntryPC);
    const std::set<uint64_t> GuardReads =
        nonPayableGuardReads(Low, Index, Classifier, FunctionBlocks);
    StateAccessKind StateAccess = StateAccessKind::None;
    bool ReadsCallValue = false;
    bool ReturnsWord = false;
    std::set<uint64_t> ArgumentOffsets;
    for (uint64_t BlockPC : FunctionBlocks) {
      const LowBlock *LowBlock = Low.findBlock(BlockPC);
      const MedBlock *Block = Index.block(BlockPC);
      if (!LowBlock || !Block) {
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
        continue;
      }
      if (LowBlock->HasIndirectSuccessor)
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
      for (const MedOperation &Operation : Block->Operations) {
        const bool IsGuardRead = GuardReads.contains(Operation.PC);
        StateAccess =
            mergeStateAccess(StateAccess, IsGuardRead ? StateAccessKind::None
                                                      : Operation.StateAccess);
        ReadsCallValue |= !IsGuardRead && Operation.CallValueAccess ==
                                              CallValueAccessKind::Read;

        if (Operation.Op == Opcode::CALLDATALOAD &&
            Operation.Inputs.size() == 1) {
          const auto Offset = asAddress(Med.findValue(Operation.Inputs[0]));
          if (Offset && *Offset >= kSelectorBytes)
            ArgumentOffsets.insert(*Offset);
        }
        if (Operation.Op == Opcode::RETURN && Operation.Inputs.size() == 2) {
          const MedValue *Size = Med.findValue(Operation.Inputs[1]);
          ReturnsWord |= Size && Size->Constant &&
                         *Size->Constant == llvm::APInt(kWordBits, kWordBytes);
        }
      }
    }
    unsigned ArgumentIndex = 0;
    for (uint64_t Offset : ArgumentOffsets) {
      RecoveredArgument Argument;
      Argument.Index = ArgumentIndex;
      Argument.CalldataOffset = Offset;
      Argument.Name =
          kRecoveredArgumentPrefix.str() + std::to_string(ArgumentIndex++);
      Function.Arguments.push_back(std::move(Argument));
    }
    if (ReturnsWord)
      Function.Returns.push_back(kDefaultRecoveredWordType.str());
    Function.StateMutability = recoveredMutability(StateAccess, ReadsCallValue);
    High.Functions.push_back(Function);
    High.Regions.push_back({Function.EntryPC,
                            RegionKind::Function,
                            {FunctionBlocks.begin(), FunctionBlocks.end()}});
  }

  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOperation &Operation : Block.Operations) {
      const LowInstruction *Instruction = instructionAt(Low, Operation.PC);
      if (!Instruction || !Instruction->isExecutable())
        continue;
      if (Operation.Op == Opcode::SLOAD || Operation.Op == Opcode::SSTORE ||
          Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE) {
        StorageFact Fact;
        Fact.PC = Operation.PC;
        Fact.IsWrite =
            Operation.Op == Opcode::SSTORE || Operation.Op == Opcode::TSTORE;
        Fact.IsTransient =
            Operation.Op == Opcode::TLOAD || Operation.Op == Opcode::TSTORE;
        if (Index.valid() && !Operation.Inputs.empty())
          if (const MedValue *Key = Med.findValue(Operation.Inputs[0]);
              Key && Key->Constant)
            Fact.Slot = Key->Constant;
        Fact.SuggestedName = kUnknownStorageName.str();
        if (Fact.Slot)
          Fact.SuggestedName =
              kStorageSlotPrefix.str() + wordHexDigits(*Fact.Slot);
        High.Storage.push_back(std::move(Fact));
      }
      if (evm::isLog(Operation.Op)) {
        EventFact Fact;
        Fact.PC = Operation.PC;
        Fact.Topics = logTopicCount(Operation.Op);
        if (Index.valid() && Fact.Topics != 0 && Operation.Inputs.size() > 2)
          if (const MedValue *Topic = Med.findValue(Operation.Inputs[2]);
              Topic && Topic->Constant)
            Fact.Topic0 = Topic->Constant;
        Fact.SuggestedName =
            kRecoveredEventPrefix.str() + llvm::utohexstr(Operation.PC);
        High.Events.push_back(std::move(Fact));
      }
    }
  }

  for (size_t I = 0; I < Low.Instructions.size(); ++I) {
    const LowInstruction &Instruction = Low.Instructions[I];
    if (!Instruction.is(Opcode::REVERT))
      continue;
    ErrorFact Fact;
    Fact.PC = Instruction.PC;
    if (auto Candidate = precedingPush(Low, I, kErrorSelectorSearchWindow);
        Candidate && Candidate->getActiveBits() <= kSelectorBits)
      Fact.Selector = static_cast<uint32_t>(Candidate->getZExtValue());
    Fact.SuggestedName =
        Fact.Selector
            ? kRecoveredErrorPrefix.str() +
                  wordHexDigits(llvm::APInt(kSelectorBits, *Fact.Selector),
                                kSelectorHexDigits)
            : kRecoveredRevertName.str();
    High.Errors.push_back(std::move(Fact));
  }

  if (Index.valid()) {
    for (const MedBlock &Block : Med.Blocks) {
      const LowBlock *LowBlock = Low.findBlock(Block.StartPC);
      if (!LowBlock || !LowBlock->Reachable)
        continue;
      for (const MedOperation &Operation : Block.Operations) {
        if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
          continue;
        const SemanticValue Condition =
            Classifier.classify(Operation.Inputs[1]);
        const auto Destination = jumpDestination(Med, Operation);
        if (Condition.Kind == SemanticKind::IsZeroCalldataSize && Destination &&
            hasConcreteTrueEdge(Low, Block.StartPC, *Destination))
          High.HasReceive = true;
      }
    }
  }

  High.HasFallback = true;
  if (High.Regions.empty()) {
    StructuredRegion Root;
    Root.EntryPC = kEntryPC;
    Root.Kind = RegionKind::CFG;
    for (const LowBlock &Block : Low.Blocks)
      Root.Blocks.push_back(Block.StartPC);
    High.Regions.push_back(std::move(Root));
  }
  return High;
}

} // namespace neverd::evm
