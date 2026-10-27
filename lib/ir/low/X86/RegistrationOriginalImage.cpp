//===- RegistrationOriginalImage.cpp - Scoped PE32 values ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep original-image arithmetic and immutable values in an explicit scope.
//===----------------------------------------------------------------------===//

#include "RegistrationOriginalImage.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::registration_state {

bool OriginalImageFrameProof::hasMappedIdentity(uint32_t Address,
                                                size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, Mapped.size() + 1))
    return false;
  const auto [It, New] = Mapped.try_emplace(Address, false);
  if (New)
    It->second = Address >= Image.Base &&
                 registration_abi::hasSEHCallbackImageMemory(Image, Address, 1,
                                                             false, Work);
  return It->second;
}

const registration_abi::ImmutableRegistrationScalar *
OriginalImageFrameProof::read(va_t Address, uint16_t Width, size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, Reads.size() + 1))
    return nullptr;
  const auto [It, New] = Reads.try_emplace({Address, Width});
  if (New)
    It->second = registration_abi::readImmutableRegistrationScalar(
        Image, Address, Width, Work);
  return It->second ? &*It->second : nullptr;
}

bool OriginalImageFrameProof::permitsRead(va_t Address, uint16_t Width,
                                          size_t &Work) {
  return read(Address, Width, Work) != nullptr;
}

bool OriginalImageFrameProof::recordAddress(uint32_t Address, size_t &Work) {
  if (!hasMappedIdentity(Address, Work) ||
      !registration_abi::chargeCalleeWork(Work, Addresses.size() + 1))
    return false;
  Addresses.insert(Address);
  return true;
}

bool OriginalImageFrameProof::recordReadAddress(const FrameValue &Address,
                                                uint16_t Width, size_t &Work) {
  const auto VA = Address.preferredAddress();
  if (!VA || !Width ||
      !registration_abi::chargeCalleeWork(Work,
                                          ReadAddressIdentities.size() + 1))
    return false;
  const bool Exact = Address.ImageAddress &&
                     Address == FrameValue::imageAddress(*Address.ImageAddress);
  const auto [It, New] =
      ReadAddressIdentities.emplace(std::make_pair(*VA, Width), Exact);
  if (!New)
    It->second &= Exact;
  return true;
}

bool OriginalImageFrameProof::recordCode(const LowFunc &Function,
                                         size_t &Work) {
  std::vector<ExceptionAddressRange> Ranges;
  if (!registration_abi::collectCalleeCodeRanges(Function, Image, Work, Ranges))
    return false;
  for (const auto &Range : Ranges) {
    if (!Range.isValid() ||
        !registration_abi::chargeCalleeWork(Work, Range.size() + Code.size()))
      return false;
    const auto *Bytes = Image.readVA(Range.Begin, Range.size());
    if (!Bytes)
      return false;
    const std::vector<uint8_t> Copy(Bytes, Bytes + Range.size());
    const auto [It, New] = Code.emplace(Range.Begin, Copy);
    if (!New && It->second != Copy)
      return false;
  }
  return !Ranges.empty();
}

std::optional<FrameValue> OriginalImageFrameProof::readScalar(va_t Address,
                                                              uint16_t Width,
                                                              size_t &Work) {
  const auto *Read = read(Address, Width, Work);
  if (Read && Read->Value)
    return FrameValue::constant(*Read->Value);
  return std::nullopt;
}

std::optional<FrameValue>
OriginalImageFrameProof::evaluate(const LowOp &Op,
                                  const FrameTransfer &Transfer, size_t &Work) {
  if (!registration_abi::chargeCalleeWork(Work, 1) ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.MemoryOrdering != NdMemoryOrdering::None || !Op.Output.Size ||
      (!Op.Output.isReg() && !Op.Output.isTemp()))
    return std::nullopt;
  if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs == 2 && Op.Output.Size == 4 &&
      Op.Inputs[0].Size == 4 && Op.Inputs[1].Size == 4) {
    const auto Left = Transfer.read(Op.Inputs[0]);
    const auto Right = Transfer.read(Op.Inputs[1]);
    if (!Left.ImageAddress || !Right.ImageAddress ||
        Left != FrameValue::imageAddress(*Left.ImageAddress) ||
        Right != FrameValue::imageAddress(*Right.ImageAddress) ||
        !hasMappedIdentity(*Left.ImageAddress, Work) ||
        !hasMappedIdentity(*Right.ImageAddress, Work) ||
        !registration_abi::chargeCalleeWork(Work, Addresses.size() + 2))
      return std::nullopt;
    Addresses.insert(*Left.ImageAddress);
    Addresses.insert(*Right.ImageAddress);
    return FrameValue::constant(*Left.ImageAddress - *Right.ImageAddress);
  }
  // The x86 lifter zero-extends a PE32 address into its canonical 64-bit
  // memory temporary. Its exact image identity still comes from the 32-bit
  // producer; an arbitrary wide register supplies no such address shape.
  if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
      (Op.Inputs[0].Size == 4 ||
       (Op.Inputs[0].isTemp() && Op.Inputs[0].Size == 8))) {
    const auto Address = Transfer.read(Op.Inputs[0]);
    if (Address.preferredAddress() &&
        !recordReadAddress(Address, Op.Output.Size, Work))
      return std::nullopt;
    if (Address.ImageAddress &&
        Address == FrameValue::imageAddress(*Address.ImageAddress))
      return readScalar(*Address.ImageAddress, Op.Output.Size, Work);
  }
  return std::nullopt;
}

} // namespace neverd::registration_state
