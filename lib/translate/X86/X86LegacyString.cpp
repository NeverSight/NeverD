//===- X86LegacyString.cpp - Protected string element execution ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compose scalar memory/flags with segmented string indices and REP progress.
//===----------------------------------------------------------------------===//

#include "X86LegacyString.h"

#include <cstdint>
#include <utility>

namespace neverd::translate::detail {

std::optional<X86LegacyStringInstruction>
decodeLegacyString(llvm::ArrayRef<uint8_t> Bytes, bool Code32) {
  X86LegacyEncoding E(Bytes, Code32);
  const uint8_t Opcode = E.byte();
  if (!E.done() || (Opcode < 0xa4 || Opcode > 0xaf) || Opcode == 0xa8 ||
      Opcode == 0xa9)
    return std::nullopt;
  const unsigned Kind = Opcode & ~1u;
  const bool Compare = Kind == 0xa6 || Kind == 0xae;
  if (E.Group1Prefix == 0xf2 && !Compare)
    return std::nullopt;
  X86LegacyStringInstruction F;
  F.AddressBytes = E.AddressBytes;
  F.Repeat =
      E.Group1Prefix == 0xf2 || E.Group1Prefix == 0xf3 ? E.Group1Prefix : 0;
  F.Locked = E.Group1Prefix == 0xf0;
  F.SourceIndex = Kind == 0xa4 || Kind == 0xa6 || Kind == 0xac;
  F.DestinationIndex = Kind != 0xac;
  auto &Element = F.Element;
  Element.Bytes = Element.SourceBytes = Opcode & 1 ? E.OperandBytes : 1;
  Element.Operation = Compare ? X86LegacyScalarOperation::Compare
                              : X86LegacyScalarOperation::Move;
  X86LegacyScalarOperand Source, Destination, Accumulator;
  Source.MemorySegment = E.Segment.value_or(X86SegmentRegister::DS);
  Source.AddressBytes = Destination.AddressBytes = F.AddressBytes;
  Source.Base = 6;
  Destination.MemorySegment = X86SegmentRegister::ES;
  Destination.Base = 7;
  Accumulator.Register = 0;
  if (Compare) {
    Element.Destination = F.SourceIndex ? Source : Accumulator;
    Element.Source = Destination;
    Element.ReadSourceFirst = true;
  } else {
    Element.Destination = F.DestinationIndex ? Destination : Accumulator;
    Element.Source = F.SourceIndex ? Source : Accumulator;
  }
  return F;
}

llvm::Expected<X86LegacyStringResult>
executeLegacyString(X86LegacyExecutionState &S, X86PagedMemory &M,
                    const X86LegacyStringInstruction &F, uint32_t Fallthrough,
                    const X86LegacyExecutionOptions &O) {
  X86LegacyStringResult R;
  if (F.Locked) {
    R.Status = X86PageWalkStatus::Exception;
    R.Exception = X86MemoryException{6, 0, {}};
    return R;
  }
  const uint32_t Mask = F.AddressBytes == 2 ? UINT16_MAX : UINT32_MAX;
  const uint32_t Count = S.Core.GPR[1] & Mask;
  if (F.Repeat && !Count) {
    S.EIP = Fallthrough;
    return R;
  }
  const uint32_t EntryPC = S.EIP, EntryFlags = S.EFlags;
  auto Element = executeLegacyScalar(S, M, F.Element, Fallthrough, O);
  if (!Element)
    return Element.takeError();
  static_cast<X86MemoryAccessResult &>(R) = std::move(*Element);
  if (R.Status != X86PageWalkStatus::Completed)
    return R;
  const auto Advance = [&](unsigned Register, uint32_t Delta) {
    auto &V = S.Core.GPR[Register];
    V = (V & ~Mask) | ((V + Delta) & Mask);
  };
  const uint32_t Delta =
      EntryFlags & 0x400 ? -F.Element.Bytes : F.Element.Bytes;
  if (F.SourceIndex)
    Advance(6, Delta);
  if (F.DestinationIndex)
    Advance(7, Delta);
  if (F.Repeat) {
    Advance(1, UINT32_MAX);
    const bool Compare =
        F.Element.Operation == X86LegacyScalarOperation::Compare;
    R.RepeatContinue =
        Count != 1 && (!Compare || bool(S.EFlags & 64) == (F.Repeat == 0xf3));
    if (R.RepeatContinue) {
      S.EIP = EntryPC;
      S.EFlags = EntryFlags;
    }
  }
  return R;
}

} // namespace neverd::translate::detail
