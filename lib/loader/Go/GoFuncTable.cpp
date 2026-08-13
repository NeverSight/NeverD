//===- GoFuncTable.cpp - Go functab and _func record decoding -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeDetail.h"

#include <algorithm>
#include <optional>

namespace neverd::go_loader::detail {

FuncLayout getFuncLayout(uint32_t Magic, unsigned PtrSize) {
  FuncLayout L;
  if (Magic == Go12Magic) {
    L.EntryIsOffset = false;
    L.FuncDataIsPointer = true;
    L.FuncTabEntrySize = 2 * PtrSize;
    // `nfuncdata` closes the fixed part in both shapes this magic covers: the
    // older one spells it as a word at PtrSize+28 and the newer one as the
    // last byte of the word at PtrSize+28, and the pcdata array starts after
    // that word either way.
    L.FuncIDOffset = PtrSize + 28;
    L.HeaderSize = PtrSize + 32;
    L.OpenCodedDeferInfoIndex = FuncDataOpenCodedDeferInfoPreGo116;
    L.HasUnsafePointTable = false;
    L.StackMapPCDataIndex = std::nullopt;
    return L;
  }
  if (Magic == Go116Magic) {
    // `entry uintptr` leads the record, and the functab is pairs of pointers.
    L.EntryIsOffset = false;
    L.FuncDataIsPointer = true;
    L.FuncTabEntrySize = 2 * PtrSize;
    L.FuncIDOffset = PtrSize + 32;
    L.HeaderSize = PtrSize + 36;
    return L;
  }
  // Go 1.20 inserted `startLine` ahead of the trailing byte fields.
  L.FuncIDOffset = Magic == Go118Magic ? 36 : 40;
  L.HeaderSize = L.FuncIDOffset + 4;
  return L;
}

std::optional<RawFunc> decodeFunc(const ImageReader &R, const FuncLayout &L,
                                  va_t RecordVA) {
  RawFunc F;
  F.RecordVA = RecordVA;
  if (L.EntryIsOffset) {
    std::optional<uint32_t> EntryOff = R.u32(RecordVA);
    if (!EntryOff)
      return std::nullopt;
    F.EntryOffset = *EntryOff;
  } else {
    std::optional<uint64_t> Entry = R.word(RecordVA);
    if (!Entry)
      return std::nullopt;
    F.EntryOffset = *Entry;
  }
  const va_t Rest = RecordVA + (L.EntryIsOffset ? 4 : R.pointerSize());
  std::optional<int32_t> NameOff = R.i32(Rest);
  std::optional<uint32_t> PcSP = R.u32(Rest + 12);
  std::optional<uint32_t> PCDataCount = R.u32(Rest + 24);
  if (!NameOff || !PcSP || !PCDataCount)
    return std::nullopt;
  if (*NameOff < 0 || *PCDataCount > MaxPCDataTables)
    return std::nullopt;
  F.NameOffset = *NameOff;
  F.PcSP = *PcSP;
  F.PCDataCount = *PCDataCount;

  if (L.PreGo112Record) {
    std::optional<uint32_t> FuncDataCount = R.u32(RecordVA + L.FuncIDOffset);
    if (!FuncDataCount || *FuncDataCount > MaxFuncDataTables)
      return std::nullopt;
    F.FuncDataCount = static_cast<uint8_t>(*FuncDataCount);
    return F;
  }

  std::optional<uint32_t> DeferReturn = R.u32(Rest + 8);
  std::optional<uint8_t> FuncID = R.u8(RecordVA + L.FuncIDOffset);
  std::optional<uint8_t> Flag = R.u8(RecordVA + L.FuncIDOffset + 1);
  std::optional<uint8_t> FuncDataCount = R.u8(RecordVA + L.FuncIDOffset + 3);
  if (!DeferReturn || !FuncID || !Flag || !FuncDataCount)
    return std::nullopt;
  if (*FuncDataCount > MaxFuncDataTables)
    return std::nullopt;
  F.DeferReturn = *DeferReturn;
  F.FuncID = *FuncID;
  F.Flag = *Flag;
  F.FuncDataCount = *FuncDataCount;
  return F;
}

/// Offset into `pctab` of the \p Index-th pc-value table.  Zero, which the
/// record uses to mean the table is absent, is reported as absent.
std::optional<uint32_t> getPCDataOffset(const ImageReader &R,
                                        const FuncLayout &L, const RawFunc &F,
                                        unsigned Index) {
  if (Index >= F.PCDataCount)
    return std::nullopt;
  std::optional<uint32_t> Offset = R.u32(F.RecordVA + L.HeaderSize + Index * 4);
  if (!Offset || *Offset == 0)
    return std::nullopt;
  return *Offset;
}

/// Address of the \p Index-th `_func` record, resolved through the functab.
std::optional<va_t> getFuncRecordAddress(const ImageReader &R,
                                         const FuncLayout &L, const PcHeader &H,
                                         uint64_t Index) {
  if (Index > (InvalidVA - H.FuncTab) / L.FuncTabEntrySize)
    return std::nullopt;
  const va_t Slot = H.FuncTab + Index * L.FuncTabEntrySize;
  std::optional<uint64_t> Offset;
  if (L.EntryIsOffset) {
    if (std::optional<uint32_t> Narrow = R.u32(Slot + 4))
      Offset = *Narrow;
  } else {
    Offset = R.wordAt(Slot, 1);
  }
  if (!Offset || *Offset > InvalidVA - H.FuncRecordBase)
    return std::nullopt;
  return H.FuncRecordBase + static_cast<va_t>(*Offset);
}

/// Decide which of the two `_func` shapes a Go 1.2 table uses.
///
/// The magic spans Go 1.2 through Go 1.15 and the record changed shape in the
/// middle of that span, so the header cannot say which.  `nfuncdata` can: the
/// older shape spells it as a whole word and the newer one as that word's high
/// byte, with `funcID` taking the low byte and the two in between fixed at
/// zero.  A function declares only a handful of funcdata entries, so a high
/// byte in range can only be the newer shape — under the older reading it
/// would be claiming sixteen million tables — and a low byte in range with a
/// zero high byte is evidence for the older one.  Neither reading is decidable
/// from a single record, because a runtime function with a special `funcID`
/// and no funcdata looks exactly like an older record, so the shape is voted
/// on: a real image has far more functions with pointer maps than it has
/// special ones.
bool usesPreGo112Record(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H) {
  unsigned PreGo112Votes = 0;
  unsigned Go112Votes = 0;
  const uint64_t Limit = std::min<uint64_t>(H.FuncCount, FuncLayoutVoteTarget);
  for (uint64_t I = 0; I < Limit; ++I) {
    std::optional<va_t> RecordVA = getFuncRecordAddress(R, L, H, I);
    if (!RecordVA)
      break;
    std::optional<uint32_t> Word = R.u32(*RecordVA + L.FuncIDOffset);
    // A zero word reads as no funcdata under either shape, so it is not a
    // record either side can claim.
    if (!Word || *Word == 0)
      continue;
    const uint32_t High = *Word >> 24;
    const uint32_t Padding = (*Word >> 8) & 0xFFFF;
    if (Padding == 0 && High != 0 && High <= MaxFuncDataTables)
      ++Go112Votes;
    else if (*Word <= MaxFuncDataTables)
      ++PreGo112Votes;
  }
  return PreGo112Votes > Go112Votes;
}

/// Address of the \p Index-th funcdata payload.  Absent both when the function
/// declares fewer tables and when it declares the table but has no data for
/// it.  \p GoFuncBase is ignored on the pointer layout, where each entry is
/// already a relocated address.
std::optional<va_t> getFuncDataAddress(const ImageReader &R,
                                       const FuncLayout &L, const RawFunc &F,
                                       unsigned Index, va_t GoFuncBase) {
  if (Index >= F.FuncDataCount)
    return std::nullopt;
  const va_t ArrayStart = F.RecordVA + L.HeaderSize + F.PCDataCount * 4;
  if (L.FuncDataIsPointer) {
    const unsigned PtrSize = R.pointerSize();
    // `runtime.funcdata` rounds the array up to a pointer boundary, because
    // the pcdata array ahead of it is 32-bit and can leave it half aligned.
    const va_t Aligned =
        PtrSize == 8 && (ArrayStart & 4) != 0 ? ArrayStart + 4 : ArrayStart;
    std::optional<uint64_t> Pointer = R.wordAt(Aligned, Index);
    if (!Pointer || *Pointer == 0)
      return std::nullopt;
    return static_cast<va_t>(*Pointer);
  }
  std::optional<uint32_t> Offset = R.u32(ArrayStart + Index * 4);
  if (!Offset || *Offset == NoFuncDataOffset)
    return std::nullopt;
  if (*Offset > InvalidVA - GoFuncBase)
    return std::nullopt;
  return GoFuncBase + static_cast<va_t>(*Offset);
}

/// Largest stack-pointer delta the function reaches, decoded from its `pcsp`
/// table.  This is the frame size the open-coded defer offsets are measured
/// against, so without it those offsets cannot be turned into stack slots.
std::optional<int32_t> decodeMaxFrameSize(const ImageReader &R, va_t PcTab,
                                          uint32_t PcSPOffset, uint8_t MinLC) {
  int32_t Largest = 0;
  bool Implausible = false;
  // The addresses this walk produces are discarded, so the entry it measures
  // them from does not matter and is not asked for.
  const bool Walked = forEachPCValue(
      R, PcTab, PcSPOffset, 0, MinLC, [&](const PCValueRange &V) {
        if (V.Value < 0 || static_cast<uint64_t>(V.Value) > MaxFrameSize) {
          Implausible = true;
          return false;
        }
        Largest = std::max(Largest, V.Value);
        return true;
      });
  if (!Walked || Implausible)
    return std::nullopt;
  return Largest;
}

} // namespace neverd::go_loader::detail
