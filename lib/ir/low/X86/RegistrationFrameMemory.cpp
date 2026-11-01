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
      const auto Merged = join(It->second, Value);
      It->second = FrameValue::taint(true, Merged.FrameOnlyFromSEHScalar);
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
    for (uint32_t I = 0; I < Width; I += 4) {
      const auto Taint = FrameValue::taint(true, Value.FrameOnlyFromSEHScalar);
      const auto [Cell, Added] =
          Cells.emplace(static_cast<int32_t>(uint32_t(Offset) + I), Taint);
      if (!Added)
        Cell->second = join(Cell->second, Taint);
    }
}

FrameValue loadCell(const std::map<int32_t, FrameValue> &Cells,
                    std::optional<int32_t> Offset, uint16_t Width) {
  if (Offset && Width == 4)
    if (auto It = Cells.find(*Offset); It != Cells.end())
      // Narrow stores retain a conservative four-byte taint cell. Its scalar
      // provenance cannot hide physical pointer bits in overlapping cells.
      if (!It->second.FrameOnlyFromSEHScalar)
        return It->second;
  auto It = Offset ? Cells.lower_bound(int32_t(
                         std::max<int64_t>(INT32_MIN, int64_t(*Offset) - 3)))
                   : Cells.begin();
  FrameValue Result;
  for (; It != Cells.end() &&
         (!Offset || int64_t(It->first) < int64_t(*Offset) + Width);
       ++It)
    if (It->second.MayBeFrame)
      Result = join(Result,
                    FrameValue::taint(true, It->second.FrameOnlyFromSEHScalar));
  return Result;
}

} // namespace

void discardFrameCellsBelow(FrameCells &Snapshot, int32_t Boundary) {
  if (Snapshot.empty() || Snapshot.begin()->first >= Boundary)
    return;
  Snapshot.edit([&](FrameCells::Map &Cells) {
    auto It = Cells.lower_bound(
        int32_t(std::max<int64_t>(INT32_MIN, int64_t(Boundary) - 3)));
    Cells.erase(Cells.begin(), It);
    while (It != Cells.end() && It->first < Boundary) {
      if (It->second.MayBeFrame) {
        It->second = FrameValue::taint(true, It->second.FrameOnlyFromSEHScalar);
        ++It;
      } else
        It = Cells.erase(It);
    }
  });
}

size_t FrameState::memoryAccessWork(const FrameValue &Address, uint16_t Width,
                                    bool Write) const {
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
  const size_t Count = Space->size();
  size_t Work =
      4 * std::bit_width(Count) + std::min(Count, size_t(Width) + 3) + 1;
  if (Write) {
    const size_t Inserts = (size_t(Width) + 3) / 4;
    Work += Inserts * (1 + 2 * std::bit_width(Count + Inserts));
  }
  return Work;
}

void FrameState::store(int32_t Offset, uint16_t Width,
                       const FrameValue &Value) {
  Cells.edit(
      [&](FrameCells::Map &Cells) { storeCell(Cells, Offset, Width, Value); });
}

void FrameState::forgetCellValues(int32_t Offset, uint32_t Width) {
  if (!Width || Cells.empty())
    return;
  Cells.edit([&](FrameCells::Map &Cells) {
    auto It = Cells.lower_bound(
        int32_t(std::max<int64_t>(INT32_MIN, int64_t(Offset) - 3)));
    while (It != Cells.end() && int64_t(It->first) < int64_t(Offset) + Width) {
      if (It->second.MayBeFrame) {
        It->second = {{}, {}, false, true};
        ++It;
      } else
        It = Cells.erase(It);
    }
  });
}
void FrameState::storeEntry(int32_t Offset, uint16_t Width,
                            const FrameValue &Value) {
  EntryCells.edit(
      [&](FrameCells::Map &Cells) { storeCell(Cells, Offset, Width, Value); });
}
FrameValue FrameState::load(std::optional<int32_t> Offset,
                            uint16_t Width) const {
  auto Value = loadCell(Cells.read(), Offset, Width);
  if (!Offset) {
    Value = join(Value, loadCell(EntryCells.read(), std::nullopt, Width));
    Value = join(Value, loadCell(CallbackCells.read(), std::nullopt, Width));
  }
  return Value;
}
FrameValue FrameState::loadEntry(int32_t Offset, uint16_t Width) const {
  return loadCell(EntryCells.read(), Offset, Width);
}

void FrameState::storeCallback(int32_t Offset, uint16_t Width,
                               const FrameValue &Value) {
  CallbackCells.edit(
      [&](FrameCells::Map &Cells) { storeCell(Cells, Offset, Width, Value); });
  for (int64_t Byte = Offset; Byte < int64_t(Offset) + Width; ++Byte)
    InitializedCallbackBytes.insert(int32_t(Byte));
}
FrameValue FrameState::loadCallback(int32_t Offset, uint16_t Width) const {
  return loadCell(CallbackCells.read(), Offset, Width);
}

} // namespace neverd::registration_state
