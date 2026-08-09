//===- Analyzer.cpp - Staged EVM bytecode analysis ----------------------===//

#include "neverd/evm/Analyzer.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace neverd::evm {
namespace {

using Constant = std::optional<llvm::APInt>;

inline constexpr size_t kDefaultPrecedingPushWindow = 2;
inline constexpr size_t kDispatcherEqualitySearchDistance = 3;
inline constexpr size_t kDispatcherDestinationSearchDistance = 2;
inline constexpr size_t kEventTopicSearchWindow = 8;
inline constexpr size_t kErrorSelectorSearchWindow = 10;

llvm::Error analysisError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

std::string byteHex(uint8_t Byte) {
  static constexpr char Digits[] = "0123456789abcdef";
  std::string Result = "0x00";
  Result[2] = Digits[Byte >> kHexDigitBits];
  Result[3] = Digits[Byte & kHexDigitMask];
  return Result;
}

std::string wordHex(const llvm::APInt &Value, unsigned MinDigits = 1) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, 16);
  std::string Result = Digits.str().str();
  if (Result.size() < MinDigits)
    Result.insert(Result.begin(), MinDigits - Result.size(), '0');
  return "0x" + Result;
}

OpcodeInfo unknownOpcode(uint8_t Byte) {
  return OpcodeInfo{static_cast<Opcode>(Byte),
                    kUnknownOpcodeName,
                    0,
                    0,
                    0,
                    OpcodeClass::Unknown,
                    Hardfork::Frontier,
                    EffectKind::Unknown,
                    true,
                    false,
                    false};
}

Constant boolWord(bool Value) {
  return llvm::APInt(kWordBits, Value ? 1 : 0);
}

llvm::APInt modularExponent(llvm::APInt Base, llvm::APInt Exponent) {
  llvm::APInt Result(kWordBits, 1);
  while (!Exponent.isZero()) {
    if (Exponent[0])
      Result *= Base;
    Exponent.lshrInPlace(1);
    Base *= Base;
  }
  return Result;
}

llvm::APInt signExtend(const llvm::APInt &ByteIndex, const llvm::APInt &Value) {
  if (ByteIndex.uge(kWordBytes))
    return Value;
  const unsigned Width =
      static_cast<unsigned>((ByteIndex.getZExtValue() + 1) * kBitsPerByte);
  return Value.trunc(Width).sext(kWordBits);
}

Constant evaluatePure(Opcode Op, const std::vector<Constant> &Inputs,
                      uint64_t PC, size_t CodeSize) {
  auto Has = [&](size_t Count) {
    if (Inputs.size() < Count)
      return false;
    for (size_t I = 0; I < Count; ++I)
      if (!Inputs[I])
        return false;
    return true;
  };
  if (Op == Opcode::PC)
    return llvm::APInt(kWordBits, PC);
  if (Op == Opcode::CODESIZE)
    return llvm::APInt(kWordBits, CodeSize);
  if (Op == Opcode::ISZERO && Has(1))
    return boolWord(Inputs[0]->isZero());
  if (Op == Opcode::NOT && Has(1))
    return ~*Inputs[0];
  if (Op == Opcode::CLZ && Has(1))
    return llvm::APInt(kWordBits, Inputs[0]->countl_zero());
  if (!Has(2))
    return std::nullopt;

  const llvm::APInt &A = *Inputs[0];
  const llvm::APInt &B = *Inputs[1];
  switch (Op) {
  case Opcode::ADD:
    return A + B;
  case Opcode::MUL:
    return A * B;
  case Opcode::SUB:
    return A - B;
  case Opcode::DIV:
    return B.isZero() ? llvm::APInt(kWordBits, 0) : A.udiv(B);
  case Opcode::SDIV:
    if (B.isZero())
      return llvm::APInt(kWordBits, 0);
    if (A.isMinSignedValue() && B.isAllOnes())
      return A;
    return A.sdiv(B);
  case Opcode::MOD:
    return B.isZero() ? llvm::APInt(kWordBits, 0) : A.urem(B);
  case Opcode::SMOD:
    return B.isZero() || (A.isMinSignedValue() && B.isAllOnes())
               ? llvm::APInt(kWordBits, 0)
               : A.srem(B);
  case Opcode::ADDMOD:
  case Opcode::MULMOD: {
    if (!Has(3))
      return std::nullopt;
    const llvm::APInt &Modulus = *Inputs[2];
    if (Modulus.isZero())
      return llvm::APInt(kWordBits, 0);
    const llvm::APInt Wide =
        Op == Opcode::ADDMOD
            ? A.zext(kWideWordBits) + B.zext(kWideWordBits)
            : A.zext(kWideWordBits) * B.zext(kWideWordBits);
    return Wide.urem(Modulus.zext(kWideWordBits)).trunc(kWordBits);
  }
  case Opcode::EXP:
    return modularExponent(A, B);
  case Opcode::SIGNEXTEND:
    return signExtend(A, B);
  case Opcode::LT:
    return boolWord(A.ult(B));
  case Opcode::GT:
    return boolWord(A.ugt(B));
  case Opcode::SLT:
    return boolWord(A.slt(B));
  case Opcode::SGT:
    return boolWord(A.sgt(B));
  case Opcode::EQ:
    return boolWord(A == B);
  case Opcode::AND:
    return A & B;
  case Opcode::OR:
    return A | B;
  case Opcode::XOR:
    return A ^ B;
  case Opcode::BYTE: {
    if (A.uge(kWordBytes))
      return llvm::APInt(kWordBits, 0);
    const unsigned Shift = static_cast<unsigned>(
        (kWordBytes - 1 - A.getZExtValue()) * kBitsPerByte);
    return llvm::APInt(
        kWordBits, B.extractBitsAsZExtValue(kBitsPerByte, Shift));
  }
  case Opcode::SHL:
    return A.uge(kWordBits) ? llvm::APInt(kWordBits, 0)
                      : B.shl(static_cast<unsigned>(A.getZExtValue()));
  case Opcode::SHR:
    return A.uge(kWordBits) ? llvm::APInt(kWordBits, 0)
                      : B.lshr(static_cast<unsigned>(A.getZExtValue()));
  case Opcode::SAR:
    return A.uge(kWordBits)
               ? (B.isNegative() ? llvm::APInt::getAllOnes(kWordBits)
                                 : llvm::APInt(kWordBits, 0))
                      : B.ashr(static_cast<unsigned>(A.getZExtValue()));
  default:
    return std::nullopt;
  }
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
    const Opcode Op = Instruction.Op;
    if (isPush(Op)) {
      Stack.push_back(Instruction.Immediate);
      continue;
    }
    if (isDup(Op)) {
      const size_t Depth = dupDepth(Op);
      Stack.push_back(Stack.size() >= Depth ? Stack[Stack.size() - Depth]
                                            : Constant{});
      continue;
    }
    if (isSwap(Op)) {
      const size_t Depth = swapDepth(Op);
      if (Stack.size() > Depth)
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      else
        Stack.clear();
      continue;
    }
    if (Op == Opcode::POP) {
      (void)Pop();
      continue;
    }

    std::vector<Constant> Inputs;
    Inputs.reserve(Instruction.Info.StackInputs);
    for (uint8_t I = 0; I < Instruction.Info.StackInputs; ++I)
      Inputs.push_back(Pop());
    if (isJump(Op)) {
      Result.JumpTarget = Inputs.empty() ? Constant{} : Inputs[0];
      if (Op == Opcode::JUMPI)
        Result.JumpCondition = Inputs.size() > 1 ? Inputs[1] : Constant{};
    }
    const Constant Folded =
        evaluatePure(Op, Inputs, Instruction.PC, Low.Code.size());
    for (uint8_t I = 0; I < Instruction.Info.StackOutputs; ++I)
      Stack.push_back(I == 0 ? Folded : Constant{});
  }
  return Result;
}

std::optional<uint64_t> asAddress(const Constant &Value) {
  if (!Value ||
      Value->getActiveBits() > std::numeric_limits<uint64_t>::digits)
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
  Block.Successors.push_back({Kind, *Address});
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
  std::unordered_map<uint64_t, size_t> BlockIndex;
  for (size_t I = 0; I < Low.Blocks.size(); ++I)
    BlockIndex.emplace(Low.Blocks[I].StartPC, I);

  while (!Worklist.empty()) {
    const size_t BI = Worklist.front();
    Worklist.pop_front();
    Queued[BI] = false;
    LowBlock &Block = Low.Blocks[BI];
    size_t Height = *Block.EntryStackHeight;
    for (size_t II = Block.FirstInstruction;
         II < Block.FirstInstruction + Block.InstructionCount; ++II) {
      const LowInstruction &Instruction = Low.Instructions[II];
      size_t Required = Instruction.Info.StackInputs;
      int Delta = static_cast<int>(Instruction.Info.StackOutputs) -
                  static_cast<int>(Instruction.Info.StackInputs);
      if (isPush(Instruction.Op)) {
        Required = 0;
        Delta = 1;
      } else if (isDup(Instruction.Op)) {
        Required = dupDepth(Instruction.Op);
        Delta = 1;
      } else if (isSwap(Instruction.Op)) {
        Required = swapDepth(Instruction.Op) + 1;
        Delta = 0;
      }
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
      Height = static_cast<size_t>(static_cast<int64_t>(Height) + Delta);
      if (Height > kStackLimit) {
        if (Options.Strict)
          return analysisError(Instruction.PC,
                               "stack limit exceeds " + llvm::Twine(kStackLimit));
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
  std::unordered_map<uint64_t, size_t> Indices;
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

std::optional<llvm::APInt> precedingPush(const EVMLowIR &Low, size_t Index,
                                         size_t Window =
                                             kDefaultPrecedingPushWindow) {
  const size_t Begin = Index > Window ? Index - Window : 0;
  for (size_t I = Index; I-- > Begin;) {
    if (isPush(Low.Instructions[I].Op))
      return Low.Instructions[I].Immediate;
    if (Low.Instructions[I].Info.Class == OpcodeClass::Control)
      break;
  }
  return std::nullopt;
}

} // namespace

llvm::Expected<EVMLowIR> decodeLowIR(std::span<const uint8_t> Code,
                                     AnalyzeOptions Options) {
  if (Code.empty())
    return analysisError(0, "empty bytecode");
  if (Code.size() > Options.MaxCodeSize)
    return analysisError(0, "bytecode exceeds configured size limit");

  EVMLowIR Low;
  Low.Fork = Options.Fork;
  Low.Strict = Options.Strict;
  Low.Code.assign(Code.begin(), Code.end());

  for (size_t PC = 0; PC < Code.size();) {
    const size_t Start = PC;
    const uint8_t Byte = Code[PC++];
    const auto Info = opcodeInfo(Byte, Options.Fork);
    LowInstruction Instruction;
    Instruction.PC = Start;
    Instruction.Op = static_cast<Opcode>(Byte);
    Instruction.Info = Info.value_or(unknownOpcode(Byte));
    Instruction.Known = Info.has_value();
    Instruction.Encoding.push_back(Byte);
    if (!Info) {
      const bool AssignedLater = opcodeInfo(Byte, Hardfork::Latest).has_value();
      const std::string Reason =
          AssignedLater ? "inactive opcode " : "unknown opcode ";
      if (Options.Strict)
        return analysisError(Start, Reason + llvm::Twine(byteHex(Byte)));
      Low.Diagnostics.push_back({Start, Reason + byteHex(Byte)});
    }

    if (Instruction.Info.ImmediateBytes != 0) {
      const uint8_t Width = Instruction.Info.ImmediateBytes;
      llvm::APInt Value(kWordBits, 0);
      for (uint8_t I = 0; I < Width; ++I) {
        Value <<= kBitsPerByte;
        if (PC < Code.size()) {
          Value |= Code[PC];
          Instruction.Encoding.push_back(Code[PC++]);
        } else {
          Instruction.ImmediateTruncated = true;
        }
      }
      Instruction.Immediate = std::move(Value);
    }
    Instruction.NextPC = PC;
    if (Instruction.Op == Opcode::JUMPDEST)
      Low.JumpDestinations.insert(Start);
    Low.Instructions.push_back(std::move(Instruction));
  }

  std::set<uint64_t> Starts{0};
  for (const auto &Instruction : Low.Instructions) {
    if (Instruction.Op == Opcode::JUMPDEST)
      Starts.insert(Instruction.PC);
    if ((Instruction.Info.IsTerminator || isJump(Instruction.Op)) &&
        Instruction.NextPC < Low.Code.size())
      Starts.insert(Instruction.NextPC);
  }

  std::unordered_map<uint64_t, size_t> InstructionIndex;
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
    if (Last.Op == Opcode::JUMP) {
      if (llvm::Error E = addValidatedJump(Low, Block, EdgeKind::Jump, Last.PC,
                                           Constants.JumpTarget, Options))
        return std::move(E);
    } else if (Last.Op == Opcode::JUMPI) {
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
          Block.Successors.push_back({EdgeKind::ConditionalFalse, *Next});
    } else if (!Last.Info.IsTerminator) {
      if (auto Next = nextBlock(Low, BI))
        Block.Successors.push_back({EdgeKind::Fallthrough, *Next});
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
      Operation.Op = Instruction.Op;
      Operation.Name = std::string(Instruction.Info.Name);
      Operation.Effect = Instruction.Known ? Instruction.Info.Effect
                                           : EffectKind::Unknown;

      if (isPush(Instruction.Op)) {
        ValueID Value = addValue(Med, ValueKind::Constant, Instruction.PC,
                                 "constant", {}, Instruction.Immediate);
        Stack.push_back(Value);
        Operation.Outputs.push_back(Value);
      } else if (isDup(Instruction.Op)) {
        const size_t Depth = dupDepth(Instruction.Op);
        Ensure(Depth, Instruction.PC);
        ValueID Input = Stack[Stack.size() - Depth];
        Constant Folded = Med.Values[Input].Constant;
        ValueID Output = addValue(Med, ValueKind::Instruction, Instruction.PC,
                                  Operation.Name, {Input}, Folded);
        Operation.Inputs.push_back(Input);
        Operation.Outputs.push_back(Output);
        Stack.push_back(Output);
      } else if (isSwap(Instruction.Op)) {
        const size_t Depth = swapDepth(Instruction.Op);
        Ensure(Depth + 1, Instruction.PC);
        Operation.Inputs = {Stack.back(), Stack[Stack.size() - Depth - 1]};
        std::swap(Stack.back(), Stack[Stack.size() - Depth - 1]);
      } else {
        Ensure(Instruction.Info.StackInputs, Instruction.PC);
        for (uint8_t I = 0; I < Instruction.Info.StackInputs; ++I)
          Operation.Inputs.push_back(Pop());
        std::vector<Constant> Constants;
        Constants.reserve(Operation.Inputs.size());
        for (ValueID Input : Operation.Inputs)
          Constants.push_back(Med.Values[Input].Constant);
        Constant Folded = evaluatePure(Instruction.Op, Constants,
                                       Instruction.PC, Low.Code.size());
        for (uint8_t I = 0; I < Instruction.Info.StackOutputs; ++I) {
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

  std::unordered_map<uint64_t, size_t> Indices;
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

  for (size_t I = 0; I < Low.Instructions.size(); ++I) {
    const auto &Push = Low.Instructions[I];
    if (Push.Info.ImmediateBytes != kSelectorBytes)
      continue;
    size_t EQ = I + 1;
    while (EQ < Low.Instructions.size() &&
           EQ <= I + kDispatcherEqualitySearchDistance &&
           Low.Instructions[EQ].Op != Opcode::EQ)
      ++EQ;
    if (EQ >= Low.Instructions.size() ||
        EQ > I + kDispatcherEqualitySearchDistance)
      continue;
    size_t DestinationPush = EQ + 1;
    while (DestinationPush < Low.Instructions.size() &&
           DestinationPush <= EQ + kDispatcherDestinationSearchDistance &&
           !isPush(Low.Instructions[DestinationPush].Op))
      ++DestinationPush;
    if (DestinationPush >= Low.Instructions.size() ||
        DestinationPush > EQ + kDispatcherDestinationSearchDistance ||
        DestinationPush + 1 >= Low.Instructions.size() ||
        Low.Instructions[DestinationPush + 1].Op != Opcode::JUMPI)
      continue;
    const auto Entry = asAddress(Low.Instructions[DestinationPush].Immediate);
    if (!Entry || !Low.JumpDestinations.contains(*Entry))
      continue;
    const uint32_t Selector =
        static_cast<uint32_t>(Push.Immediate.getZExtValue());
    RecoveredFunction &Function = Functions[Selector];
    Function.Selector = Selector;
    Function.EntryPC = *Entry;
    Function.Name =
        kRecoveredFunctionPrefix.str() +
        wordHex(llvm::APInt(kSelectorBits, Selector), kSelectorHexDigits)
            .substr(2);
  }

  for (auto &[Selector, Function] : Functions) {
    const std::set<uint64_t> FunctionBlocks =
        reachableFrom(Low, Function.EntryPC);
    bool ReadsState = false;
    bool WritesState = false;
    bool ReturnsWord = false;
    std::set<uint64_t> ArgumentOffsets;
    for (uint64_t BlockPC : FunctionBlocks) {
      const LowBlock *Block = Low.findBlock(BlockPC);
      if (!Block)
        continue;
      for (size_t I = Block->FirstInstruction;
           I < Block->FirstInstruction + Block->InstructionCount; ++I) {
        const LowInstruction &Instruction = Low.Instructions[I];
        const Opcode Op = Instruction.Op;
        ReadsState |= Instruction.Info.Effect == EffectKind::StorageRead ||
                      Instruction.Info.Effect == EffectKind::TransientRead ||
                      Instruction.Info.Effect == EffectKind::ContextRead;
        WritesState |=
            Instruction.Info.Effect == EffectKind::StorageWrite ||
            Instruction.Info.Effect == EffectKind::TransientWrite ||
            Instruction.Info.Effect == EffectKind::Log ||
            Instruction.Info.Effect == EffectKind::Create ||
            Instruction.Info.Effect == EffectKind::ExternalCall ||
            Op == Opcode::SELFDESTRUCT;
        if (Op == Opcode::CALLDATALOAD && I > 0 &&
            isPush(Low.Instructions[I - 1].Op)) {
          const auto Offset = asAddress(Low.Instructions[I - 1].Immediate);
          if (Offset && *Offset >= kSelectorBytes)
            ArgumentOffsets.insert(*Offset);
        }
        if (Op == Opcode::RETURN && I > Block->FirstInstruction) {
          for (size_t J = I; J-- > Block->FirstInstruction;)
            if (Low.Instructions[J].Op == Opcode::MSTORE) {
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
    Function.StateMutability =
        WritesState ? Mutability::NonPayable
                    : (ReadsState ? Mutability::View : Mutability::Pure);
    High.Functions.push_back(Function);
    High.Regions.push_back({Function.EntryPC,
                            RegionKind::Function,
                            {FunctionBlocks.begin(), FunctionBlocks.end()}});
  }

  for (size_t I = 0; I < Low.Instructions.size(); ++I) {
    const auto &Instruction = Low.Instructions[I];
    if (Instruction.Op == Opcode::SLOAD ||
        Instruction.Op == Opcode::SSTORE ||
        Instruction.Op == Opcode::TLOAD ||
        Instruction.Op == Opcode::TSTORE) {
      StorageFact Fact;
      Fact.PC = Instruction.PC;
      Fact.IsWrite = Instruction.Op == Opcode::SSTORE ||
                     Instruction.Op == Opcode::TSTORE;
      Fact.IsTransient = Instruction.Op == Opcode::TLOAD ||
                         Instruction.Op == Opcode::TSTORE;
      Fact.Slot = precedingPush(Low, I);
      Fact.SuggestedName = kUnknownStorageName.str();
      if (Fact.Slot)
        Fact.SuggestedName =
            kStorageSlotPrefix.str() + wordHex(*Fact.Slot).substr(2);
      High.Storage.push_back(std::move(Fact));
    }
    if (isLog(Instruction.Op)) {
      EventFact Fact;
      Fact.PC = Instruction.PC;
      Fact.Topics = logTopicCount(Instruction.Op);
      Fact.Topic0 = precedingPush(Low, I, kEventTopicSearchWindow);
      Fact.SuggestedName =
          kRecoveredEventPrefix.str() + llvm::utohexstr(Instruction.PC);
      High.Events.push_back(std::move(Fact));
    }
    if (Instruction.Op == Opcode::REVERT) {
      ErrorFact Fact;
      Fact.PC = Instruction.PC;
      if (auto Candidate = precedingPush(Low, I, kErrorSelectorSearchWindow);
          Candidate && Candidate->getActiveBits() <= kSelectorBits)
        Fact.Selector = static_cast<uint32_t>(Candidate->getZExtValue());
      Fact.SuggestedName =
          Fact.Selector
              ? kRecoveredErrorPrefix.str() +
                    wordHex(llvm::APInt(kSelectorBits, *Fact.Selector),
                            kSelectorHexDigits)
                        .substr(2)
              : kRecoveredRevertName.str();
      High.Errors.push_back(std::move(Fact));
    }
  }

  for (size_t I = 1; I < Low.Instructions.size(); ++I)
    if (Low.Instructions[I].Op == Opcode::ISZERO &&
        Low.Instructions[I - 1].Op == Opcode::CALLDATASIZE)
      High.HasReceive = true;
  High.HasFallback = true;
  if (High.Regions.empty()) {
    StructuredRegion Root;
    Root.EntryPC = 0;
    Root.Kind = RegionKind::CFG;
    for (const auto &Block : Low.Blocks)
      Root.Blocks.push_back(Block.StartPC);
    High.Regions.push_back(std::move(Root));
  }
  return High;
}

llvm::Expected<EVMProgram> analyze(std::span<const uint8_t> Code,
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
      if (Instruction.Info.ImmediateBytes)
        OS << " " << wordHex(Instruction.Immediate);
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
  OS << "evm.med word=i256\n";
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
      if (Operation.Effect != EffectKind::None)
        OS << " ; " << effectName(Operation.Effect);
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
       << " entry 0x"
       << llvm::utohexstr(Function.EntryPC) << "\n";
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
