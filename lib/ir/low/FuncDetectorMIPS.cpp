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
#include "neverd/loader/ELF/MIPSCodeMode.h"
#include "neverd/support/BinaryEncoding.h"

namespace neverd::func_detect_detail {

void scanSegmentCallsMIPS(const BinaryImage &Img, Decoder &Dec,
                          const Segment *Seg, va_t Start, va_t End,
                          std::set<va_t> &Out, const Cancellation *Control) {
  const bool Mixed = Img.Mode == InstructionMode::MixedMIPS ||
                     Img.Mode == InstructionMode::MicroMIPS;
  const unsigned Align = Mixed ? 2 : 4;
  unsigned Step = Align;
  for (va_t At = (Start + Align - 1) & ~va_t(Align - 1);
       At < End && End - At >= Align; At += Step) {
    checkCancellation(Control);
    Step = Align;
    const auto *Bytes = Seg->Data.data() + (At - Seg->VA);
    if (!Mixed &&
        readTargetEndian<uint32_t>(Bytes, archLittleEndian(Img.Arch)) >> 26 !=
            3)
      continue;
    if (!Dec.selectMode(Img, At))
      continue;
    DecodedExecutionUnit Unit;
    const int Size =
        Dec.decodeUnitForLift(Seg->Data.data() + (At - Seg->VA),
                              static_cast<size_t>(End - At), At, Unit);
    if (Size <= 0)
      continue;
    Step = Unit.Head.Size;
    const va_t Target = Dec.directCallTarget(Unit.Head);
    if (Target == InvalidVA || !Img.isFunctionEntryCandidateAt(Target) ||
        !Img.hasExecutableCodeOwnerRange(At, Size) ||
        !mips::codeModeRangeMatches(Img, At, Size, Dec.currentMode()))
      continue;
    auto Destination = canonicalizeLowControlTarget(
        Target, Dec.currentMode(),
        Dec.controlTargetMode(Unit.Head, Dec.currentMode()));
    if (!Destination) {
      llvm::consumeError(Destination.takeError());
      continue;
    }
    if (!Img.instructionModeAt(Destination->Address, Destination->Mode))
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
