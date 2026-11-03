//===- FuncDetectorAArch64.cpp - AArch64 function-entry scanning ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 direct-call (BL) discovery, and the entries of functions packed
/// with no padding between them.  Instructions are fixed 4-byte width and
/// 4-byte aligned, so each word is classified with a mask+shift instead of a
/// full decoder walk.  The BL encoding is shared with AArch64Lifter.
///
//===----------------------------------------------------------------------===//

#include "../FuncDetectorDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"
#include "neverd/support/ProloguePatterns.h"

#include <algorithm>
#include <optional>

namespace neverd {
namespace func_detect_detail {

namespace {

std::optional<uint32_t> wordAt(const BinaryImage &Img, va_t Addr) {
  if (!Img.hasExecutableCodeOwnerRange(Addr, 4))
    return std::nullopt;
  const uint8_t *Bytes = Img.readVA(Addr, 4);
  if (!Bytes)
    return std::nullopt;
  return readLE<uint32_t>(Bytes);
}

std::optional<va_t>
markedEntryAfterFunctionEnd(const BinaryImage &Img, va_t Addr,
                            InternalNoReturnIndex &NoReturn) {
  const auto Marker = wordAt(Img, Addr);
  if (!Marker || !isFunctionEntryMarkerWordAArch64(*Marker))
    return std::nullopt;
  // Calls provide a boundary only when the shared CFG proof establishes
  // that they cannot resume at the marker.
  std::optional<va_t> After;
  for (va_t Before = Addr;
       Before >= 4 &&
       Addr - Before < 4 * limits::ir::kAArch64EntryFillerWords;) {
    Before -= 4;
    const auto Word = wordAt(Img, Before);
    const va_t Callee =
        Word ? AArch64Lifter::decodeBranchLinkTarget(*Word, Before) : InvalidVA;
    const bool EndsCall = Callee != InvalidVA &&
                          NoReturn.mayNeverReturn(Callee) &&
                          NoReturn.neverReturns(Callee, 0);
    if (!Word || isAArch64BlockEndWord(*Word) || EndsCall) {
      After = Before + 4;
      break;
    }
    if (isFunctionEntryMarkerWordAArch64(*Word))
      return std::nullopt;
  }
  return After;
}

} // namespace

void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out,
                             AArch64EntryScan &Entries,
                             const Cancellation *Control) {
  va_t Cur = (Start + 3) & ~static_cast<va_t>(3);
  const uint8_t *Data = Seg->Data.data();
  const size_t DataSize = Seg->Data.size();
  InternalNoReturnIndex NoReturn(Img, nullptr, nullptr, nullptr, nullptr);
  // A binary file's or a PE's executable bytes can hold constants, as a
  // kernel Image's one executable section holds its read-only data: a word
  // there that reads as BL names no function.  A page of them, read as
  // instructions, holds unallocated encodings or small constants (UDF #imm);
  // zero is padding.  Other formats' sections keep code apart, and their
  // headers, which share a page with the code, hold small constants too.
  const bool MayHoldData =
      Img.Format == BinaryFormat::Raw || Img.Format == BinaryFormat::COFF;
  va_t ClassifiedPage = InvalidVA;
  bool DataPage = false;
  const auto InDataPage = [&](va_t Addr) {
    const va_t Page =
        Addr & ~static_cast<va_t>(limits::support::kAArch64PageBytes - 1);
    if (Page != ClassifiedPage) {
      ClassifiedPage = Page;
      const va_t From = std::max(Page, Seg->VA);
      const size_t Offset = static_cast<size_t>(From - Seg->VA);
      const size_t Size =
          std::min<size_t>(limits::support::kAArch64PageBytes - (From - Page),
                           DataSize - Offset);
      DataPage = isAArch64DataPage(Data + Offset, Size);
    }
    return DataPage;
  };

  while (Cur <= End && End - Cur >= 4) {
    func_detect_detail::checkCancellation(Control);
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off + 4 > DataSize)
      break;
    if (!Img.hasExecutableCodeOwnerRange(Cur, 4)) {
      Cur += 4;
      continue;
    }
    const uint32_t Word = readLE<uint32_t>(Data + Off);
    va_t Tgt = AArch64Lifter::decodeBranchLinkTarget(Word, Cur);
    if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt) &&
        !(MayHoldData && InDataPage(Cur)))
      Out.insert(Tgt);
    // Targets from every scan chunk participate before any marker is
    // admitted, including branches into filler before a distant prologue.
    if (const auto Displacement =
            branch::a64ConditionalBranchDisplacement(Word);
        Displacement && *Displacement > 0 &&
        static_cast<uint64_t>(*Displacement) <= InvalidVA - Cur)
      Entries.ConditionalTargets.push_back(Cur + *Displacement);
    if (isFunctionEntryMarkerWordAArch64(Word))
      if (const auto After = markedEntryAfterFunctionEnd(Img, Cur, NoReturn))
        Entries.EntryCandidates.emplace_back(Cur, *After);
    Cur += 4;
  }
}

} // namespace func_detect_detail
} // namespace neverd
