//===- FuncDetectorARM.cpp - ARM32 function-entry scanning ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM32 (ARM and Thumb) direct-call discovery.  Thumb BL is two halfwords
/// and ARM/Thumb interworking can change the decoder state, so this walk
/// uses the architecture decoder rather than a fixed-width mask.
///
//===----------------------------------------------------------------------===//

#include "../FuncDetectorDetail.h"

namespace neverd {
namespace func_detect_detail {

void scanSegmentCallsARM(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out, const Cancellation *Control) {
  va_t Cur = Start;
  while (Cur < End) {
    func_detect_detail::checkCancellation(Control);
    size_t Off = static_cast<size_t>(Cur - Seg->VA);
    if (Off >= Seg->Data.size())
      break;
    if (!Dec.selectMode(Img, Cur)) {
      ++Cur;
      continue;
    }
    DecodedInsn DI;
    const size_t Remain =
        static_cast<size_t>(std::min<va_t>(Seg->Data.size() - Off, End - Cur));
    int Sz = Dec.decodeOne(Seg->Data.data() + Off, Remain, Cur, DI);
    if (Sz == 0) {
      Cur++;
      continue;
    }
    if (!Img.hasExecutableCodeOwnerRange(Cur, static_cast<uint64_t>(Sz)) ||
        Img.instructionModeAt(Cur + Sz - 1) != Dec.currentMode()) {
      Cur += Sz;
      continue;
    }
    va_t Tgt = Dec.directCallTarget(DI);
    if (Tgt != InvalidVA && Img.hasExecutableCodeOwnerAt(Tgt))
      Out.insert(Tgt);
    Cur += Sz;
  }
}

} // namespace func_detect_detail
} // namespace neverd
