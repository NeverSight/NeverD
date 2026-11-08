//===- DecoderExecutionUnit.cpp - Delayed-control decode units -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Share semantic-unit decoding and lifting without changing disassembly's
/// physical instruction lengths or losing delay-slot byte provenance.
///
//===----------------------------------------------------------------------===//

#include "neverd/decode/Decoder.h"
#include "neverd/lift/MIPS/MIPSLifter.h"

namespace neverd {

int Decoder::decodeUnitForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                               DecodedExecutionUnit &Out) {
  Out.HasDelaySlot = false;
  Out.Head = {};
  Out.Slot = {};
  const int Size = decodeOneForLift(Bytes, Len, Addr, Out.Head);
  if (Size <= 0 || !MIPS)
    return Size;
  const unsigned RequiredSlot = MIPS->delaySlotBytes(*Out.Head.Raw);
  if (!RequiredSlot)
    return Size;
  if (Addr > InvalidVA - Size || Len <= size_t(Size) ||
      (!isMIPS64(TargetArch) && Addr > UINT32_MAX - Size))
    return 0;

  Out.HeadStorage = *Out.Head.Raw;
  if (Out.Head.Raw->detail) {
    Out.HeadDetail = *Out.Head.Raw->detail;
    Out.HeadStorage.detail = &Out.HeadDetail;
  }
  Out.Head.Raw = &Out.HeadStorage;
  const int SlotSize =
      decodeOneForLift(Bytes + Size, Len - Size, Addr + Size, Out.Slot);
  if (SlotSize <= 0 ||
      (RequiredSlot != 1 && unsigned(SlotSize) != RequiredSlot) ||
      Addr > InvalidVA - Size - SlotSize ||
      (!isMIPS64(TargetArch) && Addr > UINT32_MAX - Size - SlotSize + 1))
    return 0;
  Out.SlotStorage = *Out.Slot.Raw;
  if (Out.Slot.Raw->detail) {
    Out.SlotDetail = *Out.Slot.Raw->detail;
    Out.SlotStorage.detail = &Out.SlotDetail;
  }
  Out.Slot.Raw = &Out.SlotStorage;
  Out.HasDelaySlot = true;
  return Out.spanBytes();
}

std::optional<LowDelaySlot>
Decoder::liftUnitToLow(const DecodedExecutionUnit &Unit,
                       std::vector<LowOp> &Ops,
                       llvm::ArrayRef<RelocatedAddressOperand> Relocs,
                       llvm::ArrayRef<RelocatedScalarOperand> ScalarRelocs) {
  if (!Unit.HasDelaySlot) {
    liftToLow(Unit.Head, Ops, Relocs, ScalarRelocs);
    return std::nullopt;
  }
  return MIPS->liftUnit(*Unit.Head.Raw, *Unit.Slot.Raw, Ops);
}

} // namespace neverd
