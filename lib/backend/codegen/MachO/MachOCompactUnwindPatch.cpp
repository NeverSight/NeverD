//===- MachOCompactUnwindPatch.cpp - Generated unwind input --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOCompactUnwindPatch.h"

#include "MachOStrictLayout.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/MachO/MachOExceptionPatch.h"
#include "neverd/loader/MachO/CompactUnwind.h"
#include "neverd/object/MachOLayout.h"
#include "neverd/object/SectionNames.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace neverd {

char MachOCompactUnwindLocateError::ID;
char MachOCompactUnwindParseError::ID;
char MachOCompactUnwindDwarfBindError::ID;
char MachOCompactUnwindRangeMapError::ID;
char MachOCompactUnwindMergeError::ID;
char MachOCompactUnwindEncodeError::ID;
char MachOCompactUnwindInstallError::ID;

MachOCompactUnwindLocateError::MachOCompactUnwindLocateError(
    MachOCompactUnwindLocateFailure Reason, std::string Detail)
    : Reason(Reason), Detail(std::move(Detail)) {}

void MachOCompactUnwindLocateError::log(llvm::raw_ostream &OS) const {
  OS << "Mach-O compact unwind location: ";
  switch (Reason) {
  case MachOCompactUnwindLocateFailure::InvalidLoadCommands:
    OS << "load-command region is malformed";
    break;
  case MachOCompactUnwindLocateFailure::InvalidFileLayout:
    OS << "segment or section file layout is invalid";
    break;
  case MachOCompactUnwindLocateFailure::InvalidHeaderMapping:
    OS << "Mach header has no unique file-to-virtual mapping";
    break;
  case MachOCompactUnwindLocateFailure::AmbiguousSection:
    OS << "final compact-unwind section is ambiguous";
    break;
  case MachOCompactUnwindLocateFailure::InvalidSection:
    OS << "final compact-unwind section is invalid";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindLocateError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

llvm::Expected<std::optional<MachOCompactUnwindRegion>>
findMachOCompactUnwindRegion(llvm::ArrayRef<uint8_t> Binary) {
  using namespace llvm::MachO;
  using macho_patch_detail::MachOFileRange;

  const auto Fail = [](MachOCompactUnwindLocateFailure Reason,
                       llvm::StringRef Detail = {})
      -> llvm::Expected<std::optional<MachOCompactUnwindRegion>> {
    return llvm::make_error<MachOCompactUnwindLocateError>(Reason,
                                                           Detail.str());
  };

  MachOHeaderInfo Header;
  if (!macho_patch_detail::validateLoadCommandRegion(Binary, Header))
    return Fail(MachOCompactUnwindLocateFailure::InvalidLoadCommands);

  std::vector<MachOFileRange> SegmentRanges;
  std::vector<MachOFileRange> SectionRanges;
  if (!macho_patch_detail::collectMachOFileRanges(Binary, SegmentRanges,
                                                  SectionRanges))
    return Fail(MachOCompactUnwindLocateFailure::InvalidFileLayout);

  const uint64_t LoadCommandsEnd = Header.HeaderSize + Header.SizeOfCmds;
  uint64_t MachHeaderVA = 0;
  unsigned HeaderMappingCount = 0;
  unsigned UnwindSectionCount = 0;
  std::optional<MachOCompactUnwindRegion> Found;
  bool InvalidSection = false;

  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *LCPtr, uint32_t Cmd, uint32_t CmdSize, bool Is64) {
        if (Cmd != getMachOSegmentCmdID(Is64))
          return;
        const MachOSegFields Segment = readMachOSegment(LCPtr, Is64);
        if (Segment.FileOff == 0 && Segment.FileSize >= LoadCommandsEnd) {
          ++HeaderMappingCount;
          MachHeaderVA = Segment.VMAddr;
        }

        const uint32_t BaseSize = getMachOSegmentCmdSize(Is64);
        const uint32_t SectionSize = getMachOSectionSize(Is64);
        const uint32_t Count =
            Is64 ? reinterpret_cast<const segment_command_64 *>(LCPtr)->nsects
                 : reinterpret_cast<const segment_command *>(LCPtr)->nsects;
        if (BaseSize + uint64_t(Count) * SectionSize != CmdSize) {
          InvalidSection = true;
          return;
        }
        for (uint32_t I = 0; I < Count; ++I) {
          const uint8_t *SectionPtr =
              LCPtr + BaseSize + uint64_t(I) * SectionSize;
          const auto *Section64 =
              Is64 ? reinterpret_cast<const section_64 *>(SectionPtr) : nullptr;
          const auto *Section32 =
              Is64 ? nullptr : reinterpret_cast<const section *>(SectionPtr);
          const char *Name = Is64 ? Section64->sectname : Section32->sectname;
          if (readMachOName(Name) != section_names::macho::Unwind)
            continue;

          ++UnwindSectionCount;
          if (UnwindSectionCount != 1) {
            Found.reset();
            continue;
          }

          const char *SectionSegmentName =
              Is64 ? Section64->segname : Section32->segname;
          const uint64_t Address = Is64 ? Section64->addr : Section32->addr;
          const uint64_t Size = Is64 ? Section64->size : Section32->size;
          const uint32_t FileOff = Is64 ? Section64->offset : Section32->offset;
          const uint32_t Flags = Is64 ? Section64->flags : Section32->flags;
          uint64_t SegmentFileEnd = 0;
          if (readMachOName(Segment.SegName) != section_names::macho::TextSeg ||
              readMachOName(SectionSegmentName) !=
                  section_names::macho::TextSeg ||
              Size == 0 || FileOff < LoadCommandsEnd ||
              (Flags & SECTION_TYPE) != S_REGULAR ||
              !macho_patch_detail::checkedAdd(Segment.FileOff, Segment.FileSize,
                                              SegmentFileEnd) ||
              FileOff < Segment.FileOff || FileOff > SegmentFileEnd ||
              Size > SegmentFileEnd - FileOff ||
              !rangeInBounds(FileOff, Size, Binary.size())) {
            InvalidSection = true;
            continue;
          }
          const uint64_t Delta = FileOff - Segment.FileOff;
          if (Delta > Segment.VMSize || Size > Segment.VMSize - Delta ||
              Delta > std::numeric_limits<uint64_t>::max() - Segment.VMAddr ||
              Segment.VMAddr + Delta != Address) {
            InvalidSection = true;
            continue;
          }

          MachOCompactUnwindRegion Region;
          Region.Is64 = Is64;
          Region.SectionVA = Address;
          Region.SectionFileOff = FileOff;
          Region.SectionSize = Size;
          Region.LimitFileOff = FileOff + Size;
          Region.SectionHeaderOff =
              static_cast<uint64_t>(SectionPtr - Binary.data());
          Found = Region;
        }
      });

  if (HeaderMappingCount != 1)
    return Fail(MachOCompactUnwindLocateFailure::InvalidHeaderMapping);
  if (UnwindSectionCount > 1)
    return Fail(MachOCompactUnwindLocateFailure::AmbiguousSection);
  if (InvalidSection)
    return Fail(MachOCompactUnwindLocateFailure::InvalidSection);
  if (UnwindSectionCount == 0)
    return std::optional<MachOCompactUnwindRegion>();
  if (!Found)
    return Fail(MachOCompactUnwindLocateFailure::InvalidSection);

  const bool HasExactFileRange =
      llvm::any_of(SectionRanges, [&](const MachOFileRange &Range) {
        return Range.Begin == Found->SectionFileOff &&
               Range.End == Found->SectionFileOff + Found->SectionSize &&
               Range.HeaderOff == Found->SectionHeaderOff;
      });
  if (!HasExactFileRange)
    return Fail(MachOCompactUnwindLocateFailure::InvalidFileLayout);
  Found->MachHeaderVA = MachHeaderVA;
  return Found;
}

MachOCompactUnwindParseError::MachOCompactUnwindParseError(
    MachOCompactUnwindParseFailure Reason, uint64_t RecordIndex,
    std::string Detail)
    : Reason(Reason), RecordIndex(RecordIndex), Detail(std::move(Detail)) {}

void MachOCompactUnwindParseError::log(llvm::raw_ostream &OS) const {
  OS << "generated Mach-O compact unwind";
  if (RecordIndex != NoRecord)
    OS << " record " << RecordIndex;
  OS << ": ";
  switch (Reason) {
  case MachOCompactUnwindParseFailure::InvalidCompiledImage:
    OS << "compiled image is not complete";
    break;
  case MachOCompactUnwindParseFailure::InvalidSourceImage:
    OS << "source image is not Mach-O";
    break;
  case MachOCompactUnwindParseFailure::MissingSection:
    OS << "linker-input section is missing";
    break;
  case MachOCompactUnwindParseFailure::AmbiguousSection:
    OS << "linker-input section is ambiguous";
    break;
  case MachOCompactUnwindParseFailure::InvalidSectionStorage:
    OS << "linker-input section storage is inconsistent";
    break;
  case MachOCompactUnwindParseFailure::UnsupportedPointerWidth:
    OS << "pointer width is unsupported";
    break;
  case MachOCompactUnwindParseFailure::ArchitecturePointerWidthMismatch:
    OS << "architecture and pointer width disagree";
    break;
  case MachOCompactUnwindParseFailure::UnsupportedArchitecture:
    OS << "architecture has no generated-record decoder";
    break;
  case MachOCompactUnwindParseFailure::UnsupportedEndianness:
    OS << "byte order is not explicit";
    break;
  case MachOCompactUnwindParseFailure::InvalidSectionAlignment:
    OS << "linker-input section alignment is invalid";
    break;
  case MachOCompactUnwindParseFailure::SectionTooShort:
    OS << "linker-input section is shorter than one record";
    break;
  case MachOCompactUnwindParseFailure::TrailingBytes:
    OS << "linker-input section has a partial trailing record";
    break;
  case MachOCompactUnwindParseFailure::EmptyRange:
    OS << "function range is empty";
    break;
  case MachOCompactUnwindParseFailure::RangeOverflow:
    OS << "function range overflows";
    break;
  case MachOCompactUnwindParseFailure::FunctionOutsideCode:
    OS << "function range is not in one generated code section";
    break;
  case MachOCompactUnwindParseFailure::LSDAOutsideGeneratedImage:
    OS << "LSDA is not in one allocated generated section";
    break;
  case MachOCompactUnwindParseFailure::UnsupportedEncoding:
    OS << "compact encoding is incompatible with the target";
    break;
  case MachOCompactUnwindParseFailure::EncodingFieldMismatch:
    OS << "encoding flags disagree with pointer fields";
    break;
  case MachOCompactUnwindParseFailure::MissingFixup:
    OS << "symbolic pointer field has no fixup identity";
    break;
  case MachOCompactUnwindParseFailure::AmbiguousFixup:
    OS << "symbolic pointer field has more than one fixup identity";
    break;
  case MachOCompactUnwindParseFailure::InvalidFixup:
    OS << "symbolic fixup shape is unsupported";
    break;
  case MachOCompactUnwindParseFailure::MissingSymbolValue:
    OS << "function fixup symbol has no generated address";
    break;
  case MachOCompactUnwindParseFailure::SymbolValueMismatch:
    OS << "fixup identity and resolved symbol value disagree";
    break;
  case MachOCompactUnwindParseFailure::MissingPersonalitySlot:
    OS << "personality has no input-image pointer slot";
    break;
  case MachOCompactUnwindParseFailure::AmbiguousPersonalitySlot:
    OS << "personality maps to more than one input-image pointer slot";
    break;
  case MachOCompactUnwindParseFailure::InvalidPersonalitySlot:
    OS << "personality pointer slot cannot be encoded as an image RVA";
    break;
  case MachOCompactUnwindParseFailure::OverlappingRanges:
    OS << "normalized function ranges overlap";
    break;
  case MachOCompactUnwindParseFailure::MissingFunctionRangeId:
    OS << "function fixup has no compiler-authenticated range ID";
    break;
  case MachOCompactUnwindParseFailure::DuplicateFunctionRangeId:
    OS << "compiler-authenticated range ID is used more than once";
    break;
  case MachOCompactUnwindParseFailure::DanglingFunctionRangeId:
    OS << "function fixup names no retained compiler range";
    break;
  case MachOCompactUnwindParseFailure::FunctionRangeSymbolMismatch:
    OS << "function fixup does not name the retained private begin label";
    break;
  case MachOCompactUnwindParseFailure::FunctionRangeBoundaryMismatch:
    OS << "compact row and retained compiler range disagree";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindParseError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

MachOCompactUnwindDwarfBindError::MachOCompactUnwindDwarfBindError(
    MachOCompactUnwindDwarfBindFailure Reason, uint64_t RecordIndex,
    std::string Detail)
    : Reason(Reason), RecordIndex(RecordIndex), Detail(std::move(Detail)) {}

void MachOCompactUnwindDwarfBindError::log(llvm::raw_ostream &OS) const {
  OS << "generated Mach-O compact unwind DWARF binding";
  if (RecordIndex != NoRecord)
    OS << " record " << RecordIndex;
  OS << ": ";
  switch (Reason) {
  case MachOCompactUnwindDwarfBindFailure::UnsupportedArchitecture:
    OS << "architecture has no DWARF compact-unwind binding";
    break;
  case MachOCompactUnwindDwarfBindFailure::PrepopulatedDwarfOffset:
    OS << "linker-input DWARF offset is already populated";
    break;
  case MachOCompactUnwindDwarfBindFailure::MissingFDE:
    OS << "no FDE begins at the generated function";
    break;
  case MachOCompactUnwindDwarfBindFailure::AmbiguousFDE:
    OS << "more than one FDE exactly matches the generated range";
    break;
  case MachOCompactUnwindDwarfBindFailure::FDERangeMismatch:
    OS << "FDE start exists but its range is not an exact match";
    break;
  case MachOCompactUnwindDwarfBindFailure::FDERecordAddressUnderflow:
    OS << "FDE record precedes the installed __eh_frame section";
    break;
  case MachOCompactUnwindDwarfBindFailure::ZeroFDESectionOffset:
    OS << "FDE record has the reserved zero section offset";
    break;
  case MachOCompactUnwindDwarfBindFailure::FDESectionOffsetOverflow:
    OS << "FDE section offset exceeds the compact-unwind field";
    break;
  case MachOCompactUnwindDwarfBindFailure::MissingInstallReceipt:
    OS << "no verified __eh_frame installation is available";
    break;
  case MachOCompactUnwindDwarfBindFailure::SymbolIdentityMismatch:
    OS << "installed FDE provenance disagrees with the generated symbol";
    break;
  case MachOCompactUnwindDwarfBindFailure::MissingFunctionRange:
    OS << "installed receipt has no matching compiler range";
    break;
  case MachOCompactUnwindDwarfBindFailure::FunctionRangeIdentityMismatch:
    OS << "installed compiler range disagrees with the compact row";
    break;
  case MachOCompactUnwindDwarfBindFailure::OwnerIdentityMismatch:
    OS << "installed owner symbol and address disagree";
    break;
  case MachOCompactUnwindDwarfBindFailure::ReceiptTargetMismatch:
    OS << "installed receipt target metadata disagrees with compact unwind";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindDwarfBindError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

MachOCompactUnwindRangeMapError::MachOCompactUnwindRangeMapError(
    MachOCompactUnwindRangeMapFailure Reason, uint64_t RecordIndex,
    std::string Detail)
    : Reason(Reason), RecordIndex(RecordIndex), Detail(std::move(Detail)) {}

void MachOCompactUnwindRangeMapError::log(llvm::raw_ostream &OS) const {
  OS << "Mach-O compact unwind range mapping";
  if (RecordIndex != NoRecord)
    OS << " record " << RecordIndex;
  OS << ": ";
  switch (Reason) {
  case MachOCompactUnwindRangeMapFailure::ArchitectureMismatch:
    OS << "generated metadata architecture disagrees with the image";
    break;
  case MachOCompactUnwindRangeMapFailure::MissingSection:
    OS << "the image has no final compact-unwind section";
    break;
  case MachOCompactUnwindRangeMapFailure::MissingTrampoline:
    OS << "no installed trampoline targets the generated range";
    break;
  case MachOCompactUnwindRangeMapFailure::AmbiguousTrampoline:
    OS << "more than one installed trampoline targets the generated range";
    break;
  case MachOCompactUnwindRangeMapFailure::TrampolineSymbolMismatch:
    OS << "installed trampoline symbol does not match generated provenance";
    break;
  case MachOCompactUnwindRangeMapFailure::MissingSourceRange:
    OS << "trampoline source is not an original compact-unwind recipe start";
    break;
  case MachOCompactUnwindRangeMapFailure::AmbiguousSourceRange:
    OS << "trampoline source names more than one original recipe";
    break;
  case MachOCompactUnwindRangeMapFailure::DuplicateSourceRange:
    OS << "more than one generated row consumes the same original recipe";
    break;
  case MachOCompactUnwindRangeMapFailure::InvalidFunctionRangeIdentity:
    OS << "generated fragment provenance is incomplete or duplicated";
    break;
  case MachOCompactUnwindRangeMapFailure::CrossOwnerSourceReuse:
    OS << "different generated owners reuse one original recipe";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindRangeMapError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

MachOCompactUnwindMergeError::MachOCompactUnwindMergeError(
    MachOCompactUnwindMergeFailure Reason,
    MachOCompactUnwindMergeInputKind InputKind, uint64_t InputIndex,
    std::string Detail)
    : Reason(Reason), InputKind(InputKind), InputIndex(InputIndex),
      Detail(std::move(Detail)) {}

void MachOCompactUnwindMergeError::log(llvm::raw_ostream &OS) const {
  OS << "Mach-O compact unwind merge";
  if (InputKind != MachOCompactUnwindMergeInputKind::None) {
    OS << ' ';
    switch (InputKind) {
    case MachOCompactUnwindMergeInputKind::None:
      break;
    case MachOCompactUnwindMergeInputKind::OriginalEncoding:
      OS << "original encoding";
      break;
    case MachOCompactUnwindMergeInputKind::OriginalRecord:
      OS << "original record";
      break;
    case MachOCompactUnwindMergeInputKind::GeneratedRecord:
      OS << "generated record";
      break;
    case MachOCompactUnwindMergeInputKind::RangeMapping:
      OS << "range mapping";
      break;
    case MachOCompactUnwindMergeInputKind::MergedRecord:
      OS << "merged record";
      break;
    }
    if (InputIndex != NoInput)
      OS << ' ' << InputIndex;
  }
  OS << ": ";
  switch (Reason) {
  case MachOCompactUnwindMergeFailure::InvalidOriginalTable:
    OS << "retained original table is not strict parser output";
    break;
  case MachOCompactUnwindMergeFailure::UnsupportedArchitecture:
    OS << "architecture has no compact-unwind merger";
    break;
  case MachOCompactUnwindMergeFailure::ArchitectureMismatch:
    OS << "generated metadata architecture disagrees with the target";
    break;
  case MachOCompactUnwindMergeFailure::UnsupportedEncoding:
    OS << "encoding is incompatible with the target architecture";
    break;
  case MachOCompactUnwindMergeFailure::PersonalityIndexOutOfRange:
    OS << "personality index does not name an original pointer slot";
    break;
  case MachOCompactUnwindMergeFailure::LSDAEncodingMismatch:
    OS << "LSDA flag and LSDA index entry disagree";
    break;
  case MachOCompactUnwindMergeFailure::InvalidGeneratedRecord:
    OS << "generated record violates the normalized parser contract";
    break;
  case MachOCompactUnwindMergeFailure::AddressOutsideImageRVA:
    OS << "address cannot be represented relative to the Mach header";
    break;
  case MachOCompactUnwindMergeFailure::InvalidRangeMapping:
    OS << "range mapping is malformed or uses the wrong mode";
    break;
  case MachOCompactUnwindMergeFailure::SourceRangeNotExact:
    OS << "source range does not exactly match one original recipe";
    break;
  case MachOCompactUnwindMergeFailure::AmbiguousSourceRange:
    OS << "source range is mapped more than once";
    break;
  case MachOCompactUnwindMergeFailure::DestinationRangeNotExact:
    OS << "destination range does not exactly match one generated recipe";
    break;
  case MachOCompactUnwindMergeFailure::AmbiguousDestinationRange:
    OS << "generated destination is mapped more than once";
    break;
  case MachOCompactUnwindMergeFailure::UnmappedGeneratedRange:
    OS << "generated recipe has no exact old-to-new mapping";
    break;
  case MachOCompactUnwindMergeFailure::DuplicateMergedRange:
    OS << "merged recipes have duplicate start addresses";
    break;
  case MachOCompactUnwindMergeFailure::OverlappingMergedRanges:
    OS << "merged recipes overlap";
    break;
  case MachOCompactUnwindMergeFailure::TooManyPersonalities:
    OS << "merged table requires more than three personality slots";
    break;
  case MachOCompactUnwindMergeFailure::ByteOrderMismatch:
    OS << "generated metadata byte order is not supported by the target";
    break;
  case MachOCompactUnwindMergeFailure::MissingDwarfFDEOffset:
    OS << "DWARF fallback has no bound __eh_frame FDE offset";
    break;
  case MachOCompactUnwindMergeFailure::MissingFunctionRangeId:
    OS << "generated provenance has no range ID";
    break;
  case MachOCompactUnwindMergeFailure::DuplicateFunctionRangeId:
    OS << "generated provenance reuses a range ID";
    break;
  case MachOCompactUnwindMergeFailure::DanglingFunctionRangeId:
    OS << "mapping range ID names no generated fragment";
    break;
  case MachOCompactUnwindMergeFailure::FunctionRangeIdentityMismatch:
    OS << "mapping and generated fragment provenance disagree";
    break;
  case MachOCompactUnwindMergeFailure::CrossOwnerSourceReuse:
    OS << "different generated owners reuse one original recipe";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindMergeError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

MachOCompactUnwindEncodeError::MachOCompactUnwindEncodeError(
    MachOCompactUnwindEncodeFailure Reason,
    MachOCompactUnwindEncodeInputKind InputKind, uint64_t InputIndex,
    std::string Detail)
    : Reason(Reason), InputKind(InputKind), InputIndex(InputIndex),
      Detail(std::move(Detail)) {}

void MachOCompactUnwindEncodeError::log(llvm::raw_ostream &OS) const {
  OS << "Mach-O compact unwind encode";
  if (InputKind != MachOCompactUnwindEncodeInputKind::None) {
    OS << ' ';
    switch (InputKind) {
    case MachOCompactUnwindEncodeInputKind::None:
      break;
    case MachOCompactUnwindEncodeInputKind::PersonalitySlot:
      OS << "personality slot";
      break;
    case MachOCompactUnwindEncodeInputKind::Record:
      OS << "record";
      break;
    }
    if (InputIndex != NoInput)
      OS << ' ' << InputIndex;
  }
  OS << ": ";
  switch (Reason) {
  case MachOCompactUnwindEncodeFailure::UnsupportedArchitecture:
    OS << "architecture has no final compact-unwind encoder";
    break;
  case MachOCompactUnwindEncodeFailure::UnsupportedEndianness:
    OS << "target byte order is unsupported";
    break;
  case MachOCompactUnwindEncodeFailure::EmptyRecords:
    OS << "there are no recipes to encode";
    break;
  case MachOCompactUnwindEncodeFailure::TooManyPersonalities:
    OS << "personality array has more than three pointer slots";
    break;
  case MachOCompactUnwindEncodeFailure::DuplicatePersonalitySlot:
    OS << "personality pointer slot occurs more than once";
    break;
  case MachOCompactUnwindEncodeFailure::InvalidTerminalBoundary:
    OS << "terminal sentinel does not equal the final recipe end";
    break;
  case MachOCompactUnwindEncodeFailure::InvalidRecordRange:
    OS << "recipe range is empty, reversed, or beyond the sentinel";
    break;
  case MachOCompactUnwindEncodeFailure::UnsortedOrOverlappingRecords:
    OS << "recipe starts are not strictly ordered and disjoint";
    break;
  case MachOCompactUnwindEncodeFailure::NonContiguousRecords:
    OS << "recipe end has no exact following lookup boundary";
    break;
  case MachOCompactUnwindEncodeFailure::UnsupportedEncoding:
    OS << "encoding is incompatible with the target architecture";
    break;
  case MachOCompactUnwindEncodeFailure::PersonalityIndexOutOfRange:
    OS << "encoding personality index leaves the personality array";
    break;
  case MachOCompactUnwindEncodeFailure::PersonalityEncodingMismatch:
    OS << "encoding personality index and pointer slot disagree";
    break;
  case MachOCompactUnwindEncodeFailure::LSDAEncodingMismatch:
    OS << "encoding LSDA flag and LSDA offset disagree";
    break;
  case MachOCompactUnwindEncodeFailure::SectionSizeOverflow:
    OS << "canonical section layout is not representable";
    break;
  case MachOCompactUnwindEncodeFailure::StrictRoundTripFailure:
    OS << "strict parser rejected the encoded section";
    break;
  case MachOCompactUnwindEncodeFailure::SemanticRoundTripMismatch:
    OS << "strict parser output differs from the encoded recipes";
    break;
  }
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindEncodeError::convertToErrorCode() const {
  return std::make_error_code(std::errc::invalid_argument);
}

MachOCompactUnwindInstallError::MachOCompactUnwindInstallError(
    MachOCompactUnwindInstallFailure Reason, uint64_t RequiredBytes,
    uint64_t AvailableBytes, std::string Detail)
    : Reason(Reason), RequiredBytes(RequiredBytes),
      AvailableBytes(AvailableBytes), Detail(std::move(Detail)) {}

void MachOCompactUnwindInstallError::log(llvm::raw_ostream &OS) const {
  OS << "Mach-O compact unwind install: ";
  switch (Reason) {
  case MachOCompactUnwindInstallFailure::UnexpectedMappingsForNoOp:
    OS << "a no-op request carries range mappings";
    break;
  case MachOCompactUnwindInstallFailure::MissingSection:
    OS << "the image has no final compact-unwind section";
    break;
  case MachOCompactUnwindInstallFailure::SectionSizeOverflow:
    OS << "the final compact-unwind section cannot be represented";
    break;
  case MachOCompactUnwindInstallFailure::InsufficientCapacity:
    OS << "the final compact-unwind section has insufficient capacity";
    break;
  case MachOCompactUnwindInstallFailure::InvalidPlan:
    OS << "the install plan is internally inconsistent";
    break;
  case MachOCompactUnwindInstallFailure::StaleRegion:
    OS << "the final compact-unwind region changed after preflight";
    break;
  case MachOCompactUnwindInstallFailure::StalePreimage:
    OS << "the final compact-unwind bytes changed after preflight";
    break;
  case MachOCompactUnwindInstallFailure::PostLocateFailure:
    OS << "the installed compact-unwind region failed relocation";
    break;
  case MachOCompactUnwindInstallFailure::PostParseFailure:
    OS << "the installed compact-unwind section failed strict parsing";
    break;
  case MachOCompactUnwindInstallFailure::PostSemanticMismatch:
    OS << "the installed compact-unwind semantics differ from the plan";
    break;
  case MachOCompactUnwindInstallFailure::ArchitectureMismatch:
    OS << "the Mach-O header CPU architecture differs from the plan";
    break;
  }
  if (Reason == MachOCompactUnwindInstallFailure::InsufficientCapacity)
    OS << " (required " << RequiredBytes << ", available " << AvailableBytes
       << ')';
  if (!Detail.empty())
    OS << " (" << Detail << ')';
}

std::error_code MachOCompactUnwindInstallError::convertToErrorCode() const {
  if (Reason == MachOCompactUnwindInstallFailure::InsufficientCapacity)
    return std::make_error_code(std::errc::no_buffer_space);
  return std::make_error_code(std::errc::invalid_argument);
}

namespace {

using ParseFailure = MachOCompactUnwindParseFailure;
using BindFailure = MachOCompactUnwindDwarfBindFailure;
using Fixup = CompiledFixupReference;

bool machOHeaderMatchesArchitecture(llvm::ArrayRef<uint8_t> Binary,
                                    Arch TargetArch);

llvm::Error parseError(ParseFailure Reason, uint64_t RecordIndex,
                       const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindParseError>(Reason, RecordIndex,
                                                        Detail.str());
}

llvm::Error parseError(ParseFailure Reason, const llvm::Twine &Detail = {}) {
  return parseError(Reason, MachOCompactUnwindParseError::NoRecord, Detail);
}

llvm::Error bindError(BindFailure Reason, uint64_t RecordIndex,
                      const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindDwarfBindError>(Reason, RecordIndex,
                                                            Detail.str());
}

llvm::Error bindError(BindFailure Reason, const llvm::Twine &Detail = {}) {
  return bindError(Reason, MachOCompactUnwindDwarfBindError::NoRecord, Detail);
}

uint64_t readPointer(const uint8_t *Bytes, uint8_t PointerWidth,
                     llvm::endianness ByteOrder) {
  if (PointerWidth == 8)
    return llvm::support::endian::read<uint64_t>(Bytes, ByteOrder);
  return llvm::support::endian::read<uint32_t>(Bytes, ByteOrder);
}

uint32_t readU32(const uint8_t *Bytes, llvm::endianness ByteOrder) {
  return llvm::support::endian::read<uint32_t>(Bytes, ByteOrder);
}

bool isPowerOfTwo(uint64_t Value) {
  return Value != 0 && (Value & (Value - 1)) == 0;
}

bool supportsDwarfBinding(Arch TargetArch) {
  return TargetArch == Arch::X86 || TargetArch == Arch::X64 ||
         TargetArch == Arch::ARM || TargetArch == Arch::AArch64;
}

bool isDwarfEncoding(Arch TargetArch, uint32_t Encoding) {
  using namespace macho_unwind;
  const uint32_t Mode = Encoding & kModeMask;
  switch (TargetArch) {
  case Arch::X86:
    return Mode == kX86ModeDwarf;
  case Arch::X64:
    return Mode == kX86_64ModeDwarf;
  case Arch::AArch64:
    return Mode == kARM64ModeDwarf;
  case Arch::ARM:
    return Mode == kARMModeDwarf;
  default:
    return false;
  }
}

bool containsRange(const CompiledSection &Section, uint64_t Begin,
                   uint64_t Length) {
  if (Begin < Section.VA)
    return false;
  const uint64_t Offset = Begin - Section.VA;
  return Offset <= Section.Size && Length <= Section.Size - Offset;
}

bool hasExactSectionStorage(const CompiledImage &Compiled,
                            const CompiledSection &Section) {
  if (!Section.IsAllocated)
    return false;
  if (!Section.IsInImage)
    return Section.Offset == 0 && Section.Size == Section.ExternalBytes.size();
  if (!Section.ExternalBytes.empty() || Section.VA < Compiled.BaseVA)
    return false;
  const uint64_t ExpectedOffset = Section.VA - Compiled.BaseVA;
  return Section.Offset == ExpectedOffset &&
         Section.Offset <= Compiled.Bytes.size() &&
         Section.Size <= Compiled.Bytes.size() - Section.Offset;
}

llvm::Error validateEncoding(Arch TargetArch, uint32_t Encoding,
                             uint64_t RecordIndex) {
  using namespace macho_unwind;

  if ((Encoding & kPersonalityMask) != 0)
    return parseError(ParseFailure::UnsupportedEncoding, RecordIndex,
                      "linker-input personality index is nonzero");

  const uint32_t Mode = Encoding & kModeMask;
  const uint32_t CommonFlags = kIsNotFunctionStart | kHasLSDA;
  uint32_t PayloadMask = 0;
  bool SupportedMode = false;
  switch (TargetArch) {
  case Arch::X64:
  case Arch::X86:
    if (Mode == kX86_64ModeRBPFrame) {
      PayloadMask = kX86_64RBPFrameOffsetMask | kX86_64RBPFrameRegistersMask;
      SupportedMode = true;
    } else if (Mode == kX86_64ModeStackImmediate ||
               Mode == kX86_64ModeStackIndirect) {
      PayloadMask =
          kX86_64FramelessStackSizeMask | kX86_64FramelessStackAdjustMask |
          kX86_64FramelessRegCountMask | kX86_64FramelessRegPermutationMask;
      SupportedMode = true;
    } else if (Mode == kX86_64ModeDwarf) {
      SupportedMode = true;
    }
    break;
  case Arch::AArch64: {
    constexpr uint32_t RegisterPairs =
        kARM64FrameX19X20Pair | kARM64FrameX21X22Pair | kARM64FrameX23X24Pair |
        kARM64FrameX25X26Pair | kARM64FrameX27X28Pair | kARM64FrameD8D9Pair |
        kARM64FrameD10D11Pair | kARM64FrameD12D13Pair | kARM64FrameD14D15Pair;
    if (Mode == kARM64ModeFrameless) {
      PayloadMask = kARM64FramelessStackSizeMask | RegisterPairs;
      SupportedMode = true;
    } else if (Mode == kARM64ModeFrame) {
      PayloadMask = RegisterPairs;
      SupportedMode = true;
    } else if (Mode == kARM64ModeDwarf) {
      SupportedMode = true;
    }
    break;
  }
  case Arch::ARM:
    if (Mode == kARMModeFrame || Mode == kARMModeFrameD) {
      PayloadMask = kARMFrameStackAdjustMask | kARMFrameFirstPushMask |
                    kARMFrameSecondPushMask;
      if (Mode == kARMModeFrameD)
        PayloadMask |= kARMFrameDRegisterCountMask;
      SupportedMode = true;
    } else if (Mode == kARMModeDwarf) {
      SupportedMode = true;
    }
    break;
  default:
    break;
  }

  if (!SupportedMode ||
      (Encoding & ~(CommonFlags | kModeMask | PayloadMask)) != 0)
    return parseError(ParseFailure::UnsupportedEncoding, RecordIndex,
                      "mode or reserved payload bits are invalid");

  CompactUnwindEntry Decoded;
  if (!decodeEncoding(TargetArch, Encoding, Decoded) ||
      Decoded.Kind == CompactUnwindKind::Unknown ||
      Decoded.Kind == CompactUnwindKind::None)
    return parseError(ParseFailure::UnsupportedEncoding, RecordIndex,
                      "encoding does not describe a valid frame");
  return llvm::Error::success();
}

llvm::Error validateFixupShape(const Fixup &Reference, uint8_t PointerWidth,
                               uint64_t RecordIndex) {
  const unsigned ExpectedKind =
      PointerWidth == 8 ? llvm::FK_Data_8 : llvm::FK_Data_4;
  if (Reference.Kind != ExpectedKind || Reference.Symbol.empty() ||
      !Reference.SubtractSymbol.empty() || Reference.Addend != 0 ||
      Reference.Specifier != 0 || Reference.IsPCRel || !Reference.IsResolved ||
      Reference.BitWidth != unsigned(PointerWidth) * 8)
    return parseError(ParseFailure::InvalidFixup, RecordIndex,
                      llvm::Twine("section offset ") +
                          llvm::Twine(Reference.Offset));
  return llvm::Error::success();
}

llvm::Error validateKnownSymbolValue(const CompiledImage &Compiled,
                                     const Fixup &Reference, uint64_t Value,
                                     uint64_t RecordIndex, bool MustBeDefined) {
  const auto It = Compiled.SymbolAddrs.find(Reference.Symbol);
  if (It == Compiled.SymbolAddrs.end()) {
    if (MustBeDefined)
      return parseError(ParseFailure::MissingSymbolValue, RecordIndex,
                        Reference.Symbol);
    return llvm::Error::success();
  }
  if (It->second != Value)
    return parseError(ParseFailure::SymbolValueMismatch, RecordIndex,
                      Reference.Symbol);
  return llvm::Error::success();
}

llvm::Expected<uint32_t>
resolvePersonalitySlot(const BinaryImage &SourceImage,
                       llvm::StringRef PersonalitySymbol, uint8_t PointerWidth,
                       uint64_t MachHeaderVA, uint64_t RecordIndex) {
  std::optional<uint64_t> SlotVA;
  for (const auto &[Address, Symbol] : SourceImage.ImportPtrSlots) {
    if (Symbol != PersonalitySymbol)
      continue;
    if (SlotVA)
      return parseError(ParseFailure::AmbiguousPersonalitySlot, RecordIndex,
                        PersonalitySymbol);
    SlotVA = Address;
  }
  if (!SlotVA)
    return parseError(ParseFailure::MissingPersonalitySlot, RecordIndex,
                      PersonalitySymbol);

  unsigned ContainingSections = 0;
  for (const Section &Section : SourceImage.Sections) {
    if (*SlotVA < Section.VA)
      continue;
    const uint64_t Offset = *SlotVA - Section.VA;
    if (Offset <= Section.Size && PointerWidth <= Section.Size - Offset)
      ++ContainingSections;
  }
  if (ContainingSections != 1 || *SlotVA < MachHeaderVA) {
    return parseError(ParseFailure::InvalidPersonalitySlot, RecordIndex,
                      PersonalitySymbol);
  }
  const uint64_t RVA = *SlotVA - MachHeaderVA;
  if (RVA > std::numeric_limits<uint32_t>::max())
    return parseError(ParseFailure::InvalidPersonalitySlot, RecordIndex,
                      PersonalitySymbol);
  return static_cast<uint32_t>(RVA);
}

} // namespace

llvm::Error validateGeneratedMachOCompactUnwindRecordEncoding(
    Arch TargetArch, const MachOCompactUnwindRecord &Record) {
  using namespace macho_unwind;
  if (llvm::Error Error = validateEncoding(TargetArch, Record.Encoding,
                                           Record.SourceRecordIndex))
    return Error;
  const bool HasPersonality = !Record.PersonalitySymbol.empty();
  if (HasPersonality != Record.PersonalitySlotRVA.has_value())
    return parseError(ParseFailure::EncodingFieldMismatch,
                      Record.SourceRecordIndex, "personality field shape");
  const bool HasLSDA = !Record.LSDASymbol.empty();
  if (HasLSDA != Record.LSDAVA.has_value() ||
      HasLSDA != ((Record.Encoding & kHasLSDA) != 0))
    return parseError(ParseFailure::EncodingFieldMismatch,
                      Record.SourceRecordIndex, "LSDA field shape");
  return llvm::Error::success();
}

llvm::Expected<MachOCompactUnwindRecords> parseGeneratedMachOCompactUnwind(
    const CompiledImage &Compiled, const BinaryImage &SourceImage,
    uint64_t MachHeaderVA, llvm::endianness ByteOrder) {
  using namespace macho_unwind;

  if (!Compiled.Success || Compiled.Format != BinaryFormat::MachO ||
      !Compiled.FunctionRangesValid ||
      !llvm::mc_rewrite::validateRewriteFunctionRanges(
          Compiled.FunctionRanges, Compiled.FunctionOwnerAddrs))
    return parseError(ParseFailure::InvalidCompiledImage);
  if (SourceImage.Format != BinaryFormat::MachO)
    return parseError(ParseFailure::InvalidSourceImage);
  if (Compiled.TargetArch != SourceImage.Arch)
    return parseError(ParseFailure::InvalidCompiledImage,
                      "compiled architecture does not match the source image");
  if (ByteOrder != llvm::endianness::little &&
      ByteOrder != llvm::endianness::big)
    return parseError(ParseFailure::UnsupportedEndianness);
  if (Compiled.ByteOrder != ByteOrder)
    return parseError(ParseFailure::InvalidCompiledImage,
                      "compiled byte order does not match parser input");

  const uint32_t SourcePointerWidth = SourceImage.getPointerSize();
  if (SourcePointerWidth != 4 && SourcePointerWidth != 8)
    return parseError(ParseFailure::UnsupportedPointerWidth,
                      llvm::Twine(SourcePointerWidth));
  const uint8_t PointerWidth = static_cast<uint8_t>(SourcePointerWidth);
  if (Compiled.PointerWidth != PointerWidth)
    return parseError(ParseFailure::InvalidCompiledImage,
                      "compiled pointer width does not match the source image");
  switch (SourceImage.Arch) {
  case Arch::X64:
  case Arch::AArch64:
    if (PointerWidth != 8)
      return parseError(ParseFailure::ArchitecturePointerWidthMismatch);
    break;
  case Arch::X86:
  case Arch::ARM:
    if (PointerWidth != 4)
      return parseError(ParseFailure::ArchitecturePointerWidthMismatch);
    break;
  default:
    return parseError(ParseFailure::UnsupportedArchitecture);
  }

  const CompiledSection *Compact = nullptr;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (Section.Name != section_names::macho::CompactUnwind)
      continue;
    if (Compact)
      return parseError(ParseFailure::AmbiguousSection);
    Compact = &Section;
  }
  if (!Compact)
    return parseError(ParseFailure::MissingSection);
  if (Compact->IsAllocated || Compact->IsInImage || Compact->Offset != 0 ||
      Compact->VA != 0 || Compact->Size != Compact->ExternalBytes.size())
    return parseError(ParseFailure::InvalidSectionStorage);
  if (!isPowerOfTwo(Compact->Alignment) || Compact->Alignment != PointerWidth)
    return parseError(ParseFailure::InvalidSectionAlignment);

  const uint64_t FunctionFieldOffset = 0;
  const uint64_t SizeFieldOffset = PointerWidth;
  const uint64_t EncodingFieldOffset = SizeFieldOffset + 4;
  const uint64_t PersonalityFieldOffset = EncodingFieldOffset + 4;
  const uint64_t LSDAFieldOffset = PersonalityFieldOffset + PointerWidth;
  const uint64_t RecordSize = LSDAFieldOffset + PointerWidth;
  if (Compact->ExternalBytes.size() < RecordSize)
    return parseError(ParseFailure::SectionTooShort);
  if (Compact->ExternalBytes.size() % RecordSize != 0)
    return parseError(ParseFailure::TrailingBytes);

  std::map<uint64_t, const Fixup *> Fixups;
  for (const Fixup &Reference : Compact->FixupReferences) {
    if (Reference.Offset >= Compact->ExternalBytes.size())
      return parseError(ParseFailure::InvalidFixup,
                        llvm::Twine("out-of-bounds section offset ") +
                            llvm::Twine(Reference.Offset));
    const uint64_t RecordIndex = Reference.Offset / RecordSize;
    const uint64_t FieldOffset = Reference.Offset % RecordSize;
    if (FieldOffset != FunctionFieldOffset &&
        FieldOffset != PersonalityFieldOffset && FieldOffset != LSDAFieldOffset)
      return parseError(ParseFailure::InvalidFixup, RecordIndex,
                        llvm::Twine("unexpected field offset ") +
                            llvm::Twine(FieldOffset));
    if (!Fixups.emplace(Reference.Offset, &Reference).second)
      return parseError(ParseFailure::AmbiguousFixup, RecordIndex,
                        llvm::Twine("section offset ") +
                            llvm::Twine(Reference.Offset));
    if (llvm::Error Error =
            validateFixupShape(Reference, PointerWidth, RecordIndex))
      return std::move(Error);
    if (FieldOffset != FunctionFieldOffset && Reference.FunctionRangeId != 0)
      return parseError(ParseFailure::InvalidFixup, RecordIndex,
                        "non-function field carries a function-range ID");
  }

  std::map<uint64_t, const CompiledFunctionRange *> FunctionRangesById;
  std::set<std::string> BeginSymbols;
  for (const CompiledFunctionRange &Range : Compiled.FunctionRanges) {
    FunctionRangesById.emplace(Range.Id, &Range);
    if (!BeginSymbols.insert(Range.BeginSymbol).second)
      return parseError(ParseFailure::InvalidCompiledImage,
                        "private begin symbol is not unique");
  }
  std::set<uint64_t> UsedFunctionRangeIds;

  auto GetFixup = [&](uint64_t Offset) -> const Fixup * {
    const auto It = Fixups.find(Offset);
    return It == Fixups.end() ? nullptr : It->second;
  };

  MachOCompactUnwindRecords Result;
  Result.TargetArch = SourceImage.Arch;
  Result.PointerWidth = PointerWidth;
  Result.ByteOrder = ByteOrder;
  Result.Records.reserve(Compact->ExternalBytes.size() / RecordSize);

  for (uint64_t RecordIndex = 0;
       RecordIndex < Compact->ExternalBytes.size() / RecordSize;
       ++RecordIndex) {
    const uint64_t RecordOffset = RecordIndex * RecordSize;
    const uint8_t *Record = Compact->ExternalBytes.data() + RecordOffset;

    MachOCompactUnwindRecord Parsed;
    Parsed.SourceRecordIndex = RecordIndex;
    Parsed.FunctionVA =
        readPointer(Record + FunctionFieldOffset, PointerWidth, ByteOrder);
    Parsed.RangeLength = readU32(Record + SizeFieldOffset, ByteOrder);
    Parsed.Encoding = readU32(Record + EncodingFieldOffset, ByteOrder);
    const uint64_t PersonalityValue =
        readPointer(Record + PersonalityFieldOffset, PointerWidth, ByteOrder);
    const uint64_t LSDAValue =
        readPointer(Record + LSDAFieldOffset, PointerWidth, ByteOrder);

    if (Parsed.RangeLength == 0)
      return parseError(ParseFailure::EmptyRange, RecordIndex);
    if (Parsed.RangeLength >
        std::numeric_limits<uint64_t>::max() - Parsed.FunctionVA)
      return parseError(ParseFailure::RangeOverflow, RecordIndex);
    Parsed.FunctionEndVA = Parsed.FunctionVA + Parsed.RangeLength;
    if (llvm::Error Error =
            validateEncoding(SourceImage.Arch, Parsed.Encoding, RecordIndex))
      return std::move(Error);

    unsigned ContainingCodeSections = 0;
    for (const CompiledSection &Section : Compiled.Sections) {
      if (Section.IsAllocated &&
          Section.Kind == llvm::mc_rewrite::RewriteSectionKind::Code &&
          containsRange(Section, Parsed.FunctionVA, Parsed.RangeLength)) {
        if (!hasExactSectionStorage(Compiled, Section))
          return parseError(ParseFailure::InvalidSectionStorage, RecordIndex,
                            Section.Name);
        ++ContainingCodeSections;
      }
    }
    if (ContainingCodeSections != 1)
      return parseError(ParseFailure::FunctionOutsideCode, RecordIndex);

    const Fixup *FunctionFixup = GetFixup(RecordOffset + FunctionFieldOffset);
    if (!FunctionFixup)
      return parseError(ParseFailure::MissingFixup, RecordIndex,
                        "function field");
    if (FunctionFixup->FunctionRangeId == 0)
      return parseError(ParseFailure::MissingFunctionRangeId, RecordIndex,
                        FunctionFixup->Symbol);
    const auto RangeIt =
        FunctionRangesById.find(FunctionFixup->FunctionRangeId);
    if (RangeIt == FunctionRangesById.end())
      return parseError(ParseFailure::DanglingFunctionRangeId, RecordIndex,
                        llvm::Twine(FunctionFixup->FunctionRangeId));
    if (!UsedFunctionRangeIds.insert(FunctionFixup->FunctionRangeId).second)
      return parseError(ParseFailure::DuplicateFunctionRangeId, RecordIndex,
                        llvm::Twine(FunctionFixup->FunctionRangeId));
    const CompiledFunctionRange &FunctionRange = *RangeIt->second;
    if (FunctionFixup->Symbol != FunctionRange.BeginSymbol)
      return parseError(ParseFailure::FunctionRangeSymbolMismatch, RecordIndex,
                        FunctionFixup->Symbol);
    if (Parsed.FunctionVA != FunctionRange.BeginVA ||
        Parsed.FunctionEndVA != FunctionRange.EndVA)
      return parseError(ParseFailure::FunctionRangeBoundaryMismatch,
                        RecordIndex,
                        llvm::Twine(FunctionFixup->FunctionRangeId));
    Parsed.FunctionRangeId = FunctionRange.Id;
    Parsed.OwnerSymbol = FunctionRange.OwnerSymbol;
    Parsed.OwnerVA = FunctionRange.OwnerVA;
    Parsed.FunctionSymbol = FunctionFixup->Symbol;

    const Fixup *PersonalityFixup =
        GetFixup(RecordOffset + PersonalityFieldOffset);
    if (PersonalityValue != 0 && !PersonalityFixup)
      return parseError(ParseFailure::MissingFixup, RecordIndex,
                        "personality field");
    if (PersonalityValue == 0 && PersonalityFixup)
      return parseError(ParseFailure::EncodingFieldMismatch, RecordIndex,
                        "personality field/fixup presence");
    if (PersonalityFixup) {
      Parsed.PersonalitySymbol = PersonalityFixup->Symbol;
      auto Slot =
          resolvePersonalitySlot(SourceImage, Parsed.PersonalitySymbol,
                                 PointerWidth, MachHeaderVA, RecordIndex);
      if (!Slot)
        return Slot.takeError();
      Parsed.PersonalitySlotRVA = *Slot;
    }

    const Fixup *LSDAFixup = GetFixup(RecordOffset + LSDAFieldOffset);
    if (LSDAValue != 0 && !LSDAFixup)
      return parseError(ParseFailure::MissingFixup, RecordIndex, "LSDA field");
    if (LSDAValue == 0 && LSDAFixup)
      return parseError(ParseFailure::EncodingFieldMismatch, RecordIndex,
                        "LSDA field/fixup presence");
    if (LSDAFixup) {
      unsigned ContainingLSDASections = 0;
      for (const CompiledSection &Section : Compiled.Sections) {
        if (Section.IsAllocated && containsRange(Section, LSDAValue, 1)) {
          if (!hasExactSectionStorage(Compiled, Section))
            return parseError(ParseFailure::InvalidSectionStorage, RecordIndex,
                              Section.Name);
          ++ContainingLSDASections;
        }
      }
      if (ContainingLSDASections != 1)
        return parseError(ParseFailure::LSDAOutsideGeneratedImage, RecordIndex);
      Parsed.LSDASymbol = LSDAFixup->Symbol;
      Parsed.LSDAVA = LSDAValue;
      if (llvm::Error Error = validateKnownSymbolValue(Compiled, *LSDAFixup,
                                                       LSDAValue, RecordIndex,
                                                       /*MustBeDefined=*/false))
        return std::move(Error);
    }

    const bool EncodingHasLSDA = (Parsed.Encoding & kHasLSDA) != 0;
    if (EncodingHasLSDA != static_cast<bool>(LSDAFixup))
      return parseError(ParseFailure::EncodingFieldMismatch, RecordIndex,
                        "LSDA flag");

    Result.Records.push_back(std::move(Parsed));
  }

  std::sort(Result.Records.begin(), Result.Records.end(),
            [](const MachOCompactUnwindRecord &Left,
               const MachOCompactUnwindRecord &Right) {
              return std::tie(Left.FunctionVA, Left.FunctionEndVA,
                              Left.SourceRecordIndex) <
                     std::tie(Right.FunctionVA, Right.FunctionEndVA,
                              Right.SourceRecordIndex);
            });
  for (size_t I = 1; I < Result.Records.size(); ++I) {
    if (Result.Records[I].FunctionVA < Result.Records[I - 1].FunctionEndVA)
      return parseError(ParseFailure::OverlappingRanges,
                        Result.Records[I].SourceRecordIndex);
  }
  return Result;
}

llvm::Expected<MachOCompactUnwindRecords>
bindMachOCompactUnwindDwarfFDEs(const MachOCompactUnwindRecords &Generated,
                                const MachOEHFrameInstallReceipt &Receipt) {
  using namespace macho_unwind;

  if (!supportsDwarfBinding(Generated.TargetArch))
    return bindError(BindFailure::UnsupportedArchitecture);

  const bool HasDwarfRecord = std::any_of(
      Generated.Records.begin(), Generated.Records.end(),
      [&](const MachOCompactUnwindRecord &Record) {
        return isDwarfEncoding(Generated.TargetArch, Record.Encoding);
      });
  if (!HasDwarfRecord)
    return Generated;
  if (Receipt.disposition() != MachOEHFrameInstallDisposition::Installed ||
      !Receipt.region())
    return bindError(BindFailure::MissingInstallReceipt);
  if (Generated.TargetArch != Receipt.targetArch())
    return bindError(BindFailure::ReceiptTargetMismatch, "architecture");
  if (Generated.PointerWidth != Receipt.pointerWidth())
    return bindError(BindFailure::ReceiptTargetMismatch, "pointer width");
  if (Generated.ByteOrder != Receipt.byteOrder())
    return bindError(BindFailure::ReceiptTargetMismatch, "byte order");

  const uint64_t EHFrameSectionVA = Receipt.region()->SectionVA;
  const llvm::ArrayRef<DwarfEHFrameRecord> FDEs = Receipt.installedFDEs();
  const llvm::ArrayRef<CompiledFunctionRange> AuthenticatedRanges =
      Receipt.authenticatedFunctionRanges();

  std::map<uint64_t, const CompiledFunctionRange *> RangesById;
  for (const CompiledFunctionRange &Range : AuthenticatedRanges) {
    const auto Owner =
        Receipt.authenticatedFunctionOwnerAddrs().find(Range.OwnerSymbol);
    if (Owner == Receipt.authenticatedFunctionOwnerAddrs().end() ||
        Owner->second != Range.OwnerVA)
      return bindError(BindFailure::OwnerIdentityMismatch,
                       MachOCompactUnwindDwarfBindError::NoRecord,
                       Range.OwnerSymbol);
    if (Range.Id == 0 || !RangesById.emplace(Range.Id, &Range).second)
      return bindError(BindFailure::FunctionRangeIdentityMismatch,
                       MachOCompactUnwindDwarfBindError::NoRecord,
                       llvm::Twine(Range.Id));
  }
  if (!llvm::mc_rewrite::validateRewriteFunctionRanges(
          AuthenticatedRanges, Receipt.authenticatedFunctionOwnerAddrs()))
    return bindError(BindFailure::FunctionRangeIdentityMismatch);

  struct FDEMatch {
    const DwarfEHFrameRecord *Record = nullptr;
    size_t Count = 0;
  };
  std::map<std::pair<uint64_t, uint64_t>, FDEMatch> ExactFDEs;
  std::set<uint64_t> FDEStarts;
  for (const DwarfEHFrameRecord &FDE : FDEs) {
    FDEStarts.insert(FDE.BeginVA);
    FDEMatch &Match = ExactFDEs[{FDE.BeginVA, FDE.EndVA}];
    if (!Match.Record)
      Match.Record = &FDE;
    ++Match.Count;
  }

  MachOCompactUnwindRecords Result = Generated;
  std::set<uint64_t> UsedDwarfRangeIds;
  for (size_t I = 0; I < Generated.Records.size(); ++I) {
    const MachOCompactUnwindRecord &Input = Generated.Records[I];
    if (!isDwarfEncoding(Generated.TargetArch, Input.Encoding))
      continue;
    if ((Input.Encoding & kDwarfSectionOffsetMask) != 0)
      return bindError(BindFailure::PrepopulatedDwarfOffset,
                       Input.SourceRecordIndex);
    if (Input.FunctionRangeId == 0 ||
        !UsedDwarfRangeIds.insert(Input.FunctionRangeId).second)
      return bindError(BindFailure::FunctionRangeIdentityMismatch,
                       Input.SourceRecordIndex,
                       llvm::Twine(Input.FunctionRangeId));

    const auto RangeIt = RangesById.find(Input.FunctionRangeId);
    if (RangeIt == RangesById.end())
      return bindError(BindFailure::MissingFunctionRange,
                       Input.SourceRecordIndex,
                       llvm::Twine(Input.FunctionRangeId));
    const CompiledFunctionRange &Range = *RangeIt->second;
    if (Range.BeginSymbol != Input.FunctionSymbol ||
        Range.BeginVA != Input.FunctionVA ||
        Range.EndVA != Input.FunctionEndVA ||
        Range.OwnerSymbol != Input.OwnerSymbol ||
        Range.OwnerVA != Input.OwnerVA)
      return bindError(BindFailure::FunctionRangeIdentityMismatch,
                       Input.SourceRecordIndex, Input.OwnerSymbol);

    const auto Owner =
        Receipt.authenticatedFunctionOwnerAddrs().find(Input.OwnerSymbol);
    if (Owner == Receipt.authenticatedFunctionOwnerAddrs().end() ||
        Owner->second != Input.OwnerVA)
      return bindError(BindFailure::OwnerIdentityMismatch,
                       Input.SourceRecordIndex, Input.OwnerSymbol);

    const auto Match = ExactFDEs.find({Input.FunctionVA, Input.FunctionEndVA});
    if (Match == ExactFDEs.end()) {
      if (FDEStarts.contains(Input.FunctionVA))
        return bindError(BindFailure::FDERangeMismatch,
                         Input.SourceRecordIndex);
      return bindError(BindFailure::MissingFDE, Input.SourceRecordIndex);
    }
    if (Match->second.Count != 1)
      return bindError(BindFailure::AmbiguousFDE, Input.SourceRecordIndex);

    const DwarfEHFrameRecord &FDE = *Match->second.Record;
    if (FDE.RecordVA < EHFrameSectionVA)
      return bindError(BindFailure::FDERecordAddressUnderflow,
                       Input.SourceRecordIndex);
    const uint64_t SectionOffset = FDE.RecordVA - EHFrameSectionVA;
    if (SectionOffset == 0)
      return bindError(BindFailure::ZeroFDESectionOffset,
                       Input.SourceRecordIndex);
    if (SectionOffset > kDwarfSectionOffsetMask)
      return bindError(BindFailure::FDESectionOffsetOverflow,
                       Input.SourceRecordIndex);

    Result.Records[I].Encoding = (Input.Encoding & ~kDwarfSectionOffsetMask) |
                                 static_cast<uint32_t>(SectionOffset);
  }
  return Result;
}

namespace {

using MergeFailure = MachOCompactUnwindMergeFailure;
using MergeInputKind = MachOCompactUnwindMergeInputKind;
using MergedRecord = MachOCompactUnwindMergedRecord;

llvm::Error rangeMapError(MachOCompactUnwindRangeMapFailure Reason,
                          uint64_t RecordIndex,
                          const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindRangeMapError>(Reason, RecordIndex,
                                                           Detail.str());
}

llvm::Error rangeMapError(MachOCompactUnwindRangeMapFailure Reason,
                          const llvm::Twine &Detail = {}) {
  return rangeMapError(Reason, MachOCompactUnwindRangeMapError::NoRecord,
                       Detail);
}

llvm::Error mergeError(MergeFailure Reason, MergeInputKind InputKind,
                       uint64_t InputIndex, const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindMergeError>(
      Reason, InputKind, InputIndex, Detail.str());
}

llvm::Error mergeError(MergeFailure Reason, const llvm::Twine &Detail = {}) {
  return mergeError(Reason, MergeInputKind::None,
                    MachOCompactUnwindMergeError::NoInput, Detail);
}

bool isMergeArchitecture(Arch TargetArch) {
  return TargetArch == Arch::X64 || TargetArch == Arch::X86 ||
         TargetArch == Arch::ARM || TargetArch == Arch::AArch64;
}

uint8_t pointerWidthFor(Arch TargetArch) {
  return TargetArch == Arch::X86 || TargetArch == Arch::ARM ? 4 : 8;
}

llvm::Error validateRawEncoding(Arch TargetArch, uint32_t Encoding,
                                MergeInputKind InputKind, uint64_t InputIndex,
                                bool AllowDwarfLinearSearch) {
  using namespace macho_unwind;

  if (Encoding == 0)
    return llvm::Error::success();

  const uint32_t Mode = Encoding & kModeMask;
  const uint32_t SharedFields =
      kIsNotFunctionStart | kHasLSDA | kPersonalityMask | kModeMask;
  uint32_t PayloadMask = 0;
  bool SupportedMode = false;
  switch (TargetArch) {
  case Arch::X64:
  case Arch::X86:
    if (Mode == kX86_64ModeRBPFrame) {
      PayloadMask = kX86_64RBPFrameOffsetMask | kX86_64RBPFrameRegistersMask;
      SupportedMode = true;
    } else if (Mode == kX86_64ModeStackImmediate ||
               Mode == kX86_64ModeStackIndirect) {
      PayloadMask =
          kX86_64FramelessStackSizeMask | kX86_64FramelessStackAdjustMask |
          kX86_64FramelessRegCountMask | kX86_64FramelessRegPermutationMask;
      SupportedMode = true;
    } else if (Mode == kX86_64ModeDwarf) {
      PayloadMask = kDwarfSectionOffsetMask;
      SupportedMode = true;
    }
    break;
  case Arch::AArch64: {
    constexpr uint32_t RegisterPairs =
        kARM64FrameX19X20Pair | kARM64FrameX21X22Pair | kARM64FrameX23X24Pair |
        kARM64FrameX25X26Pair | kARM64FrameX27X28Pair | kARM64FrameD8D9Pair |
        kARM64FrameD10D11Pair | kARM64FrameD12D13Pair | kARM64FrameD14D15Pair;
    if (Mode == kARM64ModeFrameless) {
      PayloadMask = kARM64FramelessStackSizeMask | RegisterPairs;
      SupportedMode = true;
    } else if (Mode == kARM64ModeFrame) {
      PayloadMask = RegisterPairs;
      SupportedMode = true;
    } else if (Mode == kARM64ModeDwarf) {
      PayloadMask = kDwarfSectionOffsetMask;
      SupportedMode = true;
    }
    break;
  }
  case Arch::ARM:
    if (Mode == kARMModeFrame || Mode == kARMModeFrameD) {
      PayloadMask = kARMFrameStackAdjustMask | kARMFrameFirstPushMask |
                    kARMFrameSecondPushMask;
      if (Mode == kARMModeFrameD)
        PayloadMask |= kARMFrameDRegisterCountMask;
      SupportedMode = true;
    } else if (Mode == kARMModeDwarf) {
      PayloadMask = kDwarfSectionOffsetMask;
      SupportedMode = true;
    }
    break;
  default:
    break;
  }

  if (!SupportedMode || (Encoding & ~(SharedFields | PayloadMask)) != 0)
    return mergeError(MergeFailure::UnsupportedEncoding, InputKind, InputIndex);
  if (!AllowDwarfLinearSearch && isDwarfEncoding(TargetArch, Encoding) &&
      (Encoding & kDwarfSectionOffsetMask) == 0)
    return mergeError(MergeFailure::MissingDwarfFDEOffset, InputKind,
                      InputIndex);

  CompactUnwindEntry Decoded;
  if (!decodeEncoding(TargetArch, Encoding, Decoded) ||
      Decoded.Kind == CompactUnwindKind::Unknown ||
      Decoded.Kind == CompactUnwindKind::None)
    return mergeError(MergeFailure::UnsupportedEncoding, InputKind, InputIndex);
  return llvm::Error::success();
}

llvm::Expected<uint32_t> toImageRVA(uint64_t VA, uint64_t MachHeaderVA,
                                    MergeInputKind InputKind,
                                    uint64_t InputIndex,
                                    llvm::StringRef Field) {
  if (VA < MachHeaderVA ||
      VA - MachHeaderVA > std::numeric_limits<uint32_t>::max())
    return mergeError(MergeFailure::AddressOutsideImageRVA, InputKind,
                      InputIndex, Field);
  return static_cast<uint32_t>(VA - MachHeaderVA);
}

struct NormalizedOriginalTable {
  uint32_t TerminalFunctionRVA = 0;
  std::vector<MergedRecord> Records;
};

llvm::Expected<NormalizedOriginalTable>
normalizeOriginalTable(Arch TargetArch,
                       const macho_unwind::CompactUnwindRawSection &Original) {
  using namespace macho_unwind;

  if (Original.OriginalBytes.empty())
    return mergeError(MergeFailure::InvalidOriginalTable,
                      "retained byte image is empty");
  auto Reparsed = parseCompactUnwindRaw(Original.OriginalBytes);
  if (!Reparsed)
    return mergeError(MergeFailure::InvalidOriginalTable,
                      llvm::toString(Reparsed.takeError()));
  if (*Reparsed != Original)
    return mergeError(MergeFailure::InvalidOriginalTable,
                      "decoded fields differ from retained bytes");

  uint64_t EncodingIndex = 0;
  for (uint32_t Encoding : Original.CommonEncodings) {
    if (llvm::Error Error = validateRawEncoding(
            TargetArch, Encoding, MergeInputKind::OriginalEncoding,
            EncodingIndex++, /*AllowDwarfLinearSearch=*/true))
      return std::move(Error);
  }
  for (const CompactUnwindRawPage &Page : Original.Pages) {
    for (uint32_t Encoding : Page.LocalEncodings) {
      if (llvm::Error Error = validateRawEncoding(
              TargetArch, Encoding, MergeInputKind::OriginalEncoding,
              EncodingIndex++, /*AllowDwarfLinearSearch=*/true))
        return std::move(Error);
    }
  }

  std::vector<std::pair<uint32_t, uint32_t>> Entries;
  for (const CompactUnwindRawPage &Page : Original.Pages) {
    if (Page.Kind == kSecondLevelRegular) {
      for (const CompactUnwindRawRegularEntry &Entry : Page.RegularEntries)
        Entries.emplace_back(Entry.FunctionOffset, Entry.Encoding);
    } else {
      for (const CompactUnwindRawCompressedEntry &Entry :
           Page.CompressedEntries)
        Entries.emplace_back(Entry.FunctionOffset, Entry.Encoding);
    }
  }
  if (Entries.empty() || Original.Index.empty())
    return mergeError(MergeFailure::InvalidOriginalTable,
                      "strict table has no semantic records or sentinel");

  std::map<uint32_t, uint32_t> RemainingLSDAs;
  for (const CompactUnwindRawLSDAEntry &Entry : Original.LSDAEntries)
    RemainingLSDAs.emplace(Entry.FunctionOffset, Entry.LSDAOffset);

  NormalizedOriginalTable Result;
  Result.TerminalFunctionRVA = Original.Index.back().FunctionOffset;
  Result.Records.reserve(Entries.size());
  for (size_t I = 0; I < Entries.size(); ++I) {
    const auto [FunctionRVA, Encoding] = Entries[I];
    const uint32_t FunctionEndRVA = I + 1 == Entries.size()
                                        ? Result.TerminalFunctionRVA
                                        : Entries[I + 1].first;
    if (FunctionRVA >= FunctionEndRVA)
      return mergeError(MergeFailure::InvalidOriginalTable,
                        MergeInputKind::OriginalRecord, I,
                        "derived half-open range is empty or reversed");
    if (llvm::Error Error = validateRawEncoding(
            TargetArch, Encoding, MergeInputKind::OriginalRecord, I,
            /*AllowDwarfLinearSearch=*/true))
      return std::move(Error);

    const uint32_t PersonalityIndex =
        (Encoding & kPersonalityMask) >> kPersonalityShift;
    if (PersonalityIndex > Original.PersonalitySlotOffsets.size())
      return mergeError(MergeFailure::PersonalityIndexOutOfRange,
                        MergeInputKind::OriginalRecord, I);

    const auto LSDAIt = RemainingLSDAs.find(FunctionRVA);
    const bool HasLSDAEntry = LSDAIt != RemainingLSDAs.end();
    if (((Encoding & kHasLSDA) != 0) != HasLSDAEntry)
      return mergeError(MergeFailure::LSDAEncodingMismatch,
                        MergeInputKind::OriginalRecord, I);

    MergedRecord Record;
    Record.FunctionRVA = FunctionRVA;
    Record.FunctionEndRVA = FunctionEndRVA;
    Record.Encoding = Encoding & ~kPersonalityMask;
    if (PersonalityIndex != 0)
      Record.PersonalitySlotRVA =
          Original.PersonalitySlotOffsets[PersonalityIndex - 1];
    if (HasLSDAEntry) {
      Record.LSDARVA = LSDAIt->second;
      RemainingLSDAs.erase(LSDAIt);
    }
    Record.Origin = MachOCompactUnwindRecordOrigin::Original;
    Record.InputRecordIndex = I;
    Result.Records.push_back(std::move(Record));
  }
  if (!RemainingLSDAs.empty())
    return mergeError(MergeFailure::LSDAEncodingMismatch,
                      "LSDA key does not begin a compact-unwind recipe");
  return Result;
}

llvm::Expected<std::vector<MergedRecord>>
normalizeGeneratedRecords(Arch TargetArch, uint64_t MachHeaderVA,
                          const MachOCompactUnwindRecords &Generated) {
  using namespace macho_unwind;

  if (Generated.TargetArch != TargetArch ||
      Generated.PointerWidth != pointerWidthFor(TargetArch))
    return mergeError(MergeFailure::ArchitectureMismatch);
  if (Generated.ByteOrder != llvm::endianness::little)
    return mergeError(
        MergeFailure::ByteOrderMismatch,
        "x86, x86-64, ARM, and AArch64 Mach-O targets are little-endian");

  std::vector<MergedRecord> Result;
  Result.reserve(Generated.Records.size());
  std::set<uint64_t> FunctionRangeIds;
  for (size_t I = 0; I < Generated.Records.size(); ++I) {
    const MachOCompactUnwindRecord &Input = Generated.Records[I];
    if (Input.FunctionRangeId == 0)
      return mergeError(MergeFailure::MissingFunctionRangeId,
                        MergeInputKind::GeneratedRecord, I);
    if (!FunctionRangeIds.insert(Input.FunctionRangeId).second)
      return mergeError(MergeFailure::DuplicateFunctionRangeId,
                        MergeInputKind::GeneratedRecord, I,
                        llvm::Twine(Input.FunctionRangeId));
    if (Input.FunctionVA >= Input.FunctionEndVA ||
        Input.FunctionEndVA - Input.FunctionVA != Input.RangeLength ||
        Input.FunctionSymbol.empty() || Input.OwnerSymbol.empty() ||
        ((Input.Encoding & kPersonalityMask) != 0) ||
        (Input.PersonalitySymbol.empty() ==
         Input.PersonalitySlotRVA.has_value()) ||
        (Input.LSDASymbol.empty() == Input.LSDAVA.has_value()))
      return mergeError(MergeFailure::InvalidGeneratedRecord,
                        MergeInputKind::GeneratedRecord, I);
    if (llvm::Error Error = validateRawEncoding(
            TargetArch, Input.Encoding, MergeInputKind::GeneratedRecord, I,
            /*AllowDwarfLinearSearch=*/false))
      return std::move(Error);

    if (((Input.Encoding & kHasLSDA) != 0) != Input.LSDAVA.has_value())
      return mergeError(MergeFailure::LSDAEncodingMismatch,
                        MergeInputKind::GeneratedRecord, I);

    auto FunctionRVA =
        toImageRVA(Input.FunctionVA, MachHeaderVA,
                   MergeInputKind::GeneratedRecord, I, "function start");
    if (!FunctionRVA)
      return FunctionRVA.takeError();
    auto FunctionEndRVA =
        toImageRVA(Input.FunctionEndVA, MachHeaderVA,
                   MergeInputKind::GeneratedRecord, I, "function end");
    if (!FunctionEndRVA)
      return FunctionEndRVA.takeError();
    auto OwnerRVA =
        toImageRVA(Input.OwnerVA, MachHeaderVA, MergeInputKind::GeneratedRecord,
                   I, "function owner");
    if (!OwnerRVA)
      return OwnerRVA.takeError();

    MergedRecord Record;
    Record.FunctionRVA = *FunctionRVA;
    Record.FunctionEndRVA = *FunctionEndRVA;
    Record.FunctionRangeId = Input.FunctionRangeId;
    Record.OwnerSymbol = Input.OwnerSymbol;
    Record.OwnerVA = Input.OwnerVA;
    Record.Encoding = Input.Encoding;
    Record.PersonalitySlotRVA = Input.PersonalitySlotRVA;
    if (Input.LSDAVA) {
      auto LSDARVA = toImageRVA(*Input.LSDAVA, MachHeaderVA,
                                MergeInputKind::GeneratedRecord, I, "LSDA");
      if (!LSDARVA)
        return LSDARVA.takeError();
      Record.LSDARVA = *LSDARVA;
    }
    Record.Origin = MachOCompactUnwindRecordOrigin::Generated;
    Record.InputRecordIndex = Input.SourceRecordIndex;
    Result.push_back(std::move(Record));
  }

  for (size_t I = 1; I < Result.size(); ++I) {
    if (Result[I].FunctionRVA <= Result[I - 1].FunctionRVA ||
        Result[I].FunctionRVA < Result[I - 1].FunctionEndRVA)
      return mergeError(MergeFailure::InvalidGeneratedRecord,
                        MergeInputKind::GeneratedRecord, I,
                        "records are duplicated, unsorted, or overlapping");
  }
  return Result;
}

} // namespace

llvm::Expected<std::vector<MachOCompactUnwindRangeMapping>>
buildMachOCompactUnwindRangeMappings(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<PatchedFunctionEntry> InstalledTrampolines,
    llvm::endianness ByteOrder) {
  using MapFailure = MachOCompactUnwindRangeMapFailure;

  if (Generated.Records.empty())
    return std::vector<MachOCompactUnwindRangeMapping>{};
  if (Generated.TargetArch != TargetArch || Generated.ByteOrder != ByteOrder ||
      !machOHeaderMatchesArchitecture(Binary, TargetArch))
    return rangeMapError(MapFailure::ArchitectureMismatch);

  auto RegionOrErr = findMachOCompactUnwindRegion(Binary);
  if (!RegionOrErr)
    return RegionOrErr.takeError();
  if (!*RegionOrErr)
    return rangeMapError(MapFailure::MissingSection);
  const MachOCompactUnwindRegion &Region = **RegionOrErr;
  if (Region.SectionSize > std::numeric_limits<size_t>::max() ||
      !rangeInBounds(Region.SectionFileOff, Region.SectionSize, Binary.size()))
    return rangeMapError(MapFailure::MissingSection,
                         "declared section storage is not representable");

  macho_unwind::CompactUnwindRawParseOptions ParseOptions;
  ParseOptions.ByteOrder = ByteOrder;
  auto Original = macho_unwind::parseCompactUnwindRaw(
      Binary.slice(static_cast<size_t>(Region.SectionFileOff),
                   static_cast<size_t>(Region.SectionSize)),
      ParseOptions);
  if (!Original)
    return Original.takeError();
  auto OriginalTable = normalizeOriginalTable(TargetArch, *Original);
  if (!OriginalTable)
    return OriginalTable.takeError();

  struct SourceOwner {
    bool Used = false;
    std::string Symbol;
    uint64_t VA = 0;
  };
  std::vector<SourceOwner> SourceOwners(OriginalTable->Records.size());
  std::set<uint64_t> RangeIds;
  std::vector<MachOCompactUnwindRangeMapping> Result;
  Result.reserve(Generated.Records.size());
  for (size_t I = 0; I < Generated.Records.size(); ++I) {
    const MachOCompactUnwindRecord &GeneratedRecord = Generated.Records[I];
    if (GeneratedRecord.FunctionRangeId == 0 ||
        GeneratedRecord.OwnerSymbol.empty() ||
        GeneratedRecord.FunctionSymbol.empty() ||
        GeneratedRecord.FunctionVA >= GeneratedRecord.FunctionEndVA ||
        !RangeIds.insert(GeneratedRecord.FunctionRangeId).second)
      return rangeMapError(MapFailure::InvalidFunctionRangeIdentity,
                           GeneratedRecord.SourceRecordIndex,
                           llvm::Twine(GeneratedRecord.FunctionRangeId));

    const PatchedFunctionEntry *Trampoline = nullptr;
    size_t TrampolineCount = 0;
    for (const PatchedFunctionEntry &Installed : InstalledTrampolines) {
      if (Installed.OwnerVA != GeneratedRecord.OwnerVA ||
          Installed.OwnerSymbol != GeneratedRecord.OwnerSymbol)
        continue;
      if (!Trampoline)
        Trampoline = &Installed;
      ++TrampolineCount;
    }
    if (TrampolineCount == 0) {
      const bool HasOwnerVA = llvm::any_of(
          InstalledTrampolines, [&](const PatchedFunctionEntry &Installed) {
            return Installed.OwnerVA == GeneratedRecord.OwnerVA;
          });
      return rangeMapError(HasOwnerVA ? MapFailure::TrampolineSymbolMismatch
                                      : MapFailure::MissingTrampoline,
                           GeneratedRecord.SourceRecordIndex,
                           GeneratedRecord.OwnerSymbol);
    }
    if (TrampolineCount != 1)
      return rangeMapError(MapFailure::AmbiguousTrampoline,
                           GeneratedRecord.SourceRecordIndex,
                           GeneratedRecord.OwnerSymbol);
    if (Trampoline->OriginalVA < Region.MachHeaderVA ||
        Trampoline->OriginalVA - Region.MachHeaderVA >
            std::numeric_limits<uint32_t>::max())
      return rangeMapError(MapFailure::MissingSourceRange,
                           GeneratedRecord.SourceRecordIndex,
                           "source is outside the image-relative range");

    const uint32_t SourceRVA =
        static_cast<uint32_t>(Trampoline->OriginalVA - Region.MachHeaderVA);
    std::optional<size_t> SourceIndex;
    size_t SourceCount = 0;
    for (size_t S = 0; S < OriginalTable->Records.size(); ++S) {
      if (OriginalTable->Records[S].FunctionRVA != SourceRVA)
        continue;
      if (!SourceIndex)
        SourceIndex = S;
      ++SourceCount;
    }
    if (SourceCount == 0)
      return rangeMapError(MapFailure::MissingSourceRange,
                           GeneratedRecord.SourceRecordIndex,
                           GeneratedRecord.OwnerSymbol);
    if (SourceCount != 1)
      return rangeMapError(MapFailure::AmbiguousSourceRange,
                           GeneratedRecord.SourceRecordIndex,
                           GeneratedRecord.OwnerSymbol);
    SourceOwner &BoundOwner = SourceOwners[*SourceIndex];
    if (BoundOwner.Used && (BoundOwner.Symbol != GeneratedRecord.OwnerSymbol ||
                            BoundOwner.VA != GeneratedRecord.OwnerVA))
      return rangeMapError(MapFailure::CrossOwnerSourceReuse,
                           GeneratedRecord.SourceRecordIndex,
                           GeneratedRecord.OwnerSymbol);
    BoundOwner.Used = true;
    BoundOwner.Symbol = GeneratedRecord.OwnerSymbol;
    BoundOwner.VA = GeneratedRecord.OwnerVA;

    const MergedRecord &Source = OriginalTable->Records[*SourceIndex];
    if (Source.FunctionEndRVA >
        std::numeric_limits<uint64_t>::max() - Region.MachHeaderVA)
      return rangeMapError(MapFailure::MissingSourceRange,
                           GeneratedRecord.SourceRecordIndex,
                           "source range end overflows");

    MachOCompactUnwindRangeMapping Mapping;
    Mapping.SourceVA = Trampoline->OriginalVA;
    Mapping.SourceEndVA = Region.MachHeaderVA + Source.FunctionEndRVA;
    Mapping.DestinationVA = GeneratedRecord.FunctionVA;
    Mapping.DestinationEndVA = GeneratedRecord.FunctionEndVA;
    Mapping.Mode = MachOCompactUnwindRangeMode::NewSegment;
    Mapping.FunctionRangeId = GeneratedRecord.FunctionRangeId;
    Mapping.OwnerSymbol = GeneratedRecord.OwnerSymbol;
    Mapping.OwnerVA = GeneratedRecord.OwnerVA;
    Result.push_back(std::move(Mapping));
  }
  return Result;
}

llvm::Expected<MachOCompactUnwindMergeResult> mergeMachOCompactUnwind(
    Arch TargetArch, uint64_t MachHeaderVA,
    const macho_unwind::CompactUnwindRawSection &Original,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<MachOCompactUnwindRangeMapping> Mappings) {
  using namespace macho_unwind;

  if (!isMergeArchitecture(TargetArch))
    return mergeError(MergeFailure::UnsupportedArchitecture);
  auto OriginalTable = normalizeOriginalTable(TargetArch, Original);
  if (!OriginalTable)
    return OriginalTable.takeError();
  auto GeneratedRecords =
      normalizeGeneratedRecords(TargetArch, MachHeaderVA, Generated);
  if (!GeneratedRecords)
    return GeneratedRecords.takeError();

  struct SourceOwner {
    bool Used = false;
    std::string Symbol;
    uint64_t VA = 0;
  };
  std::vector<SourceOwner> SourceOwners(OriginalTable->Records.size());
  std::vector<uint8_t> GeneratedUse(GeneratedRecords->size(), 0);
  std::vector<bool> RemoveSource(OriginalTable->Records.size(), false);
  std::set<uint64_t> MappingIds;
  for (size_t I = 0; I < Mappings.size(); ++I) {
    const MachOCompactUnwindRangeMapping &Mapping = Mappings[I];
    if (Mapping.FunctionRangeId == 0)
      return mergeError(MergeFailure::MissingFunctionRangeId,
                        MergeInputKind::RangeMapping, I);
    if (!MappingIds.insert(Mapping.FunctionRangeId).second)
      return mergeError(MergeFailure::DuplicateFunctionRangeId,
                        MergeInputKind::RangeMapping, I,
                        llvm::Twine(Mapping.FunctionRangeId));
    if (Mapping.OwnerSymbol.empty())
      return mergeError(MergeFailure::FunctionRangeIdentityMismatch,
                        MergeInputKind::RangeMapping, I,
                        "owner symbol is empty");
    if (Mapping.SourceVA >= Mapping.SourceEndVA ||
        Mapping.DestinationVA >= Mapping.DestinationEndVA)
      return mergeError(MergeFailure::InvalidRangeMapping,
                        MergeInputKind::RangeMapping, I,
                        "range is empty or reversed");

    switch (Mapping.Mode) {
    case MachOCompactUnwindRangeMode::NewSegment:
      if (Mapping.SourceVA == Mapping.DestinationVA)
        return mergeError(MergeFailure::InvalidRangeMapping,
                          MergeInputKind::RangeMapping, I,
                          "new-segment destination has the source address");
      break;
    case MachOCompactUnwindRangeMode::SameVAInPlace:
      if (Mapping.SourceVA != Mapping.DestinationVA)
        return mergeError(MergeFailure::InvalidRangeMapping,
                          MergeInputKind::RangeMapping, I,
                          "inplace destination does not keep the source VA");
      break;
    default:
      return mergeError(MergeFailure::InvalidRangeMapping,
                        MergeInputKind::RangeMapping, I, "unknown mode");
    }

    auto SourceRVA =
        toImageRVA(Mapping.SourceVA, MachHeaderVA, MergeInputKind::RangeMapping,
                   I, "source start");
    if (!SourceRVA)
      return SourceRVA.takeError();
    auto SourceEndRVA =
        toImageRVA(Mapping.SourceEndVA, MachHeaderVA,
                   MergeInputKind::RangeMapping, I, "source end");
    if (!SourceEndRVA)
      return SourceEndRVA.takeError();
    auto DestinationRVA =
        toImageRVA(Mapping.DestinationVA, MachHeaderVA,
                   MergeInputKind::RangeMapping, I, "destination start");
    if (!DestinationRVA)
      return DestinationRVA.takeError();
    auto DestinationEndRVA =
        toImageRVA(Mapping.DestinationEndVA, MachHeaderVA,
                   MergeInputKind::RangeMapping, I, "destination end");
    if (!DestinationEndRVA)
      return DestinationEndRVA.takeError();

    std::optional<size_t> SourceIndex;
    for (size_t S = 0; S < OriginalTable->Records.size(); ++S) {
      const MergedRecord &Record = OriginalTable->Records[S];
      if (Record.FunctionRVA == *SourceRVA &&
          Record.FunctionEndRVA == *SourceEndRVA) {
        SourceIndex = S;
        break;
      }
    }
    if (!SourceIndex)
      return mergeError(MergeFailure::SourceRangeNotExact,
                        MergeInputKind::RangeMapping, I);
    SourceOwner &BoundOwner = SourceOwners[*SourceIndex];
    if (BoundOwner.Used && (BoundOwner.Symbol != Mapping.OwnerSymbol ||
                            BoundOwner.VA != Mapping.OwnerVA))
      return mergeError(MergeFailure::CrossOwnerSourceReuse,
                        MergeInputKind::RangeMapping, I);
    BoundOwner.Used = true;
    BoundOwner.Symbol = Mapping.OwnerSymbol;
    BoundOwner.VA = Mapping.OwnerVA;

    std::optional<size_t> GeneratedIndex;
    for (size_t G = 0; G < GeneratedRecords->size(); ++G) {
      const MergedRecord &Record = (*GeneratedRecords)[G];
      if (Record.FunctionRangeId == Mapping.FunctionRangeId) {
        GeneratedIndex = G;
        break;
      }
    }
    if (!GeneratedIndex)
      return mergeError(MergeFailure::DanglingFunctionRangeId,
                        MergeInputKind::RangeMapping, I);
    const MergedRecord &GeneratedRecord = (*GeneratedRecords)[*GeneratedIndex];
    if (GeneratedRecord.FunctionRVA != *DestinationRVA ||
        GeneratedRecord.FunctionEndRVA != *DestinationEndRVA)
      return mergeError(MergeFailure::DestinationRangeNotExact,
                        MergeInputKind::RangeMapping, I);
    if (GeneratedRecord.OwnerSymbol != Mapping.OwnerSymbol ||
        GeneratedRecord.OwnerVA != Mapping.OwnerVA)
      return mergeError(MergeFailure::FunctionRangeIdentityMismatch,
                        MergeInputKind::RangeMapping, I);
    if (GeneratedUse[*GeneratedIndex] != 0)
      return mergeError(MergeFailure::AmbiguousDestinationRange,
                        MergeInputKind::RangeMapping, I);
    ++GeneratedUse[*GeneratedIndex];

    if (Mapping.Mode == MachOCompactUnwindRangeMode::SameVAInPlace)
      RemoveSource[*SourceIndex] = true;
  }

  for (size_t I = 0; I < GeneratedUse.size(); ++I) {
    if (GeneratedUse[I] == 0)
      return mergeError(MergeFailure::UnmappedGeneratedRange,
                        MergeInputKind::GeneratedRecord, I);
  }

  std::vector<MergedRecord> Records;
  Records.reserve(OriginalTable->Records.size() + GeneratedRecords->size());
  for (size_t I = 0; I < OriginalTable->Records.size(); ++I) {
    if (!RemoveSource[I])
      Records.push_back(OriginalTable->Records[I]);
  }
  Records.insert(Records.end(), GeneratedRecords->begin(),
                 GeneratedRecords->end());
  std::sort(Records.begin(), Records.end(),
            [](const MergedRecord &Left, const MergedRecord &Right) {
              return std::tie(Left.FunctionRVA, Left.FunctionEndRVA,
                              Left.Origin, Left.InputRecordIndex) <
                     std::tie(Right.FunctionRVA, Right.FunctionEndRVA,
                              Right.Origin, Right.InputRecordIndex);
            });
  for (size_t I = 1; I < Records.size(); ++I) {
    if (Records[I].FunctionRVA == Records[I - 1].FunctionRVA)
      return mergeError(MergeFailure::DuplicateMergedRange,
                        MergeInputKind::MergedRecord, I);
    if (Records[I].FunctionRVA < Records[I - 1].FunctionEndRVA)
      return mergeError(MergeFailure::OverlappingMergedRanges,
                        MergeInputKind::MergedRecord, I);
  }

  uint32_t TerminalFunctionRVA = OriginalTable->TerminalFunctionRVA;
  for (const MergedRecord &Record : Records)
    TerminalFunctionRVA = std::max(TerminalFunctionRVA, Record.FunctionEndRVA);

  std::vector<MergedRecord> RecordsWithBoundaries;
  RecordsWithBoundaries.reserve(Records.size() * 2);
  for (const MergedRecord &Record : Records) {
    if (!RecordsWithBoundaries.empty()) {
      const MergedRecord &Previous = RecordsWithBoundaries.back();
      if (Previous.FunctionEndRVA < Record.FunctionRVA) {
        MergedRecord Boundary;
        Boundary.FunctionRVA = Previous.FunctionEndRVA;
        Boundary.FunctionEndRVA = Record.FunctionRVA;
        Boundary.Origin = MachOCompactUnwindRecordOrigin::GapBoundary;
        RecordsWithBoundaries.push_back(std::move(Boundary));
      }
    }
    RecordsWithBoundaries.push_back(Record);
  }
  if (!RecordsWithBoundaries.empty()) {
    const MergedRecord &Last = RecordsWithBoundaries.back();
    if (Last.FunctionEndRVA < TerminalFunctionRVA) {
      MergedRecord Boundary;
      Boundary.FunctionRVA = Last.FunctionEndRVA;
      Boundary.FunctionEndRVA = TerminalFunctionRVA;
      Boundary.Origin = MachOCompactUnwindRecordOrigin::GapBoundary;
      RecordsWithBoundaries.push_back(std::move(Boundary));
    }
  }

  MachOCompactUnwindMergeResult Result;
  Result.TargetArch = TargetArch;
  Result.TerminalFunctionRVA = TerminalFunctionRVA;
  Result.Records = std::move(RecordsWithBoundaries);
  for (size_t I = 0; I < Result.Records.size(); ++I) {
    MergedRecord &Record = Result.Records[I];
    Record.Encoding &= ~kPersonalityMask;
    if (!Record.PersonalitySlotRVA)
      continue;
    auto It =
        std::find(Result.PersonalitySlotRVAs.begin(),
                  Result.PersonalitySlotRVAs.end(), *Record.PersonalitySlotRVA);
    if (It == Result.PersonalitySlotRVAs.end()) {
      if (Result.PersonalitySlotRVAs.size() == 3)
        return mergeError(MergeFailure::TooManyPersonalities,
                          MergeInputKind::MergedRecord, I);
      Result.PersonalitySlotRVAs.push_back(*Record.PersonalitySlotRVA);
      It = std::prev(Result.PersonalitySlotRVAs.end());
    }
    const uint32_t PersonalityIndex =
        static_cast<uint32_t>(
            std::distance(Result.PersonalitySlotRVAs.begin(), It)) +
        1;
    Record.Encoding |= PersonalityIndex << kPersonalityShift;
  }
  return Result;
}

namespace {

using EncodeFailure = MachOCompactUnwindEncodeFailure;
using EncodeInputKind = MachOCompactUnwindEncodeInputKind;

constexpr uint64_t kFinalHeaderSize = 28;
constexpr uint64_t kFirstLevelIndexEntrySize = 12;
constexpr uint64_t kLSDAIndexEntrySize = 8;
constexpr uint64_t kRegularPageSize = 4096;
constexpr uint64_t kRegularPageHeaderSize = 8;
constexpr uint64_t kRegularEntrySize = 8;
constexpr uint64_t kRegularEntriesPerPage =
    (kRegularPageSize - kRegularPageHeaderSize) / kRegularEntrySize;

static_assert(kRegularEntriesPerPage == 511,
              "regular compact-unwind page capacity is fixed by its ABI");

llvm::Error encodeError(EncodeFailure Reason, EncodeInputKind InputKind,
                        uint64_t InputIndex, const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindEncodeError>(
      Reason, InputKind, InputIndex, Detail.str());
}

llvm::Error encodeError(EncodeFailure Reason, const llvm::Twine &Detail = {}) {
  return encodeError(Reason, EncodeInputKind::None,
                     MachOCompactUnwindEncodeError::NoInput, Detail);
}

bool checkedAdd(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

bool checkedMultiply(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Left != 0 && Right > std::numeric_limits<uint64_t>::max() / Left)
    return false;
  Result = Left * Right;
  return true;
}

struct RegularEncodeLayout {
  uint32_t CommonEncodingsOffset = 0;
  uint32_t PersonalityOffset = 0;
  uint32_t IndexOffset = 0;
  uint32_t IndexCount = 0;
  uint32_t LSDAOffset = 0;
  uint32_t PagesOffset = 0;
  uint32_t PageCount = 0;
  uint64_t LSDAEntryCount = 0;
  size_t TotalSize = 0;
};

llvm::Expected<RegularEncodeLayout>
planRegularLayout(const MachOCompactUnwindMergeResult &Input,
                  uint64_t LSDAEntryCount) {
  RegularEncodeLayout Layout;
  Layout.CommonEncodingsOffset = kFinalHeaderSize;
  Layout.PersonalityOffset = kFinalHeaderSize;
  Layout.LSDAEntryCount = LSDAEntryCount;

  const uint64_t RecordCount = Input.Records.size();
  const uint64_t PageCount = 1 + (RecordCount - 1) / kRegularEntriesPerPage;
  if (PageCount >= std::numeric_limits<uint32_t>::max())
    return encodeError(EncodeFailure::SectionSizeOverflow,
                       "first-level index count overflows");
  Layout.PageCount = static_cast<uint32_t>(PageCount);
  Layout.IndexCount = static_cast<uint32_t>(PageCount + 1);

  uint64_t Cursor = kFinalHeaderSize;
  uint64_t ByteCount = 0;
  if (!checkedMultiply(Input.PersonalitySlotRVAs.size(), sizeof(uint32_t),
                       ByteCount) ||
      !checkedAdd(Cursor, ByteCount, Cursor) ||
      Cursor > std::numeric_limits<uint32_t>::max())
    return encodeError(EncodeFailure::SectionSizeOverflow,
                       "personality array end overflows");
  Layout.IndexOffset = static_cast<uint32_t>(Cursor);

  if (!checkedMultiply(Layout.IndexCount, kFirstLevelIndexEntrySize,
                       ByteCount) ||
      !checkedAdd(Cursor, ByteCount, Cursor) ||
      Cursor > std::numeric_limits<uint32_t>::max())
    return encodeError(EncodeFailure::SectionSizeOverflow,
                       "first-level index end overflows");
  Layout.LSDAOffset = static_cast<uint32_t>(Cursor);

  if (!checkedMultiply(LSDAEntryCount, kLSDAIndexEntrySize, ByteCount) ||
      !checkedAdd(Cursor, ByteCount, Cursor) ||
      Cursor > std::numeric_limits<uint32_t>::max())
    return encodeError(EncodeFailure::SectionSizeOverflow,
                       "LSDA index end overflows");
  Layout.PagesOffset = static_cast<uint32_t>(Cursor);

  if (!checkedMultiply(PageCount, kRegularPageSize, ByteCount) ||
      !checkedAdd(Cursor, ByteCount, Cursor) ||
      Cursor > std::numeric_limits<uint32_t>::max() ||
      Cursor > std::numeric_limits<size_t>::max())
    return encodeError(EncodeFailure::SectionSizeOverflow,
                       "second-level page array end overflows");
  Layout.TotalSize = static_cast<size_t>(Cursor);
  return Layout;
}

llvm::Expected<uint64_t>
validateRegularEncodeInput(const MachOCompactUnwindMergeResult &Input,
                           llvm::endianness ByteOrder) {
  using namespace macho_unwind;

  if (!isMergeArchitecture(Input.TargetArch))
    return encodeError(EncodeFailure::UnsupportedArchitecture);
  if (ByteOrder != llvm::endianness::little)
    return encodeError(
        EncodeFailure::UnsupportedEndianness,
        "x86, x86-64, ARM, and AArch64 Mach-O targets are little-endian");
  if (Input.Records.empty())
    return encodeError(EncodeFailure::EmptyRecords);
  if (Input.PersonalitySlotRVAs.size() > 3)
    return encodeError(EncodeFailure::TooManyPersonalities);

  for (size_t I = 0; I < Input.PersonalitySlotRVAs.size(); ++I) {
    for (size_t J = 0; J < I; ++J) {
      if (Input.PersonalitySlotRVAs[J] == Input.PersonalitySlotRVAs[I])
        return encodeError(EncodeFailure::DuplicatePersonalitySlot,
                           EncodeInputKind::PersonalitySlot, I);
    }
  }

  uint64_t LSDAEntryCount = 0;
  for (size_t I = 0; I < Input.Records.size(); ++I) {
    const MachOCompactUnwindMergedRecord &Record = Input.Records[I];
    if (Record.FunctionRVA >= Record.FunctionEndRVA ||
        Record.FunctionEndRVA > Input.TerminalFunctionRVA)
      return encodeError(EncodeFailure::InvalidRecordRange,
                         EncodeInputKind::Record, I);
    if (I != 0) {
      const MachOCompactUnwindMergedRecord &Previous = Input.Records[I - 1];
      if (Record.FunctionRVA <= Previous.FunctionRVA ||
          Record.FunctionRVA < Previous.FunctionEndRVA)
        return encodeError(EncodeFailure::UnsortedOrOverlappingRecords,
                           EncodeInputKind::Record, I);
      if (Previous.FunctionEndRVA != Record.FunctionRVA)
        return encodeError(EncodeFailure::NonContiguousRecords,
                           EncodeInputKind::Record, I - 1,
                           "record end differs from the next start");
    }

    if (llvm::Error Error = validateRawEncoding(
            Input.TargetArch, Record.Encoding,
            MachOCompactUnwindMergeInputKind::MergedRecord, I,
            /*AllowDwarfLinearSearch=*/
            Record.Origin == MachOCompactUnwindRecordOrigin::Original)) {
      llvm::consumeError(std::move(Error));
      return encodeError(EncodeFailure::UnsupportedEncoding,
                         EncodeInputKind::Record, I);
    }

    const uint32_t PersonalityIndex =
        (Record.Encoding & kPersonalityMask) >> kPersonalityShift;
    if (PersonalityIndex > Input.PersonalitySlotRVAs.size())
      return encodeError(EncodeFailure::PersonalityIndexOutOfRange,
                         EncodeInputKind::Record, I);
    if ((PersonalityIndex == 0) != !Record.PersonalitySlotRVA.has_value())
      return encodeError(EncodeFailure::PersonalityEncodingMismatch,
                         EncodeInputKind::Record, I,
                         "index presence differs from the retained slot");
    if (PersonalityIndex != 0 &&
        Input.PersonalitySlotRVAs[PersonalityIndex - 1] !=
            *Record.PersonalitySlotRVA)
      return encodeError(EncodeFailure::PersonalityEncodingMismatch,
                         EncodeInputKind::Record, I,
                         "index names a different pointer slot");

    const bool EncodingHasLSDA = (Record.Encoding & kHasLSDA) != 0;
    if (EncodingHasLSDA != Record.LSDARVA.has_value())
      return encodeError(EncodeFailure::LSDAEncodingMismatch,
                         EncodeInputKind::Record, I);
    LSDAEntryCount += Record.LSDARVA.has_value();
  }

  if (Input.Records.back().FunctionEndRVA != Input.TerminalFunctionRVA)
    return encodeError(EncodeFailure::InvalidTerminalBoundary,
                       EncodeInputKind::Record, Input.Records.size() - 1);
  return LSDAEntryCount;
}

void writeU16LE(std::vector<uint8_t> &Bytes, uint64_t Offset, uint16_t Value) {
  llvm::support::endian::write16le(Bytes.data() + Offset, Value);
}

void writeU32LE(std::vector<uint8_t> &Bytes, uint64_t Offset, uint32_t Value) {
  llvm::support::endian::write32le(Bytes.data() + Offset, Value);
}

void writeRegularSection(const MachOCompactUnwindMergeResult &Input,
                         const RegularEncodeLayout &Layout,
                         std::vector<uint8_t> &Bytes) {
  using namespace macho_unwind;

  writeU32LE(Bytes, 0, kUnwindSectionVersion);
  writeU32LE(Bytes, 4, Layout.CommonEncodingsOffset);
  writeU32LE(Bytes, 8, 0);
  writeU32LE(Bytes, 12, Layout.PersonalityOffset);
  writeU32LE(Bytes, 16,
             static_cast<uint32_t>(Input.PersonalitySlotRVAs.size()));
  writeU32LE(Bytes, 20, Layout.IndexOffset);
  writeU32LE(Bytes, 24, Layout.IndexCount);

  for (size_t I = 0; I < Input.PersonalitySlotRVAs.size(); ++I)
    writeU32LE(Bytes, uint64_t(Layout.PersonalityOffset) + I * sizeof(uint32_t),
               Input.PersonalitySlotRVAs[I]);

  uint64_t PreviousLSDACount = 0;
  for (uint64_t PageIndex = 0; PageIndex < Layout.PageCount; ++PageIndex) {
    const uint64_t FirstRecord = PageIndex * kRegularEntriesPerPage;
    const uint64_t IndexOffset =
        uint64_t(Layout.IndexOffset) + PageIndex * kFirstLevelIndexEntrySize;
    const uint64_t PageOffset =
        uint64_t(Layout.PagesOffset) + PageIndex * kRegularPageSize;
    writeU32LE(Bytes, IndexOffset, Input.Records[FirstRecord].FunctionRVA);
    writeU32LE(Bytes, IndexOffset + 4, static_cast<uint32_t>(PageOffset));
    writeU32LE(Bytes, IndexOffset + 8,
               Layout.LSDAOffset + static_cast<uint32_t>(PreviousLSDACount *
                                                         kLSDAIndexEntrySize));

    const uint64_t PageEnd = std::min<uint64_t>(
        Input.Records.size(), FirstRecord + kRegularEntriesPerPage);
    for (uint64_t I = FirstRecord; I < PageEnd; ++I)
      PreviousLSDACount += Input.Records[I].LSDARVA.has_value();
  }

  const uint64_t SentinelOffset =
      uint64_t(Layout.IndexOffset) +
      uint64_t(Layout.PageCount) * kFirstLevelIndexEntrySize;
  writeU32LE(Bytes, SentinelOffset, Input.TerminalFunctionRVA);
  writeU32LE(Bytes, SentinelOffset + 4, 0);
  writeU32LE(Bytes, SentinelOffset + 8, Layout.PagesOffset);

  uint64_t LSDAIndex = 0;
  for (const MachOCompactUnwindMergedRecord &Record : Input.Records) {
    if (!Record.LSDARVA)
      continue;
    const uint64_t Offset =
        uint64_t(Layout.LSDAOffset) + LSDAIndex * kLSDAIndexEntrySize;
    writeU32LE(Bytes, Offset, Record.FunctionRVA);
    writeU32LE(Bytes, Offset + 4, *Record.LSDARVA);
    ++LSDAIndex;
  }

  for (uint64_t PageIndex = 0; PageIndex < Layout.PageCount; ++PageIndex) {
    const uint64_t FirstRecord = PageIndex * kRegularEntriesPerPage;
    const uint64_t PageEnd = std::min<uint64_t>(
        Input.Records.size(), FirstRecord + kRegularEntriesPerPage);
    const uint64_t EntryCount = PageEnd - FirstRecord;
    const uint64_t PageOffset =
        uint64_t(Layout.PagesOffset) + PageIndex * kRegularPageSize;
    writeU32LE(Bytes, PageOffset, kSecondLevelRegular);
    writeU16LE(Bytes, PageOffset + 4, kRegularPageHeaderSize);
    writeU16LE(Bytes, PageOffset + 6, static_cast<uint16_t>(EntryCount));
    for (uint64_t I = 0; I < EntryCount; ++I) {
      const MachOCompactUnwindMergedRecord &Record =
          Input.Records[FirstRecord + I];
      const uint64_t EntryOffset =
          PageOffset + kRegularPageHeaderSize + I * kRegularEntrySize;
      writeU32LE(Bytes, EntryOffset, Record.FunctionRVA);
      writeU32LE(Bytes, EntryOffset + 4, Record.Encoding);
    }
  }
}

llvm::Error
verifyRegularRoundTrip(const MachOCompactUnwindMergeResult &Input,
                       const RegularEncodeLayout &Layout,
                       const macho_unwind::CompactUnwindRawSection &Raw,
                       llvm::ArrayRef<uint8_t> Bytes) {
  using namespace macho_unwind;

  const CompactUnwindRawHeader &Header = Raw.Header;
  if (Raw.OriginalBytes.size() != Bytes.size() ||
      !std::equal(Raw.OriginalBytes.begin(), Raw.OriginalBytes.end(),
                  Bytes.begin(), Bytes.end()) ||
      Header.Version != kUnwindSectionVersion ||
      Header.CommonEncodingsSectionOffset != Layout.CommonEncodingsOffset ||
      Header.CommonEncodingsCount != 0 ||
      Header.PersonalityArraySectionOffset != Layout.PersonalityOffset ||
      Header.PersonalityArrayCount != Input.PersonalitySlotRVAs.size() ||
      Header.IndexSectionOffset != Layout.IndexOffset ||
      Header.IndexCount != Layout.IndexCount || !Raw.CommonEncodings.empty() ||
      Raw.PersonalitySlotOffsets != Input.PersonalitySlotRVAs ||
      Raw.Index.size() != Layout.IndexCount ||
      Raw.Pages.size() != Layout.PageCount ||
      Raw.LSDAEntries.size() != Layout.LSDAEntryCount)
    return encodeError(EncodeFailure::SemanticRoundTripMismatch,
                       "header or side-table shape changed");

  size_t FlattenedRecord = 0;
  size_t FlattenedLSDA = 0;
  for (size_t PageIndex = 0; PageIndex < Raw.Pages.size(); ++PageIndex) {
    const size_t FirstRecord = PageIndex * kRegularEntriesPerPage;
    const size_t ExpectedEntryCount = std::min<size_t>(
        Input.Records.size() - FirstRecord, kRegularEntriesPerPage);
    const uint32_t ExpectedPageOffset = static_cast<uint32_t>(
        uint64_t(Layout.PagesOffset) + PageIndex * kRegularPageSize);
    const CompactUnwindRawIndexEntry &Index = Raw.Index[PageIndex];
    const CompactUnwindRawPage &Page = Raw.Pages[PageIndex];
    if (Index.FunctionOffset != Input.Records[FirstRecord].FunctionRVA ||
        Index.SecondLevelPageSectionOffset != ExpectedPageOffset ||
        Index.LSDAIndexArraySectionOffset !=
            Layout.LSDAOffset + FlattenedLSDA * kLSDAIndexEntrySize ||
        Page.SectionOffset != ExpectedPageOffset ||
        Page.Kind != kSecondLevelRegular ||
        Page.EntryPageOffset != kRegularPageHeaderSize ||
        Page.EntryCount != ExpectedEntryCount ||
        Page.EncodingsPageOffset != 0 || Page.EncodingsCount != 0 ||
        !Page.LocalEncodings.empty() || !Page.CompressedEntries.empty() ||
        Page.RegularEntries.size() != ExpectedEntryCount)
      return encodeError(EncodeFailure::SemanticRoundTripMismatch,
                         EncodeInputKind::Record, FirstRecord,
                         "first- or second-level page fields changed");

    for (const CompactUnwindRawRegularEntry &Entry : Page.RegularEntries) {
      const MachOCompactUnwindMergedRecord &Record =
          Input.Records[FlattenedRecord];
      if (Entry.FunctionOffset != Record.FunctionRVA ||
          Entry.Encoding != Record.Encoding)
        return encodeError(EncodeFailure::SemanticRoundTripMismatch,
                           EncodeInputKind::Record, FlattenedRecord,
                           "recipe changed after strict parsing");
      if (Record.LSDARVA) {
        const CompactUnwindRawLSDAEntry &LSDA = Raw.LSDAEntries[FlattenedLSDA];
        if (LSDA.FunctionOffset != Record.FunctionRVA ||
            LSDA.LSDAOffset != *Record.LSDARVA)
          return encodeError(EncodeFailure::SemanticRoundTripMismatch,
                             EncodeInputKind::Record, FlattenedRecord,
                             "LSDA entry changed after strict parsing");
        ++FlattenedLSDA;
      }
      ++FlattenedRecord;
    }
  }

  const CompactUnwindRawIndexEntry &Sentinel = Raw.Index.back();
  if (FlattenedRecord != Input.Records.size() ||
      FlattenedLSDA != Layout.LSDAEntryCount ||
      Sentinel.FunctionOffset != Input.TerminalFunctionRVA ||
      Sentinel.SecondLevelPageSectionOffset != 0 ||
      Sentinel.LSDAIndexArraySectionOffset != Layout.PagesOffset)
    return encodeError(EncodeFailure::SemanticRoundTripMismatch,
                       "terminal sentinel or flattened entry count changed");
  return llvm::Error::success();
}

} // namespace

llvm::Expected<std::vector<uint8_t>>
encodeMachOCompactUnwindRegular(const MachOCompactUnwindMergeResult &Input,
                                llvm::endianness ByteOrder) {
  auto LSDAEntryCount = validateRegularEncodeInput(Input, ByteOrder);
  if (!LSDAEntryCount)
    return LSDAEntryCount.takeError();
  auto Layout = planRegularLayout(Input, *LSDAEntryCount);
  if (!Layout)
    return Layout.takeError();

  std::vector<uint8_t> Bytes(Layout->TotalSize, 0);
  writeRegularSection(Input, *Layout, Bytes);

  auto Parsed = macho_unwind::parseCompactUnwindRaw(Bytes);
  if (!Parsed)
    return encodeError(EncodeFailure::StrictRoundTripFailure,
                       llvm::toString(Parsed.takeError()));
  if (llvm::Error Error =
          verifyRegularRoundTrip(Input, *Layout, *Parsed, Bytes))
    return std::move(Error);
  return Bytes;
}

namespace {

using InstallDisposition = MachOCompactUnwindInstallDisposition;
using InstallFailure = MachOCompactUnwindInstallFailure;

llvm::Error installError(InstallFailure Reason, uint64_t RequiredBytes = 0,
                         uint64_t AvailableBytes = 0,
                         const llvm::Twine &Detail = {}) {
  return llvm::make_error<MachOCompactUnwindInstallError>(
      Reason, RequiredBytes, AvailableBytes, Detail.str());
}

bool sameCompactUnwindRegion(const MachOCompactUnwindRegion &Left,
                             const MachOCompactUnwindRegion &Right) {
  return Left.Is64 == Right.Is64 && Left.MachHeaderVA == Right.MachHeaderVA &&
         Left.SectionVA == Right.SectionVA &&
         Left.SectionFileOff == Right.SectionFileOff &&
         Left.SectionSize == Right.SectionSize &&
         Left.LimitFileOff == Right.LimitFileOff &&
         Left.SectionHeaderOff == Right.SectionHeaderOff;
}

bool machOHeaderMatchesArchitecture(llvm::ArrayRef<uint8_t> Binary,
                                    Arch TargetArch) {
  using namespace llvm::MachO;

  const MachOHeaderInfo Header = parseMachOHeader(Binary.data(), Binary.size());
  if (Header.HeaderSize == 0)
    return false;
  const uint32_t CPUType =
      Header.Is64
          ? reinterpret_cast<const mach_header_64 *>(Binary.data())->cputype
          : reinterpret_cast<const mach_header *>(Binary.data())->cputype;
  switch (TargetArch) {
  case Arch::X86:
    return !Header.Is64 && CPUType == CPU_TYPE_X86;
  case Arch::X64:
    return Header.Is64 && CPUType == CPU_TYPE_X86_64;
  case Arch::ARM:
    return !Header.Is64 && CPUType == CPU_TYPE_ARM;
  case Arch::AArch64:
    return Header.Is64 && CPUType == CPU_TYPE_ARM64;
  default:
    return false;
  }
}

uint64_t rawRecipeCount(const macho_unwind::CompactUnwindRawSection &Original) {
  uint64_t Count = 0;
  for (const macho_unwind::CompactUnwindRawPage &Page : Original.Pages)
    Count += Page.Kind == macho_unwind::kSecondLevelRegular
                 ? Page.RegularEntries.size()
                 : Page.CompressedEntries.size();
  return Count;
}

struct InstalledRecipe {
  uint32_t FunctionRVA = 0;
  uint32_t Encoding = 0;
};

llvm::Error verifyInstalledSemantics(
    const MachOCompactUnwindMergeResult &Expected,
    const macho_unwind::CompactUnwindRawSection &Installed) {
  using namespace macho_unwind;

  if (Installed.Index.empty())
    return llvm::createStringError(llvm::errc::invalid_argument,
                                   "installed table has no sentinel");
  if (Installed.PersonalitySlotOffsets != Expected.PersonalitySlotRVAs)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "installed personality array differs from the merge result");
  if (Installed.Index.back().FunctionOffset != Expected.TerminalFunctionRVA)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "installed terminal boundary differs from the merge result");

  std::vector<InstalledRecipe> Recipes;
  Recipes.reserve(Expected.Records.size());
  for (const CompactUnwindRawPage &Page : Installed.Pages) {
    for (const CompactUnwindRawRegularEntry &Entry : Page.RegularEntries)
      Recipes.push_back({Entry.FunctionOffset, Entry.Encoding});
    for (const CompactUnwindRawCompressedEntry &Entry : Page.CompressedEntries)
      Recipes.push_back({Entry.FunctionOffset, Entry.Encoding});
  }
  if (Recipes.size() != Expected.Records.size())
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "installed recipe count differs from the merge result");

  std::map<uint32_t, uint32_t> LSDAs;
  for (const CompactUnwindRawLSDAEntry &Entry : Installed.LSDAEntries)
    LSDAs.emplace(Entry.FunctionOffset, Entry.LSDAOffset);
  const size_t ExpectedLSDACount = static_cast<size_t>(
      std::count_if(Expected.Records.begin(), Expected.Records.end(),
                    [](const MachOCompactUnwindMergedRecord &Record) {
                      return Record.LSDARVA.has_value();
                    }));
  if (LSDAs.size() != ExpectedLSDACount)
    return llvm::createStringError(
        llvm::errc::invalid_argument,
        "installed LSDA count differs from the merge result");

  for (size_t I = 0; I < Expected.Records.size(); ++I) {
    const MachOCompactUnwindMergedRecord &ExpectedRecord = Expected.Records[I];
    const InstalledRecipe &InstalledRecord = Recipes[I];
    const uint32_t InstalledEnd = I + 1 == Recipes.size()
                                      ? Installed.Index.back().FunctionOffset
                                      : Recipes[I + 1].FunctionRVA;
    if (InstalledRecord.FunctionRVA != ExpectedRecord.FunctionRVA ||
        InstalledEnd != ExpectedRecord.FunctionEndRVA ||
        InstalledRecord.Encoding != ExpectedRecord.Encoding)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "installed recipe range or encoding differs at index %zu", I);

    const uint32_t PersonalityIndex =
        (InstalledRecord.Encoding & kPersonalityMask) >> kPersonalityShift;
    std::optional<uint32_t> InstalledPersonality;
    if (PersonalityIndex != 0) {
      if (PersonalityIndex > Installed.PersonalitySlotOffsets.size())
        return llvm::createStringError(
            llvm::errc::invalid_argument,
            "installed personality index leaves its array at index %zu", I);
      InstalledPersonality =
          Installed.PersonalitySlotOffsets[PersonalityIndex - 1];
    }
    if (InstalledPersonality != ExpectedRecord.PersonalitySlotRVA)
      return llvm::createStringError(
          llvm::errc::invalid_argument,
          "installed personality slot differs at index %zu", I);

    std::optional<uint32_t> InstalledLSDA;
    if (auto It = LSDAs.find(InstalledRecord.FunctionRVA); It != LSDAs.end())
      InstalledLSDA = It->second;
    if (InstalledLSDA != ExpectedRecord.LSDARVA)
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "installed LSDA differs at index %zu", I);
  }
  return llvm::Error::success();
}

bool hasCanonicalNoOpShape(const MachOCompactUnwindInstallPlan &Plan) {
  return !Plan.expectedRegion() && Plan.expectedOriginalBytes().empty() &&
         Plan.encodedBytes().empty() &&
         Plan.expectedSemantics().Records.empty() &&
         Plan.expectedSemantics().PersonalitySlotRVAs.empty() &&
         Plan.rangeMappings().empty() && Plan.originalRecordCount() == 0 &&
         Plan.generatedRecordCount() == 0;
}

bool hasCanonicalRewriteShape(const MachOCompactUnwindInstallPlan &Plan) {
  if (!Plan.expectedRegion() || Plan.expectedSemantics().Records.empty() ||
      Plan.encodedBytes().empty() || Plan.generatedRecordCount() == 0 ||
      Plan.originalRecordCount() == 0 ||
      Plan.rangeMappings().size() != Plan.generatedRecordCount() ||
      Plan.byteOrder() != llvm::endianness::little ||
      !isMergeArchitecture(Plan.expectedSemantics().TargetArch) ||
      Plan.expectedRegion()->SectionSize !=
          Plan.expectedOriginalBytes().size() ||
      Plan.encodedBytes().size() > Plan.expectedRegion()->SectionSize)
    return false;
  uint64_t ExpectedLimit = 0;
  return macho_patch_detail::checkedAdd(Plan.expectedRegion()->SectionFileOff,
                                        Plan.expectedRegion()->SectionSize,
                                        ExpectedLimit) &&
         ExpectedLimit == Plan.expectedRegion()->LimitFileOff;
}

} // namespace

llvm::Expected<MachOCompactUnwindInstallPlan> prepareMachOCompactUnwindInstall(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<MachOCompactUnwindRangeMapping> Mappings,
    llvm::endianness ByteOrder) {
  MachOCompactUnwindInstallPlan Plan;
  Plan.ByteOrder = ByteOrder;
  if (Generated.Records.empty()) {
    if (!Mappings.empty())
      return installError(InstallFailure::UnexpectedMappingsForNoOp);
    return Plan;
  }

  auto RegionOrErr = findMachOCompactUnwindRegion(Binary);
  if (!RegionOrErr)
    return RegionOrErr.takeError();
  if (!*RegionOrErr)
    return installError(InstallFailure::MissingSection);
  const MachOCompactUnwindRegion &Region = **RegionOrErr;
  if (!machOHeaderMatchesArchitecture(Binary, TargetArch))
    return installError(InstallFailure::ArchitectureMismatch);
  if (Region.SectionSize > std::numeric_limits<size_t>::max() ||
      !rangeInBounds(Region.SectionFileOff, Region.SectionSize, Binary.size()))
    return installError(InstallFailure::SectionSizeOverflow);

  const auto SectionBytes =
      Binary.slice(static_cast<size_t>(Region.SectionFileOff),
                   static_cast<size_t>(Region.SectionSize));
  macho_unwind::CompactUnwindRawParseOptions ParseOptions;
  ParseOptions.ByteOrder = ByteOrder;
  auto Original =
      macho_unwind::parseCompactUnwindRaw(SectionBytes, ParseOptions);
  if (!Original)
    return Original.takeError();
  auto Merged = mergeMachOCompactUnwind(TargetArch, Region.MachHeaderVA,
                                        *Original, Generated, Mappings);
  if (!Merged)
    return Merged.takeError();
  auto Encoded = encodeMachOCompactUnwindRegular(*Merged, ByteOrder);
  if (!Encoded)
    return Encoded.takeError();
  if (Encoded->size() > Region.SectionSize)
    return installError(InstallFailure::InsufficientCapacity, Encoded->size(),
                        Region.SectionSize);

  Plan.Disposition = InstallDisposition::RewrittenInPlace;
  Plan.ExpectedRegion = Region;
  Plan.ExpectedOriginalBytes.assign(SectionBytes.begin(), SectionBytes.end());
  Plan.EncodedBytes = std::move(*Encoded);
  Plan.ExpectedSemantics = std::move(*Merged);
  Plan.RangeMappings.assign(Mappings.begin(), Mappings.end());
  Plan.OriginalRecordCount = rawRecipeCount(*Original);
  Plan.GeneratedRecordCount = Generated.Records.size();
  return Plan;
}

llvm::Expected<MachOCompactUnwindInstallReceipt>
applyMachOCompactUnwindInstall(std::vector<uint8_t> &Binary,
                               const MachOCompactUnwindInstallPlan &Plan) {
  MachOCompactUnwindInstallReceipt Receipt;
  Receipt.Disposition = Plan.Disposition;
  if (Plan.Disposition == InstallDisposition::Unchanged) {
    if (!hasCanonicalNoOpShape(Plan))
      return installError(InstallFailure::InvalidPlan);
    return Receipt;
  }
  if (Plan.Disposition != InstallDisposition::RewrittenInPlace ||
      !hasCanonicalRewriteShape(Plan))
    return installError(InstallFailure::InvalidPlan);

  const MachOCompactUnwindRegion &ExpectedRegion = *Plan.ExpectedRegion;
  auto CurrentOrErr = findMachOCompactUnwindRegion(Binary);
  if (!CurrentOrErr)
    return installError(InstallFailure::StaleRegion, 0, 0,
                        llvm::toString(CurrentOrErr.takeError()));
  if (!machOHeaderMatchesArchitecture(Binary,
                                      Plan.ExpectedSemantics.TargetArch))
    return installError(InstallFailure::ArchitectureMismatch);
  if (!*CurrentOrErr ||
      !sameCompactUnwindRegion(**CurrentOrErr, ExpectedRegion))
    return installError(InstallFailure::StaleRegion);
  if (!rangeInBounds(ExpectedRegion.SectionFileOff, ExpectedRegion.SectionSize,
                     Binary.size()))
    return installError(InstallFailure::StaleRegion);

  const auto CurrentBytes = llvm::ArrayRef<uint8_t>(Binary).slice(
      static_cast<size_t>(ExpectedRegion.SectionFileOff),
      static_cast<size_t>(ExpectedRegion.SectionSize));
  if (!std::equal(CurrentBytes.begin(), CurrentBytes.end(),
                  Plan.ExpectedOriginalBytes.begin(),
                  Plan.ExpectedOriginalBytes.end()))
    return installError(InstallFailure::StalePreimage);

  std::vector<uint8_t> Candidate = Binary;
  const size_t SectionOffset =
      static_cast<size_t>(ExpectedRegion.SectionFileOff);
  std::copy(Plan.EncodedBytes.begin(), Plan.EncodedBytes.end(),
            Candidate.begin() + SectionOffset);
  std::fill(Candidate.begin() + SectionOffset + Plan.EncodedBytes.size(),
            Candidate.begin() + SectionOffset +
                static_cast<size_t>(ExpectedRegion.SectionSize),
            uint8_t{0});

  auto InstalledRegionOrErr = findMachOCompactUnwindRegion(Candidate);
  if (!InstalledRegionOrErr)
    return installError(InstallFailure::PostLocateFailure, 0, 0,
                        llvm::toString(InstalledRegionOrErr.takeError()));
  if (!*InstalledRegionOrErr ||
      !sameCompactUnwindRegion(**InstalledRegionOrErr, ExpectedRegion))
    return installError(InstallFailure::PostLocateFailure);

  const auto InstalledBytes = llvm::ArrayRef<uint8_t>(Candidate).slice(
      static_cast<size_t>(ExpectedRegion.SectionFileOff),
      static_cast<size_t>(ExpectedRegion.SectionSize));
  macho_unwind::CompactUnwindRawParseOptions ParseOptions;
  ParseOptions.ByteOrder = Plan.ByteOrder;
  auto Installed =
      macho_unwind::parseCompactUnwindRaw(InstalledBytes, ParseOptions);
  if (!Installed)
    return installError(InstallFailure::PostParseFailure, 0, 0,
                        llvm::toString(Installed.takeError()));
  if (llvm::Error Error =
          verifyInstalledSemantics(Plan.ExpectedSemantics, *Installed))
    return installError(InstallFailure::PostSemanticMismatch, 0, 0,
                        llvm::toString(std::move(Error)));

  Receipt.Region = ExpectedRegion;
  Receipt.EncodedSize = Plan.EncodedBytes.size();
  Receipt.Capacity = ExpectedRegion.SectionSize;
  Receipt.ClearedTailSize =
      ExpectedRegion.SectionSize - Plan.EncodedBytes.size();
  Receipt.OriginalRecordCount = Plan.OriginalRecordCount;
  Receipt.GeneratedRecordCount = Plan.GeneratedRecordCount;
  Receipt.FinalRecordCount = Plan.ExpectedSemantics.Records.size();
  Receipt.RangeMappings = Plan.RangeMappings;
  Binary.swap(Candidate);
  return Receipt;
}

} // namespace neverd
