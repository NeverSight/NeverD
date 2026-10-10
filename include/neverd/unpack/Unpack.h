//===- Unpack.h - Recover the image a packed executable builds -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Unpacking recovers the program a protected executable reconstructs in its
/// own address space. Recovery observes a bounded guest process and
/// rebuilds the image at the first transfer into code that did not exist when
/// the image was mapped. It neither guesses an entry point nor removes
/// virtualization; protected functions stay protected in the output.
///
/// Nothing here depends on one container, instruction set or guest system.
/// The input's container format selects how the file is validated and
/// rebuilt, its instruction set selects how a transfer is judged, and both
/// select the guest process profile that executes it. An RVA is an address
/// relative to the image base in every container.
///
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_UNPACK_H
#define NEVERD_UNPACK_UNPACK_H

#include "neverd/emulation/DriverSession.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd::unpack {
namespace defaults {
#define NEVERD_UNPACK_LIMIT(Name, Value) inline constexpr uint64_t Name = Value;
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_LIMIT
} // namespace defaults

enum class FormatKind {
#define NEVERD_UNPACK_FORMAT(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_FORMAT
};
enum class PackerKind {
#define NEVERD_UNPACK_PACKER(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_PACKER
};
enum class PackerEvidence {
#define NEVERD_UNPACK_EVIDENCE(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_EVIDENCE
};
enum class UnpackOutcome {
#define NEVERD_UNPACK_OUTCOME(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_OUTCOME
};
enum class EntrySource {
#define NEVERD_UNPACK_ENTRY_SOURCE(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_ENTRY_SOURCE
};
enum class ImportOrigin {
#define NEVERD_UNPACK_IMPORT_ORIGIN(Name, Text) Name,
#include "neverd/unpack/Unpack.def"
#undef NEVERD_UNPACK_IMPORT_ORIGIN
};
const char *formatKindName(FormatKind Kind);
const char *packerKindName(PackerKind Kind);
const char *packerEvidenceName(PackerEvidence Evidence);
const char *unpackOutcomeName(UnpackOutcome Outcome);
const char *importOriginName(ImportOrigin Origin);
const char *entrySourceName(EntrySource Source);

/// Compatibility vocabulary for consumers of earlier unpack reports.
/// Generic recovery does not identify protectors and leaves this empty.
struct PackerIdentification {
  PackerKind Kind = PackerKind::Unidentified;
  std::vector<PackerEvidence> Evidence;
};

/// Validate an image without executing it. An unsupported container or
/// malformed input returns Error. Valid images are Unidentified without
/// evidence; protector-specific identification is no longer performed.
llvm::Expected<PackerIdentification>
identifyPacker(llvm::ArrayRef<uint8_t> File);

/// Unpacking runs the input in its guest OS environment. Common resource
/// limits apply to both process and driver execution; each environment owns
/// its explicit inputs.
struct UnpackOptions {
  /// Applies the unpacking defaults of Unpack.def, including those of each
  /// guest OS model.
  UnpackOptions();
  emulation::ProcessOptions Process;
  /// Driver scenario inputs for a native-subsystem x64 PE. The common
  /// backend, contract and resource limits above apply to this run too.
  /// User images reject this option rather than ignoring kernel inputs.
  std::optional<emulation::DriverOptions> Driver;
  /// The one-based transfer into generated code to accept as the entry. Zero
  /// accepts the first transfer in the main program's entry invocation made
  /// on the stack that invocation started with,
  /// which is how a stub hands control to the program it carries. A protector
  /// that unpacks in stages, or that calls the program instead of jumping to
  /// it, needs the explicit position that a previous report lists.
  uint64_t Transfer = 0;
  /// Write an image snapshot for analysis, even when runtime dependencies
  /// cannot be reconstructed. This produces Snapshot rather
  /// than Unpacked and does not claim a runnable native executable.
  bool SnapshotOnly = false;
  /// Materialize supported OS-owned resources in a self-contained image.
  /// Restored outputs retain an explicit environment contract and fixed
  /// addresses. This is mutually exclusive with an analysis-only snapshot.
  bool RestoreRuntime = false;
};

/// A pointer-sized value in captured image or thread-local bytes that falls
/// in a live process heap allocation. The value could be an integer or unused
/// data. No pointer type or relocation is inferred.
struct UnpackHeapReference {
  enum class Storage { Image, ThreadLocal };
  uint64_t Offset, Address, AllocationAddress, AllocationSize;
  Storage Location = Storage::Image;
};

struct UnpackRuntimeState {
  /// Other profile-owned resources cannot be inferred from integer matches.
  bool AdditionalStateInventoryKnown = false;
  bool HasAdditionalDependencies = false;
  /// Diagnostic explanations supplied by the OS owner. These are not pointer
  /// provenance or a reconstruction recipe; the dependency flag remains the
  /// authority when a profile does not provide individual explanations.
  std::vector<std::string> AdditionalDependencyReasons;
  bool HeapInventoryKnown = false;
  uint64_t PossibleHeapReferences = 0;
  /// Direct model service calls witnessed during entry and import discovery.
  /// Their numeric binding has not been made portable to native Windows.
  uint64_t DirectServiceCalls = 0;
  /// At most the first 64 matches, image first and then TLS, in offset order.
  /// The count covers all matches.
  std::vector<UnpackHeapReference> HeapReferences;
  bool EncodedPointerInventoryKnown = false;
  uint64_t PossibleEncodedPointers = 0;
  /// Exact value matches, not pointer provenance or permission to re-encode.
  struct EncodedPointerReference {
    uint64_t Offset, Value;
    UnpackHeapReference::Storage Location;
  };
  /// At most 64 locations, image first and then TLS, in offset order.
  std::vector<EncodedPointerReference> EncodedPointerReferences;
  /// Dynamic slot ownership/values are not reconstructed by static TLS repair.
  bool DynamicThreadLocalInventoryKnown = false;
  uint64_t LiveDynamicTLSSlots = 0, LiveDynamicFLSSlots = 0;
};

/// One transfer into code newer than the code that was running.
struct UnpackTransfer {
  uint64_t RVA;
  /// The stack pointer equals its initial value for the main program's entry
  /// invocation (or the first initializer before that invocation starts).
  bool StackBalanced;
  /// One for code generated from the loaded image, two for code generated by
  /// that code, and so on.
  uint64_t Generation;
  /// The OS model is executing the main program's entry invocation rather
  /// than an initialization or teardown callback it owns.
  bool ProgramInvocation = false;
};

/// One mapped extent of the rebuilt file: a section, or a segment in a
/// container that maps segments.
struct UnpackedSection {
  std::string Name;
  uint64_t RVA, VirtualSize, FileSize;
  /// Bytes that differ from the image as the guest loader mapped it.
  uint64_t GeneratedBytes;
};

struct UnpackedImport {
  std::string Module, Name;
  std::optional<uint16_t> Ordinal;
  /// The pointer-sized cell the program calls through.
  uint64_t SlotRVA;
  ImportOrigin Origin;
};

struct UnpackResult {
  FormatKind Format = FormatKind::PE64;
  PackerIdentification Packer;
  UnpackOutcome Outcome = UnpackOutcome::NoEntry;
  std::string Diagnostic;
  /// Stable guest architecture, profile, backend and process stop vocabulary
  /// from the guest process report.
  std::string Architecture, Profile;
  uint64_t ImageBase = 0;
  uint64_t PackedEntryRVA = 0;
  /// The first instruction of the recovered program and how it was
  /// established. Absent when no entry was accepted; never estimated.
  std::optional<uint64_t> EntryRVA;
  EntrySource Source = EntrySource::Transfer;
  /// Every transfer observed, in order, up to the one at which the image was
  /// rebuilt.
  std::vector<UnpackTransfer> Transfers;
  std::vector<UnpackedSection> Sections;
  std::vector<UnpackedImport> Imports;
  /// Dependencies observed before reconstruction. A Restored outcome retains
  /// these diagnostics even when its initializer materializes their owners.
  /// An empty inventory does not certify other OS state or unreached code.
  UnpackRuntimeState RuntimeState;
  /// The rebuilt file, in the container of the input. Empty without an
  /// accepted entry or when runtime state requires refusal.
  std::vector<uint8_t> Image;
  std::string Backend, BackendSelectionReason;
  std::string ProcessStop, ProcessDiagnostic;
  uint64_t PC = 0, Instructions = 0, Events = 0;
  /// Import repair discovers export calls and observes helper state in two
  /// additional bounded processes. Counts sum both runs; Stop describes the
  /// last run. These observations cover only the paths those processes reached.
  /// Setup failures and incomplete runs are visible independently of entry
  /// recovery; an unpacked outcome does not certify every protected import.
  struct ImportRepairReport {
    std::string Stop, Diagnostic;
    uint64_t ObservedCalls = 0, RepairedCalls = 0, ConflictingCalls = 0;
    uint64_t ObservedLoads = 0, RepairedLoads = 0;
    uint64_t Instructions = 0, Events = 0;
  } ImportRepair;
  /// TLS startup callbacks whose memory effects were captured. The rebuilt
  /// file bypasses their process-attach calls and forwards other TLS reasons.
  uint64_t MaterializedTLSCallbacks = 0;
};

/// Observe \p Input in its bounded guest environment and rebuild its image at
/// the transfer accepted as its entry. Invalid input, options or an unavailable
/// backend return Error. A run that ends before such a transfer returns a
/// result whose outcome, transfers and process stop explain why.
llvm::Expected<UnpackResult> unpackFile(const std::filesystem::path &Input,
                                        const UnpackOptions &Options = {});

/// Strict options decoding. User images accept guest process options plus
/// "transfer", "snapshot_only" and "restore_runtime". Native-subsystem x64
/// images use common limits/backend and an optional "driver" scenario; user
/// process inputs do not apply. Unknown fields, null values, invalid types
/// and nonpositive limits are errors.
llvm::Expected<UnpackOptions> unpackOptionsFromJSON(llvm::StringRef Text);
/// The report omits the rebuilt bytes and records their size and digest.
std::string unpackResultJSON(const UnpackResult &Result,
                             llvm::StringRef OutputPath = {});
} // namespace neverd::unpack
#endif
