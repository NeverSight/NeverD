//===- MachOExceptionPatch.cpp - Mach-O unwind-record rewrite ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"
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
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-macho-patch"

namespace neverd {

using namespace llvm::MachO;

namespace {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "macho exception patch: " + Message);
}

bool checkedAdd(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
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

bool validateLoadCommandRegion(llvm::ArrayRef<uint8_t> Binary,
                               MachOHeaderInfo &Header) {
  Header = parseMachOHeader(Binary.data(), Binary.size());
  if (Header.HeaderSize == 0 || Header.NCmds == 0 ||
      Header.SizeOfCmds >
          std::numeric_limits<uint64_t>::max() - Header.HeaderSize)
    return false;
  const uint64_t End = Header.HeaderSize + Header.SizeOfCmds;
  if (End > Binary.size())
    return false;

  uint64_t Cursor = Header.HeaderSize;
  for (uint32_t I = 0; I < Header.NCmds; ++I) {
    if (!rangeInBounds(Cursor, sizeof(load_command), End))
      return false;
    const auto *Command = reinterpret_cast<const load_command *>(
        Binary.data() + static_cast<size_t>(Cursor));
    if (Command->cmdsize < sizeof(load_command) ||
        !rangeInBounds(Cursor, Command->cmdsize, End) ||
        (Command->cmdsize % (Header.Is64 ? 8u : 4u)) != 0)
      return false;
    if (Command->cmd == getMachOSegmentCmdID(Header.Is64)) {
      const uint32_t BaseSize = getMachOSegmentCmdSize(Header.Is64);
      const uint32_t SectionSize = getMachOSectionSize(Header.Is64);
      if (Command->cmdsize < BaseSize)
        return false;
      const uint32_t Nsects =
          Header.Is64
              ? reinterpret_cast<const segment_command_64 *>(Command)->nsects
              : reinterpret_cast<const segment_command *>(Command)->nsects;
      if (Nsects >
              (std::numeric_limits<uint32_t>::max() - BaseSize) / SectionSize ||
          BaseSize + Nsects * SectionSize != Command->cmdsize)
        return false;
    }
    Cursor += Command->cmdsize;
  }
  return Cursor == End;
}

struct MachOFileRange {
  uint64_t Begin = 0;
  uint64_t End = 0;
  uint64_t HeaderOff = 0;
};

bool collectMachOFileRanges(llvm::ArrayRef<uint8_t> Binary,
                            std::vector<MachOFileRange> &Segments,
                            std::vector<MachOFileRange> &Sections) {
  bool Valid = true;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (!Valid || Cmd != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(LCPtr, Is64);
        uint64_t SegmentFileEnd = 0;
        uint64_t SegmentVMEnd = 0;
        if (!checkedAdd(Segment.FileOff, Segment.FileSize, SegmentFileEnd) ||
            !checkedAdd(Segment.VMAddr, Segment.VMSize, SegmentVMEnd) ||
            Segment.FileSize > Segment.VMSize ||
            !rangeInBounds(Segment.FileOff, Segment.FileSize, Binary.size())) {
          Valid = false;
          return;
        }
        if (Segment.FileSize != 0)
          Segments.push_back({Segment.FileOff, SegmentFileEnd,
                              static_cast<uint64_t>(LCPtr - Binary.data())});

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Count =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (BaseSize + uint64_t(Count) * SectionSize != CmdSize) {
          Valid = false;
          return;
        }
        for (uint32_t I = 0; I < Count; ++I) {
          const uint8_t *SectionPtr =
              LCPtr + BaseSize + uint64_t(I) * SectionSize;
          const uint64_t Address =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr)->addr
                   : reinterpret_cast<const section *>(SectionPtr)->addr;
          const uint64_t Size =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr)->size
                   : reinterpret_cast<const section *>(SectionPtr)->size;
          const uint32_t FileOff =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr)->offset
                   : reinterpret_cast<const section *>(SectionPtr)->offset;
          const uint32_t Flags =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr)->flags
                   : reinterpret_cast<const section *>(SectionPtr)->flags;
          if (Size == 0 ||
              isVirtualSection(static_cast<uint8_t>(Flags & SECTION_TYPE)))
            continue;
          uint64_t End = 0;
          if (!checkedAdd(FileOff, Size, End) || FileOff < Segment.FileOff ||
              End > SegmentFileEnd ||
              !rangeInBounds(FileOff, Size, Binary.size())) {
            Valid = false;
            return;
          }
          const uint64_t Delta = FileOff - Segment.FileOff;
          if (Delta > Segment.VMSize || Size > Segment.VMSize - Delta ||
              Delta > std::numeric_limits<uint64_t>::max() - Segment.VMAddr ||
              Segment.VMAddr + Delta != Address) {
            Valid = false;
            return;
          }
          Sections.push_back(
              {FileOff, End,
               static_cast<uint64_t>(SectionPtr - Binary.data())});
        }
      });
  if (!Valid)
    return false;

  auto ByBegin = [](const MachOFileRange &Left, const MachOFileRange &Right) {
    return std::tie(Left.Begin, Left.End, Left.HeaderOff) <
           std::tie(Right.Begin, Right.End, Right.HeaderOff);
  };
  std::sort(Segments.begin(), Segments.end(), ByBegin);
  std::sort(Sections.begin(), Sections.end(), ByBegin);
  auto HasOverlap = [](const std::vector<MachOFileRange> &Ranges) {
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].Begin < Ranges[I - 1].End)
        return true;
    return false;
  };
  return !HasOverlap(Segments) && !HasOverlap(Sections);
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

llvm::Error installMachOEHFrame(std::vector<uint8_t> &Binary,
                                const std::optional<MachOEHFrameRegion> &Region,
                                const CompiledImage &Compiled,
                                const llvm::Module &Mod) {
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements)
    return Requirements.takeError();
  const bool Required = Requirements->RequiresRegisteredUnwind;
  if (Required && !Compiled.Unresolved.empty())
    return patchError("required unwind output has unresolved symbols");
  auto RequiredFunctions = exception_rewrite::resolveRequiredFunctionAddresses(
      *Requirements, Compiled);
  if (!RequiredFunctions)
    return RequiredFunctions.takeError();

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
    if (Required)
      return patchError("no registrable __eh_frame produced");
    return llvm::Error::success();
  }

  if (!Region) {
    if (Required)
      return patchError("the image declares no __eh_frame to register in");
    return llvm::Error::success();
  }

  const std::optional<MachOEHFrameRegion> Current =
      findMachOEHFrameRegion(Binary);
  if (!Current || !sameRegion(*Current, *Region)) {
    if (Required)
      return patchError("the public __eh_frame region is not the image's "
                        "exact current layout");
    return llvm::Error::success();
  }

  auto Records = decodeDwarfEHFrameRecords(Generated->ExternalBytes,
                                           Generated->VA, Current->Is64);
  if (!Records) {
    if (Required)
      return Records.takeError();
    llvm::consumeError(Records.takeError());
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: malformed optional __eh_frame; "
                  "leaving the image unchanged\n");
    return llvm::Error::success();
  }
  const uint64_t ExistingSize =
      Current->AppendFileOff - Current->SectionFileOff;
  auto ExistingRecords = decodeDwarfEHFrameRecords(
      llvm::ArrayRef<uint8_t>(Binary.data() +
                                  static_cast<size_t>(Current->SectionFileOff),
                              static_cast<size_t>(ExistingSize)),
      Current->SectionVA, Current->Is64);
  if (!ExistingRecords) {
    if (Required)
      return ExistingRecords.takeError();
    llvm::consumeError(ExistingRecords.takeError());
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: malformed input __eh_frame; "
                  "leaving the image unchanged\n");
    return llvm::Error::success();
  }

  if (llvm::Error Err = validateCombinedFDERanges(*ExistingRecords, *Records)) {
    if (Required)
      return Err;
    llvm::consumeError(std::move(Err));
    LLVM_DEBUG(llvm::dbgs()
               << "macho exception patch: overlapping optional FDE ranges; "
                  "leaving the image unchanged\n");
    return llvm::Error::success();
  }

  if (Required) {
    for (uint64_t Address : *RequiredFunctions)
      if (std::none_of(Records->begin(), Records->end(),
                       [&](const DwarfEHFrameRecord &Record) {
                         return Record.BeginVA == Address &&
                                Record.covers(Address);
                       }))
        return patchError("regenerated __eh_frame does not cover a required "
                          "function");
  }

  const bool Registered = appendGeneratedEHFrame(Binary, *Current, *Generated);
  if (!Registered && Required)
    return patchError("cannot register regenerated __eh_frame");

  LLVM_DEBUG({
    if (!Registered)
      llvm::dbgs() << "macho exception patch: omitting unregistered CFI-only "
                      "__eh_frame records\n";
  });
  return llvm::Error::success();
}

} // namespace neverd
