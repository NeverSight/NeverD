//===- FunctionBody.cpp - Darwin arm64e function extents ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/FunctionBody.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>

namespace neverd {
std::optional<MachOLiftedSource> machOLiftedSource(const BinaryImage &Image,
                                                   va_t Start, va_t NextEntry) {
  constexpr auto Word = aarch64::kInsnSize;
  if (!Image.isArm64e() || Start % Word ||
      !Image.hasAuthenticatedFunctionEntryAt(Start) || NextEntry <= Start ||
      NextEntry - Start < Word)
    return std::nullopt;
  const auto *Segment = Image.getSegmentFor(Start);
  if (!Segment || !Segment->isExecutable() || Segment->isWritable() ||
      !rangeInBounds(Start - Segment->VA, Word, Segment->FileSz))
    return std::nullopt;
  const auto *Section = Image.getSectionFor(Start);
  if (Section && (!(Section->Type & llvm::MachO::S_ATTR_PURE_INSTRUCTIONS) ||
                  Section->FileSz != Section->Size ||
                  !rangeInBounds(Start - Section->VA, Word, Section->Size)))
    return std::nullopt;
  const auto *First = Image.readVA(Start, Word);
  if (!First)
    return std::nullopt;
  std::optional<va_t> ForwardTarget;
  if (const auto Jump =
          decodeDirectBranch(Arch::AArch64, InstructionMode::Default, First,
                             Word, Start, {.Jumps = true}))
    if (const auto Target = Jump->target();
        Target && *Target != Start &&
        Image.hasAuthenticatedFunctionEntryAt(*Target))
      ForwardTarget = Target;
  va_t End = Start + Word;
  if (!ForwardTarget) {
    const auto *Frame = Image.ExceptionMetadata.findFunction(Start);
    if (!Frame || !Frame->Compact)
      return std::nullopt;
    const va_t Limit =
        Section ? Section->VA + Section->Size : Frame->CodeRange.End;
    End = std::min(NextEntry, Limit);
    if (End <= Start || (End - Start) % Word || Frame->CodeRange.End < End ||
        !rangeInBounds(Start - Segment->VA, End - Start, Segment->FileSz))
      return std::nullopt;
  }
  const auto Data = Image.DataInCode.lower_bound(Start);
  if (Data != Image.DataInCode.end() && *Data < End)
    return std::nullopt;
  if (!Section && llvm::any_of(Image.Sections, [&](const auto &S) {
        return S.Size && S.VA < End && (S.VA >= Start || Start - S.VA < S.Size);
      }))
    return std::nullopt;
  if (ForwardTarget)
    return MachOLiftedSource{{First, Word}, ForwardTarget};
  const auto *Bytes = Image.readVA(Start, End - Start);
  if (!Bytes)
    return std::nullopt;
  return MachOLiftedSource{{Bytes, size_t(End - Start)}, std::nullopt};
}

} // namespace neverd
