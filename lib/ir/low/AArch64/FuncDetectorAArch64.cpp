//===- FuncDetectorAArch64.cpp - AArch64 function-entry scanning ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// AArch64 direct-call (BL) discovery.  Instructions are fixed 4-byte width
/// and 4-byte aligned, so each word is classified with a mask+shift instead
/// of a full decoder walk.  The BL encoding is shared with AArch64Lifter.
///
//===----------------------------------------------------------------------===//

#include "../FuncDetectorDetail.h"

#include "neverd/lift/AArch64/AArch64Lifter.h"

namespace neverd {
namespace func_detect_detail {

void scanSegmentCallsAArch64(const BinaryImage &Img, const Segment *Seg,
                             va_t Start, va_t End, std::set<va_t> &Out,
                             const Cancellation *Control) {
  va_t Cur = (Start + 3) & ~static_cast<va_t>(3);
  const uint8_t *Data = Seg->Data.data();
  const size_t DataSize = Seg->Data.size();
  while (Cur <= End && End - Cur >= 4) {
    func_detect_detail::checkCancellation(Control);
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off + 4 > DataSize)
      break;
    if (!Img.hasExecutableCodeOwnerRange(Cur, 4)) {
      Cur += 4;
      continue;
    }
    const uint8_t *P = Data + Off;
    uint32_t Word = static_cast<uint32_t>(P[0]) |
                    (static_cast<uint32_t>(P[1]) << 8) |
                    (static_cast<uint32_t>(P[2]) << 16) |
                    (static_cast<uint32_t>(P[3]) << 24);
    va_t Tgt = AArch64Lifter::decodeBranchLinkTarget(Word, Cur);
    if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt))
      Out.insert(Tgt);
    Cur += 4;
  }
}

} // namespace func_detect_detail
} // namespace neverd
