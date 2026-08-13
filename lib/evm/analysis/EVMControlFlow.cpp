//===- EVMControlFlow.cpp - Whole-program EVM control-flow analysis -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMControlFlow.h"

#include "neverd/evm/analysis/EVMAnalyzer.h"
#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <optional>
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

class AbstractWord {
public:
  static AbstractWord unknown() { return AbstractWord(); }

  static AbstractWord constant(const llvm::APInt &Value) {
    AbstractWord Result;
    Result.IsUnknown = false;
    Result.Values.push_back(Value);
    return Result;
  }

  static AbstractWord finite(std::vector<llvm::APInt> NewValues,
                             bool IsOverapproximation = false) {
    if (NewValues.empty())
      return unknown();
    llvm::sort(NewValues, wordLess);
    NewValues.erase(std::unique(NewValues.begin(), NewValues.end()),
                    NewValues.end());
    AbstractWord Result;
    Result.IsUnknown = false;
    Result.IsOverapproximation = IsOverapproximation;
    Result.Values = std::move(NewValues);
    return Result;
  }

  [[nodiscard]] bool isUnknown() const { return IsUnknown; }
  [[nodiscard]] bool isExact() const {
    return !IsUnknown && !IsOverapproximation;
  }
  [[nodiscard]] llvm::ArrayRef<llvm::APInt> values() const { return Values; }

  [[nodiscard]] bool mayBeZero() const {
    return IsUnknown || llvm::any_of(Values, [](const llvm::APInt &Value) {
             return Value.isZero();
           });
  }

  [[nodiscard]] bool mayBeNonZero() const {
    return IsUnknown || llvm::any_of(Values, [](const llvm::APInt &Value) {
             return !Value.isZero();
           });
  }

  bool join(const AbstractWord &Other, size_t MaximumValues) {
    if (IsUnknown)
      return false;
    if (Other.IsUnknown) {
      IsUnknown = true;
      IsOverapproximation = false;
      Values.clear();
      return true;
    }

    bool Changed = false;
    if (Other.IsOverapproximation && !IsOverapproximation) {
      IsOverapproximation = true;
      Changed = true;
    }
    for (const llvm::APInt &Value : Other.Values) {
      auto It = std::lower_bound(Values.begin(), Values.end(), Value, wordLess);
      if (It != Values.end() && *It == Value)
        continue;
      Values.insert(It, Value);
      Changed = true;
      if (Values.size() > MaximumValues) {
        IsUnknown = true;
        IsOverapproximation = false;
        Values.clear();
        return true;
      }
    }
    return Changed;
  }

private:
  bool IsUnknown = true;
  bool IsOverapproximation = false;
  std::vector<llvm::APInt> Values;
};

using AbstractStack = std::vector<AbstractWord>;

/// One joined operand stack per concrete height preserves the EVM's valid
/// path-dependent stack-height behavior without manufacturing slot alignment.
struct BlockState {
  std::map<size_t, AbstractStack> StacksByHeight;
};

AbstractWord evaluateAbstractPure(Opcode Op,
                                  llvm::ArrayRef<AbstractWord> Inputs,
                                  uint64_t PC, size_t CodeSize,
                                  size_t MaximumValues) {
  if (Op == Opcode::PC)
    return AbstractWord::constant(llvm::APInt(kWordBits, PC));
  if (Op == Opcode::CODESIZE)
    return AbstractWord::constant(llvm::APInt(kWordBits, CodeSize));

  const auto Info = assignedOpcodeInfo(Op);
  if (!Info || !isALU(*Info))
    return AbstractWord::unknown();

  size_t ProductSize = 1;
  size_t VaryingInputs = 0;
  bool IsOverapproximation = false;
  for (const AbstractWord &Input : Inputs) {
    if (Input.isUnknown())
      return AbstractWord::unknown();
    IsOverapproximation |= !Input.isExact();
    VaryingInputs += Input.values().size() > 1 ? 1 : 0;
    if (Input.values().size() > MaximumValues / ProductSize)
      return AbstractWord::unknown();
    ProductSize *= Input.values().size();
  }

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
    for (const llvm::APInt &Value : Inputs[Index].values()) {
      ConcreteInputs.push_back(Value);
      if (!Self(Self, Index + 1))
        return false;
      ConcreteInputs.pop_back();
    }
    return true;
  };
  if (!Enumerate(Enumerate, 0))
    return AbstractWord::unknown();
  IsOverapproximation |= VaryingInputs > 1;
  return AbstractWord::finite(std::move(Results), IsOverapproximation);
}

class ControlFlowAnalysis {
public:
  ControlFlowAnalysis(EVMLowIR &Low, const AnalyzeOptions &Options)
      : Low(Low), Options(Options), States(Low.Blocks.size()),
        Queued(Low.Blocks.size(), false) {
    for (size_t Index = 0; Index < Low.Blocks.size(); ++Index)
      BlockIndices.emplace(Low.Blocks[Index].StartPC, Index);
  }

  llvm::Error run() {
    if (Options.MaxAbstractValuesPerSlot == 0)
      return optionError("MaxAbstractValuesPerSlot");
    if (Options.MaxStackHeightVariants == 0)
      return optionError("MaxStackHeightVariants");
    if (Low.Blocks.empty())
      return llvm::Error::success();

    States.front().StacksByHeight.emplace(0, AbstractStack{});
    enqueue(0);
    while (!Worklist.empty()) {
      const size_t BlockIndex = Worklist.front();
      Worklist.pop_front();
      Queued[BlockIndex] = false;

      std::vector<AbstractStack> EntryStacks;
      EntryStacks.reserve(States[BlockIndex].StacksByHeight.size());
      for (const auto &[Height, Stack] : States[BlockIndex].StacksByHeight) {
        (void)Height;
        EntryStacks.push_back(Stack);
      }
      for (AbstractStack &Stack : EntryStacks)
        if (llvm::Error Error = transferBlock(BlockIndex, std::move(Stack)))
          return Error;
    }

    finalize();
    return llvm::Error::success();
  }

private:
  llvm::Expected<bool> joinEntry(size_t BlockIndex,
                                 const AbstractStack &Incoming) {
    BlockState &State = States[BlockIndex];
    auto Existing = State.StacksByHeight.find(Incoming.size());
    if (Existing == State.StacksByHeight.end()) {
      if (State.StacksByHeight.size() >= Options.MaxStackHeightVariants)
        return analysisError(Low.Blocks[BlockIndex].StartPC,
                             "abstract stack-height variant limit " +
                                 llvm::Twine(Options.MaxStackHeightVariants) +
                                 " exceeded");
      State.StacksByHeight.emplace(Incoming.size(), Incoming);
      return true;
    }

    bool Changed = false;
    for (size_t Slot = 0; Slot < Incoming.size(); ++Slot)
      Changed |= Existing->second[Slot].join(Incoming[Slot],
                                             Options.MaxAbstractValuesPerSlot);
    return Changed;
  }

  llvm::Error propagate(uint64_t TargetPC, const AbstractStack &Stack) {
    const auto Target = BlockIndices.find(TargetPC);
    if (Target == BlockIndices.end())
      return analysisError(TargetPC, "internal CFG target has no basic block");
    auto Changed = joinEntry(Target->second, Stack);
    if (!Changed)
      return Changed.takeError();
    if (*Changed)
      enqueue(Target->second);
    return llvm::Error::success();
  }

  void enqueue(size_t BlockIndex) {
    if (Queued[BlockIndex])
      return;
    Worklist.push_back(BlockIndex);
    Queued[BlockIndex] = true;
  }

  void addEdge(LowBlock &Block, EdgeKind Kind, std::optional<uint64_t> Target) {
    const auto Duplicate =
        llvm::find_if(Block.Successors, [&](const LowEdge &E) {
          return E.Kind == Kind && E.Target == Target;
        });
    if (Duplicate != Block.Successors.end())
      return;
    Block.Successors.push_back({Kind, Target});
    if (Kind == EdgeKind::Indirect)
      Block.HasIndirectSuccessor = true;
  }

  void addDiagnostic(uint64_t PC, std::string Message) {
    const auto Duplicate =
        llvm::find_if(Low.Diagnostics, [&](const Diagnostic &D) {
          return D.PC == PC && D.Message == Message;
        });
    if (Duplicate == Low.Diagnostics.end())
      Low.Diagnostics.push_back({PC, std::move(Message)});
  }

  llvm::Error reportFault(uint64_t PC, llvm::Twine Message) {
    if (Options.Strict)
      return analysisError(PC, Message);
    addDiagnostic(PC, Message.str());
    return llvm::Error::success();
  }

  llvm::Error resolveJump(size_t BlockIndex, EdgeKind Kind, uint64_t JumpPC,
                          const AbstractWord &Target,
                          const AbstractStack &Stack) {
    LowBlock &Block = Low.Blocks[BlockIndex];
    if (Target.isUnknown()) {
      addEdge(Block, EdgeKind::Indirect, std::nullopt);
      return llvm::Error::success();
    }

    for (const llvm::APInt &Value : Target.values()) {
      std::optional<uint64_t> Address;
      if (Value.getActiveBits() <= std::numeric_limits<uint64_t>::digits)
        Address = Value.getZExtValue();
      if (!Address || !Low.JumpDestinations.contains(*Address)) {
        const std::string Message =
            "jump target " + wordHex(Value) + " is not a JUMPDEST";
        if (Target.isExact()) {
          if (llvm::Error Error = reportFault(JumpPC, Message))
            return Error;
        } else {
          addDiagnostic(JumpPC, kOverapproximatedJumpTargetPrefix.str() +
                                    wordHex(Value) + " is not a JUMPDEST");
        }
        continue;
      }
      addEdge(Block, Kind, Address);
      if (llvm::Error Error = propagate(*Address, Stack))
        return Error;
    }
    return llvm::Error::success();
  }

  llvm::Error transferBlock(size_t BlockIndex, AbstractStack Stack) {
    LowBlock &Block = Low.Blocks[BlockIndex];
    Block.EntryStackHeights.insert(Stack.size());

    for (size_t Index = Block.FirstInstruction;
         Index < Block.FirstInstruction + Block.InstructionCount; ++Index) {
      const LowInstruction &Instruction = Low.Instructions[Index];
      if (!Instruction.isExecutable()) {
        Block.ExitStackHeights.insert(Stack.size());
        return llvm::Error::success();
      }

      if (Stack.size() < Instruction.requiredStackHeight()) {
        Block.ExitStackHeights.insert(Stack.size());
        return reportFault(Instruction.PC,
                           "stack underflow in " +
                               llvm::Twine(Instruction.Info.Name));
      }
      const std::ptrdiff_t ResultHeight =
          static_cast<std::ptrdiff_t>(Stack.size()) + Instruction.stackDelta();
      if (ResultHeight > static_cast<std::ptrdiff_t>(kStackLimit)) {
        Block.ExitStackHeights.insert(Stack.size());
        return reportFault(Instruction.PC,
                           "stack limit exceeds " + llvm::Twine(kStackLimit));
      }

      if (Instruction.isPush()) {
        Stack.push_back(AbstractWord::constant(Instruction.Immediate));
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

      llvm::SmallVector<AbstractWord, kMaxOpcodeStackPops> Inputs;
      Inputs.reserve(Instruction.Info.StackPops);
      for (uint8_t Input = 0; Input < Instruction.Info.StackPops; ++Input) {
        Inputs.push_back(std::move(Stack.back()));
        Stack.pop_back();
      }

      if (Instruction.is(Opcode::JUMP)) {
        Block.ExitStackHeights.insert(Stack.size());
        return resolveJump(BlockIndex, EdgeKind::Jump, Instruction.PC,
                           Inputs.front(), Stack);
      }
      if (Instruction.is(Opcode::JUMPI)) {
        Block.ExitStackHeights.insert(Stack.size());
        const bool MaybeTrue = Inputs[1].mayBeNonZero();
        const bool MaybeFalse = Inputs[1].mayBeZero();
        if (MaybeTrue)
          if (llvm::Error Error =
                  resolveJump(BlockIndex, EdgeKind::ConditionalTrue,
                              Instruction.PC, Inputs.front(), Stack))
            return Error;
        if (MaybeFalse && BlockIndex + 1 < Low.Blocks.size()) {
          const uint64_t Fallthrough = Low.Blocks[BlockIndex + 1].StartPC;
          addEdge(Block, EdgeKind::ConditionalFalse, Fallthrough);
          if (llvm::Error Error = propagate(Fallthrough, Stack))
            return Error;
        }
        return llvm::Error::success();
      }

      const AbstractWord Folded = evaluateAbstractPure(
          Instruction.opcode(), Inputs, Instruction.PC, Low.Code.size(),
          Options.MaxAbstractValuesPerSlot);
      for (uint8_t Output = 0; Output < Instruction.Info.StackPushes; ++Output)
        Stack.push_back(Output == 0 ? Folded : AbstractWord::unknown());

      if (Instruction.Info.IsTerminator) {
        Block.ExitStackHeights.insert(Stack.size());
        return llvm::Error::success();
      }
    }

    Block.ExitStackHeights.insert(Stack.size());
    if (BlockIndex + 1 >= Low.Blocks.size())
      return llvm::Error::success();
    const uint64_t Fallthrough = Low.Blocks[BlockIndex + 1].StartPC;
    addEdge(Block, EdgeKind::Fallthrough, Fallthrough);
    return propagate(Fallthrough, Stack);
  }

  void finalize() {
    const auto EdgeLess = [](const LowEdge &Left, const LowEdge &Right) {
      if (Left.Kind != Right.Kind)
        return Left.Kind < Right.Kind;
      return Left.Target < Right.Target;
    };
    for (size_t Index = 0; Index < Low.Blocks.size(); ++Index) {
      LowBlock &Block = Low.Blocks[Index];
      Block.Reachable = !States[Index].StacksByHeight.empty();
      llvm::sort(Block.Successors, EdgeLess);
      Block.Successors.erase(
          std::unique(Block.Successors.begin(), Block.Successors.end(),
                      [](const LowEdge &Left, const LowEdge &Right) {
                        return Left.Kind == Right.Kind &&
                               Left.Target == Right.Target;
                      }),
          Block.Successors.end());
      Block.Predecessors.clear();
    }

    for (const LowBlock &Block : Low.Blocks)
      for (const LowEdge &Edge : Block.Successors)
        if (Edge.Target)
          if (LowBlock *Target = Low.findBlock(*Edge.Target))
            Target->Predecessors.push_back(Block.StartPC);
    for (LowBlock &Block : Low.Blocks) {
      llvm::sort(Block.Predecessors);
      Block.Predecessors.erase(
          std::unique(Block.Predecessors.begin(), Block.Predecessors.end()),
          Block.Predecessors.end());
    }

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
  }

  EVMLowIR &Low;
  const AnalyzeOptions &Options;
  std::vector<BlockState> States;
  std::map<uint64_t, size_t> BlockIndices;
  std::deque<size_t> Worklist;
  std::vector<bool> Queued;
};

} // namespace

llvm::Error analyzeControlFlow(EVMLowIR &Low, const AnalyzeOptions &Options) {
  return ControlFlowAnalysis(Low, Options).run();
}

} // namespace neverd::evm
