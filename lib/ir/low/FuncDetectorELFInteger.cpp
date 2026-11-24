//===- FuncDetectorELFInteger.cpp - RISC-V and PowerPC direct calls ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walk physical instruction boundaries. Stop a sweep at an undecodable prefix
/// so its payload cannot supply a spurious call.
///
//===----------------------------------------------------------------------===//

#include "FuncDetectorDetail.h"

namespace neverd::func_detect_detail {
void scanELFIntegerCalls(const BinaryImage &Img, Decoder &Dec,
                         const Segment *Seg, va_t Start, va_t End,
                         std::set<va_t> &Out, const Cancellation *Control) {
  for (va_t At = Start; At < End;) {
    checkCancellation(Control);
    DecodedInsn Insn;
    const int Size = Dec.decodeOneLight(Seg->Data.data() + (At - Seg->VA),
                                        End - At, At, Insn);
    if (Size <= 0)
      break;
    va_t Target = Dec.directCallTarget(Insn);
    if (Target != InvalidVA && Img.hasExecutableCodeOwnerRange(At, Size) &&
        Img.isFunctionEntryCandidateAt(Target))
      Out.insert(Target);
    At += Size;
  }
}
} // namespace neverd::func_detect_detail
