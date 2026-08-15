//===- MachOCompactUnwindPatch.h - Generated unwind input ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the strict boundary between LLVM's linker-input
/// `__LD,__compact_unwind` records and NeverD's final Mach-O unwind writer.
/// This parser retains symbolic fixup identity; resolved pointer values alone
/// are never treated as evidence for an import or personality routine.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOCOMPACTUNWINDPATCH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOCOMPACTUNWINDPATCH_H

#include "neverd/Common.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace neverd {

struct BinaryImage;
struct CompiledImage;
class MachOEHFrameInstallReceipt;
struct PatchedFunctionEntry;

namespace macho_unwind {
struct CompactUnwindRawSection;
}

/// Stable failures while locating the final `__TEXT,__unwind_info` storage.
/// A well-formed image with no such section is represented by a successful
/// empty optional, never by one of these failures.
enum class MachOCompactUnwindLocateFailure : uint8_t {
  InvalidLoadCommands = 0,
  InvalidFileLayout = 1,
  InvalidHeaderMapping = 2,
  AmbiguousSection = 3,
  InvalidSection = 4,
};

static_assert(static_cast<uint8_t>(
                  MachOCompactUnwindLocateFailure::InvalidLoadCommands) == 0 &&
                  static_cast<uint8_t>(
                      MachOCompactUnwindLocateFailure::InvalidSection) == 4,
              "compact-unwind location diagnostic values are an API contract");

class MachOCompactUnwindLocateError final
    : public llvm::ErrorInfo<MachOCompactUnwindLocateError> {
public:
  static char ID;

  MachOCompactUnwindLocateError(MachOCompactUnwindLocateFailure Reason,
                                std::string Detail = {});

  MachOCompactUnwindLocateFailure reason() const { return Reason; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindLocateFailure Reason;
  std::string Detail;
};

/// Exact file-backed extent of the unique final compact-unwind section.
/// Version 1 deliberately sets LimitFileOff to the declared section end; bytes
/// in an unowned gap are not treated as writable capacity.
struct MachOCompactUnwindRegion {
  bool Is64 = false;
  uint64_t MachHeaderVA = 0;
  uint64_t SectionVA = 0;
  uint64_t SectionFileOff = 0;
  uint64_t SectionSize = 0;
  uint64_t LimitFileOff = 0;
  uint64_t SectionHeaderOff = 0;
};

/// Locate the unique file-backed `__TEXT,__unwind_info` section after a full
/// load-command, segment, section, overlap, and linear VA/file mapping audit.
/// Returns an empty optional only when the Mach-O is structurally valid and no
/// section with that name exists.  Malformed, misplaced, or duplicate sections
/// fail closed with MachOCompactUnwindLocateError.
llvm::Expected<std::optional<MachOCompactUnwindRegion>>
findMachOCompactUnwindRegion(llvm::ArrayRef<uint8_t> Binary);

/// Stable failure categories for generated compact-unwind validation.
enum class MachOCompactUnwindParseFailure : uint8_t {
  InvalidCompiledImage = 0,
  InvalidSourceImage = 1,
  MissingSection = 2,
  AmbiguousSection = 3,
  InvalidSectionStorage = 4,
  UnsupportedPointerWidth = 5,
  ArchitecturePointerWidthMismatch = 6,
  UnsupportedArchitecture = 7,
  UnsupportedEndianness = 8,
  InvalidSectionAlignment = 9,
  SectionTooShort = 10,
  TrailingBytes = 11,
  EmptyRange = 12,
  RangeOverflow = 13,
  FunctionOutsideCode = 14,
  LSDAOutsideGeneratedImage = 15,
  UnsupportedEncoding = 16,
  EncodingFieldMismatch = 17,
  MissingFixup = 18,
  AmbiguousFixup = 19,
  InvalidFixup = 20,
  MissingSymbolValue = 21,
  SymbolValueMismatch = 22,
  MissingPersonalitySlot = 23,
  AmbiguousPersonalitySlot = 24,
  InvalidPersonalitySlot = 25,
  OverlappingRanges = 26,
  MissingFunctionRangeId = 27,
  DuplicateFunctionRangeId = 28,
  DanglingFunctionRangeId = 29,
  FunctionRangeSymbolMismatch = 30,
  FunctionRangeBoundaryMismatch = 31,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindParseFailure::InvalidCompiledImage) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindParseFailure::OverlappingRanges) == 26 &&
        static_cast<uint8_t>(
            MachOCompactUnwindParseFailure::FunctionRangeBoundaryMismatch) ==
            31,
    "compact-unwind diagnostic values are an API contract");

/// A typed parse failure.  Callers inspect reason() and recordIndex(); Detail
/// is explanatory text and is not part of the decision contract.
class MachOCompactUnwindParseError final
    : public llvm::ErrorInfo<MachOCompactUnwindParseError> {
public:
  static char ID;
  static constexpr uint64_t NoRecord = UINT64_MAX;

  MachOCompactUnwindParseError(MachOCompactUnwindParseFailure Reason,
                               uint64_t RecordIndex = NoRecord,
                               std::string Detail = {});

  MachOCompactUnwindParseFailure reason() const { return Reason; }
  uint64_t recordIndex() const { return RecordIndex; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindParseFailure Reason;
  uint64_t RecordIndex;
  std::string Detail;
};

/// One normalized linker-input row.  Function ranges are half-open.  Empty
/// personality/LSDA symbols mean that the source row had no such field.
struct MachOCompactUnwindRecord {
  /// Exact compiler-authenticated fragment identity and its public owner.
  /// FunctionSymbol remains the private MC begin label and is not expected in
  /// CompiledImage::SymbolAddrs.
  uint64_t FunctionRangeId = 0;
  std::string OwnerSymbol;
  uint64_t OwnerVA = 0;
  uint64_t FunctionVA = 0;
  uint64_t FunctionEndVA = 0;
  uint32_t RangeLength = 0;
  uint32_t Encoding = 0;
  std::string FunctionSymbol;
  std::string PersonalitySymbol;
  std::optional<uint32_t> PersonalitySlotRVA;
  std::string LSDASymbol;
  std::optional<uint64_t> LSDAVA;
  uint64_t SourceRecordIndex = 0;
};

struct MachOCompactUnwindRecords {
  Arch TargetArch = Arch::Unknown;
  uint8_t PointerWidth = 0;
  llvm::endianness ByteOrder = llvm::endianness::little;
  /// Sorted by (FunctionVA, FunctionEndVA).  Adjacent and disjoint fragments
  /// of one source function remain separate rows.
  std::vector<MachOCompactUnwindRecord> Records;
};

/// Revalidate the architecture-specific linker-input encoding and its
/// personality/LSDA field shape.  This is shared by the raw parser and any
/// later consumer that receives retained generated records.
llvm::Error validateGeneratedMachOCompactUnwindRecordEncoding(
    Arch TargetArch, const MachOCompactUnwindRecord &Record);

/// Strictly parse LLVM-generated linker-input compact-unwind rows retained in
/// \p Compiled.  Pointer width comes from \p SourceImage; byte order is
/// explicit because neither CompiledImage nor BinaryImage silently guesses it.
/// \p MachHeaderVA is the authenticated virtual address of the Mach header in
/// the binary being patched; personality-slot RVAs are always based on this
/// value, never re-derived from mutable loader context.
///
/// Every symbolic pointer field must have one direct, pointer-width fixup at
/// its exact section-relative offset.  A personality fixup is resolved only by
/// exact symbolic identity through SourceImage.ImportPtrSlots, yielding the
/// image-relative pointer-slot offset required by final `__unwind_info`.
/// Nothing is mutated on either success or failure.
llvm::Expected<MachOCompactUnwindRecords> parseGeneratedMachOCompactUnwind(
    const CompiledImage &Compiled, const BinaryImage &SourceImage,
    uint64_t MachHeaderVA, llvm::endianness ByteOrder);

/// Stable failures while binding linker-input DWARF fallback rows to their
/// installed `__eh_frame` FDEs.
enum class MachOCompactUnwindDwarfBindFailure : uint8_t {
  UnsupportedArchitecture = 0,
  PrepopulatedDwarfOffset = 1,
  MissingFDE = 2,
  AmbiguousFDE = 3,
  FDERangeMismatch = 4,
  FDERecordAddressUnderflow = 5,
  ZeroFDESectionOffset = 6,
  FDESectionOffsetOverflow = 7,
  MissingInstallReceipt = 8,
  SymbolIdentityMismatch = 9,
  MissingFunctionRange = 10,
  FunctionRangeIdentityMismatch = 11,
  OwnerIdentityMismatch = 12,
  ReceiptTargetMismatch = 13,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindDwarfBindFailure::UnsupportedArchitecture) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindDwarfBindFailure::FDESectionOffsetOverflow) ==
            7 &&
        static_cast<uint8_t>(
            MachOCompactUnwindDwarfBindFailure::SymbolIdentityMismatch) == 9 &&
        static_cast<uint8_t>(
            MachOCompactUnwindDwarfBindFailure::OwnerIdentityMismatch) == 12 &&
        static_cast<uint8_t>(
            MachOCompactUnwindDwarfBindFailure::ReceiptTargetMismatch) == 13,
    "compact-unwind DWARF binding diagnostic values are an API contract");

class MachOCompactUnwindDwarfBindError final
    : public llvm::ErrorInfo<MachOCompactUnwindDwarfBindError> {
public:
  static char ID;
  static constexpr uint64_t NoRecord = UINT64_MAX;

  MachOCompactUnwindDwarfBindError(MachOCompactUnwindDwarfBindFailure Reason,
                                   uint64_t RecordIndex = NoRecord,
                                   std::string Detail = {});

  MachOCompactUnwindDwarfBindFailure reason() const { return Reason; }
  uint64_t recordIndex() const { return RecordIndex; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindDwarfBindFailure Reason;
  uint64_t RecordIndex;
  std::string Detail;
};

/// Bind every generated DWARF-mode row to exactly one installed FDE with the
/// same half-open function range.  The FDE record address must produce a
/// nonzero, representable offset from EHFrameSectionVA; that offset replaces
/// the linker's zero low-24-bit placeholder in a returned copy.  Non-DWARF
/// rows are copied unchanged.  The immutable receipt is the sole public source
/// of FDE addresses: planned section VAs and uninstalled object bytes are not
/// accepted as binding evidence.  Its architecture, pointer width, and byte
/// order must exactly match the generated compact-unwind records.
llvm::Expected<MachOCompactUnwindRecords>
bindMachOCompactUnwindDwarfFDEs(const MachOCompactUnwindRecords &Generated,
                                const MachOEHFrameInstallReceipt &Receipt);

/// Whether a generated range supplements code reached through an input-image
/// trampoline or replaces the exact source range at the same virtual address.
enum class MachOCompactUnwindRangeMode : uint8_t {
  NewSegment = 0,
  SameVAInPlace = 1,
};

static_assert(
    static_cast<uint8_t>(MachOCompactUnwindRangeMode::NewSegment) == 0 &&
        static_cast<uint8_t>(MachOCompactUnwindRangeMode::SameVAInPlace) == 1,
    "compact-unwind range modes are an API contract");

/// One exact old-to-new function-range mapping.  All ranges are half-open
/// virtual-address ranges.  SameVAInPlace requires equal start addresses, but
/// permits the generated range to be shorter than the replaced source range;
/// the merger then emits an absence boundary when required.
struct MachOCompactUnwindRangeMapping {
  uint64_t SourceVA = 0;
  uint64_t SourceEndVA = 0;
  uint64_t DestinationVA = 0;
  uint64_t DestinationEndVA = 0;
  MachOCompactUnwindRangeMode Mode = MachOCompactUnwindRangeMode::NewSegment;
  uint64_t FunctionRangeId = 0;
  std::string OwnerSymbol;
  uint64_t OwnerVA = 0;
};

/// Stable failures while turning successfully installed, symbol-identified
/// trampolines into exact compact-unwind source/destination range mappings.
enum class MachOCompactUnwindRangeMapFailure : uint8_t {
  ArchitectureMismatch = 0,
  MissingSection = 1,
  MissingTrampoline = 2,
  AmbiguousTrampoline = 3,
  TrampolineSymbolMismatch = 4,
  MissingSourceRange = 5,
  AmbiguousSourceRange = 6,
  DuplicateSourceRange = 7,
  InvalidFunctionRangeIdentity = 8,
  CrossOwnerSourceReuse = 9,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindRangeMapFailure::ArchitectureMismatch) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindRangeMapFailure::DuplicateSourceRange) == 7 &&
        static_cast<uint8_t>(
            MachOCompactUnwindRangeMapFailure::CrossOwnerSourceReuse) == 9,
    "compact-unwind range-map diagnostic values are an API contract");

class MachOCompactUnwindRangeMapError final
    : public llvm::ErrorInfo<MachOCompactUnwindRangeMapError> {
public:
  static char ID;
  static constexpr uint64_t NoRecord = UINT64_MAX;

  MachOCompactUnwindRangeMapError(MachOCompactUnwindRangeMapFailure Reason,
                                  uint64_t RecordIndex = NoRecord,
                                  std::string Detail = {});

  MachOCompactUnwindRangeMapFailure reason() const { return Reason; }
  uint64_t recordIndex() const { return RecordIndex; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindRangeMapFailure Reason;
  uint64_t RecordIndex;
  std::string Detail;
};

/// Strictly bind each generated compact row to one symbol-identical installed
/// trampoline and one exact original compact-unwind recipe.  Source ranges
/// come from the lossless original table, never from symbol-size guesses.
/// Every generated record must have a unique mapping; no input is mutated.
llvm::Expected<std::vector<MachOCompactUnwindRangeMapping>>
buildMachOCompactUnwindRangeMappings(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<PatchedFunctionEntry> InstalledTrampolines,
    llvm::endianness ByteOrder);

/// Provenance retained for diagnostics and semantic round-trip validation.
enum class MachOCompactUnwindRecordOrigin : uint8_t {
  Original = 0,
  Generated = 1,
  GapBoundary = 2,
};

/// One normalized final-table recipe.  Function offsets, personality slots,
/// and LSDAs are relative to the Mach header.  Function ranges are half-open.
/// Encoding has already been remapped to index PersonalitySlotRVA in the
/// result's personality array.
struct MachOCompactUnwindMergedRecord {
  static constexpr uint64_t NoInputRecord = UINT64_MAX;

  uint32_t FunctionRVA = 0;
  uint32_t FunctionEndRVA = 0;
  uint64_t FunctionRangeId = 0;
  std::string OwnerSymbol;
  uint64_t OwnerVA = 0;
  uint32_t Encoding = 0;
  std::optional<uint32_t> PersonalitySlotRVA;
  std::optional<uint32_t> LSDARVA;
  MachOCompactUnwindRecordOrigin Origin =
      MachOCompactUnwindRecordOrigin::Original;
  uint64_t InputRecordIndex = NoInputRecord;

  bool operator==(const MachOCompactUnwindMergedRecord &) const = default;
};

/// Semantic input to the deterministic encoder.  TerminalFunctionRVA is the
/// exclusive final lookup bound and will become the first-level sentinel.
struct MachOCompactUnwindMergeResult {
  Arch TargetArch = Arch::Unknown;
  uint32_t TerminalFunctionRVA = 0;
  std::vector<uint32_t> PersonalitySlotRVAs;
  std::vector<MachOCompactUnwindMergedRecord> Records;

  bool operator==(const MachOCompactUnwindMergeResult &) const = default;
};

enum class MachOCompactUnwindMergeFailure : uint8_t {
  InvalidOriginalTable = 0,
  UnsupportedArchitecture = 1,
  ArchitectureMismatch = 2,
  UnsupportedEncoding = 3,
  PersonalityIndexOutOfRange = 4,
  LSDAEncodingMismatch = 5,
  InvalidGeneratedRecord = 6,
  AddressOutsideImageRVA = 7,
  InvalidRangeMapping = 8,
  SourceRangeNotExact = 9,
  AmbiguousSourceRange = 10,
  DestinationRangeNotExact = 11,
  AmbiguousDestinationRange = 12,
  UnmappedGeneratedRange = 13,
  DuplicateMergedRange = 14,
  OverlappingMergedRanges = 15,
  TooManyPersonalities = 16,
  ByteOrderMismatch = 17,
  MissingDwarfFDEOffset = 18,
  MissingFunctionRangeId = 19,
  DuplicateFunctionRangeId = 20,
  DanglingFunctionRangeId = 21,
  FunctionRangeIdentityMismatch = 22,
  CrossOwnerSourceReuse = 23,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindMergeFailure::InvalidOriginalTable) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindMergeFailure::ByteOrderMismatch) == 17 &&
        static_cast<uint8_t>(
            MachOCompactUnwindMergeFailure::MissingDwarfFDEOffset) == 18 &&
        static_cast<uint8_t>(
            MachOCompactUnwindMergeFailure::CrossOwnerSourceReuse) == 23,
    "compact-unwind merge diagnostic values are an API contract");

enum class MachOCompactUnwindMergeInputKind : uint8_t {
  None = 0,
  OriginalEncoding = 1,
  OriginalRecord = 2,
  GeneratedRecord = 3,
  RangeMapping = 4,
  MergedRecord = 5,
};

/// Stable typed failure for strict normalization and range-map merging.
class MachOCompactUnwindMergeError final
    : public llvm::ErrorInfo<MachOCompactUnwindMergeError> {
public:
  static char ID;
  static constexpr uint64_t NoInput = UINT64_MAX;

  MachOCompactUnwindMergeError(MachOCompactUnwindMergeFailure Reason,
                               MachOCompactUnwindMergeInputKind InputKind =
                                   MachOCompactUnwindMergeInputKind::None,
                               uint64_t InputIndex = NoInput,
                               std::string Detail = {});

  MachOCompactUnwindMergeFailure reason() const { return Reason; }
  MachOCompactUnwindMergeInputKind inputKind() const { return InputKind; }
  uint64_t inputIndex() const { return InputIndex; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindMergeFailure Reason;
  MachOCompactUnwindMergeInputKind InputKind;
  uint64_t InputIndex;
  std::string Detail;
};

/// Strictly normalize and merge original and generated compact-unwind state.
/// Original.OriginalBytes is reparsed and must reproduce every retained raw
/// field before it is trusted.  Each mapping must name one exact original
/// recipe range and one exact generated range.  NewSegment preserves both;
/// SameVAInPlace replaces only its exact source recipe.  Inputs are never
/// sorted, repaired, or mutated in place.
llvm::Expected<MachOCompactUnwindMergeResult> mergeMachOCompactUnwind(
    Arch TargetArch, uint64_t MachHeaderVA,
    const macho_unwind::CompactUnwindRawSection &Original,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<MachOCompactUnwindRangeMapping> Mappings);

/// Stable failure categories for deterministic final-table encoding.
enum class MachOCompactUnwindEncodeFailure : uint8_t {
  UnsupportedArchitecture = 0,
  UnsupportedEndianness = 1,
  EmptyRecords = 2,
  TooManyPersonalities = 3,
  DuplicatePersonalitySlot = 4,
  InvalidTerminalBoundary = 5,
  InvalidRecordRange = 6,
  UnsortedOrOverlappingRecords = 7,
  NonContiguousRecords = 8,
  UnsupportedEncoding = 9,
  PersonalityIndexOutOfRange = 10,
  PersonalityEncodingMismatch = 11,
  LSDAEncodingMismatch = 12,
  SectionSizeOverflow = 13,
  StrictRoundTripFailure = 14,
  SemanticRoundTripMismatch = 15,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindEncodeFailure::UnsupportedArchitecture) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindEncodeFailure::SemanticRoundTripMismatch) == 15,
    "compact-unwind encode diagnostic values are an API contract");

enum class MachOCompactUnwindEncodeInputKind : uint8_t {
  None = 0,
  PersonalitySlot = 1,
  Record = 2,
};

/// Typed failure for preflight validation, layout arithmetic, and the strict
/// self-check performed before encoded bytes are returned.
class MachOCompactUnwindEncodeError final
    : public llvm::ErrorInfo<MachOCompactUnwindEncodeError> {
public:
  static char ID;
  static constexpr uint64_t NoInput = UINT64_MAX;

  MachOCompactUnwindEncodeError(MachOCompactUnwindEncodeFailure Reason,
                                MachOCompactUnwindEncodeInputKind InputKind =
                                    MachOCompactUnwindEncodeInputKind::None,
                                uint64_t InputIndex = NoInput,
                                std::string Detail = {});

  MachOCompactUnwindEncodeFailure reason() const { return Reason; }
  MachOCompactUnwindEncodeInputKind inputKind() const { return InputKind; }
  uint64_t inputIndex() const { return InputIndex; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindEncodeFailure Reason;
  MachOCompactUnwindEncodeInputKind InputKind;
  uint64_t InputIndex;
  std::string Detail;
};

/// Encode one normalized merge result as final `__TEXT,__unwind_info` bytes.
/// This initial writer deliberately emits only canonical 4-KiB regular
/// second-level pages.  The header, personality array, first-level index,
/// per-page LSDA slices, and terminal sentinel are emitted in one stable
/// little-endian layout.
///
/// The input must already be canonical merge output: records are strictly
/// ordered and contiguous, every range end is represented by the following
/// lookup boundary, and the final end equals TerminalFunctionRVA.  The writer
/// neither sorts nor repairs input.  Before returning, it reparses its bytes
/// with parseCompactUnwindRaw and verifies every recipe and side-table field.
/// No image, section, or input object is mutated.
llvm::Expected<std::vector<uint8_t>>
encodeMachOCompactUnwindRegular(const MachOCompactUnwindMergeResult &Input,
                                llvm::endianness ByteOrder);

/// Whether an in-capacity compact-unwind installation changed the image.
enum class MachOCompactUnwindInstallDisposition : uint8_t {
  Unchanged = 0,
  RewrittenInPlace = 1,
};

static_assert(
    static_cast<uint8_t>(MachOCompactUnwindInstallDisposition::Unchanged) ==
            0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindInstallDisposition::RewrittenInPlace) == 1,
    "compact-unwind install dispositions are an API contract");

/// Stable failures owned by the transactional final-section installer.
/// Parser, merge, and encoder failures retain their more specific public
/// ErrorInfo types; this enum covers installation policy and commit checks.
enum class MachOCompactUnwindInstallFailure : uint8_t {
  UnexpectedMappingsForNoOp = 0,
  MissingSection = 1,
  SectionSizeOverflow = 2,
  InsufficientCapacity = 3,
  InvalidPlan = 4,
  StaleRegion = 5,
  StalePreimage = 6,
  PostLocateFailure = 7,
  PostParseFailure = 8,
  PostSemanticMismatch = 9,
  ArchitectureMismatch = 10,
};

static_assert(
    static_cast<uint8_t>(
        MachOCompactUnwindInstallFailure::UnexpectedMappingsForNoOp) == 0 &&
        static_cast<uint8_t>(
            MachOCompactUnwindInstallFailure::PostSemanticMismatch) == 9 &&
        static_cast<uint8_t>(
            MachOCompactUnwindInstallFailure::ArchitectureMismatch) == 10,
    "compact-unwind install diagnostic values are an API contract");

class MachOCompactUnwindInstallError final
    : public llvm::ErrorInfo<MachOCompactUnwindInstallError> {
public:
  static char ID;

  MachOCompactUnwindInstallError(MachOCompactUnwindInstallFailure Reason,
                                 uint64_t RequiredBytes = 0,
                                 uint64_t AvailableBytes = 0,
                                 std::string Detail = {});

  MachOCompactUnwindInstallFailure reason() const { return Reason; }
  uint64_t requiredBytes() const { return RequiredBytes; }
  uint64_t availableBytes() const { return AvailableBytes; }
  llvm::StringRef detail() const { return Detail; }

  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;

private:
  MachOCompactUnwindInstallFailure Reason;
  uint64_t RequiredBytes;
  uint64_t AvailableBytes;
  std::string Detail;
};

/// Audit receipt returned only after the candidate image has been relocated,
/// strictly reparsed, and proven semantically equivalent to the plan.
struct MachOCompactUnwindInstallReceipt {
  MachOCompactUnwindInstallDisposition Disposition =
      MachOCompactUnwindInstallDisposition::Unchanged;
  std::optional<MachOCompactUnwindRegion> Region;
  uint64_t EncodedSize = 0;
  uint64_t Capacity = 0;
  uint64_t ClearedTailSize = 0;
  uint64_t OriginalRecordCount = 0;
  uint64_t GeneratedRecordCount = 0;
  uint64_t FinalRecordCount = 0;
  std::vector<MachOCompactUnwindRangeMapping> RangeMappings;
};

/// Factory-only immutable preflight result for one exact section preimage.
/// Apply revalidates both expectedRegion() and expectedOriginalBytes() before
/// it creates its candidate copy, preventing a stale plan from overwriting a
/// concurrently changed image.  Callers can audit every planned mutation but
/// cannot forge or alter encoded bytes, semantic state, or range mappings.
class MachOCompactUnwindInstallPlan {
public:
  MachOCompactUnwindInstallPlan(const MachOCompactUnwindInstallPlan &) =
      default;
  MachOCompactUnwindInstallPlan(MachOCompactUnwindInstallPlan &&) = default;
  MachOCompactUnwindInstallPlan &
  operator=(const MachOCompactUnwindInstallPlan &) = delete;
  MachOCompactUnwindInstallPlan &
  operator=(MachOCompactUnwindInstallPlan &&) = delete;

  MachOCompactUnwindInstallDisposition disposition() const {
    return Disposition;
  }
  llvm::endianness byteOrder() const { return ByteOrder; }
  const std::optional<MachOCompactUnwindRegion> &expectedRegion() const {
    return ExpectedRegion;
  }
  llvm::ArrayRef<uint8_t> expectedOriginalBytes() const {
    return ExpectedOriginalBytes;
  }
  llvm::ArrayRef<uint8_t> encodedBytes() const { return EncodedBytes; }
  const MachOCompactUnwindMergeResult &expectedSemantics() const {
    return ExpectedSemantics;
  }
  llvm::ArrayRef<MachOCompactUnwindRangeMapping> rangeMappings() const {
    return RangeMappings;
  }
  uint64_t originalRecordCount() const { return OriginalRecordCount; }
  uint64_t generatedRecordCount() const { return GeneratedRecordCount; }

private:
  MachOCompactUnwindInstallPlan() = default;

  MachOCompactUnwindInstallDisposition Disposition =
      MachOCompactUnwindInstallDisposition::Unchanged;
  llvm::endianness ByteOrder = llvm::endianness::little;
  std::optional<MachOCompactUnwindRegion> ExpectedRegion;
  std::vector<uint8_t> ExpectedOriginalBytes;
  std::vector<uint8_t> EncodedBytes;
  MachOCompactUnwindMergeResult ExpectedSemantics;
  std::vector<MachOCompactUnwindRangeMapping> RangeMappings;
  uint64_t OriginalRecordCount = 0;
  uint64_t GeneratedRecordCount = 0;

  friend llvm::Expected<MachOCompactUnwindInstallPlan>
  prepareMachOCompactUnwindInstall(
      llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
      const MachOCompactUnwindRecords &Generated,
      llvm::ArrayRef<MachOCompactUnwindRangeMapping> Mappings,
      llvm::endianness ByteOrder);
  friend llvm::Expected<MachOCompactUnwindInstallReceipt>
  applyMachOCompactUnwindInstall(std::vector<uint8_t> &Binary,
                                 const MachOCompactUnwindInstallPlan &Plan);
};

/// Preflight an in-place rewrite of the existing final compact-unwind
/// section.  A request with no generated records and no mappings is an exact
/// no-op and does not require the image to declare the section.  Every other
/// request strictly locates and parses the current section, merges and encodes
/// the requested state, and fails when the encoded prefix exceeds the
/// section's declared size.  No input is mutated.
llvm::Expected<MachOCompactUnwindInstallPlan> prepareMachOCompactUnwindInstall(
    llvm::ArrayRef<uint8_t> Binary, Arch TargetArch,
    const MachOCompactUnwindRecords &Generated,
    llvm::ArrayRef<MachOCompactUnwindRangeMapping> Mappings,
    llvm::endianness ByteOrder);

/// Apply a preflighted plan transactionally.  The section header size remains
/// unchanged: encoded bytes replace the prefix and the remaining declared
/// extent is cleared.  Work is performed on a candidate copy; Binary is
/// swapped only after post-install location, strict parse, and semantic checks
/// all succeed.  Consequently every failure leaves Binary byte-for-byte
/// unchanged.
llvm::Expected<MachOCompactUnwindInstallReceipt>
applyMachOCompactUnwindInstall(std::vector<uint8_t> &Binary,
                               const MachOCompactUnwindInstallPlan &Plan);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOCOMPACTUNWINDPATCH_H
