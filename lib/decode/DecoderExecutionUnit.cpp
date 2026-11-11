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

#include <cstddef>
#include <cstring>

namespace neverd {
namespace {

void snapshotMIPS(DecodedInsn &Insn, cs_insn &Storage, cs_detail &Detail) {
  Storage = *Insn.Raw;
  if (const cs_detail *Source = Storage.detail) {
    // Capstone publishes the common header and exactly op_count MIPS operands.
    // Other architectures occupy the unused tail of the cs_detail union.
    const size_t Size = offsetof(cs_detail, mips) +
                        offsetof(cs_mips, operands) +
                        Source->mips.op_count * sizeof(cs_mips_op);
    std::memcpy(&Detail, Source, Size);
    Storage.detail = &Detail;
  }
  Insn.Raw = &Storage;
}

} // namespace

int Decoder::decodeUnitForLift(const uint8_t *Bytes, size_t Len, va_t Addr,
                               DecodedExecutionUnit &Out) {
  if (Out.HasDelaySlot) {
    Out.Slot = {};
    Out.HasDelaySlot = false;
  }
  const int Size = decodeOneForLift(Bytes, Len, Addr, Out.Head);
  if (Size <= 0) {
    Out.Head = {};
    return Size;
  }
  if (!MIPS)
    return Size;
  const unsigned RequiredSlot = MIPS->delaySlotBytes(*Out.Head.Raw);
  if (!RequiredSlot)
    return Size;
  if (Addr > InvalidVA - Size || Len <= size_t(Size) ||
      (!isMIPS64(TargetArch) && Addr > UINT32_MAX - Size))
    return 0;

  snapshotMIPS(Out.Head, Out.HeadStorage, Out.HeadDetail);
  const int SlotSize =
      decodeOneForLift(Bytes + Size, Len - Size, Addr + Size, Out.Slot);
  if (SlotSize <= 0 ||
      (RequiredSlot != 1 && unsigned(SlotSize) != RequiredSlot) ||
      Addr > InvalidVA - Size - SlotSize ||
      (!isMIPS64(TargetArch) && Addr > UINT32_MAX - Size - SlotSize + 1)) {
    Out.Slot = {};
    return 0;
  }
  snapshotMIPS(Out.Slot, Out.SlotStorage, Out.SlotDetail);
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
