//===- Capture.h - An observed image and how to rebuild it ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNPACK_CORE_CAPTURE_H
#define NEVERD_UNPACK_CORE_CAPTURE_H

#include "UnpackInternal.h"

#include "neverd/emulation/ProcessObserver.h"

#include <array>
#include <map>
#include <tuple>

namespace neverd::unpack {
/// One export identity a pointer-sized cell can name.
struct ExportBinding {
  std::string Module, Name;
  std::optional<uint16_t> Ordinal;

  bool operator==(const ExportBinding &Other) const {
    return std::tie(Module, Name, Ordinal) ==
           std::tie(Other.Module, Other.Name, Other.Ordinal);
  }
  bool operator!=(const ExportBinding &Other) const {
    return !(*this == Other);
  }
  /// Prefer a named alias, then a stable identity independent of enumeration.
  bool operator<(const ExportBinding &Other) const {
    if (Name.empty() != Other.Name.empty())
      return !Name.empty();
    return std::tie(Module, Name, Ordinal) <
           std::tie(Other.Module, Other.Name, Other.Ordinal);
  }
};

/// The image of a stopped process at the transfer where its entry was
/// established. Nothing in it is container specific.
struct Capture {
  /// The address the image was observed at.
  uint64_t Base;
  uint64_t EntryRVA;
  EntrySource Source;
  /// The image extent at the transfer and as the guest loader mapped it.
  std::vector<uint8_t> Memory, Baseline;
  /// Guest permissions of each page of the extent at the transfer; zero if
  /// the page was not mapped.
  std::vector<uint8_t> PageAccess;
  /// Transfers into generated code, including initialization calls before
  /// the accepted entry. Container metadata recovery can validate callback
  /// addresses against these execution witnesses.
  std::vector<UnpackTransfer> Transfers;
  /// Main-image OS initializers that returned before the program invocation.
  /// Their memory effects are already present in this entry snapshot.
  std::vector<uint64_t> Initializers;
  struct CompletedCall {
    uint64_t Entry;
    std::array<uint64_t, 3> Arguments;
  };
  /// Entries into generated code whose ABI return PC and returned stack were
  /// subsequently observed. Container metadata decides whether they are TLS
  /// callbacks; an entry address alone cannot establish completed execution.
  std::vector<CompletedCall> CompletedCalls;
  /// Live main-thread TLS outside the image, when the profile can capture it.
  std::optional<std::vector<uint8_t>> ThreadLocal;
  UnpackRuntimeState RuntimeState;
  std::shared_ptr<const emulation::ProcessRuntimeState> OwnedState;
  std::vector<emulation::ProcessModuleView> Modules;
  /// Entry addresses of every export the guest loader can bind.
  std::map<uint64_t, ExportBinding> Exports;
};

/// One runtime export use with a continuation inside the image. Call evidence
/// comes from an entered export; address-load evidence comes from a helper's
/// validated state transition.
struct TailImport {
  uint64_t ReturnAddress = 0;
  /// Identity resolved in the same process that proved this use. Addresses
  /// belong to one process and cannot be interpreted through another capture.
  ExportBinding Target;
  /// A helper returned this export address in a register without changing
  /// other registers, flags or persistent guest memory.
  bool AddressLoad = false;
  /// Actual executed start, so overlapping instruction windows cannot be
  /// inferred from a continuation address alone.
  uint64_t InstructionAddress = 0;
  /// Destination established by the complete register transition, not by
  /// guessing which register a helper's bytes might produce.
  std::optional<unsigned> ResultRegister;
};

/// Execution evidence used when rebuilding the observed memory.
struct RebuildPlan {
  /// Export calls and address loads observed by running the recovered entry.
  /// The instruction-set module validates each form before the container
  /// allocates cells or rewrites it.
  std::vector<TailImport> TailImports;
};

struct RebuiltImage {
  std::vector<uint8_t> File;
  std::vector<UnpackedSection> Sections;
  std::vector<UnpackedImport> Imports;
  uint64_t RepairedTailCalls = 0;
  uint64_t RepairedImportLoads = 0;
  uint64_t ConflictingTailCalls = 0;
  uint64_t MaterializedTLSCallbacks = 0;
  uint64_t LoaderEntryRVA = 0;
};
} // namespace neverd::unpack
#endif
