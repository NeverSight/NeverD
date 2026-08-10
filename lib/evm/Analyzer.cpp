//===- Analyzer.cpp - Staged EVM bytecode analysis ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Analyzer.h"

#include "neverd/evm/Semantics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <set>

namespace neverd::evm {
namespace {

using Constant = std::optional<llvm::APInt>;

inline constexpr size_t kDefaultPrecedingPushWindow = 2;
inline constexpr size_t kDispatcherEqualitySearchDistance = 3;
inline constexpr size_t kDispatcherDestinationSearchDistance = 2;
inline constexpr size_t kEventTopicSearchWindow = 8;
inline constexpr size_t kErrorSelectorSearchWindow = 10;

Mutability recoveredMutability(StateAccessKind Access, bool ReadsCallValue) {
  if (Access == StateAccessKind::Unknown)
    return Mutability::NonPayable;
  // Solidity payability is orthogonal to its pure/view state-access lattice.
  // An unguarded source-level CALLVALUE read cannot be declared pure, view, or
  // nonpayable in modern Solidity, even when it performs no state writes.
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

llvm::Error analysisError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits = 1) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  std::string Result = Digits.str().str();
  if (Result.size() < MinDigits)
    Result.insert(Result.begin(), MinDigits - Result.size(), '0');
  return Result;
}

std::string wordHex(const llvm::APInt &Value, unsigned MinDigits = 1) {
  return "0x" + wordHexDigits(Value, MinDigits);
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

struct BlockConstants {
  Constant JumpTarget;
  Constant JumpCondition;
};

BlockConstants analyzeBlockConstants(const EVMLowIR &Low,
                                     const LowBlock &Block) {
  std::vector<Constant> Stack;
  auto Pop = [&]() -> Constant {
    if (Stack.empty())
      return std::nullopt;
    Constant Value = Stack.back();
    Stack.pop_back();
    return Value;
  };
  BlockConstants Result;
  for (size_t Index = Block.FirstInstruction;
       Index < Block.FirstInstruction + Block.InstructionCount; ++Index) {
    const LowInstruction &Instruction = Low.Instructions[Index];
    const Opcode Op = Instruction.opcode();
    if (!Instruction.isExecutable())
      break;
    if (Instruction.isPush()) {
      Stack.push_back(Instruction.Immediate);
      continue;
    }
    if (Instruction.isDup()) {
      const size_t Depth = Instruction.dupDepth();
      Stack.push_back(Stack.size() >= Depth ? Stack[Stack.size() - Depth]
                                            : Constant{});
      continue;
    }
    if (Instruction.isSwap()) {
      const size_t Depth = Instruction.swapDepth();
      if (Stack.size() > Depth)
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      else
        Stack.clear();
      continue;
    }
    if (Instruction.isExchange()) {
      const auto [First, Second] = *Instruction.exchangeDepths();
      if (Stack.size() > Second)
        std::swap(Stack[Stack.size() - First - 1],
                  Stack[Stack.size() - Second - 1]);
      else
        Stack.clear();
      continue;
    }
    if (Instruction.is(Opcode::POP)) {
      (void)Pop();
      continue;
    }

    std::vector<Constant> Inputs;
    Inputs.reserve(Instruction.Info.StackPops);
    for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
      Inputs.push_back(Pop());
    if (Instruction.isJump()) {
      Result.JumpTarget = Inputs.empty() ? Constant{} : Inputs[0];
      if (Instruction.is(Opcode::JUMPI))
        Result.JumpCondition = Inputs.size() > 1 ? Inputs[1] : Constant{};
    }
    const Constant Folded =
        evaluatePure(Op, Inputs, Instruction.PC, Low.Code.size());
    for (uint8_t I = 0; I < Instruction.Info.StackPushes; ++I)
      Stack.push_back(I == 0 ? Folded : Constant{});
  }
  return Result;
}

std::optional<uint64_t> asAddress(const Constant &Value) {
  if (!Value || Value->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Value->getZExtValue();
}

llvm::Error addValidatedJump(EVMLowIR &Low, LowBlock &Block, EdgeKind Kind,
                             uint64_t JumpPC, const Constant &Target,
                             AnalyzeOptions Options) {
  if (!Target) {
    Block.Successors.push_back({EdgeKind::Indirect, std::nullopt});
    Block.HasIndirectSuccessor = true;
    return llvm::Error::success();
  }
  const auto Address = asAddress(Target);
  if (!Address || !Low.JumpDestinations.contains(*Address)) {
    std::string TargetText = wordHex(*Target);
    if (Options.Strict)
      return analysisError(JumpPC, "jump target " + llvm::Twine(TargetText) +
                                       " is not a JUMPDEST");
    Low.Diagnostics.push_back(
        {JumpPC, "jump target " + TargetText + " is not a JUMPDEST"});
    return llvm::Error::success();
  }
  Block.Successors.push_back({Kind, Address});
  return llvm::Error::success();
}

std::optional<uint64_t> nextBlock(const EVMLowIR &Low, size_t Index) {
  if (Index + 1 >= Low.Blocks.size())
    return std::nullopt;
  return Low.Blocks[Index + 1].StartPC;
}

llvm::Error computeStackHeights(EVMLowIR &Low, AnalyzeOptions Options) {
  if (Low.Blocks.empty())
    return llvm::Error::success();
  Low.Blocks.front().EntryStackHeight = 0;
  std::deque<size_t> Worklist{0};
  std::vector<bool> Queued(Low.Blocks.size(), false);
  Queued[0] = true;
  llvm::DenseMap<uint64_t, size_t> BlockIndex;
  for (size_t I = 0; I < Low.Blocks.size(); ++I)
    BlockIndex.insert({Low.Blocks[I].StartPC, I});

  while (!Worklist.empty()) {
    const size_t BI = Worklist.front();
    Worklist.pop_front();
    Queued[BI] = false;
    LowBlock &Block = Low.Blocks[BI];
    if (!Block.EntryStackHeight)
      return analysisError(Block.StartPC,
                           "internal CFG block has no entry stack height");
    size_t Height = Block.EntryStackHeight.value();
    for (size_t II = Block.FirstInstruction;
         II < Block.FirstInstruction + Block.InstructionCount; ++II) {
      const LowInstruction &Instruction = Low.Instructions[II];
      const size_t Required = Instruction.requiredStackHeight();
      const std::ptrdiff_t Delta = Instruction.stackDelta();
      if (Height < Required) {
        if (Options.Strict)
          return analysisError(Instruction.PC,
                               "stack underflow in " +
                                   llvm::Twine(Instruction.Info.Name));
        Low.Diagnostics.push_back(
            {Instruction.PC,
             "stack underflow in " + std::string(Instruction.Info.Name)});
        Height = Required;
      }
      Height = static_cast<size_t>(static_cast<std::ptrdiff_t>(Height) + Delta);
      if (Height > kStackLimit) {
        if (Options.Strict)
          return analysisError(Instruction.PC, "stack limit exceeds " +
                                                   llvm::Twine(kStackLimit));
        Low.Diagnostics.push_back(
            {Instruction.PC,
             "stack limit exceeds " + std::to_string(kStackLimit)});
        Height = kStackLimit;
      }
    }
    Block.ExitStackHeight = Height;
    for (const auto &Edge : Block.Successors) {
      if (!Edge.Target)
        continue;
      auto It = BlockIndex.find(*Edge.Target);
      if (It == BlockIndex.end())
        continue;
      LowBlock &Successor = Low.Blocks[It->second];
      if (!Successor.EntryStackHeight) {
        Successor.EntryStackHeight = Height;
        if (!Queued[It->second]) {
          Worklist.push_back(It->second);
          Queued[It->second] = true;
        }
      } else if (*Successor.EntryStackHeight != Height) {
        if (Options.Strict)
          return analysisError(Successor.StartPC,
                               "inconsistent stack height at CFG merge (" +
                                   llvm::Twine(*Successor.EntryStackHeight) +
                                   " vs " + llvm::Twine(Height) + ")");
        Low.Diagnostics.push_back(
            {Successor.StartPC, "inconsistent stack height at CFG merge"});
      }
    }
  }
  return llvm::Error::success();
}

void markReachable(EVMLowIR &Low) {
  if (Low.Blocks.empty())
    return;
  llvm::DenseMap<uint64_t, size_t> Indices;
  for (size_t I = 0; I < Low.Blocks.size(); ++I)
    Indices[Low.Blocks[I].StartPC] = I;
  std::queue<size_t> Queue;
  Low.Blocks[0].Reachable = true;
  Queue.push(0);
  while (!Queue.empty()) {
    const size_t BI = Queue.front();
    Queue.pop();
    for (const auto &Edge : Low.Blocks[BI].Successors) {
      if (!Edge.Target)
        continue;
      auto It = Indices.find(*Edge.Target);
      if (It != Indices.end() && !Low.Blocks[It->second].Reachable) {
        Low.Blocks[It->second].Reachable = true;
        Queue.push(It->second);
      }
    }
  }
}

ValueID addValue(EVMMedIR &Med, ValueKind Kind, uint64_t PC,
                 llvm::StringRef Name, std::vector<ValueID> Inputs = {},
                 Constant Folded = std::nullopt) {
  ValueID ID = static_cast<ValueID>(Med.Values.size());
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

const LowInstruction *instructionAt(const EVMLowIR &Low, uint64_t PC) {
  auto It =
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
    uint64_t PC = Queue.front();
    Queue.pop();
    const LowBlock *Block = Low.findBlock(PC);
    for (const auto &Edge : Block->Successors) {
      if (Edge.Target && Seen.insert(*Edge.Target).second)
        Queue.push(*Edge.Target);
    }
  }
  return Seen;
}

enum class PayabilityValueKind : uint8_t {
  Other,
  CallValue,
  IsZeroCallValue,
};

struct PayabilityValue {
  PayabilityValueKind Kind = PayabilityValueKind::Other;
  uint64_t CallValuePC = 0;
};

/// Recognize Solidity's canonical non-payable modifier without relying on
/// instruction adjacency. The condition must be ISZERO(CALLVALUE), with DUP
/// and SWAP stack transport handled symbolically, and its false edge must end
/// in REVERT. Returning the originating PC lets mutability recovery suppress
/// only the compiler guard read rather than every CALLVALUE in the function.
std::optional<uint64_t> nonPayableGuardCallValue(const EVMLowIR &Low,
                                                 const LowBlock &Block) {
  std::vector<PayabilityValue> Stack(Block.EntryStackHeight.value_or(0),
                                     PayabilityValue{});
  const auto Ensure = [&](size_t Count) {
    while (Stack.size() < Count)
      Stack.insert(Stack.begin(), PayabilityValue{});
  };
  const auto Pop = [&] {
    PayabilityValue Value = Stack.back();
    Stack.pop_back();
    return Value;
  };

  for (size_t I = Block.FirstInstruction;
       I < Block.FirstInstruction + Block.InstructionCount; ++I) {
    const LowInstruction &Instruction = Low.Instructions[I];
    if (!Instruction.isExecutable())
      return std::nullopt;
    if (Instruction.isPush()) {
      Stack.push_back({});
      continue;
    }
    if (Instruction.isDup()) {
      const size_t Depth = Instruction.dupDepth();
      Ensure(Depth);
      Stack.push_back(Stack[Stack.size() - Depth]);
      continue;
    }
    if (Instruction.isSwap()) {
      const size_t Depth = Instruction.swapDepth();
      Ensure(Depth + 1);
      std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      continue;
    }
    if (Instruction.isExchange()) {
      const auto [First, Second] = *Instruction.exchangeDepths();
      Ensure(Second + 1);
      std::swap(Stack[Stack.size() - First - 1],
                Stack[Stack.size() - Second - 1]);
      continue;
    }

    Ensure(Instruction.Info.StackPops);
    std::vector<PayabilityValue> Inputs;
    Inputs.reserve(Instruction.Info.StackPops);
    for (uint8_t Input = 0; Input < Instruction.Info.StackPops; ++Input)
      Inputs.push_back(Pop());

    PayabilityValue Output;
    if (Instruction.Info.CallValueAccess == CallValueAccessKind::Read) {
      Output = {PayabilityValueKind::CallValue, Instruction.PC};
    } else if (Instruction.is(Opcode::ISZERO) && !Inputs.empty() &&
               Inputs.front().Kind == PayabilityValueKind::CallValue) {
      Output = {PayabilityValueKind::IsZeroCallValue,
                Inputs.front().CallValuePC};
    }

    if (Instruction.is(Opcode::JUMPI)) {
      if (Inputs.size() <= 1 ||
          Inputs[1].Kind != PayabilityValueKind::IsZeroCallValue)
        return std::nullopt;
      for (const LowEdge &Edge : Block.Successors) {
        if (Edge.Kind != EdgeKind::ConditionalFalse || !Edge.Target)
          continue;
        const LowBlock *Failure = Low.findBlock(*Edge.Target);
        if (!Failure || Failure->InstructionCount == 0)
          continue;
        const LowInstruction &Last =
            Low.Instructions[Failure->FirstInstruction +
                             Failure->InstructionCount - 1];
        if (Last.is(Opcode::REVERT))
          return Inputs[1].CallValuePC;
      }
      return std::nullopt;
    }

    for (uint8_t Result = 0; Result < Instruction.Info.StackPushes; ++Result)
      Stack.push_back(Result == 0 ? Output : PayabilityValue{});
  }
  return std::nullopt;
}

std::optional<llvm::APInt>
precedingPush(const EVMLowIR &Low, size_t Index,
              size_t Window = kDefaultPrecedingPushWindow) {
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

} // namespace

llvm::Expected<EVMLowIR> decodeLowIR(llvm::ArrayRef<uint8_t> Code,
                                     AnalyzeOptions Options) {
  auto Decoded = decodeBytecode(Code, Options);
  if (!Decoded)
    return Decoded.takeError();

  EVMLowIR Low;
  Low.Fork = Decoded->Fork;
  Low.Strict = Decoded->Strict;
  Low.Code = std::move(Decoded->Code);
  Low.Instructions = std::move(Decoded->Instructions);
  Low.JumpDestinations = std::move(Decoded->JumpDestinations);
  Low.Diagnostics = std::move(Decoded->Diagnostics);

  std::set<uint64_t> Starts{kEntryPC};
  for (const auto &Instruction : Low.Instructions) {
    if (Instruction.is(Opcode::JUMPDEST))
      Starts.insert(Instruction.PC);
    if (Instruction.isTerminator() && Instruction.NextPC < Low.Code.size())
      Starts.insert(Instruction.NextPC);
  }

  llvm::DenseMap<uint64_t, size_t> InstructionIndex;
  for (size_t I = 0; I < Low.Instructions.size(); ++I)
    InstructionIndex[Low.Instructions[I].PC] = I;
  for (auto It = Starts.begin(); It != Starts.end(); ++It) {
    auto Found = InstructionIndex.find(*It);
    if (Found == InstructionIndex.end())
      continue;
    LowBlock Block;
    Block.StartPC = *It;
    Block.FirstInstruction = Found->second;
    auto Next = std::next(It);
    Block.EndPC = Next == Starts.end() ? Low.Code.size() : *Next;
    size_t EndIndex = Block.FirstInstruction;
    while (EndIndex < Low.Instructions.size() &&
           Low.Instructions[EndIndex].PC < Block.EndPC)
      ++EndIndex;
    Block.InstructionCount = EndIndex - Block.FirstInstruction;
    Low.Blocks.push_back(std::move(Block));
  }

  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    LowBlock &Block = Low.Blocks[BI];
    if (Block.InstructionCount == 0)
      continue;
    const LowInstruction &Last =
        Low.Instructions[Block.FirstInstruction + Block.InstructionCount - 1];
    const BlockConstants Constants = analyzeBlockConstants(Low, Block);
    if (Last.is(Opcode::JUMP)) {
      if (llvm::Error E = addValidatedJump(Low, Block, EdgeKind::Jump, Last.PC,
                                           Constants.JumpTarget, Options))
        return std::move(E);
    } else if (Last.is(Opcode::JUMPI)) {
      const bool MaybeTrue =
          !Constants.JumpCondition || !Constants.JumpCondition->isZero();
      const bool MaybeFalse =
          !Constants.JumpCondition || Constants.JumpCondition->isZero();
      if (MaybeTrue)
        if (llvm::Error E =
                addValidatedJump(Low, Block, EdgeKind::ConditionalTrue, Last.PC,
                                 Constants.JumpTarget, Options))
          return std::move(E);
      if (MaybeFalse)
        if (auto Next = nextBlock(Low, BI))
          Block.Successors.push_back({EdgeKind::ConditionalFalse, Next});
    } else if (!Last.isTerminator()) {
      if (auto Next = nextBlock(Low, BI))
        Block.Successors.push_back({EdgeKind::Fallthrough, Next});
    }
  }

  for (const auto &Block : Low.Blocks)
    for (const auto &Edge : Block.Successors)
      if (Edge.Target)
        if (LowBlock *Target = Low.findBlock(*Edge.Target))
          Target->Predecessors.push_back(Block.StartPC);
  for (auto &Block : Low.Blocks) {
    std::sort(Block.Predecessors.begin(), Block.Predecessors.end());
    Block.Predecessors.erase(
        std::unique(Block.Predecessors.begin(), Block.Predecessors.end()),
        Block.Predecessors.end());
  }
  markReachable(Low);
  if (llvm::Error E = computeStackHeights(Low, Options))
    return std::move(E);
  return Low;
}

llvm::Expected<EVMMedIR> lowerToMedIR(const EVMLowIR &Low) {
  EVMMedIR Med;
  Med.Diagnostics = Low.Diagnostics;
  Med.Blocks.resize(Low.Blocks.size());

  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    const LowBlock &LowBlock = Low.Blocks[BI];
    MedBlock &Block = Med.Blocks[BI];
    Block.StartPC = LowBlock.StartPC;
    const size_t EntryHeight = LowBlock.EntryStackHeight.value_or(0);
    for (size_t Slot = 0; Slot < EntryHeight; ++Slot) {
      const ValueID Phi =
          addValue(Med, ValueKind::Phi, LowBlock.StartPC, kStackPhiValueName);
      Block.EntryStack.push_back(Phi);
      Block.PhiValues.push_back(Phi);
    }
  }

  for (size_t BI = 0; BI < Low.Blocks.size(); ++BI) {
    const LowBlock &LowBlock = Low.Blocks[BI];
    MedBlock &Block = Med.Blocks[BI];
    std::vector<ValueID> Stack = Block.EntryStack;
    auto Ensure = [&](size_t Count, uint64_t PC) {
      while (Stack.size() < Count)
        Stack.insert(Stack.begin(), unknownValue(Med, PC));
    };
    auto Pop = [&]() {
      ValueID Value = Stack.back();
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
        ValueID Value = addValue(Med, ValueKind::Constant, Instruction.PC,
                                 "constant", {}, Instruction.Immediate);
        Stack.push_back(Value);
        Operation.Outputs.push_back(Value);
      } else if (Instruction.isDup()) {
        const size_t Depth = Instruction.dupDepth();
        Ensure(Depth, Instruction.PC);
        ValueID Input = Stack[Stack.size() - Depth];
        Constant Folded = Med.Values[Input].Constant;
        ValueID Output = addValue(Med, ValueKind::Instruction, Instruction.PC,
                                  Operation.Name, {Input}, Folded);
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
        std::vector<Constant> Constants;
        Constants.reserve(Operation.Inputs.size());
        for (ValueID Input : Operation.Inputs)
          Constants.push_back(Med.Values[Input].Constant);
        Constant Folded = evaluatePure(Instruction.opcode(), Constants,
                                       Instruction.PC, Low.Code.size());
        for (uint8_t I = 0; I < Instruction.Info.StackPushes; ++I) {
          ValueID Output = addValue(Med, ValueKind::Instruction, Instruction.PC,
                                    Operation.Name, Operation.Inputs,
                                    I == 0 ? Folded : Constant{});
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
    for (size_t Slot = 0; Slot < Block.PhiValues.size(); ++Slot) {
      MedValue &Phi = Med.Values[Block.PhiValues[Slot]];
      for (uint64_t PredPC : LowBlock.Predecessors) {
        auto It = Indices.find(PredPC);
        if (It == Indices.end())
          continue;
        const auto &Exit = Med.Blocks[It->second].ExitStack;
        if (Slot < Exit.size()) {
          Phi.Inputs.push_back(Exit[Slot]);
          Phi.IncomingBlocks.push_back(PredPC);
        }
      }
      if (!Phi.Inputs.empty()) {
        Constant Common = Med.Values[Phi.Inputs.front()].Constant;
        for (ValueID Input : Phi.Inputs)
          if (!Common || Med.Values[Input].Constant != Common) {
            Common.reset();
            break;
          }
        Phi.Constant = std::move(Common);
      }
    }
  }
  return Med;
}

EVMHighIR recoverHighIR(const EVMLowIR &Low, const EVMMedIR &) {
  EVMHighIR High;
  High.Diagnostics = Low.Diagnostics;
  std::map<uint32_t, RecoveredFunction> Functions;
  std::set<uint32_t> AmbiguousSelectors;

  for (size_t I = 0; I < Low.Instructions.size(); ++I) {
    const auto &Push = Low.Instructions[I];
    if (!Push.is(Opcode::PUSH4))
      continue;
    size_t EQ = I + 1;
    while (EQ < Low.Instructions.size() &&
           EQ <= I + kDispatcherEqualitySearchDistance &&
           !Low.Instructions[EQ].is(Opcode::EQ))
      ++EQ;
    if (EQ >= Low.Instructions.size() ||
        EQ > I + kDispatcherEqualitySearchDistance)
      continue;
    size_t DestinationPush = EQ + 1;
    while (DestinationPush < Low.Instructions.size() &&
           DestinationPush <= EQ + kDispatcherDestinationSearchDistance &&
           !Low.Instructions[DestinationPush].isPush())
      ++DestinationPush;
    if (DestinationPush >= Low.Instructions.size() ||
        DestinationPush > EQ + kDispatcherDestinationSearchDistance ||
        DestinationPush + 1 >= Low.Instructions.size() ||
        !Low.Instructions[DestinationPush + 1].is(Opcode::JUMPI))
      continue;
    const auto Entry = asAddress(Low.Instructions[DestinationPush].Immediate);
    if (!Entry || !Low.JumpDestinations.contains(*Entry))
      continue;
    const uint32_t Selector =
        static_cast<uint32_t>(Push.Immediate.getZExtValue());
    if (AmbiguousSelectors.contains(Selector))
      continue;
    auto [FunctionIt, Inserted] = Functions.try_emplace(Selector);
    if (!Inserted) {
      if (FunctionIt->second.EntryPC == *Entry)
        continue;
      High.Diagnostics.push_back(
          {Push.PC, "duplicate selector 0x" +
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
    Function.Name =
        kRecoveredFunctionPrefix.str() +
        wordHexDigits(llvm::APInt(kSelectorBits, Selector), kSelectorHexDigits);
  }

  for (auto &[Selector, Function] : Functions) {
    const std::set<uint64_t> FunctionBlocks =
        reachableFrom(Low, Function.EntryPC);
    std::set<uint64_t> NonPayableGuardReads;
    for (uint64_t BlockPC : FunctionBlocks)
      if (const LowBlock *Block = Low.findBlock(BlockPC))
        if (auto GuardRead = nonPayableGuardCallValue(Low, *Block))
          NonPayableGuardReads.insert(*GuardRead);
    StateAccessKind StateAccess = StateAccessKind::None;
    bool ReadsCallValue = false;
    bool ReturnsWord = false;
    std::set<uint64_t> ArgumentOffsets;
    for (uint64_t BlockPC : FunctionBlocks) {
      const LowBlock *Block = Low.findBlock(BlockPC);
      if (!Block)
        continue;
      if (Block->HasIndirectSuccessor)
        StateAccess = mergeStateAccess(StateAccess, StateAccessKind::Unknown);
      for (size_t I = Block->FirstInstruction;
           I < Block->FirstInstruction + Block->InstructionCount; ++I) {
        const LowInstruction &Instruction = Low.Instructions[I];
        const bool IsNonPayableGuardRead =
            NonPayableGuardReads.contains(Instruction.PC);
        StateAccessKind InstructionAccess = StateAccessKind::Unknown;
        if (Instruction.isExecutable())
          InstructionAccess = IsNonPayableGuardRead
                                  ? StateAccessKind::None
                                  : Instruction.Info.StateAccess;
        StateAccess = mergeStateAccess(StateAccess, InstructionAccess);
        ReadsCallValue |=
            !IsNonPayableGuardRead && Instruction.isExecutable() &&
            Instruction.Info.CallValueAccess == CallValueAccessKind::Read;
        if (Instruction.is(Opcode::CALLDATALOAD) && I > 0 &&
            Low.Instructions[I - 1].isPush()) {
          const auto Offset = asAddress(Low.Instructions[I - 1].Immediate);
          if (Offset && *Offset >= kSelectorBytes)
            ArgumentOffsets.insert(*Offset);
        }
        if (Instruction.is(Opcode::RETURN) && I > Block->FirstInstruction) {
          for (size_t J = I; J-- > Block->FirstInstruction;)
            if (Low.Instructions[J].is(Opcode::MSTORE)) {
              ReturnsWord = true;
              break;
            }
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

  for (size_t I = 0; I < Low.Instructions.size(); ++I) {
    const auto &Instruction = Low.Instructions[I];
    if (Instruction.is(Opcode::SLOAD) || Instruction.is(Opcode::SSTORE) ||
        Instruction.is(Opcode::TLOAD) || Instruction.is(Opcode::TSTORE)) {
      StorageFact Fact;
      Fact.PC = Instruction.PC;
      Fact.IsWrite =
          Instruction.is(Opcode::SSTORE) || Instruction.is(Opcode::TSTORE);
      Fact.IsTransient =
          Instruction.is(Opcode::TLOAD) || Instruction.is(Opcode::TSTORE);
      Fact.Slot = precedingPush(Low, I);
      Fact.SuggestedName = kUnknownStorageName.str();
      if (Fact.Slot)
        Fact.SuggestedName =
            kStorageSlotPrefix.str() + wordHexDigits(*Fact.Slot);
      High.Storage.push_back(std::move(Fact));
    }
    if (Instruction.isLog()) {
      EventFact Fact;
      Fact.PC = Instruction.PC;
      Fact.Topics = logTopicCount(Instruction.opcode());
      Fact.Topic0 = precedingPush(Low, I, kEventTopicSearchWindow);
      Fact.SuggestedName =
          kRecoveredEventPrefix.str() + llvm::utohexstr(Instruction.PC);
      High.Events.push_back(std::move(Fact));
    }
    if (Instruction.is(Opcode::REVERT)) {
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
  }

  for (size_t I = 1; I < Low.Instructions.size(); ++I)
    if (Low.Instructions[I].is(Opcode::ISZERO) &&
        Low.Instructions[I - 1].is(Opcode::CALLDATASIZE))
      High.HasReceive = true;
  High.HasFallback = true;
  if (High.Regions.empty()) {
    StructuredRegion Root;
    Root.EntryPC = kEntryPC;
    Root.Kind = RegionKind::CFG;
    for (const auto &Block : Low.Blocks)
      Root.Blocks.push_back(Block.StartPC);
    High.Regions.push_back(std::move(Root));
  }
  return High;
}

llvm::Expected<EVMProgram> analyze(llvm::ArrayRef<uint8_t> Code,
                                   AnalyzeOptions Options) {
  auto Low = decodeLowIR(Code, Options);
  if (!Low)
    return Low.takeError();
  auto Med = lowerToMedIR(*Low);
  if (!Med)
    return Med.takeError();
  EVMProgram Program;
  Program.Low = std::move(*Low);
  Program.Med = std::move(*Med);
  if (Options.RecoverHighLevel)
    Program.High = recoverHighIR(Program.Low, Program.Med);
  return Program;
}

std::string dumpLowIR(const EVMLowIR &Low) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.low hardfork=" << hardforkName(Low.Fork)
     << " strict=" << (Low.Strict ? "true" : "false") << "\n";
  for (const auto &Block : Low.Blocks) {
    OS << "block 0x" << llvm::utohexstr(Block.StartPC)
       << (Block.Reachable ? " reachable" : " unreachable") << "\n";
    for (size_t I = Block.FirstInstruction;
         I < Block.FirstInstruction + Block.InstructionCount; ++I) {
      const auto &Instruction = Low.Instructions[I];
      OS << "  0x" << llvm::utohexstr(Instruction.PC) << ": "
         << Instruction.Info.Name;
      if (const std::string Immediate = formatImmediate(Instruction);
          !Immediate.empty())
        OS << " " << Immediate;
      if (const std::string Annotation = formatDecodeAnnotation(Instruction);
          !Annotation.empty())
        OS << " ; " << Annotation;
      OS << "\n";
    }
    for (const auto &Edge : Block.Successors) {
      OS << "    -> ";
      if (Edge.Target)
        OS << "0x" << llvm::utohexstr(*Edge.Target);
      else
        OS << "indirect";
      OS << "\n";
    }
  }
  return Text;
}

std::string dumpMedIR(const EVMMedIR &Med) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.med word=i" << kWordBits << "\n";
  for (const auto &Block : Med.Blocks) {
    OS << "block 0x" << llvm::utohexstr(Block.StartPC) << "\n";
    for (const auto &Operation : Block.Operations) {
      for (ValueID Output : Operation.Outputs)
        OS << "  %" << Output << " = ";
      if (Operation.Outputs.empty())
        OS << "  ";
      OS << Operation.Name;
      for (ValueID Input : Operation.Inputs)
        OS << " %" << Input;
      bool HasAnnotation = false;
      const auto EmitAnnotation = [&](llvm::StringRef Annotation) {
        OS << (HasAnnotation ? ", " : " ; ") << Annotation;
        HasAnnotation = true;
      };
      if (Operation.Effect != EffectKind::None)
        EmitAnnotation(effectName(Operation.Effect));
      if (Operation.MemoryAccess != MemoryAccessKind::None)
        EmitAnnotation(memoryAccessName(Operation.MemoryAccess));
      if (Operation.StateAccess != StateAccessKind::None)
        EmitAnnotation(stateAccessName(Operation.StateAccess));
      if (Operation.CallValueAccess != CallValueAccessKind::None)
        EmitAnnotation(callValueAccessName(Operation.CallValueAccess));
      OS << "\n";
    }
  }
  return Text;
}

std::string dumpHighIR(const EVMHighIR &High) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.high\n";
  for (const auto &Function : High.Functions) {
    OS << "function " << Function.Name << " selector "
       << wordHex(llvm::APInt(kSelectorBits, Function.Selector),
                  kSelectorHexDigits)
       << " entry 0x" << llvm::utohexstr(Function.EntryPC) << "\n";
  }
  for (const auto &Storage : High.Storage)
    OS << (Storage.IsTransient ? "transient" : "storage") << " "
       << (Storage.IsWrite ? "write" : "read") << " "
       << (Storage.Slot ? wordHex(*Storage.Slot) : "dynamic") << "\n";
  for (const auto &Event : High.Events)
    OS << "event " << Event.SuggestedName << " topics=" << Event.Topics << "\n";
  for (const auto &Error : High.Errors)
    OS << "error " << Error.SuggestedName << "\n";
  return Text;
}

} // namespace neverd::evm
