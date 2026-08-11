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
#include <utility>
#include <vector>

namespace neverd::evm {
namespace {

/// The value of \c ArgumentRecovery's owner map for a value that is not a
/// calldata head slot.
inline constexpr size_t kNoArgument = std::numeric_limits<size_t>::max();

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits = 1) {
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

/// The value as a machine-word-sized number, when it is a constant that fits
/// one.
std::optional<uint64_t> constantWord(const MedValue *Value) {
  if (!Value || !Value->Constant ||
      Value->Constant->getActiveBits() > std::numeric_limits<uint64_t>::digits)
    return std::nullopt;
  return Value->Constant->getZExtValue();
}

/// The argument position a constant calldata offset designates, when the
/// offset starts a head slot. An offset inside a slot reads into a dynamic
/// value's payload rather than naming an argument of its own.
std::optional<size_t> headSlot(uint64_t Offset) {
  if (Offset < kSelectorBytes)
    return std::nullopt;
  const uint64_t Relative = Offset - kSelectorBytes;
  if (Relative % kWordBytes != 0)
    return std::nullopt;
  const uint64_t Position = Relative / kWordBytes;
  if (Position >= kMaxRecoveredArguments)
    return std::nullopt;
  return static_cast<size_t>(Position);
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

  /// The operation at \p PC and the block that contains it, which is the
  /// context a payload built by neighbouring stores has to be read in.
  [[nodiscard]] const MedOperation *operation(uint64_t PC) const {
    const auto It = Operations.find(PC);
    return It == Operations.end() ? nullptr : It->second.second;
  }
  [[nodiscard]] const MedBlock *containingBlock(uint64_t PC) const {
    const auto It = Operations.find(PC);
    return It == Operations.end() ? nullptr : It->second.first;
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
        if (!Operations
                 .emplace(Operation.PC, std::make_pair(&Block, &Operation))
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

  bool Valid = true;
  uint64_t ErrorPC = kEntryPC;
  std::vector<const MedOperation *> Producers;
  std::map<uint64_t, std::pair<const MedBlock *, const MedOperation *>>
      Operations;
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

/// The calldata head slots one function reads, and what its own code says
/// about each of them.
///
/// A loaded word is followed through the operations that only move it, so a
/// mask applied to a duplicate still narrows the argument the duplicate came
/// from. Nothing here decides a type: the observations are handed to
/// \c ABIConstraint::resolve, which is the one place the precedence lives.
class ArgumentRecovery {
public:
  ArgumentRecovery(const EVMMedIR &Med, const ProducerIndex &Index,
                   const std::set<uint64_t> &Blocks) {
    for (uint64_t PC : Blocks)
      if (const MedBlock *Block = Index.block(PC))
        Ordered.push_back(Block);
    Owner.assign(Med.Values.size(), kNoArgument);
    seed(Med);
    if (Constraints.empty())
      return;
    propagate(Med);
    for (const MedBlock *Block : Ordered)
      for (const MedOperation &Operation : Block->Operations)
        observe(Med, Operation);
  }

  /// One past the highest head slot the function read, which is the smallest
  /// argument count consistent with what it does.
  [[nodiscard]] size_t count() const { return Constraints.size(); }
  [[nodiscard]] const ABIConstraint &constraint(size_t Position) const {
    return Constraints[Position];
  }
  [[nodiscard]] bool read(size_t Position) const { return Read[Position]; }

private:
  [[nodiscard]] size_t owner(ValueID Value) const {
    return Value < Owner.size() ? Owner[Value] : kNoArgument;
  }

  bool adopt(ValueID Value, size_t Position) {
    if (Position == kNoArgument || Value >= Owner.size() ||
        Owner[Value] != kNoArgument)
      return false;
    Owner[Value] = Position;
    return true;
  }

  void seed(const EVMMedIR &Med) {
    llvm::SmallVector<std::pair<ValueID, size_t>, 8> Loads;
    size_t Highest = 0;
    for (const MedBlock *Block : Ordered)
      for (const MedOperation &Operation : Block->Operations) {
        if (Operation.Op != Opcode::CALLDATALOAD ||
            Operation.Inputs.size() != 1 || Operation.Outputs.size() != 1)
          continue;
        const auto Offset = constantWord(Med.findValue(Operation.Inputs[0]));
        if (!Offset)
          continue;
        const auto Position = headSlot(*Offset);
        if (!Position)
          continue;
        Loads.emplace_back(Operation.Outputs[0], *Position);
        Highest = std::max(Highest, *Position);
      }
    if (Loads.empty())
      return;

    Constraints.resize(Highest + 1);
    Read.assign(Highest + 1, false);
    for (const auto &[Value, Position] : Loads) {
      adopt(Value, Position);
      Read[Position] = true;
    }
  }

  void propagate(const EVMMedIR &Med) {
    for (size_t Round = 0; Round < kMaxArgumentAliasRounds; ++Round) {
      bool Changed = false;
      for (const MedBlock *Block : Ordered) {
        for (const MedOperation &Operation : Block->Operations) {
          if (!evm::isDup(Operation.Op) && !evm::isDeepDup(Operation.Op))
            continue;
          if (Operation.Inputs.size() != 1 || Operation.Outputs.size() != 1)
            continue;
          Changed |=
              adopt(Operation.Outputs[0], owner(Operation.Inputs.front()));
        }
        // A merge carries one argument only when every path brought that same
        // argument; a merge of an argument with anything else is neither.
        for (ValueID Phi : Block->PhiValues) {
          const MedValue *Value = Med.findValue(Phi);
          if (!Value || Value->Inputs.empty())
            continue;
          size_t Common = owner(Value->Inputs.front());
          for (ValueID Incoming : Value->Inputs)
            if (owner(Incoming) != Common)
              Common = kNoArgument;
          Changed |= adopt(Phi, Common);
        }
      }
      if (!Changed)
        return;
    }
  }

  void observe(const EVMMedIR &Med, const MedOperation &Operation);

  std::vector<const MedBlock *> Ordered;
  std::vector<size_t> Owner;
  std::vector<ABIConstraint> Constraints;
  std::vector<bool> Read;
};

void ArgumentRecovery::observe(const EVMMedIR &Med,
                               const MedOperation &Operation) {
  const auto Argument = [&](size_t Position) -> ABIConstraint * {
    if (Position >= Operation.Inputs.size())
      return nullptr;
    const size_t Owned = owner(Operation.Inputs[Position]);
    return Owned == kNoArgument ? nullptr : &Constraints[Owned];
  };
  const auto Literal = [&](size_t Position) -> const llvm::APInt * {
    if (Position >= Operation.Inputs.size())
      return nullptr;
    const MedValue *Value = Med.findValue(Operation.Inputs[Position]);
    return Value && Value->Constant ? &*Value->Constant : nullptr;
  };
  const auto EveryOperand = [&](ABIEvidence Evidence) {
    for (size_t I = 0; I < Operation.Inputs.size(); ++I)
      if (ABIConstraint *Constraint = Argument(I))
        Constraint->observe(Evidence);
  };

  switch (Operation.Op) {
  case Opcode::AND:
  case Opcode::OR:
    if (Operation.Inputs.size() != 2)
      break;
    for (size_t I = 0; I < 2; ++I) {
      ABIConstraint *Constraint = Argument(I);
      if (!Constraint)
        continue;
      const llvm::APInt *Mask = Literal(1 - I);
      if (!Mask) {
        Constraint->observe(ABIEvidence::Bitwise);
        continue;
      }
      // AND keeps the bytes its mask sets; OR fills them, so what survives of
      // the value is the complement.
      const llvm::APInt Kept = Operation.Op == Opcode::AND ? *Mask : ~*Mask;
      if (const auto Bytes = lowByteMaskWidth(Kept)) {
        Constraint->observe(ABIEvidence::LowByteMask);
        Constraint->narrowTo(*Bytes);
      } else if (const auto Bytes = highByteMaskWidth(Kept)) {
        Constraint->observe(ABIEvidence::HighByteMask);
        Constraint->narrowTo(*Bytes);
      } else {
        Constraint->observe(ABIEvidence::Bitwise);
      }
    }
    break;
  case Opcode::XOR:
  case Opcode::NOT:
    EveryOperand(ABIEvidence::Bitwise);
    break;
  case Opcode::SIGNEXTEND: {
    ABIConstraint *Constraint = Argument(1);
    if (!Constraint)
      break;
    Constraint->observe(ABIEvidence::SignExtended);
    if (const llvm::APInt *Index = Literal(0); Index && Index->ult(kWordBytes))
      Constraint->narrowTo(static_cast<unsigned>(Index->getZExtValue()) + 1);
    break;
  }
  case Opcode::SLT:
  case Opcode::SGT:
  case Opcode::SDIV:
  case Opcode::SMOD:
    EveryOperand(ABIEvidence::SignedCompare);
    break;
  case Opcode::ISZERO:
    if (ABIConstraint *Constraint = Argument(0))
      Constraint->observe(ABIEvidence::BooleanTest);
    break;
  case Opcode::ADD:
  case Opcode::SUB:
  case Opcode::MUL:
  case Opcode::DIV:
  case Opcode::MOD:
  case Opcode::EXP:
  case Opcode::ADDMOD:
  case Opcode::MULMOD:
  case Opcode::LT:
  case Opcode::GT:
    EveryOperand(ABIEvidence::Arithmetic);
    break;
  case Opcode::SHL:
  case Opcode::SHR:
  case Opcode::SAR:
  case Opcode::BYTE:
    // The first operand says how far to shift or which byte to take, so only
    // the second is being treated as a byte string.
    if (ABIConstraint *Constraint = Argument(1))
      Constraint->observe(ABIEvidence::BitShift);
    break;
  case Opcode::BALANCE:
  case Opcode::EXTCODESIZE:
  case Opcode::EXTCODEHASH:
    if (ABIConstraint *Constraint = Argument(0))
      Constraint->observe(ABIEvidence::CallTarget);
    break;
  case Opcode::CALL:
  case Opcode::CALLCODE:
  case Opcode::DELEGATECALL:
  case Opcode::STATICCALL:
    // Every call in the family puts the callee second, after the gas
    // allowance.
    if (ABIConstraint *Constraint = Argument(1))
      Constraint->observe(ABIEvidence::CallTarget);
    break;
  default:
    break;
  }
}

/// The last word a store placed at \p Offset plus \p Displacement inside
/// \p Block before \p BeforePC.
///
/// A revert payload is assembled by the stores that immediately precede the
/// revert, so this is what reads one back. Matching on the offset's own value
/// covers the usual case where the same expression addresses both, and
/// matching on equal constants covers a payload written field by field.
const MedValue *storedWordAt(const EVMMedIR &Med, const MedBlock &Block,
                             ValueID Offset, uint64_t Displacement,
                             uint64_t BeforePC) {
  const auto Base = constantWord(Med.findValue(Offset));
  const MedValue *Found = nullptr;
  for (const MedOperation &Operation : Block.Operations) {
    if (Operation.PC >= BeforePC)
      break;
    if (Operation.Op != Opcode::MSTORE || Operation.Inputs.size() != 2)
      continue;
    const bool SameExpression =
        Displacement == 0 && Operation.Inputs[0] == Offset;
    const auto At = constantWord(Med.findValue(Operation.Inputs[0]));
    const bool SameAddress =
        Base && At &&
        Displacement <= std::numeric_limits<uint64_t>::max() - *Base &&
        *At == *Base + Displacement;
    if (SameExpression || SameAddress)
      Found = Med.findValue(Operation.Inputs[1]);
  }
  return Found;
}

/// What a revert hands back, to the extent the stores before it prove.
ErrorFact classifyRevert(const EVMMedIR &Med, const MedBlock &Block,
                         const MedOperation &Revert) {
  ErrorFact Fact;
  Fact.PC = Revert.PC;
  Fact.SuggestedName = kRecoveredRevertName.str();
  if (Revert.Inputs.size() != 2)
    return Fact;

  // A payload shorter than a selector cannot carry one, and an empty one is
  // the bare revert a require without a message compiles to.
  if (const auto Size = constantWord(Med.findValue(Revert.Inputs[1]));
      Size && *Size < kSelectorBytes)
    return Fact;

  const MedValue *Payload =
      storedWordAt(Med, Block, Revert.Inputs[0], 0, Revert.PC);
  if (!Payload || !Payload->Constant ||
      Payload->Constant->getBitWidth() != kWordBits)
    return Fact;

  // The ABI left-aligns a selector, so it is the leading four bytes of the
  // word the store wrote.
  const auto Selector =
      static_cast<uint32_t>(Payload->Constant->extractBitsAsZExtValue(
          kSelectorBits, kWordBits - kSelectorBits));
  if (Selector == 0)
    return Fact;

  Fact.Selector = Selector;
  Fact.Known = findKnownError(Selector);
  Fact.Kind = RevertKind::Custom;
  if (Fact.Known == &getLanguageRevertInfo(LanguageRevert::Message)) {
    Fact.Kind = RevertKind::Message;
  } else if (Fact.Known == &getLanguageRevertInfo(LanguageRevert::Panic)) {
    Fact.Kind = RevertKind::Panic;
    if (const MedValue *Code = storedWordAt(Med, Block, Revert.Inputs[0],
                                            kSelectorBytes, Revert.PC))
      if (const auto Value = constantWord(Code))
        Fact.Panic = findPanicCode(*Value);
  }
  Fact.SuggestedName =
      Fact.Known ? Fact.Known->name().str()
                 : kRecoveredErrorPrefix.str() + selectorHex(Selector);
  return Fact;
}

/// How the key of a storage access was formed, which is what separates a
/// declared variable from an element the program addressed.
StorageKeyKind storageKeyKind(const EVMMedIR &Med, const ProducerIndex &Index,
                              ValueID Key) {
  const MedValue *Value = Med.findValue(Key);
  if (!Value)
    return StorageKeyKind::Unknown;
  if (Value->Constant)
    return StorageKeyKind::Slot;
  const MedOperation *Producer = Index.producer(Key);
  if (!Producer)
    return StorageKeyKind::Unknown;
  if (Producer->Op == Opcode::SHA3)
    return StorageKeyKind::Hashed;
  // A mapping addresses its elements by hash; an array element, and a struct
  // field inside a mapping, are that hash plus a displacement.
  if (Producer->Op == Opcode::ADD)
    for (ValueID Input : Producer->Inputs)
      if (const MedOperation *Operand = Index.producer(Input);
          Operand && Operand->Op == Opcode::SHA3)
        return StorageKeyKind::HashedOffset;
  return StorageKeyKind::Unknown;
}

/// Where a call site's callee came from, and what that address is.
struct CalleeProvenance {
  CalleeKind Kind = CalleeKind::Dynamic;
  /// The address itself, when the code fixes it rather than loading it.
  std::optional<llvm::APInt> Address;
  /// The constant slot the address was loaded from.
  std::optional<llvm::APInt> Slot;
  const KnownSlotInfo *Named = nullptr;
};

/// Follow a call's callee operand back to whatever established it.
///
/// The walk reads through the operations that only move or clean the value,
/// because a compiler masks a loaded slot to twenty bytes before calling
/// through it and duplicates it to keep a copy for the return-data forwarding
/// that follows. Nothing here is specific to delegation: an upgradeable
/// proxy's implementation and a vault's underlying token are the same operand
/// established the same way.
CalleeProvenance traceCallee(const EVMMedIR &Med, const ProducerIndex &Index,
                             ValueID Callee) {
  CalleeProvenance Result;
  for (size_t Step = 0; Step < kMaxCalleeTraceSteps; ++Step) {
    const MedValue *Value = Med.findValue(Callee);
    if (!Value)
      return Result;
    if (Value->Constant) {
      Result.Kind = CalleeKind::Fixed;
      Result.Address = Value->Constant;
      return Result;
    }
    const MedOperation *Producer = Index.producer(Callee);
    if (!Producer)
      return Result;

    if (Producer->Op == Opcode::SLOAD || Producer->Op == Opcode::TLOAD) {
      if (Producer->Inputs.size() != 1)
        return Result;
      const ValueID Key = Producer->Inputs.front();
      if (const MedValue *Slot = Med.findValue(Key); Slot && Slot->Constant) {
        Result.Slot = Slot->Constant;
        Result.Named = findKnownSlot(*Slot->Constant);
        Result.Kind =
            Result.Named ? CalleeKind::NamedSlot : CalleeKind::ConstantSlot;
        return Result;
      }
      const StorageKeyKind KeyKind = storageKeyKind(Med, Index, Key);
      if (KeyKind == StorageKeyKind::Hashed ||
          KeyKind == StorageKeyKind::HashedOffset)
        Result.Kind = CalleeKind::ComputedSlot;
      return Result;
    }

    if ((evm::isDup(Producer->Op) || evm::isDeepDup(Producer->Op)) &&
        Producer->Inputs.size() == 1) {
      Callee = Producer->Inputs.front();
      continue;
    }
    // A mask that keeps exactly the low twenty bytes is the compiler cleaning
    // an address; anything else is arithmetic this analysis will not read
    // through.
    if (Producer->Op == Opcode::AND && Producer->Inputs.size() == 2) {
      const MedValue *First = Med.findValue(Producer->Inputs[0]);
      const MedValue *Second = Med.findValue(Producer->Inputs[1]);
      const auto IsAddressMask = [](const MedValue *Value) {
        return Value && Value->Constant &&
               *Value->Constant ==
                   llvm::APInt::getLowBitsSet(kWordBits, kAddressBits);
      };
      if (IsAddressMask(First) && !IsAddressMask(Second)) {
        Callee = Producer->Inputs[1];
        continue;
      }
      if (IsAddressMask(Second) && !IsAddressMask(First)) {
        Callee = Producer->Inputs[0];
        continue;
      }
    }
    return Result;
  }
  return Result;
}

/// The call that runs another contract's code in this contract's storage, and
/// what its target's provenance says about the shape of the proxy.
ProxyFact classifyDelegation(const EVMMedIR &Med, const ProducerIndex &Index,
                             const MedOperation &Call) {
  ProxyFact Fact;
  Fact.PC = Call.PC;
  Fact.Op = Call.Op;
  if (Call.Inputs.size() <= kCallCalleeOperand)
    return Fact;
  const CalleeProvenance Callee =
      traceCallee(Med, Index, Call.Inputs[kCallCalleeOperand]);
  Fact.Kind = Callee.Kind;
  Fact.Implementation = Callee.Address;
  Fact.Slot = Callee.Slot;
  Fact.Known = Callee.Named;
  return Fact;
}

/// The selector a call places at the start of the calldata it hands its
/// callee.
///
/// A compiler assembles that calldata by storing the left-aligned selector at
/// the head of the argument window and the arguments after it, which is the
/// same shape a revert payload has. The word the window starts with is
/// therefore what names the operation being requested.
std::optional<uint32_t> outboundSelector(const EVMMedIR &Med,
                                         const MedBlock &Block,
                                         const CallFamilyInfo &Family,
                                         const MedOperation &Call) {
  if (Call.Inputs.size() <= Family.argumentsLengthOperand())
    return std::nullopt;
  const ValueID Offset = Call.Inputs[Family.argumentsOffsetOperand()];
  const ValueID Size = Call.Inputs[Family.argumentsLengthOperand()];

  // Calldata too short to hold a selector carries none. An empty window is
  // what paying an address compiles to, and that address may have no code at
  // all.
  if (const auto Length = constantWord(Med.findValue(Size));
      Length && *Length < kSelectorBytes)
    return std::nullopt;

  const MedValue *Head = storedWordAt(Med, Block, Offset, 0, Call.PC);
  if (!Head || !Head->Constant || Head->Constant->getBitWidth() != kWordBits)
    return std::nullopt;
  const auto Selector =
      static_cast<uint32_t>(Head->Constant->extractBitsAsZExtValue(
          kSelectorBits, kWordBits - kSelectorBits));
  if (Selector == 0)
    return std::nullopt;
  return Selector;
}

/// One call out of this program, and everything the code proved about it.
CallFact classifyCall(const EVMMedIR &Med, const ProducerIndex &Index,
                      const MedBlock &Block, const CallFamilyInfo &Family,
                      const MedOperation &Call, Hardfork Fork) {
  CallFact Fact;
  Fact.PC = Call.PC;
  Fact.Op = Call.Op;
  if (Call.Inputs.size() > kCallCalleeOperand) {
    const CalleeProvenance Callee =
        traceCallee(Med, Index, Call.Inputs[kCallCalleeOperand]);
    Fact.TargetKind = Callee.Kind;
    Fact.Target = Callee.Address;
    Fact.Slot = Callee.Slot;
    Fact.NamedSlot = Callee.Named;
    if (Fact.Target)
      Fact.Precompiled = findPrecompile(*Fact.Target, Fork);
  }
  if (const auto Value = Family.valueOperand();
      Value && Call.Inputs.size() > *Value)
    if (const MedValue *Transferred = Med.findValue(Call.Inputs[*Value]);
        Transferred && Transferred->Constant)
      Fact.Value = Transferred->Constant;

  Fact.Selector = outboundSelector(Med, Block, Family, Call);
  if (Fact.Selector)
    Fact.Known = findKnownFunction(*Fact.Selector);

  // A reserved address names the operation outright, because native code has
  // no selector to send it.
  if (Fact.Precompiled)
    Fact.SuggestedName = Fact.Precompiled->Name.str();
  else if (Fact.Known)
    Fact.SuggestedName = Fact.Known->name().str();
  else if (Fact.Selector)
    Fact.SuggestedName =
        kRecoveredCallPrefix.str() + selectorHex(*Fact.Selector);
  else
    Fact.SuggestedName = kUnknownCallName.str();
  return Fact;
}

std::optional<uint64_t> jumpDestination(const EVMMedIR &Med,
                                        const MedOperation &Jump) {
  if (Jump.Inputs.size() != 2)
    return std::nullopt;
  return constantWord(Med.findValue(Jump.Inputs[0]));
}

bool hasConcreteTrueEdge(const EVMLowIR &Low, uint64_t BlockPC,
                         uint64_t Destination) {
  return Low.JumpDestinations.contains(Destination) &&
         Low.hasEdge(BlockPC, Destination, EdgeKind::ConditionalTrue);
}

/// True when \p Operation does something rejecting an unrecognized call does
/// not need to do.
///
/// A dispatcher that recognizes nothing reads the context to find the selector,
/// branches on it, and reverts. Reaching anything else means some code accepted
/// the call.
bool exceedsRejection(const MedOperation &Operation) {
  // INVALID and STOP halt alike but do not mean alike: one is how the machine
  // refuses a byte it cannot run, the other is how a contract accepts a call
  // and returns nothing.
  if (Operation.Op == Opcode::INVALID)
    return false;
  switch (Operation.Effect) {
  case EffectKind::StorageRead:
  case EffectKind::StorageWrite:
  case EffectKind::TransientRead:
  case EffectKind::TransientWrite:
  case EffectKind::ExternalCall:
  case EffectKind::Create:
  case EffectKind::Log:
  case EffectKind::SelfDestruct:
  case EffectKind::Return:
  case EffectKind::Halt:
    return true;
  case EffectKind::None:
  case EffectKind::ContextRead:
  case EffectKind::Control:
  case EffectKind::Revert:
  // Nothing was established, so nothing is proven.
  case EffectKind::Unknown:
    return false;
  }
  return false;
}

/// Whether the code a call reaches when its selector matched nothing does more
/// than reject it.
///
/// The walk starts at the entry and, wherever the dispatcher branches on a
/// selector equality or on calldata being empty, follows only the edge that
/// says the test failed. What remains is what a call carrying an unrecognized
/// selector can actually run. An indirect branch ends that path rather than
/// widening it, so an unreadable dispatcher reports no fallback instead of
/// claiming one.
bool reachesFallback(const EVMLowIR &Low, const ProducerIndex &Index,
                     SemanticClassifier &Classifier) {
  std::set<uint64_t> Seen{kEntryPC};
  std::vector<uint64_t> Worklist{kEntryPC};
  while (!Worklist.empty()) {
    const uint64_t BlockPC = Worklist.back();
    Worklist.pop_back();
    const LowBlock *LowBlock = Low.findBlock(BlockPC);
    const MedBlock *Block = Index.block(BlockPC);
    if (!LowBlock || !Block)
      continue;

    bool Dispatches = false;
    for (const MedOperation &Operation : Block->Operations) {
      if (exceedsRejection(Operation))
        return true;
      if (Operation.Op != Opcode::JUMPI || Operation.Inputs.size() != 2)
        continue;
      const SemanticKind Condition =
          Classifier.classify(Operation.Inputs[1]).Kind;
      Dispatches |= Condition == SemanticKind::SelectorEquality ||
                    Condition == SemanticKind::IsZeroCalldataSize;
    }

    for (const LowEdge &Edge : LowBlock->Successors) {
      if (!Edge.Target)
        continue;
      if (Dispatches && Edge.Kind == EdgeKind::ConditionalTrue)
        continue;
      if (Seen.insert(*Edge.Target).second)
        Worklist.push_back(*Edge.Target);
    }
  }
  return false;
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
              {Operation.PC, "duplicate selector 0x" + selectorHex(Selector) +
                                 " maps to multiple entry points"});
          Functions.erase(FunctionIt);
          AmbiguousSelectors.insert(Selector);
          continue;
        }
        RecoveredFunction &Function = FunctionIt->second;
        Function.Selector = Selector;
        Function.EntryPC = *Entry;
        // A tabulated signature that hashes to this selector exhibits a
        // preimage, so the name is recovered rather than invented.
        Function.Known = findKnownFunction(Selector);
        Function.Name = Function.Known ? Function.Known->name().str()
                                       : kRecoveredFunctionPrefix.str() +
                                             selectorHex(Selector);
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

        if (Operation.Op == Opcode::RETURN && Operation.Inputs.size() == 2) {
          const MedValue *Size = Med.findValue(Operation.Inputs[1]);
          ReturnsWord |= Size && Size->Constant &&
                         *Size->Constant == llvm::APInt(kWordBits, kWordBytes);
        }
      }
    }

    const ArgumentRecovery Arguments(Med, Index, FunctionBlocks);
    // A hashed signature settles the argument list, so it decides both how
    // many arguments there are and what each one is. Otherwise the head slots
    // the body read decide, and every slot below the highest is reported even
    // when nothing read it: dropping a gap would renumber the rest.
    const llvm::SmallVector<llvm::StringRef, 8> Declared =
        Function.Known ? signatureArgumentTypes(Function.Known->Signature)
                       : llvm::SmallVector<llvm::StringRef, 8>{};
    const size_t Count = Function.Known ? Declared.size() : Arguments.count();
    for (size_t Position = 0; Position < Count; ++Position) {
      RecoveredArgument Argument;
      Argument.Index = static_cast<unsigned>(Position);
      Argument.CalldataOffset = kSelectorBytes + Position * kWordBytes;
      Argument.Name = kRecoveredArgumentPrefix.str() + std::to_string(Position);
      Argument.Read = Position < Arguments.count() && Arguments.read(Position);
      if (Function.Known) {
        Argument.Type = Declared[Position].str();
        Argument.TypeSource = ABITypeSource::KnownSignature;
      } else {
        const ABIConstraint &Constraint = Arguments.constraint(Position);
        Argument.Type = Constraint.resolve().spelling();
        Argument.TypeSource = Constraint.source();
      }
      Function.Arguments.push_back(std::move(Argument));
    }

    if (Function.Known) {
      for (llvm::StringRef Type : splitTypeList(Function.Known->Returns))
        Function.Returns.push_back(Type.str());
      Function.ReturnSource = ABITypeSource::KnownSignature;
    } else if (ReturnsWord) {
      Function.Returns.push_back(kDefaultRecoveredWordType.str());
    }
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
        if (Index.valid() && !Operation.Inputs.empty()) {
          Fact.KeyKind = storageKeyKind(Med, Index, Operation.Inputs[0]);
          if (const MedValue *Key = Med.findValue(Operation.Inputs[0]);
              Key && Key->Constant) {
            Fact.Slot = Key->Constant;
            Fact.Known = findKnownSlot(*Key->Constant);
          }
        }
        Fact.SuggestedName = kUnknownStorageName.str();
        // A slot a specification fixes carries its published name; a slot a
        // compiler allocated carries only its number, because nothing outside
        // the source says what it holds.
        if (Fact.Known)
          Fact.SuggestedName = Fact.Known->Name.str();
        else if (Fact.Slot)
          Fact.SuggestedName =
              kStorageSlotPrefix.str() + wordHexDigits(*Fact.Slot);
        else if (Fact.KeyKind == StorageKeyKind::Hashed ||
                 Fact.KeyKind == StorageKeyKind::HashedOffset)
          Fact.SuggestedName =
              kStorageElementPrefix.str() + llvm::utohexstr(Operation.PC);
        High.Storage.push_back(std::move(Fact));
      }
      if (Index.valid())
        if (const CallFamilyInfo *Family = findCallFamily(Operation.Op)) {
          High.Calls.push_back(
              classifyCall(Med, Index, Block, *Family, Operation, Low.Fork));
          // A delegating call is also an outgoing call, but it is the only one
          // whose callee runs against this program's own storage, so it stays
          // reported on its own.
          if (Family->Delegates)
            High.Proxies.push_back(classifyDelegation(Med, Index, Operation));
        }
      if (evm::isLog(Operation.Op)) {
        EventFact Fact;
        Fact.PC = Operation.PC;
        Fact.Topics = logTopicCount(Operation.Op);
        if (Index.valid() && Fact.Topics != 0 && Operation.Inputs.size() > 2)
          if (const MedValue *Topic = Med.findValue(Operation.Inputs[2]);
              Topic && Topic->Constant) {
            Fact.Topic0 = Topic->Constant;
            Fact.Known = findKnownEvent(*Topic->Constant);
          }
        Fact.SuggestedName = Fact.Known ? Fact.Known->name().str()
                                        : kRecoveredEventPrefix.str() +
                                              llvm::utohexstr(Operation.PC);
        High.Events.push_back(std::move(Fact));
      }
    }
  }

  for (const LowInstruction &Instruction : Low.Instructions) {
    if (!Instruction.is(Opcode::REVERT))
      continue;
    const MedBlock *Block = Index.containingBlock(Instruction.PC);
    const MedOperation *Revert = Index.operation(Instruction.PC);
    if (Index.valid() && Block && Revert) {
      High.Errors.push_back(classifyRevert(Med, *Block, *Revert));
      continue;
    }
    // Without a usable value graph the site is still worth reporting; what it
    // hands back is not.
    ErrorFact Fact;
    Fact.PC = Instruction.PC;
    Fact.SuggestedName = kRecoveredRevertName.str();
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

  // Report the standards in table order rather than in the order the program
  // happens to mention them, so two builds of one contract summarize alike.
  std::vector<bool> Matched(knownStandardInfos().size(), false);
  const auto Note = [&](const KnownSignatureInfo *Known) {
    if (Known)
      Matched[static_cast<size_t>(Known->Standard)] = true;
  };
  for (const RecoveredFunction &Function : High.Functions)
    Note(Function.Known);
  for (const EventFact &Event : High.Events)
    Note(Event.Known);
  for (const ErrorFact &Error : High.Errors)
    Note(Error.Known);
  // A named slot says which specification the contract speaks just as plainly
  // as a matched selector, and it keeps saying so for a proxy whose whole ABI
  // belongs to the implementation behind it.
  for (const StorageFact &Storage : High.Storage)
    if (Storage.Known)
      Matched[static_cast<size_t>(Storage.Known->Standard)] = true;
  for (const ProxyFact &Proxy : High.Proxies)
    if (Proxy.Known)
      Matched[static_cast<size_t>(Proxy.Known->Standard)] = true;
  for (const KnownStandardInfo &Standard : knownStandardInfos())
    if (Matched[static_cast<size_t>(Standard.ID)])
      High.Standards.push_back(Standard.ID);

  if (Index.valid())
    High.HasFallback = reachesFallback(Low, Index, Classifier);
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
