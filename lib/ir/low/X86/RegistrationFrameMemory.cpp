//===- RegistrationFrameMemory.cpp - Bounded x86 frame-cell accesses ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Visit overlapping four-byte cells without rescanning unrelated locals.
//===----------------------------------------------------------------------===//

#include "RegistrationFrame.h"

#include <algorithm>
#include <bit>

namespace neverd::registration_state {
namespace {

void storeCell(std::map<int32_t, FrameValue> &Cells, int32_t Offset,
               uint16_t Width, const FrameValue &Value) {
  auto It = Cells.lower_bound(
      int32_t(std::max<int64_t>(INT32_MIN, int64_t(Offset) - 3)));
  while (It != Cells.end() && int64_t(It->first) < int64_t(Offset) + Width) {
    const bool FullyOverwritten =
        Offset <= It->first &&
        int64_t(It->first) + 4 <= int64_t(Offset) + Width;
    if (!FullyOverwritten && It->second.MayBeFrame) {
      It->second = {{}, {}, false, true};
      ++It;
    } else
      It = Cells.erase(It);
  }
  if (Width == 4 && Value != FrameValue{})
    Cells[Offset] = Value;
  else if (Value.MayBeFrame)
    // A pointer can be split into narrow writes and reconstructed by a
    // later load. Retain conservative four-byte coverage for every written
    // chunk, including a final partial chunk, rather than dropping its
    // provenance merely because this store is not a full pointer width.
    for (uint32_t I = 0; I < Width; I += 4)
      Cells[static_cast<int32_t>(uint32_t(Offset) + I)] = {{}, {}, false, true};
}

FrameValue loadCell(const std::map<int32_t, FrameValue> &Cells,
                    std::optional<int32_t> Offset, uint16_t Width) {
  if (Offset && Width == 4)
    if (auto It = Cells.find(*Offset); It != Cells.end())
      return It->second;
  auto It = Offset ? Cells.lower_bound(int32_t(
                         std::max<int64_t>(INT32_MIN, int64_t(*Offset) - 3)))
                   : Cells.begin();
  for (; It != Cells.end() &&
         (!Offset || int64_t(It->first) < int64_t(*Offset) + Width);
       ++It)
    if (It->second.MayBeFrame)
      return {{}, {}, false, true};
  return {};
}

} // namespace

size_t FrameState::memoryAccessWork(const FrameValue &Address,
                                    uint16_t Width) const {
  if (!Address.MayBeFrame)
    return 0;
  const auto *Space = &Cells;
  if (Address.CallbackAddress &&
      Address.CallbackAddress->Entry == CallbackEntry)
    Space = &CallbackCells;
  else if (Address.EntryOffset && *Address.EntryOffset >= -12)
    Space = &EntryCells;
  else if (!Address.Offset)
    return Cells.size() + EntryCells.size() + CallbackCells.size();
  // A known byte range intersects only keys in [Offset - 3, Offset + Width).
  // Include both ordered-map searches and the maximum overlapping entries.
  return 4 * std::bit_width(Space->size()) +
         std::min(Space->size(), size_t(Width) + 3) + 1;
}

void FrameState::store(int32_t Offset, uint16_t Width,
                       const FrameValue &Value) {
  storeCell(Cells, Offset, Width, Value);
}
void FrameState::storeEntry(int32_t Offset, uint16_t Width,
                            const FrameValue &Value) {
  storeCell(EntryCells, Offset, Width, Value);
}
FrameValue FrameState::load(std::optional<int32_t> Offset,
                            uint16_t Width) const {
  auto Value = loadCell(Cells, Offset, Width);
  if (!Offset) {
    Value = join(Value, loadCell(EntryCells, std::nullopt, Width));
    Value = join(Value, loadCell(CallbackCells, std::nullopt, Width));
  }
  return Value;
}
FrameValue FrameState::loadEntry(int32_t Offset, uint16_t Width) const {
  return loadCell(EntryCells, Offset, Width);
}

void FrameState::storeCallback(int32_t Offset, uint16_t Width,
                               const FrameValue &Value) {
  storeCell(CallbackCells, Offset, Width, Value);
  for (int64_t Byte = Offset; Byte < int64_t(Offset) + Width; ++Byte)
    InitializedCallbackBytes.insert(int32_t(Byte));
}
FrameValue FrameState::loadCallback(int32_t Offset, uint16_t Width) const {
  return loadCell(CallbackCells, Offset, Width);
}

} // namespace neverd::registration_state
