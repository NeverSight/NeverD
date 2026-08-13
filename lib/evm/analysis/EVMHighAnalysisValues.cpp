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

SemanticValue SemanticClassifier::classify(ValueID Root) {
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
      const ValueID Dependency = Current.Dependencies[Current.NextDependency++];
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

llvm::SmallVector<ValueID, kMaxALUStackPops>
SemanticClassifier::dependencies(ValueID ID) const {
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

const SemanticValue &SemanticClassifier::input(const MedOperation &Operation,
                                               size_t Index) const {
  return Results[Operation.Inputs[Index]];
}

SemanticValue SemanticClassifier::evaluatePhi(const MedValue &Phi) const {
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

bool SemanticClassifier::isSelectorMask(const SemanticValue &Value) const {
  return Value.Kind == SemanticKind::Constant &&
         Value.Word == llvm::APInt::getLowBitsSet(kWordBits, kSelectorBits);
}

SemanticValue SemanticClassifier::evaluateInstruction(ValueID ID) const {
  const MedOperation *Operation = Index.producer(ID);
  if (!Operation)
    return {};
  if ((evm::isDup(Operation->Op) || evm::isDeepDup(Operation->Op)) &&
      Operation->Inputs.size() == 1)
    return input(*Operation, 0);
  if (Operation->Op == Opcode::CALLDATALOAD && Operation->Inputs.size() == 1) {
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
    if ((First.Kind == SemanticKind::SelectorWord && isSelectorMask(Second)) ||
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

SemanticValue SemanticClassifier::evaluate(ValueID ID) const {
  const MedValue &Value = Med.Values[ID];
  if (Value.Constant)
    return SemanticValue::constant(*Value.Constant);
  if (Value.Kind == ValueKind::Phi)
    return evaluatePhi(Value);
  if (Value.Kind == ValueKind::Instruction)
    return evaluateInstruction(ID);
  return {};
}

} // namespace neverd::evm::detail
