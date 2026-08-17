//===- MachODyldRebase.cpp - Mach-O dyld rebase stream ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachODyldFixups.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/LEB128.h"

#include <optional>

namespace neverd {
namespace macho_loader {

using namespace llvm::MachO;

void parseRebaseStream(const uint8_t *BasePtr, size_t FileSize,
                       const DyldInfoOffsets &DyldInfo, BinaryImage &Img) {
  const uint64_t RebaseOff = DyldInfo.RebaseOff;
  const uint64_t RebaseSize = DyldInfo.RebaseSize;
  if (!BasePtr || RebaseOff == 0 || RebaseSize == 0 ||
      !rangeInBounds(RebaseOff, RebaseSize, FileSize))
    return;

  const uint32_t PtrSize = Img.getPointerSize();
  if (PtrSize != 4 && PtrSize != 8)
    return;

  const uint8_t *Cursor = BasePtr + RebaseOff;
  const uint8_t *End = Cursor + RebaseSize;
  uint8_t RebaseType = 0;
  std::optional<uint8_t> SegmentIndex;
  uint64_t SegmentOffset = 0;
  uint64_t NumRebases = 0;
  constexpr uint64_t MaxRebases = 1u << 22;

  auto ReadULEB = [&](uint64_t &Value) {
    if (Cursor >= End)
      return false;
    unsigned BytesRead = 0;
    const char *Error = nullptr;
    Value = llvm::decodeULEB128(Cursor, &BytesRead, End, &Error);
    if (Error || BytesRead == 0)
      return false;
    Cursor += BytesRead;
    return true;
  };

  auto OffsetCanHoldPointer = [&](uint8_t Index, uint64_t Offset) {
    if (Index >= Img.Segments.size())
      return false;
    const Segment &Seg = Img.Segments[Index];
    return Seg.Size >= PtrSize && Seg.Data.size() >= PtrSize &&
           Offset <= Seg.Size - PtrSize &&
           Offset <= Seg.Data.size() - PtrSize && Offset <= InvalidVA - Seg.VA;
  };

  auto RecordRun = [&](uint64_t Count, uint64_t Skip) {
    if (Count == 0)
      return true;
    if (!SegmentIndex || *SegmentIndex >= Img.Segments.size() ||
        Count > MaxRebases - NumRebases || Skip > InvalidVA - PtrSize)
      return false;

    const Segment &Seg = Img.Segments[*SegmentIndex];
    const uint64_t Stride = PtrSize + Skip;
    if (Seg.Size < PtrSize || Seg.Data.size() < PtrSize ||
        SegmentOffset > Seg.Size - PtrSize ||
        SegmentOffset > Seg.Data.size() - PtrSize ||
        Count - 1 > (InvalidVA - SegmentOffset) / Stride)
      return false;
    const uint64_t LastOffset = SegmentOffset + (Count - 1) * Stride;
    if (LastOffset > Seg.Size - PtrSize ||
        LastOffset > Seg.Data.size() - PtrSize ||
        LastOffset > InvalidVA - Seg.VA)
      return false;

    for (uint64_t I = 0; I < Count; ++I) {
      const uint64_t Offset = SegmentOffset + I * Stride;
      const va_t SlotVA = Seg.VA + Offset;
      const uint8_t *Pointer = Img.readVA(SlotVA, PtrSize);
      if (!Pointer)
        return false;
      if (RebaseType == REBASE_TYPE_POINTER) {
        const va_t TargetVA = readPtr(Pointer, PtrSize == 8);
        detail::recordAbsolutePointerSlot(Img, SlotVA, TargetVA);
      }
    }

    if (Count > (InvalidVA - SegmentOffset) / Stride)
      return false;
    SegmentOffset += Count * Stride;
    NumRebases += Count;
    return true;
  };

  while (Cursor < End) {
    const uint8_t Byte = *Cursor++;
    const uint8_t Opcode = Byte & REBASE_OPCODE_MASK;
    const uint8_t Immediate = Byte & REBASE_IMMEDIATE_MASK;
    switch (Opcode) {
    case REBASE_OPCODE_DONE:
      return;
    case REBASE_OPCODE_SET_TYPE_IMM:
      if (Immediate > REBASE_TYPE_TEXT_PCREL32)
        return;
      RebaseType = Immediate;
      break;
    case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
      SegmentIndex = Immediate;
      if (!ReadULEB(SegmentOffset) ||
          !OffsetCanHoldPointer(*SegmentIndex, SegmentOffset))
        return;
      break;
    case REBASE_OPCODE_ADD_ADDR_ULEB: {
      uint64_t Delta = 0;
      if (!SegmentIndex || !ReadULEB(Delta) ||
          Delta > InvalidVA - SegmentOffset)
        return;
      const uint64_t NewOffset = SegmentOffset + Delta;
      if (!OffsetCanHoldPointer(*SegmentIndex, NewOffset))
        return;
      SegmentOffset = NewOffset;
      break;
    }
    case REBASE_OPCODE_ADD_ADDR_IMM_SCALED: {
      const uint64_t Delta = static_cast<uint64_t>(Immediate) * PtrSize;
      if (!SegmentIndex || Delta > InvalidVA - SegmentOffset)
        return;
      const uint64_t NewOffset = SegmentOffset + Delta;
      if (!OffsetCanHoldPointer(*SegmentIndex, NewOffset))
        return;
      SegmentOffset = NewOffset;
      break;
    }
    case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
      if (!RecordRun(Immediate, 0))
        return;
      break;
    case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
      uint64_t Count = 0;
      if (!ReadULEB(Count) || !RecordRun(Count, 0))
        return;
      break;
    }
    case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
      uint64_t Skip = 0;
      if (!ReadULEB(Skip) || !RecordRun(1, Skip))
        return;
      break;
    }
    case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
      uint64_t Count = 0;
      uint64_t Skip = 0;
      if (!ReadULEB(Count) || !ReadULEB(Skip) || !RecordRun(Count, Skip))
        return;
      break;
    }
    default:
      return;
    }
  }
}

} // namespace macho_loader
} // namespace neverd
