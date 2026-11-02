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

#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/support/BranchEncoding.h"
#include "neverd/support/ProloguePatterns.h"
#include "neverd/support/SortedSearch.h"

#include <algorithm>
#include <vector>

namespace neverd {
namespace func_detect_detail {

void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out,
                             const Cancellation *Control) {
  va_t Cur = (Start + 3) & ~static_cast<va_t>(3);
  const uint8_t *Data = Seg->Data.data();
  const size_t DataSize = Seg->Data.size();
  const auto WordAt = [&](size_t Off) {
    const uint8_t *P = Data + Off;
    return static_cast<uint32_t>(P[0]) | (static_cast<uint32_t>(P[1]) << 8) |
           (static_cast<uint32_t>(P[2]) << 16) |
           (static_cast<uint32_t>(P[3]) << 24);
  };
  std::vector<va_t> ConditionalTargets, EntryCandidates;
  while (Cur <= End && End - Cur >= 4) {
    func_detect_detail::checkCancellation(Control);
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off + 4 > DataSize)
      break;
    if (!Img.hasExecutableCodeOwnerRange(Cur, 4)) {
      Cur += 4;
      continue;
    }
    uint32_t Word = WordAt(Off);
    va_t Tgt = AArch64Lifter::decodeBranchLinkTarget(Word, Cur);
    if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt))
      Out.insert(Tgt);
    // Index forward conditional targets once. A shrink-wrapped frame can
    // follow an early return more than a fixed look-behind window away.
    if (const auto Displacement =
            branch::a64ConditionalBranchDisplacement(Word);
        Displacement && *Displacement > 0 &&
        static_cast<uint64_t>(*Displacement) <= End - Cur)
      ConditionalTargets.push_back(Cur + static_cast<uint64_t>(*Displacement));
    // A function-entry marker (paciasp, bti c, the frame push) right after a
    // block end may begin the next of two functions the toolchain packed with
    // no padding between them -- a tail-called or address-taken routine no BL,
    // symbol, unwind row or relocation names.  The decode verifier still
    // decides each candidate.  Two checks keep a marker inside a function
    // out: the block end, since a landing pad's block falls through to its
    // marker, and the lack of a conditional branch into it from an earlier
    // instruction, since that is how a function reaches the prologue
    // shrink-wrapping moved past its frame-free early return.
    if (Off >= 4 && isFunctionEntryMarkerWordAArch64(Word) &&
        isAArch64BlockEndWord(WordAt(Off - 4)) &&
        Img.hasExecutableCodeOwnerRange(Cur - 4, 4))
      EntryCandidates.push_back(Cur);
    Cur += 4;
  }
  std::sort(ConditionalTargets.begin(), ConditionalTargets.end());
  for (va_t Entry : EntryCandidates)
    if (!sorted::contains64(ConditionalTargets.data(),
                            ConditionalTargets.size(), Entry))
      Out.insert(Entry);
}

} // namespace func_detect_detail
} // namespace neverd
