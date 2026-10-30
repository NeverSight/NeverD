//===- BinaryImageModel.h - The unified loader output -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines \ref BinaryImage, the format-agnostic structure every loader
/// produces: the segments, sections, symbols, imports, exports, relocations,
/// and exception metadata parsed out of a binary, plus the lookups the rest of
/// the pipeline reaches them through.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGEMODEL_H
#define NEVERD_LOADER_BINARYIMAGEMODEL_H

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/evm/EVMImageMetadata.h"
#include "neverd/loader/BinaryImageDynamic.h"
#include "neverd/loader/BinaryImageRelocation.h"
#include "neverd/loader/BinaryImageSection.h"
#include "neverd/loader/COFF/RichHeader.h"
#include "neverd/loader/ELF/ELFImageMetadata.h"
#include "neverd/loader/ExceptionTable.h"
#include "neverd/loader/Limits.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/ObjC/ObjCMethods.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

class ExecutableCodeOwnerIndex;
namespace macho_loader {
class DyldSharedCache;
}

/// One concrete pointer slot bound by Mach-O dyld metadata.  This native view
/// is retained for patch provenance; collectImportStorageSlots() combines it
/// with the format-neutral identity while preserving the signed addend.
struct ImportBindSlot {
  std::string Name;
  int64_t Addend = 0;
  /// Raw install-name requested by the exact positive library ordinal. Empty
  /// means no specific dependency is known; it is never an aggregate lookup.
  std::string Module = {};
  bool WeakImport = false;
  /// Exact dyld lookup scope, including SELF=0 and negative special ordinals.
  /// Absent means legacy evidence did not record the scope, never SELF.
  std::optional<int64_t> LibraryOrdinal;
};

/// A dyld signing recipe, not a runtime pointer value. The original slot bytes
/// remain encoded: offline loading cannot manufacture the process's PAC.
struct MachOAuthenticatedPointer {
  va_t Target = InvalidVA;
  /// Exact on-disk recipe, retained to invalidate analysis after byte edits.
  std::optional<uint64_t> EncodedValue;
  std::string Symbol;
  std::string Module;
  int64_t Addend = 0;
  uint16_t Diversity = 0;
  uint8_t Key = 0;
  bool AddressDiversity = false;
  bool WeakImport = false;
  /// Present only for a dyld import recipe, not a local rebase or object
  /// relocation. It distinguishes special lookups with an empty Module.
  std::optional<int64_t> LibraryOrdinal;
};

/// One dependency load command, including duplicate names and invalid-name
/// placeholders. Its position, not a deduplicated library list, owns the
/// one-based dyld library ordinal.
struct MachODylibReference {
  std::string Name;
  bool Weak = false;
};

/// Strength of the format metadata that identifies one imported pointer
/// storage slot.  The ordering is intentional: a concrete loader binding may
/// refine the zero addend supplied by an indirect-symbol table, while an
/// import-directory entry is already a complete exact-slot identity.
enum class ImportStorageEvidence : uint8_t {
  PointerTable = 0,
  ImportDirectory = 1,
  LoaderBind = 2,
};

/// Format-neutral identity of one non-executable, pointer-width storage slot
/// whose contents are supplied by the platform loader.
struct ImportStorageSlot {
  std::string Name;
  int64_t Addend = 0;
  ImportStorageEvidence Evidence = ImportStorageEvidence::PointerTable;
};

/// Canonical import-storage view consumed by format-neutral pipeline stages.
/// Conflicts remain explicit and are never represented by a first-wins slot.
struct ImportStorageSlotCollection {
  std::map<va_t, ImportStorageSlot> Slots;
  std::set<va_t> Conflicts;
};

/// Runtime contract that owns the contents of one callable pointer storage
/// slot.  Unlike a code-pointer relocation, this provenance remains meaningful
/// when the on-disk value is null because the platform loader or runtime may
/// populate the slot before user code calls through it.
enum class RuntimeCallablePointerSlotKind : uint8_t {
  GuardCFCheck,
  GuardCFDispatch,
  GuardRFFailureRoutine,
  GuardRFVerifyStackPointer,
  GuardXFGCheck,
  GuardXFGDispatch,
  GuardXFGTableDispatch,
  CastGuardOsDeterminedFailureMode,
  GuardMemcpy,
  /// The ELF DT_PLTGOT slot the dynamic linker fills with its lazy binding
  /// resolver, which the first PLT entry jumps through.
  ELFLazyResolver,
  /// The first word of an ELF TLS descriptor: the resolver the dynamic
  /// linker writes there for code to call through it.
  ELFTLSDescriptor,
};

struct RuntimeCallablePointerSlot {
  va_t SlotVA = InvalidVA;
  RuntimeCallablePointerSlotKind Kind =
      RuntimeCallablePointerSlotKind::GuardCFCheck;

  bool operator==(const RuntimeCallablePointerSlot &Other) const = default;
};

/// An ELF mapping symbol owns the bytes up to the next mapping symbol in the
/// same executable section. Data in an executable section is not an instruction
/// in either AArch32 execution state.
enum class ARMCodeRegionKind : uint8_t { ARM, Thumb, Data };

struct ARMCodeRegion {
  va_t Start = 0;
  va_t End = 0;
  ARMCodeRegionKind Kind = ARMCodeRegionKind::Data;
};

/// Exact encoded relocation field that materializes a complete address
/// operand. EncodedValue is the canonical unsigned value written to Width
/// bytes; TargetVA is the mapped address it denotes. Keeping both prevents a
/// section-difference expression (whose field is A-B but whose eventual target
/// is A) from being mistaken for a complete address occurrence.
enum class RelocatedAddressFieldKind : uint8_t {
  Generic,
  /// i386 ELF R_386_GOTOFF instruction operand.  This is distinct from an
  /// absolute R_386_32 field even when both encode the same mapped data VA.
  I386ELFGOTOFF,
};

struct RelocatedAddressField {
  uint64_t EncodedValue = 0;
  va_t TargetVA = InvalidVA;
  uint8_t Width = 0;
  /// Address inside the section/segment that semantically owns TargetVA.
  /// This differs from TargetVA for one-past relocations, which may
  /// numerically coincide with the next section's first byte.  InvalidVA
  /// means no occurrence-local owner was retained.
  va_t TargetOwnerVA = InvalidVA;
  /// The encoded field is a signed PC-relative displacement whose complete
  /// target depends on the containing instruction's end address.  Loaders do
  /// not know that boundary; CFG lifting recomputes TargetVA occurrence-
  /// locally before the architecture lifter consumes the field.
  bool PCRelativeFromInstructionEnd = false;
  /// Relocation semantics that cannot be reconstructed from the folded value
  /// or PC-relative bit alone.  Most address operands are Generic; consumers
  /// of the i386 GOT-base-zero model must require I386ELFGOTOFF explicitly so
  /// an absolute field cannot borrow that proof.
  RelocatedAddressFieldKind Kind = RelocatedAddressFieldKind::Generic;
};

/// Loader-authenticated instruction whose architectural output materializes a
/// complete address even though the relocation patches a non-contiguous
/// instruction bitfield rather than a standalone pointer-sized operand.
/// AArch64 ADRP+ADD PAGEOFF and ARM MOVW/MOVT pairs are the canonical forms.
/// Consumers must still bind this record to the exact decoded LowOp output;
/// the target value alone is never address provenance.
struct RelocatedInstructionAddressMaterialization {
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;
};

/// One R_ARM_REL32 literal after the ELF loader has applied it to its mapped
/// section.  RelocationEntry::Address is a raw section-relative r_offset for
/// ET_REL inputs, so CFG consumers must use this applied SlotVA-keyed record
/// rather than trying to match a mapped literal address against the global
/// relocation list.  EncodedValue is the exact 32-bit displacement written at
/// SlotVA; TargetVA/TargetOwnerVA retain the relocated symbol identity.
struct AppliedARMRelativeLiteral {
  uint32_t EncodedValue = 0;
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;

  bool operator==(const AppliedARMRelativeLiteral &Other) const = default;
};

/// One i386 R_386_GOTPC field after the ELF loader has applied it at its
/// mapped instruction address.  The relocation establishes the architectural
/// model used by NeverD's relocatable image: adding EncodedValue to the exact
/// get-PC seed ExpectedPCValue produces the model-zero GOT base.  Consumers
/// must still prove that the matching instruction input reaches that exact PC
/// seed; the numeric zero alone is never provenance.
struct AppliedI386GOTPCField {
  uint32_t EncodedValue = 0;
  uint32_t ExpectedPCValue = 0;

  bool operator==(const AppliedI386GOTPCField &Other) const = default;
};

/// Loader- or debug-authenticated byte extent of one data object.  This is
/// deliberately separate from Symbol: Mach-O nlist and ordinary COFF symbols
/// do not carry object sizes, and a distance to the next symbol is padding,
/// not proof of a C object boundary.  This is an internal image-model enum,
/// not a serialized or ABI-facing contract.
enum class ExactDataObjectEvidence : uint8_t {
  ELFObjectSymbol = 0,
  AuthenticatedDebug = 1,
};

/// What an authenticated extent proves to a counted-write consumer.  A
/// storage-object boundary is an upper bound only: it may describe a struct
/// containing a smaller writable member.  Only occurrence-authenticated typed
/// buffer evidence may establish an exact safety capacity; a typed non-buffer
/// is retained as authoritative negative evidence.  This is likewise internal
/// to the image model and is never emitted directly as an ABI value.
enum class ExactDataObjectPrecision : uint8_t {
  Storage = 0,
  TypedBuffer = 1,
  TypedNonBuffer = 2,
};

struct ExactDataObjectExtent {
  va_t Base = InvalidVA;
  uint64_t Size = 0;
  ExactDataObjectEvidence Evidence = ExactDataObjectEvidence::ELFObjectSymbol;
  ExactDataObjectPrecision Precision = ExactDataObjectPrecision::Storage;

  bool operator==(const ExactDataObjectExtent &Other) const = default;
};

// ===--------------------------------------------------------------------===//
// BinaryImage — the unified output of all loaders
// ===--------------------------------------------------------------------===//

/// Non-fatal metadata problems encountered while mapping an input for analysis.
/// Diagnostics do not establish semantic facts or repair the original bytes.
struct LoadDiagnostic {
  std::string Code;
  std::string Message;
};

struct BinaryImage {
  // The type is written fully-qualified so the member's name (`Arch`) does not
  // shadow the enum type `neverd::Arch` inside the struct scope — an
  // -Wchanges-meaning hard error under GCC.  All consumers still read `.Arch`.
  neverd::Arch Arch = neverd::Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;
  /// Hardware constraint for an AArch32 image whose CPU cannot execute the
  /// other instruction state.  Unlike the file-level Mode, this applies to
  /// every executable instruction and conflicts with contrary local evidence.
  InstructionMode ARMRequiredMode = InstructionMode::Default;
  /// A file-level mode is a decoder bootstrap, not evidence about an unmarked
  /// entry. ELF sets this because its symbols and mapping intervals are local
  /// proofs even when all proven entries happen to use the same mode.
  bool ARMModeRequiresLocalEvidence = false;
  /// Entry, function-symbol, and validated instruction-relocation evidence
  /// identifies one exact instruction address. It does not authorize decoding
  /// the rest of a function in that mode when mapping symbols are absent.
  std::map<va_t, InstructionMode> ARMCodeModeEntries;
  /// Sorted, disjoint section-relative ELF mapping intervals, including $d.
  std::vector<ARMCodeRegion> ARMCodeRegions;
  /// Decoded instruction spans reachable from exact AArch32 code entries.
  /// Unlike mapping intervals, these do not claim the gaps between paths.
  std::vector<ARMCodeRegion> ARMReachableCodeRegions;
  /// When reachability supplies the only interval evidence, the file-level
  /// mode does not establish a mode for instruction bytes outside those spans.
  bool ARMReachabilityConstrained = false;
  /// Exact executable targets of decoded ARM literal branch veneers. They
  /// remain function candidates until the bounded function verifier accepts
  /// them; a pointer-sized literal alone does not authenticate an entry.
  std::set<va_t> ARMVeneerTargets;
  BinaryFormat Format = BinaryFormat::Unknown;
  /// The platform a binary file's code was built for, whose conventions it
  /// follows: ELF for System V and AAPCS, COFF for Windows, Mach-O for
  /// Apple.  Only a binary file sets it; see abiFormat().
  BinaryFormat ConventionFormat = BinaryFormat::Unknown;
  /// Replaces the calling convention's choice of how lifted x86-64 code holds
  /// the x87 register stack (x87ContextOwnsStack): true owns it as one
  /// mutable context, false keeps typed binary80 values.
  std::optional<bool> X87ContextOverride;
  Bitness Bits = Bitness::Unknown;
  bool IsRelocatable = false;
  /// The image runs at its link-time addresses: no loader rebases it, so an
  /// absolute address its code or data holds stays valid without a
  /// relocation.  Set by the format loaders for ELF ET_EXEC images, PE images
  /// without base relocations and Mach-O executables without MH_PIE.
  bool LoadsAtLinkAddress = false;
  va_t Base = 0;
  va_t Entry = 0;
  std::vector<LoadDiagnostic> LoadDiagnostics;

  void addLoadDiagnostic(std::string Code, std::string Message) {
    if (LoadDiagnostics.size() <
        ::neverd::limits::loader::kBinaryImageModelMaxDiagnostics)
      LoadDiagnostics.push_back({std::move(Code), std::move(Message)});
    else if (LoadDiagnostics.size() ==
             ::neverd::limits::loader::kBinaryImageModelMaxDiagnostics)
      LoadDiagnostics.push_back({"loader.diagnostics_truncated",
                                 "Further loader diagnostics omitted."});
  }
  std::vector<Segment> Segments;
  std::vector<Section> Sections;
  std::vector<Import> Imports;
  std::vector<Export> Exports;
  /// The functions listed under a number because functions at lower
  /// addresses share their names (numberSharedFunctionNames()), by entry:
  /// the name their symbol gives them, and the name they are listed under.
  std::map<va_t, std::pair<std::string, std::string>> SharedFunctionNames;
  /// What the user states of the function at an entry, as IDA's Edit
  /// function does: true, it never returns; false, it returns.  An entry
  /// absent here is analysis's to decide.  Every layer that asks whether a
  /// call returns reads this first (userNoReturnAt).
  std::map<va_t, bool> UserNoReturn;
  std::optional<bool> userNoReturnAt(va_t Entry) const {
    const auto It = UserNoReturn.find(Entry);
    return It == UserNoReturn.end() ? std::nullopt
                                    : std::optional<bool>(It->second);
  }
  std::vector<Symbol> Symbols;
  std::vector<ObjCClass> ObjCClasses;
  std::vector<ObjCMethod> ObjCMethods;
  std::vector<ObjCProtocol> ObjCProtocols;
  std::vector<ObjCProperty> ObjCProperties;
  std::map<va_t, ObjCSourceReference> ObjCSourceReferences;
  /// Offline reference identities from authenticated or cache metadata. These
  /// declarations do not establish runtime values or source object ownership.
  std::map<va_t, ObjCSourceReference> ObjCMetadataReferences;
  std::vector<std::string> ObjCMetadataDiagnostics;
  /// A valid LC_FUNCTION_STARTS stream supplied linked Mach-O function
  /// identities.  Raw read-only metadata must not then be rescanned as an
  /// untyped absolute function-pointer table: Swift and Objective-C metadata
  /// contain relative 32-bit fields whose adjacent bytes can resemble a
  /// mapped 64-bit code address.
  bool MachOHasFunctionStarts = false;
  /// The Mach-O file header identifies an MH_DYLIB. A zero-based dylib must
  /// be loaded away from address zero, so unrelocated absolute instruction
  /// immediates cannot denote its own link-time image addresses.
  bool MachOIsDylib = false;
  uint32_t MachOCPUSubtype = 0;
  bool isArm64e() const {
    return isMachO() && Arch == Arch::AArch64 &&
           macho_loader::arm64e::isSubtype(MachOCPUSubtype);
  }
  /// Exact slots decoded by the supported chained-fixup reader. A raw nonzero
  /// slot in a chained image is not a pointer merely because it looks mapped.
  bool MachOHasChainedFixups = false;
  bool MachOChainedFixupsAmbiguous = false;
  std::set<va_t> MachOResolvedChainedPointerSlots;
  std::map<va_t, MachOAuthenticatedPointer> MachOAuthenticatedPointers;
  /// Immutable cache address space for analysis-only cross-image metadata.
  /// It does not extend readVA or establish a runtime pointer value.
  std::shared_ptr<const macho_loader::DyldSharedCache> MachOSharedCache;
  /// Exact catalogue selection and hashes of all cache files, in file order.
  /// Raw is a packed analysis view, while segment VAs retain cache addresses.
  std::string MachOSharedCacheImagePath;
  std::vector<std::array<uint8_t, 32>> MachOSharedCacheFileSHA256;
  /// Indirect-table names on cache-owned slide slots. These are symbolic
  /// references, not ordinary import-storage or unsigned-pointer proofs.
  std::map<va_t, std::string> MachOSharedCacheSymbolPointers;
  std::string MachOChainedFixupsError;
  /// Any byte of an encoded signing recipe overlaps this storage range.
  bool hasMachOAuthenticatedPointerIn(va_t Address, uint64_t Size) const {
    if (!Size)
      return false;
    const auto It =
        MachOAuthenticatedPointers.lower_bound(Address >= 7 ? Address - 7 : 0);
    return It != MachOAuthenticatedPointers.end() &&
           (It->first <= Address || It->first - Address < Size);
  }
  std::vector<MachODylibReference> MachODylibReferences;
  /// A requested module is not a specific-provider proof under flat lookup.
  /// Filled from MH_TWOLEVEL only when MH_FORCE_FLAT is absent.
  bool MachOTwoLevelNamespace = false;
  /// Exact data-object boundaries.  Conflicting entries are retained so a
  /// consumer can fail closed; producers must never collapse them by order.
  std::vector<ExactDataObjectExtent> ExactDataObjects;
  std::vector<RelocationEntry> Relocations;
  std::vector<BaseRelocation> BaseRelocations;
  /// Program-header and ABI facts retained by the ordinary ELF loader.
  std::optional<ELFImageMetadata> ELFMetadata;
  /// Present only for Solana SBF ELF inputs.  The dedicated loader owns ELF
  /// parsing; the frontend consumes this typed record and never reparses Raw.
  std::optional<sbf::Metadata> SBF;
  /// Present only for EVM inputs.  An EVM input has no header, so what kind of
  /// container it is and which compiler emitted it are conclusions the loader
  /// reaches by reading the bytes.  Dropping them at this boundary would leave
  /// the frontend to re-derive them — under a different hardfork, and therefore
  /// differently — or to report an unknown build for a contract that named
  /// itself.
  std::optional<evm::ImageMetadata> EVM;
  /// Virtual addresses that a relocation resolves to inside a read-only data
  /// segment (filled by the loader as it applies relocations).  A code constant
  /// equal to one of these is a genuine pointer into rodata — e.g. a low-VA
  /// i386 GOTOFF `.rodata` base the numeric-VA heuristic would read as an
  /// integer literal — so the emitter can hoist it.  Only relocation forms that
  /// bake an absolute data VA into the code are recorded (i386 GOTOFF/abs32);
  /// PC/page-relative forms (x86-64 rip, AArch64 ADRP) already resolve through
  /// the existing >kMinGlobalDataAddr path and are intentionally left out.
  std::set<uint64_t> RelocDataAddrs;
  /// Exact relocation-field VA -> resolved data-address operand for relocation
  /// forms that materialize the address in an instruction.  Unlike
  /// RelocDataAddrs this is occurrence provenance: CFG lifting consults the
  /// field inside the current instruction before tagging the matching constant
  /// as DataAddress, so an unrelated integer that merely equals the same target
  /// VA never inherits pointer meaning.  i386 R_386_GOTOFF is the canonical
  /// producer; its relocated displacement is the complete target under the
  /// loader's GOT-base-zero model.
  std::map<va_t, RelocatedAddressField> DataAddressRelocOperands;
  /// Exact instruction VA -> complete address written by that instruction.
  /// Kept separate from DataAddressRelocOperands because PAGEOFF/MovW fields
  /// are encoded bitfields, not byte-address operands a generic consumer may
  /// read or use as a relocation slot.
  std::map<va_t, RelocatedInstructionAddressMaterialization>
      InstructionAddressMaterializations;
  /// Exact ADRP/PAGEBASE instruction VA -> symbol target/owner whose page the
  /// instruction materializes.  A PAGEOFF output is complete only when its
  /// reaching base is a matching record from this map.
  std::map<va_t, RelocatedInstructionAddressMaterialization>
      InstructionPageAddressFragments;
  /// Applied ARM-state R_ARM_REL32 literal fields keyed by mapped slot VA.
  /// A record authenticates the literal field only; CFG lifting must still
  /// prove that an exact LOAD of this slot reaches the architectural PC ADD.
  std::map<va_t, AppliedARMRelativeLiteral> ARMRelativeLiteralFields;
  /// Applied i386 R_386_GOTPC fields keyed by mapped relocation-field VA.
  /// RelocationEntry::Address is section-relative for ET_REL, so CFG/Med
  /// provenance must consume this mapped occurrence instead of rescanning the
  /// raw relocation inventory.
  std::map<va_t, AppliedI386GOTPCField> I386GOTPCFields;
  /// Mapped i386 instruction fields for which at least one R_386_GOTPC and
  /// another value-writing relocation claim the same four bytes.  These are
  /// deliberately not AppliedI386GOTPCFields: the loader applies every record
  /// from the original REL addend, but no one record owns the final scalar
  /// provenance.  CFG lifting binds this tombstone to the exact decoded
  /// output so only a GOTOFF consumer that may depend on the ambiguous field
  /// is retained as an opaque indirect branch.
  std::set<va_t> AmbiguousI386GOTPCFields;
  /// Mapped i386 instruction fields for which an R_386_GOTOFF relocation and
  /// another value-writing relocation overlap the same four-byte span.  The
  /// loader still applies every record from the original REL addend, but this
  /// negative certificate prevents the final numeric displacement from being
  /// mistaken for a uniquely owned table address.
  std::set<va_t> AmbiguousI386GOTOFFFields;
  /// Complete mapped write footprint of a supported ELF ET_REL inventory.
  /// Absence means the inventory or a writer width could not be certified.
  /// Only a field disjoint from this complete set can be classified as an
  /// unrelocated numeric immediate; target-VA coincidence supplies no role.
  std::optional<std::set<va_t>> ObjectRelocationWriteBytes;
  /// Virtual addresses that a relocation resolves to inside a WRITABLE data
  /// segment (.data/.bss), filled by the loader as it applies relocations.  The
  /// writable counterpart of RelocDataAddrs: it proves a constant is a genuine
  /// pointer to a mutable global (a taken address `&G`, emitted via
  /// GOTOFF/abs32) rather than an ordinary data value that merely collides with
  /// the segment's VA range.  A relocatable .o places a large `static` array at
  /// a low VA, so a wide mutable run spans plain data immediates (an LCG
  /// increment) that must not be mistaken for a stored pointer base; the
  /// emitter gates writable store-value symbolization on membership here.
  std::set<uint64_t> WritableRelocDataAddrs;
  /// Maps a GOTOFF-folded rodata table base (`symbol + addend`) to the VA of
  /// the rodata segment that *contains the referenced symbol*.  clang's
  /// switch-to- lookup-table indexed by the unbiased case value folds the base
  /// to `table - min_case*stride`, a negative addend that lands BEFORE the
  /// table's segment (often inside `.text`).  The bare folded VA therefore
  /// resolves to the wrong segment (or is rejected as code); this map lets the
  /// emitter pin such a base to the symbol's own rodata global with a (possibly
  /// negative) byte offset, so `base + runtime_index` lands back inside the
  /// table.  Filled by the loader for an i386 instruction operand, GOTOFF or
  /// absolute (non-PIC `lea T-0x20(,%esi,4)`), whose symbol is in clean
  /// rodata.
  std::map<uint64_t, uint64_t> RodataAnchorSeg;
  /// Virtual addresses of relocation slots that hold an absolute pointer into
  /// an executable segment (filled by the loader for R_X86_64_64 /
  /// R_AARCH64_ABS64 / R_386_32 / R_ARM_ABS32 whose target is code).  A run of
  /// these starting at a jump-table base marks the entries as absolute (not
  /// PIC-relative) and its length is the exact entry count — this is how a
  /// computed-goto / threaded dispatch table (`.data.rel.ro` code-pointer
  /// array) is recovered, since it has no comparison guard to bound it and
  /// 4-byte entries are otherwise ambiguous with PIC self-relative offsets.
  std::set<uint64_t> CodePtrRelocSlots;
  /// Bases of tables the code reads through a runtime-indexed fixed-base
  /// memory operand of a CALL instruction — `call [base+idx*s]` — where the
  /// base lies in an executable section (coff_loader::
  /// recordIndexedTableBasesInCode). Such a table holds callable entries
  /// (MinGW's CRT constructor list among the code), never another function's
  /// case labels, so its relocation slots promote their targets directly as
  /// callback candidates instead of joining the jump-table label analysis.
  std::set<uint64_t> CallDispatchTableBases;
  /// [begin, end) runs of tables the code reads through a runtime-indexed
  /// fixed-base memory operand, in any readable section (coff_loader::
  /// recordIndexedTableBasesInCode).  A discovery scan must not synthesize
  /// function symbols for the run's targets: an indexed table's entries are
  /// its reader's case labels or callbacks, not independent prologue-backed
  /// functions, and a jump table in .rdata (lld-link's layout) would otherwise
  /// promote every switch case.
  std::vector<std::pair<uint64_t, uint64_t>> IndexedTableRuns;
  /// Virtual addresses of relocation slots that hold an absolute pointer into
  /// mapped non-code data (R_386_32 / R_ARM_ABS32 / R_X86_64_64 /
  /// R_AARCH64_ABS64, or their Mach-O dyld rebase equivalents).  These are the
  /// entries of an absolute data-pointer table — e.g. the `.data.rel.ro` array
  /// of `const char *` a 32-bit-target switch-returning-string lowers to.  The
  /// emitter rebuilds such a table with `ptrtoint(@recompiled_data)` fields so
  /// the pointers survive relinking (the data analogue of CodePtrRelocSlots),
  /// instead of leaving the stale original VAs that would read unmapped memory.
  /// Linked PE header mappings also record their format-owned ImageBase field
  /// here. It remains distinct from the file's explicit BaseRelocations.
  std::set<uint64_t> DataPtrRelocSlots;
  /// Data-pointer slot -> address inside the relocation symbol's owning
  /// section/segment.  Slot bytes alone cannot disambiguate a one-past target
  /// from the next section's start, and VA zero is a valid ET_REL/COFF object
  /// address rather than an implicit null when this map has an entry.
  std::map<va_t, va_t> DataPtrRelocTargetOwners;
  /// Virtual addresses of 32-bit PC-relative relocation slots in read-only
  /// data whose targets are also non-code data.  Clang uses a contiguous run of
  /// these for a PIC switch that returns one of several data pointers (commonly
  /// string literals): the emitted code loads a signed entry and adds the table
  /// base.  Keeping the exact slot provenance lets later address lowering
  /// distinguish the source offset table from the data segment reached by the
  /// resulting pointer.
  std::set<uint64_t> RelDataPtrRelocSlots;
  /// Virtual addresses of relocation slots in a read-only data segment that
  /// hold a PC-relative reference to an executable target (R_X86_64_PC32 /
  /// R_386_PC32 / R_AARCH64_PREL32 / R_ARM_PREL31 / R_ARM_REL32 whose target is
  /// code).  These are the entries of a PIC `switch` jump table; a contiguous
  /// run starting at the table base gives the exact entry count, which is how a
  /// `switch(x % N)` table — bounded by the modulus, with no `cmp` range guard
  /// — is recovered.  Distinct from CodePtrRelocSlots so the entries keep their
  /// PC-relative (not absolute) interpretation.
  std::set<uint64_t> RelCodeRelocSlots;
  /// Virtual addresses of jump-table *bases* — the read-only data address a
  /// code relocation anchors (the `lea table(%rip)` / `adrp+add` a PIC switch
  /// materializes its table pointer from), filled by the loader for a
  /// relocation whose slot is in executable code and whose target is in
  /// read-only data.  When two unguarded PIC tables are laid out back-to-back
  /// in rodata their RelCodeReloc entry runs merge into one continuous run, so
  /// a run-length count from the first table's base over-reads into the second;
  /// the next anchor strictly above a table's base is that table's exact end
  /// and bounds the run to its real entry count.
  std::set<uint64_t> RelCodeTableAnchors;
  /// Virtual addresses of executable targets whose address is *taken as a
  /// value* — a function pointer materialized into a register by a relocation
  /// or a same-segment PC-relative idiom (`lea rip`, `adrp+add`, `movw/movt`,
  /// an ARM literal pool) and then stored, dispatched, or indirectly called.  A
  /// code constant equal to one of these is a `ptrtoint @func` reference that
  /// must survive relinking, not an integer literal.  Recording the exact
  /// target set keeps the symbolization relocation/lift-driven instead of a
  /// numeric guess, so a genuine integer that merely equals a low function VA
  /// (e.g. 0) is left untouched.  `mutable` because the same-segment x86 `lea
  /// rip` form carries no relocation and is recovered during lifting, when the
  /// image is const.
  mutable std::set<uint64_t> CodeRefTargets;
  /// Code-address counterpart of DataAddressRelocOperands.  The value-keyed
  /// CodeRefTargets remains the cross-stage relocation target inventory; this
  /// map identifies the instruction occurrence whose relocated operand
  /// actually materialized that target.
  std::map<va_t, RelocatedAddressField> CodeAddressRelocOperands;
  /// Function entries accepted only after FuncDetector's bounded semantic
  /// decode. Keep these separate from untyped exports and address-taken code
  /// references: both of those can legally identify data. Mutable because
  /// discovery runs through a const pipeline view and patch-time source
  /// authentication consumes the published result later in the same run.
  mutable std::set<uint64_t> VerifiedFunctionEntries;
  /// Maps the virtual address of a non-lazy/lazy pointer slot (Mach-O __got /
  /// __la_symbol_ptr, the indirect-symbol-table backed import pointer tables)
  /// to the imported symbol it binds to.  Most external calls go through a
  /// __stubs trampoline (recorded as an Import with the stub address), but a
  /// routine with a non-AAPCS ABI — notably Darwin's `____chkstk_darwin` stack
  /// probe — is called GOT-indirect (`ldr xN,[got]; blr xN`) with no call-site
  /// relocation, so its symbol is otherwise unrecoverable.  This native map is
  /// retained for Mach-O patch provenance and compatibility; format-neutral
  /// consumers use collectImportStorageSlots().
  std::map<va_t, std::string> ImportPtrSlots;
  /// Canonical exact-slot import storage identities, independent of object
  /// format.  Loaders retain their native maps below for patch provenance, but
  /// classifier and mirror consumers use collectImportStorageSlots().
  std::map<va_t, ImportStorageSlot> ImportStorageSlots;
  /// Slots for which format metadata supplied contradictory identities.  A
  /// conflict is sticky: later duplicate records cannot silently resurrect a
  /// first-wins interpretation.
  std::set<va_t> ConflictingImportStorageSlots;
  /// Exact callable pointer storage slots owned by a platform loader/runtime
  /// contract rather than by an ordinary static relocation.  Kept separate
  /// from ImportPtrSlots and CodePtrRelocSlots so a null on-disk value never
  /// masquerades as either an imported symbol or a resolved code identity.
  std::vector<RuntimeCallablePointerSlot> RuntimeCallablePointerSlots;
  /// Slots the dynamic linker fills with what a GNU indirect function's
  /// resolver returns: IRELATIVE slots, and slots bound to an IFUNC symbol.
  /// A call through one calls that implementation, whatever the file holds.
  /// Kept apart from RuntimeCallablePointerSlots: a static executable's
  /// writable segment holds them among plain data, which they do not make
  /// pointer storage.
  std::set<va_t> IndirectFunctionSlots;
  /// Pointer slots the image initializes with one of its own functions and
  /// no instruction of it changes or lets escape, slot -> function: MinGW's
  /// `__imp_X` variables (__MINGW_IMP_SYMBOL), initialized to the function
  /// its runtime supplies in place of an import.  A call through one calls
  /// that function (coff_loader::recordConstantCodeSlots).
  std::map<va_t, va_t> ConstantCodeSlots;
  /// Concrete Mach-O pointer bindings decoded from LC_DYLD_INFO bind bytecode
  /// or LC_DYLD_CHAINED_FIXUPS chains.  Kept separate from ImportPtrSlots so
  /// patch provenance remains an exact view of the indirect symbol table.
  std::map<va_t, ImportBindSlot> DyldBindSlots;
  /// Exact executable import veneer address -> Imports index.  Import::IATAddr
  /// intentionally keeps its format-native, API-visible meaning (a PE/ELF data
  /// slot, historically a Mach-O stub); recording another executable spelling
  /// of the import must never repurpose that field.
  std::map<va_t, size_t> ImportStubIndices;
  /// Callable identities synthesized for undefined functions of relocatable
  /// objects by ObjectExterns. Their extern storage is not executable code or
  /// an IAT pointer slot, but a relocated direct branch may tail-call them.
  std::set<va_t> ObjectExternFunctionEntries;
  /// An undefined data symbol of a relocatable object, whose storage
  /// ObjectExterns places in the `extern` segment.  Another module defines
  /// it, so the object neither owns nor sizes it; Bytes is the room kept past
  /// its address for the offsets the object's relocations reach.
  struct ObjectExternData {
    std::string Name;
    uint64_t Bytes = 0;
    bool operator==(const ObjectExternData &) const = default;
  };
  /// Each such symbol, by the address ObjectExterns gives it.
  std::map<va_t, ObjectExternData> ObjectExternDataSymbols;
  /// The pointer cells (ELF and Mach-O GOT entries) holding such a symbol's
  /// address, by cell address: the symbol's name.
  std::map<va_t, std::string> ObjectExternDataCells;
  /// Half-open executable intervals occupied by linker/dynamic-loader import
  /// machinery whose individual entries may not be recoverable (ELF PLT
  /// families, Mach-O symbol stubs and stub helper).
  std::vector<std::pair<va_t, va_t>> ImportStubRanges;
  /// Loader/CRT invoked function entries whose calling convention is not an
  /// ordinary user-function ABI.  Patch mode preserves these original bodies.
  std::set<va_t> RuntimeFunctionAddrs;
  DynamicInfo DynInfo;
  /// Sorted (start, end) intervals describing code regions that already
  /// belong to a function.  Used by post-pdata scanners (padding scan,
  /// data-func-ptr scan) to skip candidates that fall inside a known body.
  /// Includes BOTH primary RUNTIME_FUNCTION entries and chained-info
  /// continuation chunks.
  std::vector<std::pair<va_t, va_t>> KnownCodeRanges;
  /// Starts of KnownCodeRanges that continue another function rather than
  /// begin one: x64 RUNTIME_FUNCTIONs whose unwind info carries
  /// UNW_FLAG_CHAININFO (hot/cold split and shrink-wrapped chunks).  A jump
  /// to one of these stays inside the jumping function.
  std::set<va_t> ContinuationCodeStarts;
  /// Starts of the data objects that debug information places in executable
  /// sections where the image's own code metadata gives no code: a kernel's
  /// tables and UNICODE_STRINGs in PAGE and INIT.  An exact address of one is
  /// data, though its section holds code (recordDataInCode).
  std::set<va_t> DataInCode;
  /// When nonempty, PE language-table decode and x64 unwind materialization
  /// are limited to these entries and the catch/unwind funclets they name.
  /// KnownCodeRanges keeps the requested bodies plus immediate `.pdata`
  /// neighbors so CFG can clip; it does not ingest the rest of `.pdata`.
  /// `hasKnownFunctionEntryAt` still binary-searches `COFFPDataRecords`.
  std::set<va_t> LoadOnlyFunctionEntries;
  /// Whether the load read the image's static symbol table, which strip
  /// removes: an ELF image's .symtab, also the names an SBF program carries
  /// there, a PE's COFF symbol table, or a Mach-O image's symbols that it
  /// does not export.  False when the caller declined it, so the image reads
  /// as if stripped.  The symbols dynamic linking needs are read either way,
  /// and an object file is never read without its table.
  bool ReadsStaticSymbols = true;
  /// Compact x64 `.pdata` index used when LoadOnlyFunctionEntries is set so
  /// unrequested RUNTIME_FUNCTION bodies are not materialized as
  /// ExceptionFunction records.  Sorted by BeginRVA.  Empty on a full load.
  struct COFFPDataRecord {
    uint32_t BeginRVA = 0;
    uint32_t EndRVA = 0;
    uint32_t UnwindInfoRVA = 0;
    uint32_t RecordRVA = 0;
  };
  std::vector<COFFPDataRecord> COFFPDataRecords;
  /// The PE file's Rich header: which builds of which Microsoft tools
  /// produced the objects the linker combined.  Empty for other formats and
  /// for images a non-Microsoft linker produced.
  std::optional<RichHeader> COFFRichHeader;
  /// The header bytes a linked PE image's loader maps at Base: SizeOfHeaders,
  /// up to the first section that maps over them, whether or not this image
  /// maps them as `<PE headers>`.  Zero for every other image, and for one
  /// without a nonzero 64 KiB-aligned base (imageBaseOffset).
  uint32_t PEHeaderBytes = 0;
  /// Checked, normalized table-based unwind and language exception metadata.
  /// Empty for formats/targets without a supported exception directory.
  ExceptionInfo ExceptionMetadata;
  std::vector<uint8_t> Raw;
  /// Digest of the exact file buffer consumed by the loader. Kept when a
  /// restricted load omits Raw; never recomputed by reopening a mutable path.
  /// Absent for in-memory synthetic images or loaders without file evidence.
  std::optional<std::array<uint8_t, 32>> InputFileSHA256;
  /// Immutable pre-edit image for byte-based identity only. lib/patch owns
  /// publication and retirement alongside the materialized byte overlay.
  std::shared_ptr<const BinaryImage> BytePatchOriginal;
  std::vector<std::pair<va_t, uint64_t>> BytePatchRanges;

  bool hasBytePatchIn(va_t Address, uint64_t Size) const {
    for (const auto &[Start, Width] : BytePatchRanges)
      if (Size && Width &&
          (Address <= Start ? Start - Address < Size : Address - Start < Width))
        return true;
    return false;
  }
  /// Conservative contiguous extent for identity-derived call traits. The
  /// pipeline additionally checks its recovered discontiguous basic blocks.
  bool functionHasBytePatch(va_t Address) const {
    if (BytePatchRanges.empty())
      return false;
    const auto *Segment = getSegmentFor(Address);
    if (!Segment || !isCodeAddress(Address))
      return false;
    va_t End = Segment->VA + Segment->Size;
    const va_t Next = nextKnownFunctionEntryAfter(Address);
    if (Next != InvalidVA)
      End = std::min(End, Next);
    return End > Address && hasBytePatchIn(Address, End - Address);
  }

  bool is64Bit() const { return neverd::is64Bit(Bits); }
  bool is32Bit() const { return neverd::is32Bit(Bits); }
  bool is256Bit() const { return neverd::is256Bit(Bits); }
  uint32_t getPointerSize() const {
    return getBitnessValue(Bits) / kBitsPerByte;
  }

  /// Exact ELF mapping interval, when one owns this AArch32 address. A $d
  /// interval is data even when it lies inside an executable section or a
  /// sized function symbol.
  const ARMCodeRegion *armMappingRegionAt(va_t Addr) const {
    if (Arch != neverd::Arch::ARM)
      return nullptr;
    const auto It =
        std::upper_bound(ARMCodeRegions.begin(), ARMCodeRegions.end(), Addr,
                         [](va_t Address, const ARMCodeRegion &Region) {
                           return Address < Region.Start;
                         });
    if (It == ARMCodeRegions.begin())
      return nullptr;
    const ARMCodeRegion &Region = *std::prev(It);
    return Addr < Region.End ? &Region : nullptr;
  }

  /// The loader's address-specific AArch32 evidence. An unknown byte in a
  /// mixed image, or a $d interval, cannot be decoded by guessing a mode.
  std::optional<InstructionMode>
  instructionModeAt(va_t Addr,
                    std::optional<InstructionMode> IncomingMode = {}) const {
    if (Arch != neverd::Arch::ARM)
      return Mode;
    if (!isCodeAddress(Addr))
      return std::nullopt;
    std::optional<InstructionMode> ProvenMode;
    if (const ARMCodeRegion *Region = armMappingRegionAt(Addr)) {
      if (Region->Kind == ARMCodeRegionKind::Data)
        return std::nullopt;
      ProvenMode = Region->Kind == ARMCodeRegionKind::Thumb
                       ? InstructionMode::Thumb
                       : InstructionMode::ARM;
    }
    const auto Reachable = std::upper_bound(
        ARMReachableCodeRegions.begin(), ARMReachableCodeRegions.end(), Addr,
        [](va_t Address, const ARMCodeRegion &Region) {
          return Address < Region.Start;
        });
    if (Reachable != ARMReachableCodeRegions.begin()) {
      const ARMCodeRegion &Region = *std::prev(Reachable);
      if (Addr < Region.End) {
        const InstructionMode DecodedMode =
            Region.Kind == ARMCodeRegionKind::Thumb ? InstructionMode::Thumb
                                                    : InstructionMode::ARM;
        if (ProvenMode && *ProvenMode != DecodedMode)
          return std::nullopt;
        ProvenMode = DecodedMode;
      }
    }
    if (const auto Entry = ARMCodeModeEntries.find(Addr);
        Entry != ARMCodeModeEntries.end()) {
      if (ProvenMode && *ProvenMode != Entry->second)
        return std::nullopt;
      ProvenMode = Entry->second;
    }
    if (ProvenMode && IncomingMode && *ProvenMode != *IncomingMode)
      return std::nullopt;
    if (ARMRequiredMode != InstructionMode::Default) {
      if ((ProvenMode && *ProvenMode != ARMRequiredMode) ||
          (IncomingMode && *IncomingMode != ARMRequiredMode))
        return std::nullopt;
      return ARMRequiredMode;
    }
    if (ProvenMode)
      return ProvenMode;
    if (IncomingMode)
      return IncomingMode;
    if (ARMReachabilityConstrained)
      return std::nullopt;
    // A weak file-level mode cannot authorize an unmarked function entry.
    if (ARMModeRequiresLocalEvidence)
      return std::nullopt;
    if (Mode == InstructionMode::MixedARMThumb)
      return std::nullopt;
    if (Mode == InstructionMode::Default)
      return Format == BinaryFormat::MachO
                 ? std::nullopt
                 : std::optional<InstructionMode>(InstructionMode::ARM);
    return Mode;
  }

  std::set<va_t> getSymbolAddresses() const {
    // Built from sorted addresses, each node goes in next to the one before.
    std::vector<va_t> Addrs;
    Addrs.reserve(Symbols.size());
    for (const auto &Sym : Symbols)
      Addrs.push_back(Sym.Addr);
    llvm::sort(Addrs);
    return std::set<va_t>(Addrs.begin(), Addrs.end());
  }

  /// Function starts supported only by a prologue heuristic: no other symbol
  /// names the address and no primary unwind record begins there. Padding or
  /// an unrelocated data word alone cannot confirm it. See IsBoundaryGuess.
  std::set<va_t> boundaryGuessFunctionStarts() const {
    // Debug info renames a guessed symbol in place; a real name confirms it.
    const auto Guessed = [](const Symbol &Sym) {
      return Sym.IsBoundaryGuess && isSynthesizedFuncName(Sym.Name);
    };
    std::set<va_t> Guesses;
    for (const auto &Sym : Symbols)
      if (Guessed(Sym))
        Guesses.insert(Sym.Addr);
    // Most images have no guesses, and those that do have few: drop the
    // confirmed ones rather than collecting every confirmed address.
    if (Guesses.empty())
      return Guesses;
    for (const auto &Sym : Symbols)
      if (!Guessed(Sym))
        Guesses.erase(Sym.Addr);
    for (const auto &[Start, End] : KnownCodeRanges)
      if (Guesses.count(Start) && !isFunctionContinuationAt(Start))
        Guesses.erase(Start);
    return Guesses;
  }

  const uint8_t *readVA(va_t Addr, size_t Len) const {
    for (const auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, Len, Seg.Data.size()))
        return nullptr;
      return Seg.Data.data() + Off;
    }
    return nullptr;
  }

  /// True if \p Addr falls within any mapped segment of this image.
  bool containsVA(va_t Addr) const {
    for (const auto &Seg : Segments)
      if (Seg.contains(Addr))
        return true;
    return false;
  }

  /// True when mapped/readable section metadata owns any byte of \p Seg.
  /// This is segment-local: a partial/packed image may describe sections for
  /// one mapping while leaving another mapping intentionally section-less.
  bool segmentHasReadableSectionMetadata(const Segment &Seg) const {
    if (Seg.Size > InvalidVA - Seg.VA)
      return false;
    const uint64_t SegmentEnd = Seg.VA + Seg.Size;
    for (const Section &Candidate : Sections) {
      if (!Candidate.isReadable() || Candidate.Size == 0 ||
          Candidate.Size > InvalidVA - Candidate.VA)
        continue;
      const uint64_t CandidateEnd = Candidate.VA + Candidate.Size;
      if (Candidate.VA < SegmentEnd && Seg.VA < CandidateEnd)
        return true;
    }
    return false;
  }

  /// True when \p Addr in \p Seg holds the ELF file header or program
  /// header table, which a load segment maps but no object data occupies.
  bool isELFHeaderByte(const Segment &Seg, va_t Addr) const {
    if (!ELFMetadata || Addr < Seg.VA || Addr - Seg.VA >= Seg.FileSz)
      return false;
    const uint64_t Offset = Seg.FileOff + (Addr - Seg.VA);
    if (Offset < ELFMetadata->HeaderSize)
      return true;
    const uint64_t TableSize = uint64_t(ELFMetadata->ProgramHeaderEntrySize) *
                               ELFMetadata->ProgramHeaders.size();
    return Offset >= ELFMetadata->ProgramHeaderFileOffset &&
           Offset - ELFMetadata->ProgramHeaderFileOffset < TableSize;
  }

  /// True when \p Addr is backed by format-native object data, rather than
  /// merely falling inside a coarse readable load segment. Exact section
  /// metadata excludes executable bytes plus ELF/Mach-O headers and alignment
  /// gaps; section-less images conservatively fall back to non-executable
  /// file-backed segments, less the ELF headers they map.
  bool hasObjectDataProvenance(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg || Seg->Data.empty() || Addr < Seg->VA ||
        Addr - Seg->VA >= Seg->Data.size() || !Seg->isReadable())
      return false;

    if (Sections.empty())
      return !Seg->isExecutable() && !isELFHeaderByte(*Seg, Addr);
    if (const Section *Sec = getSectionFor(Addr))
      return Sec->isReadable() &&
             (isMachO() ? !isCodeAddress(Addr) : !Sec->isExecutable());

    // Some synthetic/stripped images describe a whole data segment but omit a
    // section row for it. Fall back only if no section overlaps this
    // segment at all; otherwise an uncovered byte is a header/alignment gap.
    return !segmentHasReadableSectionMetadata(*Seg) && !Seg->isExecutable();
  }

  /// The offset of \p Addr into the image's own PE headers
  /// ([Base, Base + PEHeaderBytes)), mapped or not.  The linker's
  /// `__ImageBase` names these bytes: an emitter spells such an address from
  /// it and never copies or folds a header byte, since the headers describe
  /// the image that runs.  A base of zero would make every small number a
  /// header address, so such an image records no headers.
  std::optional<uint64_t> imageBaseOffset(va_t Addr) const {
    if (!PEHeaderBytes || Addr < Base || Addr - Base >= PEHeaderBytes)
      return std::nullopt;
    return Addr - Base;
  }

  /// The undefined data symbol of a relocatable object whose extern storage
  /// holds \p Addr (ObjectExternDataSymbols), and the offset of \p Addr into
  /// it.  An emitter reaches such an address through the symbol's
  /// declaration: the storage is another module's, never one to define or
  /// copy.
  std::optional<std::pair<const ObjectExternData *, uint64_t>>
  objectExternDataAt(va_t Addr) const {
    auto It = ObjectExternDataSymbols.upper_bound(Addr);
    if (It == ObjectExternDataSymbols.begin())
      return std::nullopt;
    --It;
    if (Addr - It->first >= It->second.Bytes)
      return std::nullopt;
    return std::pair(&It->second, Addr - It->first);
  }

  /// Loader/layout-only upper bound for constants whose numeric value may
  /// change when the image is rebuilt.  This deliberately ignores use-site
  /// heuristics: callers may conservatively retain arithmetic or CFG edges,
  /// but must never freeze a relation between two independently emitted
  /// symbols.  Keeping this fact on BinaryImage gives every format and every
  /// semantic stage one owner for mapped, anchored, relocation-proven, and
  /// one-past-end address candidates, and for the image's own headers.
  bool isPotentiallyRelocatableAddress(va_t Addr) const {
    const bool LoaderProven = RelocDataAddrs.count(Addr) != 0 ||
                              WritableRelocDataAddrs.count(Addr) != 0 ||
                              RodataAnchorSeg.count(Addr) != 0 ||
                              CodeRefTargets.count(Addr) != 0;
    if (LoaderProven)
      return true;

    // Zero remains the null integer unless exact loader provenance above says
    // otherwise. Relocatable objects may synthesize a section at VA zero, but
    // treating every pointer-width zero as its address would suppress nearly
    // all ordinary zero arithmetic in that object.
    if (Addr == 0)
      return false;

    // The headers move with the image whether or not the loader maps them.
    if (imageBaseOffset(Addr))
      return true;

    if (const Segment *Seg = getSegmentFor(Addr))
      return Seg->isReadable();

    // Both read-only runs (file-backed extent) and writable/bss runs (virtual
    // extent) may materialize a legal one-past-end pointer in the emitter.
    for (const Segment &Seg : Segments) {
      if (!Seg.isReadable())
        continue;
      const uint64_t Extents[] = {Seg.Data.size(), Seg.Size};
      for (uint64_t Extent : Extents) {
        const bool HasFileBackedOwner =
            Addr > Seg.VA && hasObjectDataProvenance(Addr - 1);
        const bool HasVirtualDataOwner =
            Extent == Seg.Size && !Seg.isExecutable();
        if (Extent != 0 && Extent <= InvalidVA - Seg.VA &&
            Addr == Seg.VA + Extent && Addr > Seg.VA &&
            (HasFileBackedOwner || HasVirtualDataOwner))
          return true;
      }
    }
    return false;
  }

  /// Write bytes at a virtual address (modifies in-memory segment data).
  /// Returns true on success.
  bool writeVA(va_t Addr, const uint8_t *Src, size_t Len) {
    for (auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, Len, Seg.Data.size()))
        return false;
      std::memcpy(Seg.Data.data() + Off, Src, Len);
      return true;
    }
    return false;
  }

  /// Patch a pointer-sized value at a virtual address.
  bool patchPtr(va_t Addr, uint64_t Value) {
    uint32_t PtrSz = getPointerSize();
    for (auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, PtrSz, Seg.Data.size()))
        return false;
      writePtr(Seg.Data.data() + Off, Value, is64Bit());
      return true;
    }
    return false;
  }

  const Segment *getSegmentFor(va_t Addr) const {
    for (const auto &Seg : Segments)
      if (Seg.contains(Addr))
        return &Seg;
    return nullptr;
  }

  /// Whether the \p Width bytes at \p Addr keep, once loading is done, what
  /// the loader left there: they lie in a read-only segment, in one the
  /// format makes read-only once relocated (Segment::ReadOnlyAfterRelocations),
  /// or in a range a linked ELF's PT_GNU_RELRO covers. Segment and section
  /// names alone never establish immutability.
  bool readOnlyAfterRelocations(va_t Addr, uint64_t Width) const;

  /// Return the mapped format-native section that owns \p Addr.
  ///
  /// Relocatable ELF metadata sections do not have runtime addresses.  Their
  /// synthetic VA remains zero, so a large `.strtab`, `.symtab`, or relocation
  /// section can numerically overlap every low-VA SHF_ALLOC section.  Such
  /// metadata must never win an address lookup: Sections with Readable set are
  /// the loader model's mapped/SHF_ALLOC sections, and the address must also be
  /// backed by a mapped segment.
  const Section *getSectionFor(va_t Addr) const {
    if (!getSegmentFor(Addr))
      return nullptr;
    for (const auto &Sec : Sections)
      if (Sec.isReadable() && Sec.contains(Addr))
        return &Sec;
    return nullptr;
  }

  /// Verify an occurrence-local relocated target against the section/segment
  /// owner retained by the loader.  Code addresses must lie strictly inside
  /// their owner; data addresses may name the one-past value used by C/C++
  /// address arithmetic.  Numeric adjacency is not ownership: a target equal
  /// to the next section's base remains one-past the recorded owner.  The
  /// optional index serves the executable-owner queries, with the same rules.
  bool relocatedTargetBelongsToOwner(
      va_t TargetVA, va_t OwnerVA,
      const ExecutableCodeOwnerIndex *Index = nullptr) const {
    if (OwnerVA == InvalidVA)
      return false;
    for (const Section &Sec : Sections) {
      if (!Sec.isReadable() || Sec.VA != OwnerVA ||
          Sec.Size > InvalidVA - Sec.VA)
        continue;
      const va_t End = Sec.VA + Sec.Size;
      const bool OwnerIsCode = hasExecutableCodeOwnerAt(OwnerVA, Index);
      return TargetVA >= Sec.VA &&
             (OwnerIsCode
                  ? TargetVA < End && hasExecutableCodeOwnerAt(TargetVA, Index)
                  : TargetVA <= End);
    }
    for (const Segment &Seg : Segments) {
      if (!Seg.isReadable() || Seg.VA != OwnerVA ||
          Seg.Size > InvalidVA - Seg.VA)
        continue;
      const va_t End = Seg.VA + Seg.Size;
      const bool OwnerIsCode = hasExecutableCodeOwnerAt(OwnerVA, Index);
      return TargetVA >= Seg.VA &&
             (OwnerIsCode
                  ? TargetVA < End && hasExecutableCodeOwnerAt(TargetVA, Index)
                  : TargetVA <= End);
    }
    // Unmapped headers own their addresses as `<PE headers>` does.
    return PEHeaderBytes && OwnerVA == Base && TargetVA >= Base &&
           TargetVA - Base <= PEHeaderBytes;
  }

  /// The owner of an address the image computes from its own fixed layout,
  /// as relocatedTargetBelongsToOwner checks it: the mapped section that
  /// holds \p Addr, or a readable section-less segment.  A gap in a segment
  /// that otherwise has section metadata has no owner.
  va_t layoutAddressOwner(va_t Addr) const {
    if (const Section *Sec = getSectionFor(Addr))
      return Sec->VA;
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg || !Seg->isReadable() || segmentHasReadableSectionMetadata(*Seg))
      return InvalidVA;
    return Seg->VA;
  }

  /// True when this relocatable ELF object's complete relocation write
  /// footprint is known and no relocation writes a byte of [VA, VA + Width):
  /// those bytes hold the assembler's final value.
  bool isUnrelocatedObjectField(va_t VA, unsigned Width) const {
    if (!isELF() || !IsRelocatable || !ObjectRelocationWriteBytes ||
        Width == 0 || VA > InvalidVA - Width)
      return false;
    const auto Writer = ObjectRelocationWriteBytes->lower_bound(VA);
    return Writer == ObjectRelocationWriteBytes->end() || *Writer >= VA + Width;
  }

  /// Verify the symbol owner retained for an i386 ELF R_386_GOTOFF field, or
  /// for an absolute instruction operand anchored the same way.  Most addends
  /// resolve inside (or one-past) their symbol's ordinary section owner and
  /// use relocatedTargetBelongsToOwner directly.  Clang may instead fold a
  /// negative selector bias into the relocation, placing S+A before the
  /// symbol's rodata load segment.  The ELF loader records that exceptional
  /// relation in RodataAnchorSeg; accept it only when the exact folded VA
  /// names the same read-only data segment as the exact section owner.
  bool relocatedI386GOTOFFTargetBelongsToOwner(va_t TargetVA,
                                               va_t OwnerVA) const {
    if (Arch != Arch::X86 || !isELF() || getPointerSize() != 4 ||
        OwnerVA == InvalidVA)
      return false;
    if (relocatedTargetBelongsToOwner(TargetVA, OwnerVA))
      return true;

    const auto Anchor = RodataAnchorSeg.find(TargetVA);
    if (Anchor == RodataAnchorSeg.end())
      return false;
    const Segment *OwnerSeg = getSegmentFor(OwnerVA);
    if (!OwnerSeg || Anchor->second != OwnerSeg->VA ||
        !OwnerSeg->isReadable() || OwnerSeg->isWritable() ||
        TargetVA > std::numeric_limits<uint32_t>::max() ||
        OwnerSeg->VA > std::numeric_limits<uint32_t>::max() ||
        hasExecutableCodeOwnerAt(OwnerVA) || !hasObjectDataProvenance(OwnerVA))
      return false;

    // GOTOFF arithmetic is performed in the i386 guest's 32-bit address
    // space.  A negative selector bias may therefore wrap below a rodata
    // segment at VA zero to (for example) 0xfffffff0.  Authenticate only the
    // exact loader-recorded anchor and a small, non-zero modular backward
    // distance; an absolute data slot never populates this relation.
    const uint32_t BackDistance =
        static_cast<uint32_t>(OwnerSeg->VA) - static_cast<uint32_t>(TargetVA);
    if (BackDistance == 0 ||
        BackDistance > limits::kMaxRodataAnchorBackDistance)
      return false;

    if (const Section *OwnerSec = getSectionFor(OwnerVA))
      return OwnerSec->VA == OwnerVA && OwnerSec->isReadable() &&
             !OwnerSec->isWritable() && !OwnerSec->isExecutable();
    return OwnerSeg->VA == OwnerVA;
  }

  /// Half-open end of the mapped section/object-storage owner containing
  /// \p Addr. An uncovered gap in a segment that otherwise has section
  /// metadata has no owner; a genuinely section-less segment owns its
  /// materialized bytes as one compatibility range.
  std::optional<va_t> mappedObjectOwnerEnd(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg || Addr < Seg->VA || Seg->Data.empty() ||
        Seg->Data.size() > InvalidVA - Seg->VA)
      return std::nullopt;
    const va_t MaterializedEnd = Seg->VA + Seg->Data.size();
    if (const ARMCodeRegion *Region = armMappingRegionAt(Addr);
        Region && Region->Kind == ARMCodeRegionKind::Data)
      return std::min<va_t>(Region->End, MaterializedEnd);
    if (const Section *Sec = getSectionFor(Addr)) {
      if (Sec->Size == 0 || Sec->Size > InvalidVA - Sec->VA)
        return std::nullopt;
      return std::min<va_t>(Sec->VA + Sec->Size, MaterializedEnd);
    }
    if (segmentHasReadableSectionMetadata(*Seg))
      return std::nullopt;
    return MaterializedEnd;
  }

  /// Classify one mapped address using the finest format-native provenance.
  /// An exact format-native section takes precedence over coarse segment
  /// permissions. Mach-O uses instruction attributes; ELF uses the section's
  /// executable flag. COFF also declares instruction storage with CNT_CODE,
  /// independently of runtime page permissions (for example INITDATA).
  /// This matters when linkers place non-code data
  /// in the same RX mapping as `.text`. A header or alignment gap is not code
  /// when section metadata exists; section-less images retain the historical
  /// segment fallback.
  bool isCodeAddress(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg)
      return false;
    if (const ARMCodeRegion *Region = armMappingRegionAt(Addr);
        Region && Region->Kind == ARMCodeRegionKind::Data)
      return false;
    if (const Section *Sec = getSectionFor(Addr)) {
      if (isMachO())
        return (Sec->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                             llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) != 0;
      if (Format == BinaryFormat::COFF &&
          (Sec->Type & llvm::COFF::IMAGE_SCN_CNT_CODE))
        return true;
      return Sec->isExecutable();
    }
    if (segmentHasReadableSectionMetadata(*Seg))
      return false;
    return Seg->isExecutable();
  }

  /// True when one complete half-open byte range has a single code owner.
  bool isCodeRange(va_t Addr, uint64_t Size) const {
    if (Size == 0 || Size - 1 > InvalidVA - Addr)
      return false;
    const va_t Last = Addr + Size - 1;
    const Segment *FirstSeg = getSegmentFor(Addr);
    if (!FirstSeg || getSegmentFor(Last) != FirstSeg || !isCodeAddress(Addr) ||
        !isCodeAddress(Last))
      return false;
    const Section *FirstSec = getSectionFor(Addr);
    const Section *LastSec = getSectionFor(Last);
    if (!((!FirstSec && !LastSec) ||
          (FirstSec && LastSec && FirstSec == LastSec)))
      return false;
    // Endpoints can both be instructions while a literal island lies between
    // them. Never grant a complete code range across an intervening $d span.
    if (Arch == neverd::Arch::ARM) {
      auto It =
          std::upper_bound(ARMCodeRegions.begin(), ARMCodeRegions.end(), Addr,
                           [](va_t Address, const ARMCodeRegion &Region) {
                             return Address < Region.Start;
                           });
      while (It != ARMCodeRegions.end() && It->Start <= Last) {
        if (It->Kind == ARMCodeRegionKind::Data)
          return false;
        ++It;
      }
    }
    return true;
  }

  bool isDataAddress(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    return Seg && Seg->isReadable() && !isCodeAddress(Addr);
  }

  /// Read a fixed scalar from an immutable AArch32 literal island. With
  /// sections, mapping symbols must prove its complete extent. A sectionless
  /// linked ELF retains immutable executable bytes but loses those symbols;
  /// reading those bytes does not classify them as code or as a literal pool.
  /// Relocations remain memory reads because the loader can change their value.
  std::optional<uint64_t> readImmutableARMLiteral(va_t Addr,
                                                  uint16_t Size) const {
    if (Arch != neverd::Arch::ARM ||
        (Size != 1 && Size != 2 && Size != 4 && Size != 8))
      return std::nullopt;
    const ARMCodeRegion *Region = armMappingRegionAt(Addr);
    const Segment *Seg = getSegmentFor(Addr);
    const bool SectionlessELF = isELF() && !IsRelocatable && Sections.empty();
    if ((Region ? Region->Kind != ARMCodeRegionKind::Data ||
                      Size > Region->End - Addr
                : !SectionlessELF) ||
        !Seg || !Seg->isReadable() || !Seg->isExecutable() || Seg->isWritable())
      return std::nullopt;
    const uint8_t *Bytes = readVA(Addr, Size);
    if (!Bytes || Addr > InvalidVA - (Size - 1))
      return std::nullopt;
    if (isELF() && IsRelocatable && ObjectRelocationWriteBytes) {
      // An object's certified write footprint names every byte a relocation
      // writes, so a scalar outside it keeps its bytes even beside a fixup.
      if (!isUnrelocatedObjectField(Addr, Size))
        return std::nullopt;
    } else {
      // A fixup can begin before this access and overlap it. Conservatively
      // exclude every possible eight-byte slot touching the scalar.
      const va_t First = Addr >= 7 ? Addr - 7 : 0;
      const va_t Last = Addr + Size - 1;
      for (va_t Byte = First;; ++Byte) {
        if (hasRelocationProvenanceAt(Byte))
          return std::nullopt;
        if (Byte == Last)
          break;
      }
    }
    uint64_t Value = 0;
    for (uint16_t I = 0; I < Size; ++I)
      Value |= static_cast<uint64_t>(Bytes[I]) << (8 * I);
    return Value;
  }

  const Section *getSectionByName(llvm::StringRef Name) const {
    for (const auto &Sec : Sections)
      if (Sec.Name == Name)
        return &Sec;
    return nullptr;
  }

  /// Read an array of pointer-sized values from the loaded image at \p Addr.
  /// Returns up to \p MaxCount entries; stops at segment boundaries.
  std::vector<va_t> readPtrArray(va_t Addr, size_t MaxCount) const {
    std::vector<va_t> Result;
    uint32_t PtrSz = getPointerSize();
    if (PtrSz == 0 || MaxCount == 0)
      return Result;
    const uint8_t *P = readVA(Addr, PtrSz);
    if (!P)
      return Result;
    for (size_t I = 0; I < MaxCount; ++I) {
      if (I > (InvalidVA - Addr) / PtrSz)
        break;
      P = readVA(Addr + I * PtrSz, PtrSz);
      if (!P)
        break;
      Result.push_back(static_cast<va_t>(readPtr(P, is64Bit())));
    }
    return Result;
  }

  std::vector<va_t> getExecutableRanges() const {
    std::vector<va_t> Ranges;
    for (const auto &Seg : Segments) {
      if (!Seg.isExecutable() || Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      Ranges.push_back(Seg.VA);
      Ranges.push_back(Seg.VA + Seg.Size);
    }
    return Ranges;
  }

  // --- Format queries ---

  bool isELF() const { return Format == BinaryFormat::ELF; }
  bool isCOFF() const { return Format == BinaryFormat::COFF; }
  bool isMachO() const { return Format == BinaryFormat::MachO; }
  bool isEVM() const { return Format == BinaryFormat::EVM; }

  /// Get the main code section (".text" / "__text").
  ///
  /// Falls back to a flag-based pick when the canonical name is absent — e.g. a
  /// binary already processed by a packer/protector that renames its code
  /// section (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names). The
  /// executable section containing the entry point wins; otherwise the largest
  /// executable section. Returns nullptr only when there is no executable
  /// section at all.
  const Section *getTextSection() const {
    if (const Section *Named = getSectionByName(getTextSectionName()))
      return Named;

    const Section *Best = nullptr;
    for (const auto &Sec : Sections) {
      if (!Sec.isExecutable())
        continue;
      if (Entry != 0 && Sec.contains(Entry))
        return &Sec;
      if (!Best || Sec.Size > Best->Size)
        Best = &Sec;
    }
    return Best;
  }

  /// Get the executable segment containing the text section.
  const Segment *getTextSegment() const {
    if (const Section *Text = getTextSection())
      return getSegmentFor(Text->VA);
    for (const auto &Seg : Segments)
      if (Seg.isExecutable())
        return &Seg;
    return nullptr;
  }

  /// Convert a virtual address to a file offset.
  /// Returns ~0ULL on failure.
  uint64_t vaToFileOffset(va_t Addr) const {
    for (const auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      uint64_t Delta = Addr - Seg.VA;
      if (Delta >= Seg.FileSz || Delta > InvalidVA - Seg.FileOff)
        return InvalidVA;
      return Seg.FileOff + Delta;
    }
    return InvalidVA;
  }

  /// Convert a file offset to a virtual address.
  /// Returns InvalidVA on failure.
  va_t fileOffsetToVA(uint64_t Offset) const {
    for (const auto &Seg : Segments) {
      if (Offset < Seg.FileOff)
        continue;
      uint64_t Delta = Offset - Seg.FileOff;
      if (Delta < Seg.FileSz && Delta <= InvalidVA - Seg.VA)
        return Seg.VA + Delta;
    }
    return InvalidVA;
  }

  /// For PE/COFF: convert RVA to VA using ImageBase.
  va_t rvaToVA(uint64_t RVA) const { return Base + RVA; }
  /// For PE/COFF: convert VA to RVA using ImageBase.
  uint64_t vaToRVA(va_t Addr) const { return Addr - Base; }

  /// Get all executable segments.
  std::vector<const Segment *> getExecutableSegments() const {
    std::vector<const Segment *> Result;
    for (const auto &Seg : Segments)
      if (Seg.isExecutable())
        Result.push_back(&Seg);
    return Result;
  }

  /// Get all writable segments.
  std::vector<const Segment *> getWritableSegments() const {
    std::vector<const Segment *> Result;
    for (const auto &Seg : Segments)
      if (Seg.isWritable())
        Result.push_back(&Seg);
    return Result;
  }

  /// Check whether this binary has any imports.
  bool hasImports() const { return !Imports.empty(); }

  /// Check whether this binary has any exports.
  bool hasExports() const { return !Exports.empty(); }

  /// Check whether this binary has any relocations.
  bool hasRelocations() const { return !Relocations.empty(); }

  /// True when loader metadata records a relocation/fixup whose storage slot
  /// begins at \p Address.  This is deliberately format-neutral: PE base
  /// relocations, ELF/Mach-O object relocations, dyld binds, imports, and the
  /// normalized absolute/relative pointer-slot sets all answer through the
  /// same query.  Target-address evidence such as RelocDataAddrs and
  /// CodeRefTargets is excluded because those sets do not identify the bytes
  /// being loaded.  hasRelocationProvenanceIn (RelocationProvenance.h) asks
  /// the same of a range of bytes; the two read the same sources.
  bool hasRelocationProvenanceAt(va_t Address) const {
    if (MachOAuthenticatedPointers.count(Address))
      return true;
    if (CodePtrRelocSlots.count(Address) || DataPtrRelocSlots.count(Address) ||
        RelDataPtrRelocSlots.count(Address) || RelCodeRelocSlots.count(Address))
      return true;
    if (auto It = ImportStorageSlots.find(Address);
        It != ImportStorageSlots.end() &&
        isValidImportStorageSlot(Address, It->second.Name))
      return true;
    if (auto It = ImportPtrSlots.find(Address);
        It != ImportPtrSlots.end() &&
        isValidImportStorageSlot(Address, It->second))
      return true;
    if (auto It = DyldBindSlots.find(Address);
        It != DyldBindSlots.end() &&
        isValidImportStorageSlot(Address, It->second.Name))
      return true;
    if (std::any_of(BaseRelocations.begin(), BaseRelocations.end(),
                    [&](const BaseRelocation &Reloc) {
                      return Reloc.Address == Address;
                    }))
      return true;
    return std::any_of(
        Relocations.begin(), Relocations.end(),
        [&](const RelocationEntry &Reloc) { return Reloc.Address == Address; });
  }

  /// Get the total virtual size of all loadable segments.
  uint64_t getVirtualSize() const {
    va_t Lo = InvalidVA, Hi = 0;
    for (const auto &Seg : Segments) {
      if (Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      if (Seg.VA < Lo)
        Lo = Seg.VA;
      va_t End = Seg.VA + Seg.Size;
      if (End > Hi)
        Hi = End;
    }
    return (Lo != InvalidVA && Hi > Lo) ? Hi - Lo : 0;
  }

  /// Find an import by name.
  const Import *findImport(llvm::StringRef Name) const {
    for (const auto &Imp : Imports)
      if (Imp.Name == Name)
        return &Imp;
    return nullptr;
  }

  /// Find an export by name.
  const Export *findExport(llvm::StringRef Name) const {
    for (const auto &Exp : Exports)
      if (Exp.Name == Name)
        return &Exp;
    return nullptr;
  }

  /// Find an export by address.
  const Export *findExportAt(va_t Addr) const {
    for (const auto &Exp : Exports)
      if (Exp.Addr == Addr)
        return &Exp;
    return nullptr;
  }

  /// Find a symbol by name.
  const Symbol *findSymbol(llvm::StringRef Name) const {
    for (const auto &Sym : Symbols)
      if (Sym.Name == Name)
        return &Sym;
    return nullptr;
  }

  /// Find the symbol that names \p Addr: the first there, a section's own
  /// symbol (Symbol::IsSection) aside.
  const Symbol *findSymbolAt(va_t Addr) const {
    for (const auto &Sym : Symbols)
      if (Sym.Addr == Addr && !Sym.IsSection)
        return &Sym;
    return nullptr;
  }

  /// True when typed symbol metadata owns p Addr as a function entry.
  /// Normalize ARM/Thumb spellings so patch-time synthetic original-VA
  /// symbols can recover the same code identity after reloading the image.
  /// An operation-scoped index makes repeated CFG queries logarithmic without
  /// mutating this publicly editable image from parallel workers. An absent or
  /// foreign index reads the current Symbols directly.
  /// Resumable fallthrough can exclude entries supported only by padding
  /// guesses; explicit transfers still consider those potential callees.
  bool hasFunctionSymbolAt(va_t Addr,
                           const ExecutableCodeOwnerIndex *Index = nullptr,
                           bool IncludeBoundaryGuesses = true) const;

  /// Largest non-function symbol size defined exactly at \p Addr (0 if none).
  /// Distinguishes a sized data object (a const array/table) from a bare label,
  /// so a NUL byte inside the object is not mistaken for a string terminator.
  uint64_t dataObjectSizeAt(va_t Addr) const {
    uint64_t Best = 0;
    for (const auto &Sym : Symbols)
      if (Sym.Addr == Addr && !Sym.IsFunc && Sym.Size > Best)
        Best = Sym.Size;
    return Best;
  }

  /// Find a segment by name.
  const Segment *findSegmentByName(llvm::StringRef Name) const {
    for (const auto &Seg : Segments)
      if (Seg.Name == Name)
        return &Seg;
    return nullptr;
  }

  /// Find a mutable segment by name.
  Segment *findSegmentByName(llvm::StringRef Name) {
    for (auto &Seg : Segments)
      if (Seg.Name == Name)
        return &Seg;
    return nullptr;
  }

  /// Get the image end address (highest VA + size across all segments).
  va_t getImageEnd() const {
    va_t Hi = 0;
    for (const auto &Seg : Segments) {
      if (Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      va_t End = Seg.VA + Seg.Size;
      if (End > Hi)
        Hi = End;
    }
    return Hi;
  }

  /// Get the format-appropriate text section name.
  llvm::StringRef getTextSectionName() const {
    if (isEVM())
      return kEVMCodeSectionName;
    return isMachO() ? section_names::macho::Text : section_names::elf::Text;
  }

  /// Get the data section (".data" / "__data").
  const Section *getDataSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Data);
    return getSectionByName(section_names::elf::Data);
  }

  /// How the kernel would enter \p FunctionEntry: it starts an ELF program at
  /// its entry point, and calls reach the rest.  functionEntryKind checks the
  /// code there is not a function that returns.
  StackEntryKind stackEntryKindAt(va_t FunctionEntry) const {
    return Format == BinaryFormat::ELF && !IsRelocatable && Entry != 0 &&
                   FunctionEntry == Entry
               ? StackEntryKind::ProcessEntry
               : StackEntryKind::Call;
  }

  /// The function symbols, sorted by address.  A symbol naming a fragment
  /// that continues another function (ContinuationCodeStarts, GCC's
  /// `foo.cold`) names no function.
  std::vector<const Symbol *> getFunctionSymbols() const {
    std::vector<const Symbol *> Result;
    for (const auto &Sym : Symbols)
      if (Sym.IsFunc && !isFunctionContinuationAt(Sym.Addr))
        Result.push_back(&Sym);
    // Aliases of one address keep their symbol table order, so the name a
    // function is listed under never depends on how many others exist.
    std::stable_sort(
        Result.begin(), Result.end(),
        [](const Symbol *A, const Symbol *B) { return A->Addr < B->Addr; });
    return Result;
  }

  /// Get the format name as a string.
  /// The format whose platform conventions the code follows: its calling
  /// conventions, callee-saved registers, stack alignment and data model.
  /// A binary file names no format of its own, so its code follows the
  /// platform it was read for; any other image follows its own format.
  /// Container questions -- sections, imports, relocations, exception
  /// tables, symbol decoration -- read Format instead.
  BinaryFormat abiFormat() const {
    return Format == BinaryFormat::Raw ? ConventionFormat : Format;
  }

  const char *getFormatName() const {
    switch (Format) {
    case BinaryFormat::ELF:
      return "ELF";
    case BinaryFormat::COFF:
      return "PE";
    case BinaryFormat::MachO:
      return "Mach-O";
    case BinaryFormat::EVM:
      return kEVMFormatName.data();
    case BinaryFormat::Raw:
      return "Binary";
    default:
      return "Unknown";
    }
  }

  /// Check if this binary has debug info (PDB path or .debug_info).
  bool hasDebugInfo() const {
    if (!DynInfo.PDBPath.empty())
      return true;
    return getSectionByName(section_names::elf::DebugInfo) != nullptr;
  }

  /// Check if the image has a section with the given name.
  bool hasSection(llvm::StringRef Name) const {
    return getSectionByName(Name) != nullptr;
  }

  /// Check if the image has a segment with the given name.
  bool hasSegment(llvm::StringRef Name) const {
    return findSegmentByName(Name) != nullptr;
  }

  /// Find a section by name, returning a mutable pointer.
  Section *findSectionByName(llvm::StringRef Name) {
    for (auto &Sec : Sections)
      if (Sec.Name == Name)
        return &Sec;
    return nullptr;
  }

  // --- Typed section queries ---

  /// Get all executable sections.
  std::vector<const Section *> getExecutableSections() const {
    std::vector<const Section *> Result;
    for (const auto &Sec : Sections)
      if (Sec.isExecutable())
        Result.push_back(&Sec);
    return Result;
  }

  /// Get the .bss section (".bss" / "__bss").
  const Section *getBssSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Bss);
    return getSectionByName(section_names::elf::Bss);
  }

  /// Get the .rodata section (".rodata" for ELF, "__const" for MachO,
  /// ".rdata" for COFF).
  const Section *getRodataSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Const);
    if (isCOFF())
      return getSectionByName(section_names::coff::Rdata);
    return getSectionByName(section_names::elf::Rodata);
  }

  /// The address of an i386 ELF PIC image's `_GLOBAL_OFFSET_TABLE_`, which a
  /// get-PC sequence plus a GOTPC add leaves in a register.  A linked image
  /// keeps no GOTPC relocation; this is the address its DT_PLTGOT (GNU ld and
  /// lld: the start of .got.plt) or its `_GLOBAL_OFFSET_TABLE_` symbol
  /// records, where both agree.  Empty for every other image.
  std::optional<va_t> linkedI386GlobalOffsetTable() const;

  /// Get the GOT section (".got" / ".got.plt" / "__got").
  const Section *getGotSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Got);
    const Section *S = getSectionByName(section_names::elf::GotPlt);
    return S ? S : getSectionByName(section_names::elf::Got);
  }

  /// Get the PLT section (".plt" / "__stubs").
  const Section *getPltSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Stubs);
    return getSectionByName(section_names::elf::Plt);
  }

  /// Get all sections in a segment (MachO) or all sections.
  std::vector<const Section *>
  getSectionsInSegment(llvm::StringRef SegName) const {
    std::vector<const Section *> Result;
    for (const auto &Sec : Sections) {
      if (Sec.SegmentName == SegName)
        Result.push_back(&Sec);
    }
    return Result;
  }

  /// Decode an executable IAT/PLT veneer at \p Addr without a prior
  /// image-wide thunk scan.  `--func` PE loads skip that scan, but a
  /// `call` to `_CxxThrowException`'s `jmp [rip+IAT]` still has to name
  /// the import.
  const Import *decodeImportThunkAt(va_t Addr) const;
  /// Whether decodeImportThunkAt reads veneers of this architecture.
  bool decodesImportThunks() const {
    return Arch == neverd::Arch::X64 || Arch == neverd::Arch::X86;
  }

  /// Find the import owned by an exact executable veneer registration.  This
  /// deliberately excludes coarse stub ranges and legacy IAT spellings: code
  /// that needs a symbolic callable identity must not splice those independent
  /// forms of evidence together.
  const Import *findImportStubAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    const auto It = ImportStubIndices.find(Normalized);
    const bool Registered =
        It != ImportStubIndices.end() && It->second < Imports.size();
    // Where the architecture decodes no veneer, only a registered one can
    // answer; the costlier entry question waits until one may.
    if ((!Registered && !decodesImportThunks()) ||
        !isFunctionEntryCandidateAt(Addr))
      return nullptr;
    return Registered ? &Imports[It->second] : decodeImportThunkAt(Normalized);
  }

  /// Find an import by its format-native IAT address or an exact executable
  /// veneer registered for it.  IAT identity retains precedence for existing
  /// data-slot callers; use findImportStubAt when executable identity matters.
  const Import *findImportAt(va_t Addr) const {
    for (const auto &Imp : Imports)
      if (Imp.IATAddr == Addr)
        return &Imp;
    return findImportStubAt(Addr);
  }

  /// Return every address spelling that can identify an import.  Data slots
  /// remain present for indirect-call recovery; executable veneers add the
  /// direct branch targets used by CFG and ABI recovery.
  std::map<va_t, std::string> getImportAddressNames() const {
    std::map<va_t, std::string> Result;
    for (const auto &Imp : Imports)
      if (Imp.IATAddr != 0 && !Imp.Name.empty())
        Result.try_emplace(Imp.IATAddr, Imp.Name);
    for (const auto &[Addr, Index] : ImportStubIndices)
      if (Index < Imports.size() && !Imports[Index].Name.empty() &&
          isFunctionEntryCandidateAt(Addr))
        Result[Addr] = Imports[Index].Name;
    return Result;
  }

  /// How well \p Sym names the function starting at its address: a function
  /// symbol a producer stated, above another symbol there, above a
  /// synthesized `sub_` start.  A section's own symbol (COFF's `.text` where
  /// an object's first function or an import library object's thunk starts)
  /// names no function a function symbol names, and a synthesized start names
  /// none a label does.  Among equal ranks the first symbol names it.
  static int functionNameRank(const Symbol &Sym) {
    if (Sym.Origin == NameOrigin::Synthesized)
      return 0;
    return Sym.IsFunc ? 2 : 1;
  }

  /// The symbol at \p Addr of the highest functionNameRank, which names the
  /// function there where no export does; null for none.
  const Symbol *functionNameSymbolAt(va_t Addr) const {
    const Symbol *Best = nullptr;
    for (const auto &Sym : Symbols)
      if (Sym.Addr == Addr && !Sym.Name.empty() && !Sym.IsSection &&
          (!Best || functionNameRank(Sym) > functionNameRank(*Best)))
        Best = &Sym;
    return Best;
  }

  /// Resolve the best available display name for a function address: an
  /// export, else the symbol there of the highest functionNameRank, as the
  /// function is listed under it (functionListName).
  std::string getFunctionNameAt(va_t Addr) const {
    if (const Export *Exp = findExportAt(Addr); Exp && !Exp->Name.empty())
      return Exp->Name;
    if (const Symbol *Best = functionNameSymbolAt(Addr))
      return functionListName(Addr, Best->Name);
    return (kAutoFuncPrefix + llvm::utohexstr(Addr)).str();
  }

  /// The name the program itself states for the function at \p Addr, as
  /// getFunctionNameAt chooses it: the export there, else the symbol of the
  /// highest functionNameRank where a producer stated it.  Empty where only
  /// analysis or the user names the function, which says nothing of what
  /// the function is.
  llvm::StringRef statedFunctionNameAt(va_t Addr) const {
    if (const Export *Exp = findExportAt(Addr); Exp && !Exp->Name.empty())
      return Exp->Name;
    if (const Symbol *Best = functionNameSymbolAt(Addr);
        Best && Best->Origin == NameOrigin::Stated)
      return Best->Name;
    return {};
  }

  /// Every name the program states for the function at \p Addr, the one it
  /// is shown under (statedFunctionNameAt) first: a linker that folds
  /// identical functions exports one entry under each of their names.
  std::vector<llvm::StringRef> statedFunctionNamesAt(va_t Addr) const {
    std::vector<llvm::StringRef> Names;
    const llvm::StringRef Shown = statedFunctionNameAt(Addr);
    if (Shown.empty())
      return Names;
    Names.push_back(Shown);
    const auto add = [&](llvm::StringRef Name) {
      if (!Name.empty() && !llvm::is_contained(Names, Name))
        Names.push_back(Name);
    };
    for (const Export &Exp : Exports)
      if (Exp.Addr == Addr)
        add(Exp.Name);
    for (const Symbol &Sym : Symbols)
      if (Sym.Addr == Addr && Sym.IsFunc && Sym.Origin == NameOrigin::Stated)
        add(Sym.Name);
    return Names;
  }

  /// The name the function at \p Addr is listed and called under when its
  /// symbol names it \p Name: the name itself, or the number
  /// numberSharedFunctionNames() gave it where functions at lower addresses
  /// take that name already.
  std::string functionListName(va_t Addr, llvm::StringRef Name) const {
    if (auto It = SharedFunctionNames.find(Addr);
        It != SharedFunctionNames.end() && It->second.first == Name)
      return It->second.second;
    return Name.str();
  }

  /// Number the names functions share as IDA numbers them: the function at
  /// the lowest address keeps the name, a program's second static `helper`
  /// is `helper_0` and its third `helper_1`, each number another symbol
  /// holds as its name skipped.  Distinct functions so keep distinct names
  /// in the listing and in C.  A function an export names is listed by it.
  /// Loading calls this, and so does a change to the symbols that can name
  /// functions (applyDebugSymbols).
  void numberSharedFunctionNames() {
    SharedFunctionNames.clear();
    std::set<va_t> Exported;
    for (const Export &Exp : Exports)
      if (!Exp.Name.empty())
        Exported.insert(Exp.Addr);
    // The first stated function symbol at an address names it, as
    // functionNameRank chooses.
    std::map<va_t, const Symbol *> Named;
    for (const Symbol &Sym : Symbols)
      if (Sym.IsFunc && !Sym.Name.empty() &&
          Sym.Origin != NameOrigin::Synthesized && !Exported.count(Sym.Addr))
        Named.try_emplace(Sym.Addr, &Sym);
    std::map<llvm::StringRef, std::vector<va_t>> Entries;
    for (const auto &[Addr, Sym] : Named)
      Entries[Sym->Name].push_back(Addr);
    std::set<std::string> Held;
    for (const auto &[Name, Addrs] : Entries) {
      if (Addrs.size() < 2)
        continue;
      if (Held.empty()) {
        for (const Symbol &Sym : Symbols)
          Held.insert(Sym.Name);
        for (const Export &Exp : Exports)
          Held.insert(Exp.Name);
      }
      unsigned Number = 0;
      for (size_t I = 1; I < Addrs.size(); ++I) {
        std::string Listed;
        do
          Listed = (Name + "_" + llvm::Twine(Number++)).str();
        while (!Held.insert(Listed).second);
        SharedFunctionNames[Addrs[I]] = {Name.str(), std::move(Listed)};
      }
    }
  }

  /// Register an executable import veneer without changing Import::IATAddr.
  bool recordImportStub(va_t StubAddr, size_t ImportIndex) {
    if (ImportIndex >= Imports.size())
      return false;
    StubAddr = normalizeCodeAddress(StubAddr, Arch, Mode);
    const Segment *Seg = getSegmentFor(StubAddr);
    if (!Seg || !Seg->isExecutable())
      return false;
    auto [It, Inserted] = ImportStubIndices.try_emplace(StubAddr, ImportIndex);
    return Inserted || It->second == ImportIndex;
  }

  /// Register a checked half-open range of import/binder machinery.
  bool recordImportStubRange(va_t Start, uint64_t Size) {
    if (Size == 0 || Size > InvalidVA - Start)
      return false;
    const Segment *Seg = getSegmentFor(Start);
    if (!Seg || !Seg->isExecutable() || Start - Seg->VA > Seg->Size ||
        Size > Seg->Size - (Start - Seg->VA))
      return false;
    std::pair<va_t, va_t> Range{Start, Start + Size};
    if (std::find(ImportStubRanges.begin(), ImportStubRanges.end(), Range) ==
        ImportStubRanges.end())
      ImportStubRanges.push_back(Range);
    return true;
  }

  /// True when \p Addr is an exact or range-backed executable import veneer.
  bool isImportStubAt(va_t Addr) const {
    auto Exact = ImportStubIndices.find(Addr);
    if (Exact != ImportStubIndices.end() && Exact->second < Imports.size())
      return true;
    for (const auto &[Start, End] : ImportStubRanges)
      if (Addr >= Start && Addr < End)
        return true;
    // Compatibility for format loaders that historically put a code stub in
    // IATAddr (Mach-O). PE/ELF loaders register callable thunks explicitly;
    // their IAT/GOT slots remain data even inside a coarse executable map.
    if (!isMachO())
      return false;
    for (const auto &Imp : Imports)
      if (Imp.IATAddr == Addr)
        return isCodeAddress(Addr);
    return false;
  }

  /// True when loader or format metadata gives \p Addr an executable-code
  /// owner. Exact section permissions are preferred, while typed function
  /// extents, unwind ranges, and explicitly registered import veneers remain
  /// authoritative for packed images whose section flags are incomplete.
  bool hasExecutableCodeOwnerAt(va_t Addr) const {
    return hasExecutableCodeOwnerAt(Addr, nullptr);
  }

  /// Share metadata lookups for an unchanged image. Null or a different
  /// image's index retains the live lookup; see ExecutableCodeOwnerIndex.
  bool hasExecutableCodeOwnerAt(va_t Addr,
                                const ExecutableCodeOwnerIndex *Index) const;

  /// Smallest finite end from a known range or sized function symbol whose
  /// raw start is exactly Entry. This does not include containing ranges,
  /// unwind metadata or the next detected function. InvalidVA means no end.
  /// Null or a different image's index uses live metadata; a matching index
  /// must belong to the current unchanged image operation.
  va_t
  getFunctionMetadataEnd(va_t Entry,
                         const ExecutableCodeOwnerIndex *Index = nullptr) const;

  /// Shared structural admission for an independent entry. This supplies no
  /// function identity, ABI or instruction semantics by itself.
  bool isFunctionEntryCandidateAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    if (DataInCode.count(Normalized) || isFunctionContinuationAt(Addr) ||
        !hasExecutableCodeOwnerAt(Normalized))
      return false;
    if (Arch == neverd::Arch::AArch64)
      return Normalized % 4 == 0;
    if (Arch == neverd::Arch::ARM) {
      const auto LocalMode = instructionModeAt(Normalized);
      const bool IsARM = LocalMode ? *LocalMode == InstructionMode::ARM
                                   : Mode == InstructionMode::ARM;
      return Normalized % (IsARM ? 4 : 2) == 0;
    }
    return true;
  }

  /// A mode-tagged Thumb address and its instruction VA have the same owner.
  bool isFunctionContinuationAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    return ContinuationCodeStarts.count(Addr) ||
           ContinuationCodeStarts.count(Normalized) ||
           (normalizeCodeAddress(Normalized | 1, Arch, Mode) == Normalized &&
            ContinuationCodeStarts.count(Normalized | 1)) ||
           ExceptionMetadata.isFunctionContinuationAt(Normalized);
  }

  /// True when p Addr is an authenticated callable entry, rather than merely
  /// an address inside executable code. Untyped COFF exports are deliberately
  /// excluded; they require decode/unwind evidence before a patcher may place
  /// a trampoline over the original bytes.
  bool hasAuthenticatedFunctionEntryAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    if (!isFunctionEntryCandidateAt(Addr))
      return false;
    if ((Entry != 0 && normalizeCodeAddress(Entry, Arch, Mode) == Normalized) ||
        isRuntimeFunctionAt(Addr) ||
        VerifiedFunctionEntries.count(Normalized) != 0)
      return true;
    if (findImportStubAt(Addr))
      return true;
    for (const auto &[Start, End] : ImportStubRanges)
      if (Start < End && Start == Normalized)
        return true;
    if (hasKnownFunctionEntryAt(Addr))
      return true;
    if (hasFunctionSymbolAt(Normalized))
      return true;
    // Export tables are not type metadata: Mach-O export tries and PE/ELF
    // dynamic exports may name data.  An export is callable on its own only
    // when an exact mapped instruction section owns the address.  A
    // section-less image still accepts typed symbols, unwind ranges, entry
    // points, and explicit import stubs above, but never upgrades every RX
    // export into a function merely because the segment is executable.
    if (Format != BinaryFormat::COFF && getSectionFor(Normalized))
      for (const Export &Exp : Exports)
        if (normalizeCodeAddress(Exp.Addr, Arch, Mode) == Normalized &&
            isCodeAddress(Normalized))
          return true;
    return false;
  }

  /// True when every byte in one decoded instruction/range has the same
  /// structural code owner. This prevents a decoder from joining an opcode at
  /// the end of a code section with displacement bytes from adjacent data.
  bool hasExecutableCodeOwnerRange(va_t Addr, uint64_t Size) const {
    if (Size == 0)
      return false;
    const va_t Start = normalizeCodeAddress(Addr, Arch, Mode);
    if (Size - 1 > InvalidVA - Start)
      return false;
    const va_t Last = Start + Size - 1;
    const Segment *Seg = getSegmentFor(Start);
    if (!Seg || getSegmentFor(Last) != Seg)
      return false;
    if ((Seg->isExecutable() || Format == BinaryFormat::COFF) &&
        isCodeRange(Start, Size))
      return true;
    if (!Seg->isExecutable())
      return false;
    for (const auto &[RangeStart, RangeEnd] : ImportStubRanges)
      if (Start >= RangeStart && Last < RangeEnd)
        return true;
    // KnownCodeRanges is sorted by start after exception parse.  A linear
    // walk of a 100k-entry .pdata table on every padding/prologue probe is
    // what made `--func` on a large PE take seconds.
    if (!KnownCodeRanges.empty()) {
      const auto It = std::upper_bound(
          KnownCodeRanges.begin(), KnownCodeRanges.end(), Start,
          [](va_t Addr, const std::pair<va_t, va_t> &Range) {
            return Addr < Range.first;
          });
      if (It != KnownCodeRanges.begin()) {
        const auto Prev = std::prev(It);
        if (Start >= Prev->first && Last < Prev->second)
          return true;
      }
    }
    for (const Symbol &Sym : Symbols) {
      if (!Sym.IsFunc)
        continue;
      const va_t SymStart = normalizeCodeAddress(Sym.Addr, Arch, Mode);
      if (Sym.Size == 0)
        continue;
      if (Sym.Size <= InvalidVA - SymStart && Start >= SymStart &&
          Last < SymStart + Sym.Size)
        return true;
    }
    return false;
  }

  /// Record a structurally identified loader/runtime function when it maps to
  /// executable code.  Address zero is valid for relocatable images.
  bool recordRuntimeFunction(va_t Addr) {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    const Segment *Seg = getSegmentFor(Normalized);
    if (!Seg || !Seg->isExecutable() || !isCodeAddress(Normalized))
      return false;
    RuntimeFunctionAddrs.insert(Addr);
    return true;
  }

  bool isRuntimeFunctionAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    return RuntimeFunctionAddrs.count(Addr) ||
           RuntimeFunctionAddrs.count(Normalized) ||
           (normalizeCodeAddress(Normalized | 1, Arch, Mode) == Normalized &&
            RuntimeFunctionAddrs.count(Normalized | 1));
  }

  /// Compact `--func` `.pdata` start.  Does not materialize the body.
  bool hasPdataFunctionEntryAt(va_t Addr) const {
    if (COFFPDataRecords.empty() || Addr < Base)
      return false;
    const uint64_t RVA64 = Addr - Base;
    if (RVA64 > std::numeric_limits<uint32_t>::max())
      return false;
    const uint32_t RVA = static_cast<uint32_t>(RVA64);
    const auto It =
        std::lower_bound(COFFPDataRecords.begin(), COFFPDataRecords.end(), RVA,
                         [](const COFFPDataRecord &Rec, uint32_t Needle) {
                           return Rec.BeginRVA < Needle;
                         });
    return It != COFFPDataRecords.end() && It->BeginRVA == RVA &&
           It->BeginRVA < It->EndRVA;
  }

  /// Next compact `--func` `.pdata` start strictly after Addr.
  va_t nextPdataFunctionEntryAfter(va_t Addr) const {
    if (COFFPDataRecords.empty() || Addr < Base)
      return InvalidVA;
    const uint64_t AfterRVA = static_cast<uint64_t>(Addr - Base) + 1;
    if (AfterRVA > std::numeric_limits<uint32_t>::max())
      return InvalidVA;
    const auto It =
        std::lower_bound(COFFPDataRecords.begin(), COFFPDataRecords.end(),
                         static_cast<uint32_t>(AfterRVA),
                         [](const COFFPDataRecord &Rec, uint32_t RVA) {
                           return Rec.BeginRVA < RVA;
                         });
    for (auto Cur = It; Cur != COFFPDataRecords.end(); ++Cur)
      if (Cur->BeginRVA < Cur->EndRVA)
        return Base + Cur->BeginRVA;
    return InvalidVA;
  }

  /// True when pdata, KnownCodeRanges, or RuntimeFunctionAddrs name Addr as a
  /// function start.  `--func` keeps the compact `.pdata` index instead of
  /// copying every range into KnownCodeRanges; a BeginRVA hit is still a
  /// start so `jmp` to an unmaterialized callee stays a tail call.  A range
  /// beginning inside an earlier range can also describe an interior code
  /// chunk, so its start alone is not independent function-entry evidence.
  bool hasKnownFunctionEntryAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    if (!isFunctionEntryCandidateAt(Addr))
      return false;
    if (isRuntimeFunctionAt(Addr))
      return true;
    if (ExceptionMetadata.hasPrimaryFunctionEntry(Normalized))
      return true;
    if (hasPdataFunctionEntryAt(Addr) || hasPdataFunctionEntryAt(Normalized))
      return true;
    if (KnownCodeRanges.empty())
      return false;
    const auto It = std::lower_bound(
        KnownCodeRanges.begin(), KnownCodeRanges.end(), Normalized,
        [](const std::pair<va_t, va_t> &Range, va_t Needle) {
          return Range.first < Needle;
        });
    if (It == KnownCodeRanges.end() || It->first != Normalized ||
        It->second <= Normalized)
      return false;
    for (auto Prev = It; Prev != KnownCodeRanges.begin();) {
      --Prev;
      if (Prev->first < Normalized && Prev->second > Normalized)
        return false;
    }
    return true;
  }

  /// Smallest function start strictly after Addr from the same metadata used
  /// by hasKnownFunctionEntryAt.  InvalidVA means this is the last start.
  va_t nextKnownFunctionEntryAfter(va_t Addr) const {
    Addr = normalizeCodeAddress(Addr, Arch, Mode);
    va_t Best = InvalidVA;
    auto Consider = [&](va_t Start) {
      Start = normalizeCodeAddress(Start, Arch, Mode);
      if (Start <= Addr || !isFunctionEntryCandidateAt(Start))
        return false;
      Best = std::min(Best, Start);
      return true;
    };
    if (auto It = RuntimeFunctionAddrs.upper_bound(Addr);
        It != RuntimeFunctionAddrs.end())
      for (; It != RuntimeFunctionAddrs.end(); ++It)
        if (Consider(*It))
          break;
    const auto &Primaries = ExceptionMetadata.PrimaryFunctionEntries;
    for (auto It = std::upper_bound(Primaries.begin(), Primaries.end(), Addr);
         It != Primaries.end(); ++It)
      if (Consider(*It))
        break;
    if (!KnownCodeRanges.empty()) {
      const auto It =
          std::upper_bound(KnownCodeRanges.begin(), KnownCodeRanges.end(), Addr,
                           [](va_t Needle, const std::pair<va_t, va_t> &Range) {
                             return Needle < Range.first;
                           });
      for (auto Cur = It; Cur != KnownCodeRanges.end(); ++Cur)
        if (hasKnownFunctionEntryAt(Cur->first)) {
          Consider(Cur->first);
          break;
        }
    }
    for (va_t Next = nextPdataFunctionEntryAfter(Addr); Next != InvalidVA;
         Next = nextPdataFunctionEntryAfter(Next))
      if (Consider(Next))
        break;
    return Best;
  }

  /// True only for a complete pointer-width slot in a mapped, non-executable
  /// segment.  Exact storage identity deliberately does not accept executable
  /// import veneers or a coarse stub range.
  bool isValidImportStorageSlot(va_t SlotVA, llvm::StringRef Name) const {
    const uint32_t PointerSize = getPointerSize();
    if (PointerSize == 0 || SlotVA % PointerSize != 0 || Name.empty() ||
        hasMachOAuthenticatedPointerIn(SlotVA, PointerSize))
      return false;
    const Segment *Seg = getSegmentFor(SlotVA);
    return Seg && !Seg->isExecutable() && readVA(SlotVA, PointerSize);
  }

  /// Merge one already-validated record into an import-storage view.  Keeping
  /// this policy shared by loader mutation and compatibility collection makes
  /// the result independent of parser and consumer order.
  static bool mergeImportStorageSlot(std::map<va_t, ImportStorageSlot> &Slots,
                                     std::set<va_t> &Conflicts, va_t SlotVA,
                                     llvm::StringRef Name, int64_t Addend,
                                     ImportStorageEvidence Evidence) {
    if (Conflicts.count(SlotVA))
      return false;

    ImportStorageSlot Incoming{Name.str(), Addend, Evidence};
    auto [It, Inserted] = Slots.try_emplace(SlotVA, Incoming);
    if (Inserted)
      return true;

    ImportStorageSlot &Existing = It->second;
    if (Existing.Name != Incoming.Name) {
      Slots.erase(It);
      Conflicts.insert(SlotVA);
      return false;
    }

    const unsigned ExistingStrength = static_cast<unsigned>(Existing.Evidence);
    const unsigned IncomingStrength = static_cast<unsigned>(Incoming.Evidence);
    if (Existing.Addend == Incoming.Addend) {
      if (IncomingStrength > ExistingStrength)
        Existing.Evidence = Incoming.Evidence;
      return true;
    }
    if (IncomingStrength > ExistingStrength) {
      Existing = std::move(Incoming);
      return true;
    }
    if (IncomingStrength < ExistingStrength)
      return true;

    Slots.erase(It);
    Conflicts.insert(SlotVA);
    return false;
  }

  /// Record one exact imported pointer-storage identity.  Compatible duplicate
  /// evidence is idempotent; stronger evidence may refine the addend.  A name
  /// disagreement or equal-strength addend disagreement permanently poisons
  /// the slot so downstream code fails closed instead of choosing by order.
  bool recordImportStorageSlot(va_t SlotVA, llvm::StringRef Name,
                               int64_t Addend, ImportStorageEvidence Evidence) {
    if (!isValidImportStorageSlot(SlotVA, Name))
      return false;
    if (ConflictingImportStorageSlots.count(SlotVA)) {
      DyldBindSlots.erase(SlotVA);
      return false;
    }
    const bool Accepted = mergeImportStorageSlot(
        ImportStorageSlots, ConflictingImportStorageSlots, SlotVA, Name, Addend,
        Evidence);
    if (!Accepted)
      DyldBindSlots.erase(SlotVA);
    return Accepted;
  }

  /// Publish an exact dyld binding without collapsing provider, lookup scope or
  /// weakness disagreements into the format-neutral name/addend view. A
  /// conflicting native or canonical identity poisons both records permanently.
  bool
  recordDyldBindSlot(va_t SlotVA, llvm::StringRef Name, int64_t Addend,
                     llvm::StringRef Module, bool WeakImport,
                     std::optional<int64_t> LibraryOrdinal = std::nullopt) {
    if (!isValidImportStorageSlot(SlotVA, Name))
      return false;
    ImportBindSlot Incoming{Name.str(), Addend, Module.str(), WeakImport,
                            LibraryOrdinal};
    auto Reject = [&]() {
      ConflictingImportStorageSlots.insert(SlotVA);
      DyldBindSlots.erase(SlotVA);
      ImportStorageSlots.erase(SlotVA);
      return false;
    };
    if (ConflictingImportStorageSlots.count(SlotVA))
      return Reject();
    auto It = DyldBindSlots.find(SlotVA);
    if (It != DyldBindSlots.end()) {
      const ImportBindSlot &Existing = It->second;
      if (Existing.Name != Incoming.Name ||
          Existing.Addend != Incoming.Addend ||
          Existing.Module != Incoming.Module ||
          Existing.WeakImport != Incoming.WeakImport ||
          (Existing.LibraryOrdinal && Incoming.LibraryOrdinal &&
           Existing.LibraryOrdinal != Incoming.LibraryOrdinal))
        return Reject();
    }
    if (!recordImportStorageSlot(SlotVA, Incoming.Name, Incoming.Addend,
                                 ImportStorageEvidence::LoaderBind))
      return Reject();
    if (It == DyldBindSlots.end())
      DyldBindSlots.emplace(SlotVA, std::move(Incoming));
    else if (!It->second.LibraryOrdinal)
      It->second.LibraryOrdinal = LibraryOrdinal;
    return true;
  }

  /// Build the single import-storage evidence surface used after loading.
  /// Native maps are merged for compatibility with format-specific patch
  /// metadata and synthetic fixtures, but undergo the same exact-slot checks
  /// and conflict rules as canonical loader records.
  ImportStorageSlotCollection collectImportStorageSlots() const {
    ImportStorageSlotCollection Result;
    Result.Conflicts = ConflictingImportStorageSlots;

    auto Merge = [&](va_t SlotVA, llvm::StringRef Name, int64_t Addend,
                     ImportStorageEvidence Evidence) {
      if (Result.Conflicts.count(SlotVA) ||
          !isValidImportStorageSlot(SlotVA, Name))
        return;
      mergeImportStorageSlot(Result.Slots, Result.Conflicts, SlotVA, Name,
                             Addend, Evidence);
    };

    for (const auto &[SlotVA, Slot] : ImportStorageSlots)
      Merge(SlotVA, Slot.Name, Slot.Addend, Slot.Evidence);
    for (const auto &[SlotVA, Name] : ImportPtrSlots)
      Merge(SlotVA, Name, 0, ImportStorageEvidence::PointerTable);
    for (const auto &[SlotVA, Binding] : DyldBindSlots)
      Merge(SlotVA, Binding.Name, Binding.Addend,
            ImportStorageEvidence::LoaderBind);
    return Result;
  }

  /// Apply the same exact-slot merge to one address without copying every
  /// import in the image. Repeated source revalidation uses this scoped view;
  /// the complete collection remains available to whole-image consumers.
  ImportStorageSlotCollection collectImportStorageSlot(va_t SlotVA) const {
    ImportStorageSlotCollection Result;
    if (ConflictingImportStorageSlots.count(SlotVA))
      Result.Conflicts.insert(SlotVA);
    auto Merge = [&](llvm::StringRef Name, int64_t Addend,
                     ImportStorageEvidence Evidence) {
      if (Result.Conflicts.count(SlotVA) ||
          !isValidImportStorageSlot(SlotVA, Name))
        return;
      mergeImportStorageSlot(Result.Slots, Result.Conflicts, SlotVA, Name,
                             Addend, Evidence);
    };
    if (auto It = ImportStorageSlots.find(SlotVA);
        It != ImportStorageSlots.end())
      Merge(It->second.Name, It->second.Addend, It->second.Evidence);
    if (auto It = ImportPtrSlots.find(SlotVA); It != ImportPtrSlots.end())
      Merge(It->second, 0, ImportStorageEvidence::PointerTable);
    if (auto It = DyldBindSlots.find(SlotVA); It != DyldBindSlots.end())
      Merge(It->second.Name, It->second.Addend,
            ImportStorageEvidence::LoaderBind);
    return Result;
  }

  /// Record one mapped data slot whose callable role comes from a runtime
  /// contract.  Multiple contracts may intentionally name the same storage,
  /// so uniqueness is by (address, kind), not address alone.
  bool recordRuntimeCallablePointerSlot(va_t SlotVA,
                                        RuntimeCallablePointerSlotKind Kind) {
    const uint32_t PointerSize = getPointerSize();
    if (SlotVA == 0 || PointerSize == 0 || SlotVA % PointerSize != 0 ||
        !isDataAddress(SlotVA) || !readVA(SlotVA, PointerSize))
      return false;
    const RuntimeCallablePointerSlot Record{SlotVA, Kind};
    if (std::find(RuntimeCallablePointerSlots.begin(),
                  RuntimeCallablePointerSlots.end(),
                  Record) == RuntimeCallablePointerSlots.end())
      RuntimeCallablePointerSlots.push_back(Record);
    return true;
  }

  bool hasRuntimeCallablePointerSlotAt(va_t SlotVA) const {
    return std::any_of(RuntimeCallablePointerSlots.begin(),
                       RuntimeCallablePointerSlots.end(),
                       [&](const RuntimeCallablePointerSlot &Slot) {
                         return Slot.SlotVA == SlotVA;
                       });
  }

  bool
  hasRuntimeCallablePointerSlotAt(va_t SlotVA,
                                  RuntimeCallablePointerSlotKind Kind) const {
    return std::find(RuntimeCallablePointerSlots.begin(),
                     RuntimeCallablePointerSlots.end(),
                     RuntimeCallablePointerSlot{SlotVA, Kind}) !=
           RuntimeCallablePointerSlots.end();
  }

  /// True when one exact storage slot carries either a static relocation or a
  /// runtime-owned pointer contract.  Keep this broader storage query separate
  /// from hasRelocationProvenanceAt(): a null runtime slot has pointer meaning,
  /// but it is not a relocation/fixup and must never become evidence for a
  /// static code or data target.
  bool hasPointerStorageProvenanceAt(va_t SlotVA) const {
    return hasRelocationProvenanceAt(SlotVA) ||
           hasRuntimeCallablePointerSlotAt(SlotVA) ||
           ConflictingImportStorageSlots.count(SlotVA) != 0;
  }

  /// Get imports for a specific module/library.
  std::vector<const Import *>
  getImportsForModule(llvm::StringRef Module) const {
    std::vector<const Import *> Result;
    for (const auto &Imp : Imports)
      if (Imp.Module == Module)
        Result.push_back(&Imp);
    return Result;
  }

  /// Check if the binary is position-independent (has base relocations
  /// or is a shared object).
  bool isPositionIndependent() const { return !BaseRelocations.empty(); }

  // --- Mutation helpers ---

  /// Add a new section to the image model (does not modify the raw binary).
  Section &addSection(llvm::StringRef Name, va_t VA, uint64_t Size,
                      SegmentFlags Flags = SegmentFlags::Readable) {
    Section Sec;
    Sec.Name = Name.str();
    Sec.VA = VA;
    Sec.Size = Size;
    Sec.Flags = Flags;
    Sections.push_back(std::move(Sec));
    return Sections.back();
  }

  /// Remove a section by name. Returns true if a section was removed.
  bool removeSection(llvm::StringRef Name) {
    auto It = std::remove_if(Sections.begin(), Sections.end(),
                             [&](const Section &S) { return S.Name == Name; });
    if (It == Sections.end())
      return false;
    Sections.erase(It, Sections.end());
    return true;
  }

  /// Add a new segment to the image model.
  Segment &addSegment(llvm::StringRef Name, va_t VA, uint64_t Size,
                      SegmentFlags Flags = SegmentFlags::Readable) {
    Segment Seg;
    Seg.Name = Name.str();
    Seg.VA = VA;
    Seg.Size = Size;
    Seg.Flags = Flags;
    Segments.push_back(std::move(Seg));
    return Segments.back();
  }

  /// Add a symbol to the image.
  Symbol &addSymbol(llvm::StringRef Name, va_t Addr, uint64_t Size = 0,
                    bool IsFunc = false) {
    Symbol S;
    S.Name = Name.str();
    S.Addr = Addr;
    S.Size = Size;
    S.IsFunc = IsFunc;
    Symbols.push_back(std::move(S));
    return Symbols.back();
  }

  /// Dump a summary for LLVM_DEBUG output (shared by all loaders).
  void debugDumpSummary(llvm::raw_ostream &OS, llvm::StringRef Prefix) const {
    OS << Prefix << " " << getFormatName() << getBitnessName(Bits)
       << " | arch=" << getArchName(Arch) << " base=0x" << llvm::utohexstr(Base)
       << " entry=0x" << llvm::utohexstr(Entry) << " segs=" << Segments.size()
       << " secs=" << Sections.size() << " syms=" << Symbols.size()
       << " imports=" << Imports.size() << " exports=" << Exports.size()
       << "\n";
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGEMODEL_H
