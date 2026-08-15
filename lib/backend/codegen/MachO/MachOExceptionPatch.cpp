//===- MachOExceptionPatch.cpp - Mach-O unwind-record rewrite ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"

#include "MachOStrictLayout.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-macho-patch"

namespace neverd {

using namespace llvm::MachO;

namespace {

using macho_patch_detail::checkedAdd;
using macho_patch_detail::collectMachOFileRanges;
using macho_patch_detail::MachOFileRange;
using macho_patch_detail::validateLoadCommandRegion;

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "macho exception patch: " + Message);
}

bool sameRegion(const MachOEHFrameRegion &Left,
                const MachOEHFrameRegion &Right) {
  return Left.Is64 == Right.Is64 && Left.SectionVA == Right.SectionVA &&
         Left.SectionFileOff == Right.SectionFileOff &&
         Left.AppendVA == Right.AppendVA &&
         Left.AppendFileOff == Right.AppendFileOff &&
         Left.LimitFileOff == Right.LimitFileOff &&
         Left.SectionHeaderOff == Right.SectionHeaderOff;
}

bool sameRegionIdentity(const MachOEHFrameRegion &Left,
                        const MachOEHFrameRegion &Right) {
  return Left.Is64 == Right.Is64 && Left.SectionVA == Right.SectionVA &&
         Left.SectionFileOff == Right.SectionFileOff &&
         Left.LimitFileOff == Right.LimitFileOff &&
         Left.SectionHeaderOff == Right.SectionHeaderOff;
}

bool sameFDE(const DwarfEHFrameRecord &Left, const DwarfEHFrameRecord &Right) {
  return Left.BeginVA == Right.BeginVA && Left.EndVA == Right.EndVA &&
         Left.RecordVA == Right.RecordVA;
}

bool sameFDEs(llvm::ArrayRef<DwarfEHFrameRecord> Left,
              llvm::ArrayRef<DwarfEHFrameRecord> Right) {
  return Left.size() == Right.size() &&
         std::equal(Left.begin(), Left.end(), Right.begin(), sameFDE);
}

bool isDwarfCompactEncoding(Arch TargetArch, uint32_t Encoding) {
  const uint32_t Mode = Encoding & macho_unwind::kModeMask;
  switch (TargetArch) {
  case Arch::X64:
  case Arch::X86:
    return Mode == macho_unwind::kX86_64ModeDwarf;
  case Arch::AArch64:
    return Mode == macho_unwind::kARM64ModeDwarf;
  case Arch::ARM:
    return Mode == macho_unwind::kARMModeDwarf;
  default:
    return false;
  }
}

uint8_t pointerWidthForArch(Arch TargetArch) {
  switch (TargetArch) {
  case Arch::X64:
  case Arch::AArch64:
    return 8;
  case Arch::X86:
  case Arch::ARM:
    return 4;
  default:
    return 0;
  }
}

using AuthenticatedCompactCoverage =
    std::map<uint64_t, const MachOCompactUnwindRecord *>;

llvm::Expected<AuthenticatedCompactCoverage>
authenticateCompactCoverage(const MachOCompactUnwindRecords *Coverage,
                            const CompiledImage &Compiled) {
  AuthenticatedCompactCoverage Result;
  if (!Coverage)
    return Result;

  if (Compiled.Format != BinaryFormat::MachO ||
      (Compiled.TargetArch != Arch::X64 && Compiled.TargetArch != Arch::X86 &&
       Compiled.TargetArch != Arch::ARM &&
       Compiled.TargetArch != Arch::AArch64) ||
      Compiled.PointerWidth != pointerWidthForArch(Compiled.TargetArch) ||
      Compiled.ByteOrder != llvm::endianness::little ||
      Coverage->TargetArch != Compiled.TargetArch ||
      Coverage->PointerWidth != Compiled.PointerWidth ||
      Coverage->ByteOrder != Compiled.ByteOrder)
    return patchError("compact coverage target metadata does not match the "
                      "compiled image");

  std::map<uint64_t, const llvm::mc_rewrite::RewriteFunctionRange *> Ranges;
  for (const llvm::mc_rewrite::RewriteFunctionRange &Range :
       Compiled.FunctionRanges)
    Ranges.emplace(Range.Id, &Range);

  for (const MachOCompactUnwindRecord &Record : Coverage->Records) {
    if (llvm::Error Error = validateGeneratedMachOCompactUnwindRecordEncoding(
            Coverage->TargetArch, Record))
      return std::move(Error);
    if (Record.FunctionRangeId == 0 ||
        !Result.emplace(Record.FunctionRangeId, &Record).second)
      return patchError("compact coverage has a missing or duplicate function "
                        "range identity");
    const auto RangeIt = Ranges.find(Record.FunctionRangeId);
    if (RangeIt == Ranges.end())
      return patchError("compact coverage has a dangling function range "
                        "identity");
    const llvm::mc_rewrite::RewriteFunctionRange &Range = *RangeIt->second;
    const auto Owner = Compiled.FunctionOwnerAddrs.find(Record.OwnerSymbol);
    if (Record.FunctionVA != Range.BeginVA ||
        Record.FunctionEndVA != Range.EndVA ||
        Range.EndVA - Range.BeginVA != Record.RangeLength ||
        Record.FunctionSymbol != Range.BeginSymbol ||
        Record.OwnerSymbol != Range.OwnerSymbol ||
        Record.OwnerVA != Range.OwnerVA ||
        Owner == Compiled.FunctionOwnerAddrs.end() ||
        Owner->second != Record.OwnerVA)
      return patchError("compact coverage does not exactly match compiler "
                        "function-range provenance");
  }
  return Result;
}

bool hasStandaloneCompactCoverage(
    const AuthenticatedCompactCoverage &Coverage,
    const llvm::mc_rewrite::RewriteFunctionRange &Range, Arch TargetArch) {
  const auto It = Coverage.find(Range.Id);
  return It != Coverage.end() &&
         !isDwarfCompactEncoding(TargetArch, It->second->Encoding);
}

std::optional<uint64_t>
getDeclaredEHFrameSize(llvm::ArrayRef<uint8_t> Binary,
                       const MachOEHFrameRegion &Region) {
  if (Region.Is64) {
    if (!rangeInBounds(Region.SectionHeaderOff, sizeof(section_64),
                       Binary.size()))
      return std::nullopt;
    return reinterpret_cast<const section_64 *>(Binary.data() +
                                                Region.SectionHeaderOff)
        ->size;
  }
  if (!rangeInBounds(Region.SectionHeaderOff, sizeof(section), Binary.size()))
    return std::nullopt;
  return reinterpret_cast<const section *>(Binary.data() +
                                           Region.SectionHeaderOff)
      ->size;
}

/// Return the byte offset at which another .eh_frame sequence can be appended.
/// A zero-length record is a terminator, so it is replaced rather than left
/// between the original and regenerated records.
std::optional<uint64_t> getEHFrameAppendOffset(llvm::ArrayRef<uint8_t> Bytes) {
  uint64_t Off = 0;
  while (Off < Bytes.size()) {
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    uint32_t Length = readLE<uint32_t>(Bytes.data() + Off);
    if (Length == 0) {
      for (uint8_t Byte : Bytes.drop_front(Off))
        if (Byte != 0)
          return std::nullopt;
      return Off;
    }

    uint64_t RecordSize = 0;
    if (Length == std::numeric_limits<uint32_t>::max()) {
      if (!rangeInBounds(Off, sizeof(uint32_t) + sizeof(uint64_t),
                         Bytes.size()))
        return std::nullopt;
      uint64_t ExtendedLength =
          readLE<uint64_t>(Bytes.data() + Off + sizeof(uint32_t));
      if (ExtendedLength > std::numeric_limits<uint64_t>::max() -
                               (sizeof(uint32_t) + sizeof(uint64_t)))
        return std::nullopt;
      RecordSize = sizeof(uint32_t) + sizeof(uint64_t) + ExtendedLength;
    } else {
      RecordSize = sizeof(uint32_t) + static_cast<uint64_t>(Length);
    }
    if (RecordSize == 0 || !rangeInBounds(Off, RecordSize, Bytes.size()))
      return std::nullopt;
    Off += RecordSize;
  }
  return Off;
}

bool appendGeneratedEHFrame(std::vector<uint8_t> &Binary,
                            const MachOEHFrameRegion &Region,
                            const CompiledSection &Generated) {
  const std::optional<MachOEHFrameRegion> Current =
      findMachOEHFrameRegion(Binary);
  if (!Current || !sameRegion(*Current, Region))
    return false;

  if (Generated.Name != section_names::macho::EhFrame || Generated.IsInImage ||
      Generated.VA != Region.AppendVA ||
      Generated.Size != Generated.ExternalBytes.size() ||
      Region.LimitFileOff < Region.AppendFileOff ||
      Region.AppendFileOff < Region.SectionFileOff ||
      Region.AppendVA < Region.SectionVA ||
      Region.AppendFileOff - Region.SectionFileOff !=
          Region.AppendVA - Region.SectionVA ||
      Generated.Size > Region.LimitFileOff - Region.AppendFileOff ||
      !rangeInBounds(Region.AppendFileOff, Generated.Size, Binary.size()))
    return false;

  if (Generated.ExternalBytes.size() > std::numeric_limits<uint64_t>::max() -
                                           (Region.AppendVA - Region.SectionVA))
    return false;
  uint64_t NewSize =
      Region.AppendVA - Region.SectionVA + Generated.ExternalBytes.size();
  if (Region.Is64) {
    if (!rangeInBounds(Region.SectionHeaderOff, sizeof(section_64),
                       Binary.size()))
      return false;
    const auto *Section = reinterpret_cast<const section_64 *>(
        Binary.data() + Region.SectionHeaderOff);
    if (readMachOName(Section->sectname) != section_names::macho::EhFrame ||
        readMachOName(Section->segname) != section_names::macho::TextSeg ||
        Section->addr != Region.SectionVA ||
        Section->offset != Region.SectionFileOff ||
        Section->size < Region.AppendVA - Region.SectionVA ||
        !rangeInBounds(Section->offset, Section->size, Binary.size()))
      return false;
    const auto LogicalAppend = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
        Binary.data() + Section->offset, static_cast<size_t>(Section->size)));
    if (!LogicalAppend || *LogicalAppend != Region.AppendVA - Region.SectionVA)
      return false;
  } else {
    if (NewSize > std::numeric_limits<uint32_t>::max() ||
        !rangeInBounds(Region.SectionHeaderOff, sizeof(section), Binary.size()))
      return false;
    const auto *Section = reinterpret_cast<const section *>(
        Binary.data() + Region.SectionHeaderOff);
    if (readMachOName(Section->sectname) != section_names::macho::EhFrame ||
        readMachOName(Section->segname) != section_names::macho::TextSeg ||
        Section->addr != Region.SectionVA ||
        Section->offset != Region.SectionFileOff ||
        Section->size < Region.AppendVA - Region.SectionVA ||
        !rangeInBounds(Section->offset, Section->size, Binary.size()))
      return false;
    const auto LogicalAppend = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
        Binary.data() + Section->offset, static_cast<size_t>(Section->size)));
    if (!LogicalAppend || *LogicalAppend != Region.AppendVA - Region.SectionVA)
      return false;
  }

  // Commit only after both the bytes and the header update are known valid.
  if (!Generated.ExternalBytes.empty())
    std::memcpy(Binary.data() + Region.AppendFileOff,
                Generated.ExternalBytes.data(), Generated.ExternalBytes.size());
  if (Region.Is64) {
    reinterpret_cast<section_64 *>(Binary.data() + Region.SectionHeaderOff)
        ->size = NewSize;
  } else {
    reinterpret_cast<section *>(Binary.data() + Region.SectionHeaderOff)->size =
        static_cast<uint32_t>(NewSize);
  }
  return true;
}

llvm::Error
validateCombinedFDERanges(llvm::ArrayRef<DwarfEHFrameRecord> Existing,
                          llvm::ArrayRef<DwarfEHFrameRecord> Generated) {
  if (Generated.size() > std::numeric_limits<size_t>::max() - Existing.size())
    return patchError("combined FDE count overflows");
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  if (Existing.size() + Generated.size() > Ranges.max_size())
    return patchError("combined FDE ranges cannot be represented");
  Ranges.reserve(Existing.size() + Generated.size());
  for (const DwarfEHFrameRecord &Record : Existing)
    Ranges.emplace_back(Record.BeginVA, Record.EndVA);
  for (const DwarfEHFrameRecord &Record : Generated)
    Ranges.emplace_back(Record.BeginVA, Record.EndVA);
  std::sort(Ranges.begin(), Ranges.end());
  for (size_t I = 1; I < Ranges.size(); ++I)
    if (Ranges[I].first < Ranges[I - 1].second)
      return patchError("input and regenerated FDE address ranges overlap");
  return llvm::Error::success();
}

} // namespace

std::optional<MachOEHFrameRegion>
findMachOEHFrameRegion(llvm::ArrayRef<uint8_t> Binary) {
  MachOHeaderInfo Header;
  if (!validateLoadCommandRegion(Binary, Header))
    return std::nullopt;
  std::vector<MachOFileRange> SegmentRanges;
  std::vector<MachOFileRange> SectionRanges;
  if (!collectMachOFileRanges(Binary, SegmentRanges, SectionRanges))
    return std::nullopt;
  std::optional<MachOEHFrameRegion> Found;
  unsigned EHFrameSectionCount = 0;
  bool Ambiguous = false;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        MachOSegFields Seg = readMachOSegment(LCPtr, Is64);
        const bool IsTextSegment =
            readMachOName(Seg.SegName) == section_names::macho::TextSeg;
        const bool SegmentIsMapped =
            rangeInBounds(Seg.FileOff, Seg.FileSize, Binary.size()) &&
            Seg.VMSize >= Seg.FileSize &&
            Seg.VMAddr <= std::numeric_limits<uint64_t>::max() - Seg.VMSize;

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Nsects =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (Nsects > (std::numeric_limits<uint32_t>::max() - BaseSize) /
                         SectionSize ||
            BaseSize + Nsects * SectionSize != CmdSize)
          return;

        uint64_t EHVA = 0;
        uint64_t EHSize = 0;
        uint64_t EHFileOff = 0;
        uint64_t HeaderOff = 0;
        std::vector<uint64_t> SectionOffsets;
        for (uint32_t I = 0; I < Nsects; ++I) {
          const uint8_t *SectionPtr = LCPtr + BaseSize + I * SectionSize;
          uint64_t Addr = 0;
          uint64_t Size = 0;
          uint32_t FileOff = 0;
          const char *Name = nullptr;
          if (Is64) {
            const auto *Section =
                reinterpret_cast<const section_64 *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          } else {
            const auto *Section = reinterpret_cast<const section *>(SectionPtr);
            Addr = Section->addr;
            Size = Section->size;
            FileOff = Section->offset;
            Name = Section->sectname;
          }
          if (FileOff != 0)
            SectionOffsets.push_back(FileOff);
          if (readMachOName(Name) != section_names::macho::EhFrame)
            continue;
          ++EHFrameSectionCount;
          if (EHFrameSectionCount != 1 || !IsTextSegment || !SegmentIsMapped) {
            Ambiguous = true;
            return;
          }
          const char *SectionSegName =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr)->segname
                   : reinterpret_cast<const section *>(SectionPtr)->segname;
          if (readMachOName(SectionSegName) != section_names::macho::TextSeg ||
              FileOff < Seg.FileOff)
            return;
          const uint64_t Delta = FileOff - Seg.FileOff;
          if (Delta > Seg.FileSize || Size > Seg.FileSize - Delta ||
              Delta > std::numeric_limits<uint64_t>::max() - Seg.VMAddr ||
              Seg.VMAddr + Delta != Addr || Delta > Seg.VMSize ||
              Size > Seg.VMSize - Delta)
            return;
          EHVA = Addr;
          EHSize = Size;
          EHFileOff = FileOff;
          HeaderOff = static_cast<uint64_t>(SectionPtr - Binary.data());
        }
        if (!IsTextSegment)
          return;
        if (EHVA == 0 || EHSize == 0 || EHFileOff == 0 ||
            !rangeInBounds(EHFileOff, EHSize, Binary.size()))
          return;

        auto LogicalSize = getEHFrameAppendOffset(llvm::ArrayRef<uint8_t>(
            Binary.data() + EHFileOff, static_cast<size_t>(EHSize)));
        if (!LogicalSize || *LogicalSize > EHSize ||
            EHVA > std::numeric_limits<uint64_t>::max() - *LogicalSize ||
            EHFileOff > std::numeric_limits<uint64_t>::max() - *LogicalSize)
          return;

        uint64_t AppendFileOff = EHFileOff + *LogicalSize;
        uint64_t Limit = Seg.FileOff + Seg.FileSize;
        for (uint64_t Offset : SectionOffsets)
          if (Offset > AppendFileOff)
            Limit = std::min(Limit, Offset);
        if (Limit < AppendFileOff || Limit > Binary.size())
          return;

        MachOEHFrameRegion Region;
        Region.Is64 = Is64;
        Region.SectionVA = EHVA;
        Region.SectionFileOff = EHFileOff;
        Region.AppendVA = EHVA + *LogicalSize;
        Region.AppendFileOff = AppendFileOff;
        Region.LimitFileOff = Limit;
        Region.SectionHeaderOff = HeaderOff;
        if (Found) {
          Ambiguous = true;
          return;
        }
        Found = Region;
      });
  if (Ambiguous || EHFrameSectionCount != 1)
    return std::nullopt;
  if (Found)
    for (const MachOFileRange &Range : SectionRanges)
      if (Range.HeaderOff != Found->SectionHeaderOff &&
          Found->AppendFileOff < Range.End && Range.Begin < Found->LimitFileOff)
        return std::nullopt;
  return Found;
}

bool requiresRegisteredMachOEHFrame(const llvm::Module &Mod) {
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements) {
    llvm::consumeError(Requirements.takeError());
    return true;
  }
  return Requirements->RequiresRegisteredUnwind;
}

llvm::Expected<MachOEHFrameInstallReceipt> installMachOEHFrameWithReceipt(
    std::vector<uint8_t> &Binary,
    const std::optional<MachOEHFrameRegion> &Region,
    const CompiledImage &Compiled, const llvm::Module &Mod,
    const MachOCompactUnwindRecords *CompactCoverage) {
  if (!Compiled.Success || Compiled.Format != BinaryFormat::MachO ||
      (Compiled.TargetArch != Arch::X64 && Compiled.TargetArch != Arch::X86 &&
       Compiled.TargetArch != Arch::AArch64 &&
       Compiled.TargetArch != Arch::ARM) ||
      Compiled.PointerWidth != pointerWidthForArch(Compiled.TargetArch) ||
      Compiled.ByteOrder != llvm::endianness::little)
    return patchError("compiled target metadata is invalid for Mach-O unwind "
                      "installation");
  MachOEHFrameInstallReceipt Receipt(Compiled.TargetArch, Compiled.PointerWidth,
                                     Compiled.ByteOrder);
  if (!Compiled.FunctionRangesValid ||
      !llvm::mc_rewrite::validateRewriteFunctionRanges(
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return patchError("compiled function-range provenance is invalid");
  auto Compact = authenticateCompactCoverage(CompactCoverage, Compiled);
  if (!Compact)
    return Compact.takeError();
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements)
    return Requirements.takeError();
  const bool Required = Requirements->RequiresRegisteredUnwind;
  if (Required && !Compiled.Unresolved.empty())
    return patchError("required unwind output has unresolved symbols");
  auto RequiredFunctions =
      exception_rewrite::resolveRequiredFunctionOwners(*Requirements, Compiled);
  if (!RequiredFunctions)
    return RequiredFunctions.takeError();

  std::vector<const llvm::mc_rewrite::RewriteFunctionRange *> RequiredFDERanges;
  if (Required) {
    for (const exception_rewrite::ResolvedFunctionOwner &Owner :
         *RequiredFunctions) {
      bool HasRange = false;
      for (const llvm::mc_rewrite::RewriteFunctionRange &Range :
           Compiled.FunctionRanges) {
        if (Range.OwnerSymbol != Owner.OwnerSymbol ||
            Range.OwnerVA != Owner.OwnerVA)
          continue;
        HasRange = true;
        if (!hasStandaloneCompactCoverage(*Compact, Range, Compiled.TargetArch))
          RequiredFDERanges.push_back(&Range);
      }
      if (!HasRange)
        return patchError("required function has no authenticated unwind "
                          "fragment");
    }
  }
  const bool MustInstall = !RequiredFDERanges.empty();

  auto OmitOrFail = [&](const llvm::Twine &Message)
      -> llvm::Expected<MachOEHFrameInstallReceipt> {
    if (MustInstall)
      return patchError(Message);
    return Receipt;
  };

  const CompiledSection *Generated = nullptr;
  for (const CompiledSection &Section : Compiled.Sections)
    if (Section.IsAllocated && Section.Name == section_names::macho::EhFrame) {
      if (Generated)
        return patchError("multiple regenerated __eh_frame sections");
      Generated = &Section;
    }

  // When the input has no __eh_frame section, the compiler leaves the
  // generated section in CompiledImage::Bytes as before.  Only externally
  // placed bytes need to be appended to an existing section.
  if (!Generated || Generated->IsInImage) {
    return OmitOrFail("no registrable __eh_frame produced");
  }

  if (Generated->ExternalBytes.empty())
    return OmitOrFail("regenerated __eh_frame is empty");

  if (!Region)
    return OmitOrFail("the image declares no __eh_frame to register in");

  const std::optional<MachOEHFrameRegion> Current =
      findMachOEHFrameRegion(Binary);
  if (!Current || !sameRegion(*Current, *Region)) {
    return OmitOrFail("the public __eh_frame region is not the image's exact "
                      "current layout");
  }

  auto Records = decodeDwarfEHFrameRecords(Generated->ExternalBytes,
                                           Generated->VA, Current->Is64);
  if (!Records) {
    if (MustInstall)
      return Records.takeError();
    llvm::consumeError(Records.takeError());
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: malformed optional __eh_frame; "
                  "leaving the image unchanged\n");
    return Receipt;
  }
  const uint64_t ExistingSize =
      Current->AppendFileOff - Current->SectionFileOff;
  auto ExistingRecords = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Binary.data() +
                                  static_cast<size_t>(Current->SectionFileOff),
                              static_cast<size_t>(ExistingSize)),
      Current->SectionVA, Current->Is64);
  if (!ExistingRecords) {
    if (MustInstall)
      return ExistingRecords.takeError();
    llvm::consumeError(ExistingRecords.takeError());
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: malformed input __eh_frame; "
                  "leaving the image unchanged\n");
    return Receipt;
  }

  for (const DwarfEHFrameRecord &Record : *Records) {
    const size_t ExactRanges = std::count_if(
        Compiled.FunctionRanges.begin(), Compiled.FunctionRanges.end(),
        [&](const llvm::mc_rewrite::RewriteFunctionRange &Range) {
          return Range.BeginVA == Record.BeginVA && Range.EndVA == Record.EndVA;
        });
    if (ExactRanges != 1)
      return patchError("regenerated __eh_frame contains an FDE without one "
                        "exact compiler range identity");
  }

  if (llvm::Error Err = validateCombinedFDERanges(*ExistingRecords, *Records)) {
    if (MustInstall)
      return Err;
    llvm::consumeError(std::move(Err));
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: overlapping optional FDE ranges; "
                  "leaving the image unchanged\n");
    return Receipt;
  }

  for (const llvm::mc_rewrite::RewriteFunctionRange *Range :
       RequiredFDERanges) {
    const size_t ExactMatches =
        std::count_if(Records->begin(), Records->end(),
                      [&](const DwarfEHFrameRecord &Record) {
                        return Record.BeginVA == Range->BeginVA &&
                               Record.EndVA == Range->EndVA;
                      });
    if (ExactMatches != 1)
      return patchError("regenerated __eh_frame does not contain exactly one "
                        "FDE for every required compiler fragment");
  }

  std::vector<uint8_t> Candidate = Binary;
  const bool Registered =
      appendGeneratedEHFrame(Candidate, *Current, *Generated);
  if (!Registered)
    return OmitOrFail("cannot register regenerated __eh_frame");

  if (!rangeInBounds(Current->AppendFileOff, Generated->ExternalBytes.size(),
                     Candidate.size()) ||
      !std::equal(
          Generated->ExternalBytes.begin(), Generated->ExternalBytes.end(),
          Candidate.begin() + static_cast<size_t>(Current->AppendFileOff)))
    return patchError("installed __eh_frame bytes differ from compiler output");

  auto InstalledRecords =
      decodeDwarfEHFrameRecords(llvm::ArrayRef<uint8_t>(Candidate).slice(
                                    static_cast<size_t>(Current->AppendFileOff),
                                    Generated->ExternalBytes.size()),
                                Generated->VA, Current->Is64);
  if (!InstalledRecords)
    return InstalledRecords.takeError();
  if (!sameFDEs(*InstalledRecords, *Records))
    return patchError("installed __eh_frame FDEs differ from validated output");
  for (const llvm::mc_rewrite::RewriteFunctionRange *Range : RequiredFDERanges)
    if (std::count_if(InstalledRecords->begin(), InstalledRecords->end(),
                      [&](const DwarfEHFrameRecord &Record) {
                        return Record.BeginVA == Range->BeginVA &&
                               Record.EndVA == Range->EndVA;
                      }) != 1)
      return patchError("installed __eh_frame lost an authenticated required "
                        "fragment");

  const std::optional<uint64_t> GeneratedLogicalSize =
      getEHFrameAppendOffset(Generated->ExternalBytes);
  if (!GeneratedLogicalSize)
    return patchError("installed __eh_frame has no exact logical end");

  const std::optional<MachOEHFrameRegion> Updated =
      findMachOEHFrameRegion(Candidate);
  if (!Updated || !sameRegionIdentity(*Updated, *Current) ||
      *GeneratedLogicalSize >
          std::numeric_limits<uint64_t>::max() - Current->AppendVA ||
      Updated->AppendVA != Current->AppendVA + *GeneratedLogicalSize ||
      *GeneratedLogicalSize >
          std::numeric_limits<uint64_t>::max() - Current->AppendFileOff ||
      Updated->AppendFileOff != Current->AppendFileOff + *GeneratedLogicalSize)
    return patchError("installed __eh_frame cannot be rediscovered exactly");

  const std::optional<uint64_t> DeclaredSize =
      getDeclaredEHFrameSize(Candidate, *Updated);
  const uint64_t OriginalLogicalSize =
      Current->AppendFileOff - Current->SectionFileOff;
  if (!DeclaredSize ||
      Generated->ExternalBytes.size() >
          std::numeric_limits<uint64_t>::max() - OriginalLogicalSize ||
      *DeclaredSize != OriginalLogicalSize + Generated->ExternalBytes.size() ||
      !rangeInBounds(Updated->SectionFileOff, *DeclaredSize, Candidate.size()))
    return patchError("installed __eh_frame section size is inconsistent");

  auto CombinedRecords = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Candidate).slice(
          static_cast<size_t>(Updated->SectionFileOff),
          static_cast<size_t>(*DeclaredSize)),
      Updated->SectionVA, Updated->Is64);
  if (!CombinedRecords)
    return CombinedRecords.takeError();
  std::vector<DwarfEHFrameRecord> ExpectedCombined = *ExistingRecords;
  ExpectedCombined.insert(ExpectedCombined.end(), Records->begin(),
                          Records->end());
  if (!sameFDEs(*CombinedRecords, ExpectedCombined))
    return patchError("combined installed __eh_frame failed semantic replay");

  Receipt.Disposition = MachOEHFrameInstallDisposition::Installed;
  Receipt.Region = *Updated;
  Receipt.InstalledFileOff = Current->AppendFileOff;
  Receipt.InstalledBytes = Generated->ExternalBytes;
  Receipt.InstalledFDEs = std::move(*InstalledRecords);
  Receipt.InstalledSymbolAddrs = Compiled.SymbolAddrs;
  Receipt.AuthenticatedFunctionOwnerAddrs = Compiled.FunctionOwnerAddrs;
  Receipt.AuthenticatedFunctionRanges = Compiled.FunctionRanges;
  Binary.swap(Candidate);

  return Receipt;
}

llvm::Error installMachOEHFrame(std::vector<uint8_t> &Binary,
                                const std::optional<MachOEHFrameRegion> &Region,
                                const CompiledImage &Compiled,
                                const llvm::Module &Mod) {
  auto Receipt =
      installMachOEHFrameWithReceipt(Binary, Region, Compiled, Mod, nullptr);
  if (!Receipt)
    return Receipt.takeError();
  return llvm::Error::success();
}

} // namespace neverd
