//===- GoDeferInfo.cpp - Go open-coded defer metadata decoding ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeDetail.h"

#include <algorithm>
#include <iterator>
#include <optional>

namespace neverd::go_loader::detail {
namespace {

/// Every spelling of `FUNCDATA_OpenCodedDeferInfo`, oldest last.  The order is
/// the order a layout vote reports them in and nothing else depends on it.
constexpr GoOpenCodedDeferLayout kOpenCodedDeferLayouts[] = {
    GoOpenCodedDeferLayout::Contiguous,
    GoOpenCodedDeferLayout::Enumerated,
    GoOpenCodedDeferLayout::LegacyEnumerated,
};

/// Bound on the argument list a Go 1.14-era deferred call could carry.  The
/// count is only read to be skipped, so this exists to stop a misread record
/// from turning into a long walk.
constexpr uint32_t MaxLegacyDeferArguments = 64;

} // namespace

/// Read `FUNCDATA_OpenCodedDeferInfo` under one of the three spellings Go has
/// given it, rejecting anything that does not describe a frame the function
/// could build.
///
/// Open-coded defers arrived in Go 1.14 and the record has been rewritten
/// twice since.  Go 1.18 made deferred functions argumentless, which removed
/// the leading maximum argument frame and each defer's own argument size and
/// argument list, leaving just the closure slots.  Go 1.22 then sorted those
/// slots into one ascending run, so the record stopped naming them and names
/// only where the run begins.
///
/// The release cannot be asked which to expect: the pclntab magic last changed
/// in Go 1.20, so one magic covers the Go 1.22 rewrite, and the Go 1.18 one
/// falls inside the span of another.  The bytes can be asked.  A closure slot
/// is a pointer-aligned frame offset above the bitmask, a slot count is at
/// most eight because the bitmask is a single byte, and an argument size is
/// bounded by the maximum the record opened with -- so a reading applied to
/// another spelling's bytes almost always lands on a field that fails one of
/// those.  Almost, not always, which is why the caller settles the layout by a
/// vote over the image rather than record by record.
std::optional<OpenCodedDeferRecord>
readOpenCodedDeferInfo(const ImageReader &R, va_t RecordVA,
                       GoOpenCodedDeferLayout Layout, unsigned PtrSize,
                       std::optional<int32_t> FrameSize) {
  const uint64_t Bound =
      FrameSize && *FrameSize > 0
          ? std::min<uint64_t>(static_cast<uint64_t>(*FrameSize),
                               MaxFrameSlotOffset)
          : MaxFrameSlotOffset;
  const bool IsLegacy = Layout == GoOpenCodedDeferLayout::LegacyEnumerated;

  va_t Cursor = RecordVA;
  // The legacy record opens with the largest argument frame any of the defers
  // needs.  That is a size rather than an offset, so unlike everything after
  // it the value may legitimately be zero.
  uint32_t MaxArgSize = 0;
  if (IsLegacy) {
    std::optional<uint32_t> Value = R.uvarint(Cursor);
    if (!Value || *Value >= Bound)
      return std::nullopt;
    MaxArgSize = *Value;
  }

  std::optional<uint32_t> DeferBits = R.uvarint(Cursor);
  if (!DeferBits || *DeferBits == 0 || *DeferBits >= Bound)
    return std::nullopt;

  OpenCodedDeferRecord Record;
  Record.Layout = Layout;
  Record.DeferBits = *DeferBits;

  // A closure slot holds a pointer, so it is pointer aligned, and the frame
  // layout places the bitmask byte below every one of them.
  auto namesASlot = [&](uint32_t Offset) {
    return Offset != 0 && Offset % PtrSize == 0 && Offset < *DeferBits;
  };

  std::optional<uint32_t> Second = R.uvarint(Cursor);
  if (!Second)
    return std::nullopt;

  if (Layout == GoOpenCodedDeferLayout::Contiguous) {
    if (!namesASlot(*Second))
      return std::nullopt;
    Record.FirstSlot = *Second;
    // The record does not store how many slots follow the first.  What it does
    // fix is where the run starts, and the run climbs from there to varp, so
    // its length is bounded by the distance between them.  The bound is the
    // exact count whenever the closure slots are the topmost pointer locals,
    // which is how the frame is normally laid out, and it can only ever be too
    // large.
    const uint32_t SlotBound = *Second / PtrSize;
    if (SlotBound > GoOpenCodedDeferInfo::MaxSlots) {
      Record.SlotsBounded = false;
      return Record;
    }
    for (uint32_t Slot = 0; Slot < SlotBound; ++Slot)
      Record.Slots.push_back(*Second - Slot * PtrSize);
    return Record;
  }

  if (*Second == 0 || *Second > GoOpenCodedDeferInfo::MaxSlots)
    return std::nullopt;
  for (uint32_t I = 0; I < *Second; ++I) {
    if (IsLegacy) {
      std::optional<uint32_t> ArgWidth = R.uvarint(Cursor);
      if (!ArgWidth || *ArgWidth > MaxArgSize)
        return std::nullopt;
    }
    std::optional<uint32_t> Slot = R.uvarint(Cursor);
    if (!Slot || !namesASlot(*Slot))
      return std::nullopt;
    Record.Slots.push_back(*Slot);
    if (!IsLegacy)
      continue;
    std::optional<uint32_t> ArgCount = R.uvarint(Cursor);
    if (!ArgCount || *ArgCount > MaxLegacyDeferArguments)
      return std::nullopt;
    // Each argument is where it is stored, how wide it is, and where the call
    // wants it.  None of that describes the frame being unwound, so it is
    // walked only to reach the next defer.
    for (uint32_t Word = 0; Word < *ArgCount * 3; ++Word) {
      std::optional<uint32_t> Skipped = R.uvarint(Cursor);
      if (!Skipped || *Skipped >= Bound)
        return std::nullopt;
    }
  }
  Record.FirstSlot = Record.Slots.front();
  return Record;
}

/// Read one record under every spelling, which is what confirming a funcdata
/// base has to do: the layout is decided from records the base resolves, so it
/// is not yet known while the base is being confirmed.
bool readsUnderAnyLayout(const ImageReader &R, va_t RecordVA, unsigned PtrSize,
                         std::optional<int32_t> FrameSize) {
  for (GoOpenCodedDeferLayout Layout : kOpenCodedDeferLayouts)
    if (readOpenCodedDeferInfo(R, RecordVA, Layout, PtrSize, FrameSize))
      return true;
  return false;
}

/// Decide which spelling of the open-coded defer record the image uses by
/// reading real records every way and counting which one they turn out to be.
/// A single record can be ambiguous -- a frame with the maximum eight defers
/// and one whose slot run begins a single pointer below varp read the same --
/// but a whole image is not, and the header cannot be asked because its magic
/// spans the releases that changed the record.
GoOpenCodedDeferLayout
resolveOpenCodedDeferLayout(const ImageReader &R, const FuncLayout &L,
                            const PcHeader &H,
                            const std::vector<RawFunc> &Funcs, va_t Base) {
  constexpr size_t Count = std::size(kOpenCodedDeferLayouts);
  unsigned Votes[Count] = {};
  unsigned Cast = 0;
  for (const RawFunc &F : Funcs) {
    if (Cast >= FuncDataBaseSampleTarget)
      break;
    std::optional<va_t> RecordVA =
        getFuncDataAddress(R, L, F, L.OpenCodedDeferInfoIndex, Base);
    if (!RecordVA)
      continue;
    const std::optional<int32_t> FrameSize =
        decodeMaxFrameSize(R, H.PcTab, F.PcSP, H.MinLC);
    size_t Accepted = Count;
    size_t Matches = 0;
    for (size_t I = 0; I < Count; ++I)
      if (readOpenCodedDeferInfo(R, *RecordVA, kOpenCodedDeferLayouts[I],
                                 R.pointerSize(), FrameSize)) {
        Accepted = I;
        ++Matches;
      }
    // Only a record that tells the spellings apart gets a vote.
    if (Matches != 1)
      continue;
    ++Votes[Accepted];
    ++Cast;
  }
  size_t Winner = 0;
  for (size_t I = 1; I < Count; ++I)
    if (Votes[I] > Votes[Winner])
      Winner = I;
  return kOpenCodedDeferLayouts[Winner];
}

} // namespace neverd::go_loader::detail
