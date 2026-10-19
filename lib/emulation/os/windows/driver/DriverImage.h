//===- DriverImage.h - Validated PE mapping plan --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private image-loading contract for Windows driver emulation.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_DRIVERIMAGE_H
#define NEVERD_EMULATION_DRIVERIMAGE_H

#include "neverd/emulation/DriverProfile.h"
#include "neverd/loader/COFF/PEProgramExports.h"
#include "neverd/loader/ExceptionTable.h"

#include "llvm/Support/Error.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace neverd::modules {
class ModuleImage;
}
namespace neverd::emulation {
struct DriverImageRegion {
  uint64_t Address = 0;
  unsigned Permissions = 0;
  std::vector<uint8_t> Bytes; // Page aligned and zero filled to virtual size.
};
struct DriverImport {
  uint64_t Slot = 0;
  std::string Module;
  std::string Name;
  std::optional<uint32_t> Ordinal;
};
struct DriverGuardControlFlow {
  bool Enabled = false;
  /// Addresses of read-only guest pointer slots, not helper entry points.
  /// The session replaces them only when Enabled; otherwise guest fallbacks
  /// and their original instructions remain authoritative.
  uint64_t CheckPointerAddress = 0;
  uint64_t DispatchPointerAddress = 0;
  std::vector<uint64_t> ValidTargets;
  struct Range {
    uint64_t RVA, Size;
  };
  std::vector<Range> MetadataRanges;
};
struct DriverImage {
  struct Relocation {
    uint64_t RVA, LoadedValue;
  };
  std::string Name;
  uint64_t Base = 0;
  uint64_t PreferredBase = 0;
  uint64_t SecurityCookieAddress = 0;
  uint64_t Entry = 0;
  uint64_t Size = 0;
  uint16_t DLLCharacteristics = 0;
  DriverGuardControlFlow Guard;
  std::vector<DriverImageRegion> Regions;
  std::vector<DriverImport> Imports;
  /// Exact validated DIR64 fields after applying this load's base difference.
  std::vector<Relocation> Relocations;
  /// Identity of the validated file, before rebasing and session fixups.
  std::array<uint8_t, 32> FileHash{};
  PEProgramExports Exports;
  /// Validated loader metadata in preferred-image coordinates. Exception
  /// dispatch performs the checked translation to this session's actual base.
  ExceptionInfo Exceptions;
};
/// Concrete guest cookie used for reproducible analysis, never for protection
/// of the host. The PE wrapper still executes its own cookie checks.
inline constexpr uint64_t DriverSecurityCookie = profile::SecurityCookie;
/// Fixed model ownership, including not-yet-mapped callback/call storage.
/// Resource admission and image preflight share this range policy.
bool overlapsDriverReservedMemory(uint64_t Address, uint64_t Size);
/// Strict x64 PE native-subsystem executable profile. LoadAddress zero selects
/// the preferred base; explicit bases require complete supported relocations.
/// Reject malformed/truncated metadata and unsupported load-time features.
llvm::Expected<DriverImage> loadDriverImage(const std::filesystem::path &Path,
                                            uint64_t MemoryLimit,
                                            uint64_t LoadAddress = 0);
/// Same execution preflight over the catalogue's immutable bytes and parsed
/// image. No file IO or debug reload; a source symbol and mapped instruction
/// retain the same original-image identity.
llvm::Expected<DriverImage> loadDriverImage(const modules::ModuleImage &,
                                            uint64_t MemoryLimit,
                                            uint64_t LoadAddress = 0,
                                            bool AllowOrdinalImports = false);
/// Native x64 guest module: same strict byte/relocation preflight, with an
/// export directory and optional zero entry allowed. Initialization is never
/// inferred or executed here; imports and exports belong to its binding.
llvm::Expected<DriverImage> loadKernelModuleImage(const modules::ModuleImage &,
                                                  uint64_t MemoryLimit,
                                                  uint64_t LoadAddress = 0);
} // namespace neverd::emulation
#endif
