//===- GoStackMaps.cpp - Go pointer maps and unsafe-point tables --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeDetail.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::go_loader::detail {
namespace {

GoUnsafePointKind classifyUnsafePoint(int32_t Value) {
  switch (Value) {
  case UnsafePointSafe:
    return GoUnsafePointKind::Safe;
  case UnsafePointUnsafe:
    return GoUnsafePointKind::Unsafe;
  case UnsafePointRestart1:
  case UnsafePointRestart2:
    return GoUnsafePointKind::RestartSequence;
  case UnsafePointRestartAtEntry:
    return GoUnsafePointKind::RestartAtEntry;
  default:
    return GoUnsafePointKind::Unknown;
  }
}

//===----------------------------------------------------------------------===//
// Stack maps
//===----------------------------------------------------------------------===//

/// Decode the `runtime.stackmap` a pointer-map funcdata entry addresses:
/// `n int32`, `nbit int32`, then `n` bitmaps of `(nbit+7)/8` bytes each.
///
/// Every field is checked against a bound before it is used to size anything,
/// because a funcdata pointer that was resolved through the wrong base still
/// addresses mapped memory and would otherwise turn four arbitrary bytes into
/// an allocation request.
std::optional<GoStackMap> decodeStackMap(const ImageReader &R, va_t RecordVA) {
  std::optional<int32_t> Count = R.i32(RecordVA);
  std::optional<int32_t> BitCount = R.i32(RecordVA + 4);
  if (!Count || !BitCount || *Count < 0 || *BitCount < 0)
    return std::nullopt;
  if (static_cast<uint32_t>(*Count) > GoStackMap::MaxBitmaps ||
      static_cast<uint32_t>(*BitCount) > GoStackMap::MaxBits)
    return std::nullopt;

  GoStackMap Map;
  Map.RecordVA = RecordVA;
  Map.BitCount = static_cast<uint32_t>(*BitCount);
  const uint32_t Stride = (Map.BitCount + 7) / 8;
  if (static_cast<uint64_t>(*Count) * Stride > GoStackMap::MaxTotalBytes)
    return std::nullopt;

  // A zero-bit map carries no bytes, but it still declares `n` bitmaps: the
  // linker hands every function whose argument area holds no pointer the same
  // shared `n=1, nbit=0` record.  Reporting no bitmaps for it would make the
  // index a stack map names unsatisfiable and throw away a table that is in
  // fact well formed, so the bitmaps are published empty rather than dropped.
  Map.Bitmaps.reserve(static_cast<size_t>(*Count));
  for (uint32_t I = 0; I < static_cast<uint32_t>(*Count); ++I) {
    GoStackMapBitmap Bitmap;
    Bitmap.Index = I;
    Bitmap.BitCount = Map.BitCount;
    if (Stride != 0) {
      const uint64_t Offset = uint64_t(8) + uint64_t(I) * Stride;
      if (Offset > InvalidVA - RecordVA)
        return std::nullopt;
      const uint8_t *Data =
          R.bytes(RecordVA + static_cast<va_t>(Offset), Stride);
      if (!Data)
        return std::nullopt;
      Bitmap.Bits.assign(Data, Data + Stride);
    }
    Map.Bitmaps.push_back(std::move(Bitmap));
  }
  return Map;
}

} // namespace

/// Which pcdata table holds `PCDATA_StackMapIndex` on the Go 1.2 layout.
///
/// Go 1.13 moved the table from index 0 to index 1, putting a register-map
/// index where it had been, and left the magic alone.  What settles it is the
/// pointer map the index selects into: an index has to name one of the map's
/// `n` bitmaps, so a candidate table that ever yields a value outside
/// `[-1, n)` is not the stack map index, and one counterexample rules it out
/// for the whole module.  Only functions whose map holds more than one bitmap
/// can rule anything out — where there is one bitmap every in-range index is
/// zero and the two candidates are indistinguishable — so those are the only
/// ones sampled.  Those same functions are why a candidate that is absent
/// everywhere is not evidence against the other one: a frame with two bitmaps
/// has to carry the table that picks between them or the runtime could not
/// pick, so the position that never appears is the position the table is not
/// at.  When both candidates survive, or neither does, nothing was proven and
/// the caller reports no ranges rather than picking one.
std::optional<unsigned> resolveStackMapPCDataIndex(
    const ImageReader &R, const FuncLayout &L, const PcHeader &H,
    const std::vector<GoFunction> &Funcs, va_t GoFuncBase) {
  constexpr size_t NumCandidates = 2;
  constexpr unsigned Candidates[NumCandidates] = {PCDataStackMapIndexPreGo113,
                                                  PCDataStackMapIndex};
  bool Disqualified[NumCandidates] = {};
  unsigned Confirmed[NumCandidates] = {};
  unsigned Sampled = 0;
  for (const GoFunction &G : Funcs) {
    if (Sampled >= StackMapProbeTarget)
      break;
    std::optional<va_t> MapVA =
        getFuncDataAddress(R, L, G.Raw, FuncDataLocalsPointerMaps, GoFuncBase);
    if (!MapVA)
      continue;
    std::optional<int32_t> BitmapCount = R.i32(*MapVA);
    if (!BitmapCount || *BitmapCount <= 1 ||
        static_cast<uint32_t>(*BitmapCount) > GoStackMap::MaxBitmaps)
      continue;
    ++Sampled;
    for (size_t C = 0; C < NumCandidates; ++C) {
      std::optional<uint32_t> Offset =
          getPCDataOffset(R, L, G.Raw, Candidates[C]);
      if (!Offset)
        continue;
      bool InRange = true;
      const bool Walked =
          forEachPCValue(R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
                         [&](const PCValueRange &V) {
                           if (V.Value < -1 || V.Value >= *BitmapCount) {
                             InRange = false;
                             return false;
                           }
                           return true;
                         });
      if (Walked && InRange)
        ++Confirmed[C];
      else
        Disqualified[C] = true;
    }
  }
  std::optional<unsigned> Winner;
  for (size_t C = 0; C < NumCandidates; ++C) {
    if (Disqualified[C] || Confirmed[C] == 0)
      continue;
    if (Winner)
      return std::nullopt;
    Winner = Candidates[C];
  }
  return Winner;
}

/// Decode `PCDATA_UnsafePoint` into a partition of the body.
void decodeUnsafePoints(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H, const GoFunction &G,
                        GoFunctionEH &EH, ExceptionParseStatus &Status,
                        std::vector<std::string> &Diagnostics) {
  if (!L.HasUnsafePointTable)
    return;
  std::optional<uint32_t> Offset =
      getPCDataOffset(R, L, G.Raw, PCDataUnsafePoint);
  if (!Offset)
    return;
  std::vector<GoUnsafePointRange> Ranges;
  bool Truncated = false;
  const bool Walked =
      forEachPCValue(R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
                     [&](const PCValueRange &V) {
                       if (Ranges.size() >= MaxPCValueRanges) {
                         Truncated = true;
                         return false;
                       }
                       GoUnsafePointRange Range;
                       Range.Range = ExceptionAddressRange{V.Begin, V.End};
                       Range.Kind = classifyUnsafePoint(V.Value);
                       Range.NativeValue = V.Value;
                       Ranges.push_back(std::move(Range));
                       return true;
                     });
  if (!Walked) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go unsafe-point table at pctab offset " +
                          std::to_string(*Offset) +
                          " is not a readable pc-value table");
    return;
  }
  if (Truncated) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go unsafe-point table has more than " +
                          std::to_string(MaxPCValueRanges) +
                          " ranges, so the tail was not decoded");
  }
  EH.UnsafePointRanges = std::move(Ranges);
}

/// Decode both pointer maps and the `PCDATA_StackMapIndex` table that selects
/// between their bitmaps.
void decodeStackMaps(const ImageReader &R, const FuncLayout &L,
                     const PcHeader &H, const GoFunction &G, va_t GoFuncBase,
                     GoFunctionEH &EH, ExceptionParseStatus &Status,
                     std::vector<std::string> &Diagnostics) {
  auto readMap = [&](unsigned Index, const char *What,
                     std::optional<GoStackMap> &Out) {
    std::optional<va_t> MapVA =
        getFuncDataAddress(R, L, G.Raw, Index, GoFuncBase);
    if (!MapVA)
      return;
    std::optional<GoStackMap> Map = decodeStackMap(R, *MapVA);
    if (!Map) {
      Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
      Diagnostics.push_back(std::string("Go ") + What + " pointer map at " +
                            llvm::utohexstr(*MapVA) +
                            " is not a readable stackmap");
      return;
    }
    Out = std::move(Map);
  };
  readMap(FuncDataArgsPointerMaps, "argument", EH.ArgsPointerMap);
  readMap(FuncDataLocalsPointerMaps, "locals", EH.LocalsPointerMap);
  if (!EH.ArgsPointerMap && !EH.LocalsPointerMap)
    return;

  if (!L.StackMapPCDataIndex)
    return;
  std::optional<uint32_t> Offset =
      getPCDataOffset(R, L, G.Raw, *L.StackMapPCDataIndex);
  if (!Offset)
    return;
  std::vector<GoStackMapRange> Ranges;
  bool Truncated = false;
  const bool Walked =
      forEachPCValue(R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
                     [&](const PCValueRange &V) {
                       if (Ranges.size() >= MaxPCValueRanges) {
                         Truncated = true;
                         return false;
                       }
                       Ranges.push_back(GoStackMapRange{
                           ExceptionAddressRange{V.Begin, V.End}, V.Value});
                       return true;
                     });
  if (!Walked) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index table at pctab offset " +
                          std::to_string(*Offset) +
                          " is not a readable pc-value table");
    return;
  }
  if (Truncated) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index table has more than " +
                          std::to_string(MaxPCValueRanges) +
                          " ranges, so the tail was not decoded");
  }
  // An index neither map can satisfy means the table being read is not the
  // one the maps belong to, so the ranges describe nothing and are dropped
  // rather than published as a selection into bitmaps that do not exist.  The
  // bar is the larger of the two counts rather than the smaller because the
  // two are allowed to differ: a function whose arguments hold no pointers
  // gets the linker's shared single-bitmap map for them while keeping a full
  // one for its locals.
  const int32_t Available = static_cast<int32_t>(
      std::max(EH.ArgsPointerMap ? EH.ArgsPointerMap->Bitmaps.size() : 0,
               EH.LocalsPointerMap ? EH.LocalsPointerMap->Bitmaps.size() : 0));
  for (const GoStackMapRange &Range : Ranges) {
    if (Range.Index < Available)
      continue;
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index " + std::to_string(Range.Index) +
                          " at " + llvm::utohexstr(Range.Range.Begin) +
                          " names no bitmap this function declares");
    return;
  }
  EH.StackMapRanges = std::move(Ranges);
}

} // namespace neverd::go_loader::detail
