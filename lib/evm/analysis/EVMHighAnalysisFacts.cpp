//===- EVMHighAnalysisFacts.cpp - EVM recovered per-site facts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMHighAnalysisDetail.h"
#include "EVMMemoryDataflow.h"

#include <limits>

namespace neverd::evm::detail {
namespace {

std::optional<uint32_t> selectorAt(const EVMMedIR &Med,
                                   const EVMMemoryDataflow &Memory,
                                   ValueID Offset, ValueID Size,
                                   uint64_t BeforePC) {
  const auto Base = constantWord(Med.findValue(Offset));
  const auto Length = constantWord(Med.findValue(Size));
  if (!Base || !Length || *Length < kSelectorBytes)
    return std::nullopt;
  const auto Bytes = Memory.read(BeforePC, *Base, kSelectorBytes);
  if (!Bytes)
    return std::nullopt;
  return static_cast<uint32_t>(Bytes->getZExtValue());
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

/// The selector a call places at the start of the calldata it hands its
/// callee.
///
/// A compiler assembles that calldata by storing the left-aligned selector at
/// the head of the argument window and the arguments after it, which is the
/// same shape a revert payload has. The word the window starts with is
/// therefore what names the operation being requested.
std::optional<uint32_t> outboundSelector(const EVMMedIR &Med,
                                         const EVMMemoryDataflow &Memory,
                                         const CallFamilyInfo &Family,
                                         const MedOperation &Call) {
  if (Call.Inputs.size() <= Family.argumentsLengthOperand())
    return std::nullopt;
  const ValueID Offset = Call.Inputs[Family.argumentsOffsetOperand()];
  const ValueID Size = Call.Inputs[Family.argumentsLengthOperand()];

  const auto Selector = selectorAt(Med, Memory, Offset, Size, Call.PC);
  return Selector && *Selector != 0 ? Selector : std::nullopt;
}

} // namespace

ErrorFact classifyRevert(const EVMMedIR &Med, const EVMMemoryDataflow &Memory,
                         const MedOperation &Revert) {
  ErrorFact Fact;
  Fact.PC = Revert.PC;
  Fact.SuggestedName = kRecoveredRevertName.str();
  if (Revert.Inputs.size() != 2)
    return Fact;

  const auto Selector =
      selectorAt(Med, Memory, Revert.Inputs[0], Revert.Inputs[1], Revert.PC);
  if (!Selector || *Selector == 0)
    return Fact;

  Fact.Selector = *Selector;
  Fact.Known = findKnownError(*Selector);
  Fact.Kind = RevertKind::Custom;
  if (Fact.Known == &getLanguageRevertInfo(LanguageRevert::Message)) {
    Fact.Kind = RevertKind::Message;
  } else if (Fact.Known == &getLanguageRevertInfo(LanguageRevert::Panic)) {
    Fact.Kind = RevertKind::Panic;
    const auto Base = constantWord(Med.findValue(Revert.Inputs[0]));
    const auto Size = constantWord(Med.findValue(Revert.Inputs[1]));
    constexpr uint64_t kPanicPayloadBytes = kSelectorBytes + kWordBytes;
    if (Base && Size && *Size >= kPanicPayloadBytes &&
        *Base <= std::numeric_limits<uint64_t>::max() - kSelectorBytes)
      if (const auto Code =
              Memory.read(Revert.PC, *Base + kSelectorBytes, kWordBytes))
        if (Code->getActiveBits() <= std::numeric_limits<uint64_t>::digits)
          Fact.Panic = findPanicCode(Code->getZExtValue());
  }
  Fact.SuggestedName =
      Fact.Known ? Fact.Known->name().str()
                 : kRecoveredErrorPrefix.str() + selectorHex(*Selector);
  return Fact;
}

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

CallFact classifyCall(const EVMMedIR &Med, const ProducerIndex &Index,
                      const EVMMemoryDataflow &Memory,
                      const CallFamilyInfo &Family, const MedOperation &Call,
                      Hardfork Fork) {
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

  Fact.Selector = outboundSelector(Med, Memory, Family, Call);
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

} // namespace neverd::evm::detail
