//===- MachODependencyState.h - Source patch support -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate and preserve native dyld bindings.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_CODEGEN_MACHO_DEPENDENCYSTATE_H
#define NEVERD_LIB_BACKEND_CODEGEN_MACHO_DEPENDENCYSTATE_H
#include "neverd/object/MachOLayout.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace neverd {
struct MachOChainedImport {
  int64_t Ordinal = 0, Addend = 0;
  unsigned Flags = 0;
  std::string Name;
};
struct MachOChainedSlot {
  uint64_t Offset = 0, Raw = 0;
};
struct MachOChainedSegment {
  uint64_t VMAddr = 0, VMSize = 0, FileOff = 0, FileSize = 0;
  uint32_t CommandOffset = 0, Protections = 0;
  uint16_t PointerFormat = 0, PageSize = 0;
  std::vector<MachOChainedSlot> Slots;
  uint32_t Flags = 0;
  /// Original file location of the validated starts-in-segment record.
  /// Threaded bind streams have no starts record.
  uint64_t StartsFileOffset = 0;
};
/// Owned facts from the strict reader, captured only for an installing caller.
struct MachOChainedFixups {
  uint64_t HeaderVA = 0;
  uint32_t CommandOffset = 0;
  bool Threaded = false;
  unsigned LinkeditIndex = 0;
  std::vector<MachOChainedSegment> Segments;
  std::vector<MachOChainedImport> Imports;
  std::vector<std::string> Libraries;
  /// Regular weak definitions exported by the original image, keyed by the
  /// exact dyld spelling and carrying the linked address of the fallback.
  std::map<std::string, uint64_t> WeakDefinitions;
  /// Strong per-thread exports name the already-registered dyld descriptor.
  std::map<std::string, uint64_t> ThreadLocalDefinitions;

  using Provider = std::pair<int64_t, bool>; // Library ordinal and weak import.
  /// A disengaged value records conflicting dyld lookup scopes for a name.
  std::map<std::string, std::optional<Provider>> uniqueImportProviders() const;
};
struct MachODependencyState {
  uint32_t CPUType = 0, CPUSubtype = 0;
  std::array<uint8_t, 32> Digest;
  std::map<std::pair<uint64_t, unsigned>, std::vector<uint8_t>> Slots;
  std::vector<std::vector<uint8_t>> Libraries, Symbols, Indirect;
  std::vector<uint8_t> Exports;
};
/// A valid header has insufficient VA space immediately below it. An
/// explicitly movable zero-base image can retry after a checked uniform slide.
class InsufficientMachOHeaderPrefix final
    : public llvm::ErrorInfo<InsufficientMachOHeaderPrefix> {
public:
  static char ID;
  void log(llvm::raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
};

struct MachOUnwindFunctionCounts;
struct MachOAuthenticatedFixup;

/// A source image's section addresses can move together when an explicitly
/// published zero-base dylib needs a larger TEXT mapping. The returned slide
/// applies to source sections. The preferred Mach header stays at zero.
llvm::Expected<uint64_t> reserveMachOPublishedUnwind(
    std::vector<uint8_t> &Binary, Arch Architecture, uint64_t ExtraFrameBytes,
    MachOUnwindFunctionCounts Additional, bool AllowSlide,
    std::optional<MachODependencyState> &SlidDependencies,
    uint64_t AdditionalCommandBytes = 0);

/// Translate the preferred section addresses of a position-independent dylib
/// with image-relative chained fixups before growing its header downwards.
llvm::Expected<MachODependencyState>
slideMachOZeroBaseDylib(std::vector<uint8_t> &Binary, uint64_t Slide);

// Unlike the analysis loader this decoder rejects every malformed or unmodeled
// byte. Its result is evidence for editing loader metadata, not best effort
// data.
llvm::Expected<MachODependencyState>
readMachODependencyState(llvm::ArrayRef<uint8_t> Bytes,
                         MachOChainedFixups *Chained = nullptr);
llvm::Error preserveMachODependencyState(const MachODependencyState &Before,
                                         const MachODependencyState &After);
llvm::Error preserveMachODependencyState(
    const MachODependencyState &Before, const MachODependencyState &After,
    const std::map<std::string, uint64_t> &AddedTLS,
    llvm::ArrayRef<MachOAuthenticatedFixup> Fixups = {});
} // namespace neverd
#endif
