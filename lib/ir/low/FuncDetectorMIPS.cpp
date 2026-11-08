//===- FuncDetectorMIPS.cpp - Classic MIPS direct-call scan --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Admit direct calls only when their complete execution unit can be lifted.
/// Each aligned word is a candidate, since a delay slot may also be entered
/// independently by another edge.

#include "FuncDetectorDetail.h"

#include "neverd/ArchSupport.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::func_detect_detail {

void scanSegmentCallsMIPS(const BinaryImage &Img, Decoder &Dec,
                          const Segment *Seg, va_t Start, va_t End,
                          std::set<va_t> &Out, const Cancellation *Control) {
  for (va_t At = (Start + 3) & ~va_t(3); At < End && End - At >= 4; At += 4) {
    checkCancellation(Control);
    const auto *Bytes = Seg->Data.data() + (At - Seg->VA);
    if (readTargetEndian<uint32_t>(Bytes, archLittleEndian(Img.Arch)) >> 26 !=
        3)
      continue;
    DecodedExecutionUnit Unit;
    const int Size =
        Dec.decodeUnitForLift(Seg->Data.data() + (At - Seg->VA),
                              static_cast<size_t>(End - At), At, Unit);
    if (Size <= 0)
      continue;
    const va_t Target = Dec.directCallTarget(Unit.Head);
    if (Target == InvalidVA || !Img.isFunctionEntryCandidateAt(Target) ||
        !Img.hasExecutableCodeOwnerRange(At, Size))
      continue;
    std::vector<LowOp> Ops;
    try {
      Dec.liftUnitToLow(Unit, Ops);
    } catch (const UnliftedInstruction &) {
      continue;
    }
    Out.insert(Target);
  }
}

} // namespace neverd::func_detect_detail
