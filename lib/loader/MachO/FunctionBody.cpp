//===- FunctionBody.cpp - Darwin arm64e function extents ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/FunctionBody.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/support/AArch64Literal.h"
#include "neverd/support/ISAEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"

#include <algorithm>

namespace neverd {
static std::optional<va_t> interiorOwner(const BinaryImage &Image, va_t VA,
                                         unsigned Width) {
  if (!Image.isArm64e() || !Image.hasExecutableCodeOwnerRange(VA, Width))
    return std::nullopt;
  const auto *Frame = Image.ExceptionMetadata.findFunction(VA);
  const auto *Segment = Image.getSegmentFor(VA);
  if (!Frame || (!Frame->Compact && !Frame->Dwarf) || !Segment ||
      !Segment->isExecutable() || Segment->isWritable() ||
      !rangeInBounds(VA - Frame->CodeRange.Begin, Width,
                     Frame->CodeRange.size()) ||
      !rangeInBounds(VA - Segment->VA, Width, Segment->FileSz))
    return std::nullopt;
  const auto Data = Image.DataInCode.lower_bound(VA);
  if (Data != Image.DataInCode.end() && *Data - VA < Width)
    return std::nullopt;
  if (Frame->Dwarf && (VA < Frame->Dwarf->InitialLocation ||
                       !rangeInBounds(VA - Frame->Dwarf->InitialLocation, Width,
                                      Frame->Dwarf->AddressRange)))
    return std::nullopt;
  // Darwin may coalesce several adjacent functions' compact unwind rows.
  // The linker's starts still delimit the original storage for each one.
  if (Image.MachOFunctionStarts.empty() && !Frame->Dwarf)
    return std::nullopt;
  const auto Next = llvm::upper_bound(Image.MachOFunctionStarts, VA);
  va_t Owner = Frame->CodeRange.Begin;
  if (Frame->Compact &&
      (Frame->Compact->NativeEncoding & macho_unwind::kIsNotFunctionStart)) {
    if (Next == Image.MachOFunctionStarts.begin())
      return std::nullopt;
    Owner = *std::prev(Next);
  } else if (Next != Image.MachOFunctionStarts.begin()) {
    Owner = std::max(Owner, *std::prev(Next));
  }
  if (Owner >= VA || Image.getSectionFor(Owner) != Image.getSectionFor(VA) ||
      Image.getSegmentFor(Owner) != Segment)
    return std::nullopt;
  return Owner;
}

std::optional<va_t> machOInteriorAddressOwner(const BinaryImage &Image,
                                              va_t VA) {
  constexpr auto Word = aarch64::kInsnSize;
  return VA % Word ? std::nullopt : interiorOwner(Image, VA, Word);
}

std::optional<va_t> machOInteriorByteOwner(const BinaryImage &Image, va_t VA) {
  return interiorOwner(Image, VA, sizeof(uint8_t));
}

std::optional<va_t> machORebasedLiteralAddress(const BinaryImage &Image,
                                               uint32_t Word, va_t PC) {
  if (!aarch64::isPointerLiteralLoad(Word))
    return std::nullopt;
  const va_t Slot = aarch64::decodeLiteralLoad(Word, PC)->Address;
  if (Image.MachOChainedFixupsAmbiguous ||
      !Image.MachOResolvedChainedPointerSlots.contains(Slot) ||
      !Image.readOnlyAfterRelocations(Slot, sizeof(va_t)))
    return std::nullopt;
  const auto *Bytes = Image.readVA(Slot, sizeof(va_t));
  return Bytes ? std::optional<va_t>(readLE<va_t>(Bytes)) : std::nullopt;
}

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
  if (Section && (!(Section->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                                     llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) ||
                  Section->FileSz != Section->Size ||
                  !rangeInBounds(Start - Section->VA, Word, Section->Size)))
    return std::nullopt;
  const auto *First = Image.readVA(Start, Word);
  if (!First ||
      !machODataInCodeRanges(Image.MachODataInCode, Start, Word).empty())
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
    return MachOLiftedSource{{First, Word}, ForwardTarget, {}};
  const auto *Bytes = Image.readVA(Start, End - Start);
  if (!Bytes)
    return std::nullopt;
  const auto Embedded =
      machODataInCodeRanges(Image.MachODataInCode, Start, End - Start);
  if (!Embedded.empty() &&
      (Embedded.front().Begin < Start || Embedded.back().End > End))
    return std::nullopt;
  return MachOLiftedSource{
      {Bytes, size_t(End - Start)}, std::nullopt, Embedded};
}

} // namespace neverd
