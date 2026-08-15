//===- MachOExceptionPatch.h - Mach-O unwind-record rewrite -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H

#include "neverd/Common.h"
#include "neverd/backend/codegen/DwarfEHFrame.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {

struct CompiledImage;
struct MachOCompactUnwindRecords;

/// The file-backed tail of `__TEXT,__eh_frame` that regenerated DWARF unwind
/// records can occupy.  dyld only exposes the `__eh_frame` an image already
/// declares to libunwind, so records placed anywhere else stay unregistered.
/// This contract is deliberately limited to DWARF registration; Mach-O
/// `__unwind_info` compact-unwind regeneration is a separate capability.
struct MachOEHFrameRegion {
  bool Is64 = true;
  uint64_t SectionVA = 0;
  uint64_t SectionFileOff = 0;
  uint64_t AppendVA = 0;
  uint64_t AppendFileOff = 0;
  uint64_t LimitFileOff = 0;
  uint64_t SectionHeaderOff = 0;
};

enum class MachOEHFrameInstallDisposition : uint8_t {
  Unchanged = 0,
  Installed = 1,
};

static_assert(
    static_cast<uint8_t>(MachOEHFrameInstallDisposition::Unchanged) == 0 &&
        static_cast<uint8_t>(MachOEHFrameInstallDisposition::Installed) == 1,
    "Mach-O EH-frame install dispositions are an API contract");

/// Immutable evidence for the exact regenerated FDE sequence that was copied
/// into the candidate image and decoded again at its installed address.  The
/// receipt also retains the compiler-authenticated range collection; a
/// non-DWARF compact range does not need a corresponding FDE.  A receipt can
/// only be produced by installMachOEHFrameWithReceipt, which permanently binds
/// its architecture, pointer width, and byte order.  Consumers must not bind
/// compact-unwind DWARF payloads to planned or object-file FDEs.
class MachOEHFrameInstallReceipt {
public:
  MachOEHFrameInstallReceipt(const MachOEHFrameInstallReceipt &) = default;
  MachOEHFrameInstallReceipt(MachOEHFrameInstallReceipt &&) = default;
  MachOEHFrameInstallReceipt &
  operator=(const MachOEHFrameInstallReceipt &) = delete;
  MachOEHFrameInstallReceipt &operator=(MachOEHFrameInstallReceipt &&) = delete;

  MachOEHFrameInstallDisposition disposition() const { return Disposition; }
  Arch targetArch() const { return TargetArch; }
  uint8_t pointerWidth() const { return PointerWidth; }
  llvm::endianness byteOrder() const { return ByteOrder; }
  const std::optional<MachOEHFrameRegion> &region() const { return Region; }
  uint64_t installedFileOff() const { return InstalledFileOff; }
  llvm::ArrayRef<uint8_t> installedBytes() const { return InstalledBytes; }
  llvm::ArrayRef<DwarfEHFrameRecord> installedFDEs() const {
    return InstalledFDEs;
  }
  const std::map<std::string, uint64_t> &installedSymbolAddrs() const {
    return InstalledSymbolAddrs;
  }
  /// Compiler-authenticated owner identities retained with the installed
  /// artifact.  These are provenance, not a claim that every range has an FDE.
  const std::map<std::string, uint64_t> &
  authenticatedFunctionOwnerAddrs() const {
    return AuthenticatedFunctionOwnerAddrs;
  }
  llvm::ArrayRef<llvm::mc_rewrite::RewriteFunctionRange>
  authenticatedFunctionRanges() const {
    return AuthenticatedFunctionRanges;
  }

private:
  MachOEHFrameInstallReceipt(Arch TargetArch, uint8_t PointerWidth,
                             llvm::endianness ByteOrder)
      : TargetArch(TargetArch), PointerWidth(PointerWidth),
        ByteOrder(ByteOrder) {}

  MachOEHFrameInstallDisposition Disposition =
      MachOEHFrameInstallDisposition::Unchanged;
  const Arch TargetArch;
  const uint8_t PointerWidth;
  const llvm::endianness ByteOrder;
  std::optional<MachOEHFrameRegion> Region;
  uint64_t InstalledFileOff = 0;
  std::vector<uint8_t> InstalledBytes;
  std::vector<DwarfEHFrameRecord> InstalledFDEs;
  std::map<std::string, uint64_t> InstalledSymbolAddrs;
  std::map<std::string, uint64_t> AuthenticatedFunctionOwnerAddrs;
  std::vector<llvm::mc_rewrite::RewriteFunctionRange>
      AuthenticatedFunctionRanges;

  friend llvm::Expected<MachOEHFrameInstallReceipt>
  installMachOEHFrameWithReceipt(
      std::vector<uint8_t> &Binary,
      const std::optional<MachOEHFrameRegion> &Region,
      const CompiledImage &Compiled, const llvm::Module &Mod,
      const MachOCompactUnwindRecords *CompactCoverage);
};

/// Locate where another `.eh_frame` sequence fits after the records \p Binary
/// already carries.  Returns nullopt when the image declares no usable
/// `__eh_frame`.  In-image placement is only optional for a module with no
/// registered-unwind contract.
std::optional<MachOEHFrameRegion>
findMachOEHFrameRegion(llvm::ArrayRef<uint8_t> Binary);

/// True when \p Mod carries source frame metadata, requests an unwind table, or
/// contains native EH IR.  Every such frame requires registered target-format
/// unwind records, including a source frame with no language personality.
bool requiresRegisteredMachOEHFrame(const llvm::Module &Mod);

/// Validate, install, rediscover, and reparse regenerated DWARF unwind records
/// on a candidate copy.  Required functions may instead be covered by a
/// validated non-DWARF compact record supplied through CompactCoverage.  The
/// returned receipt identifies only bytes and FDEs proven to be present in the
/// final candidate.  Every error leaves Binary byte-for-byte unchanged.
llvm::Expected<MachOEHFrameInstallReceipt> installMachOEHFrameWithReceipt(
    std::vector<uint8_t> &Binary,
    const std::optional<MachOEHFrameRegion> &Region,
    const CompiledImage &Compiled, const llvm::Module &Mod,
    const MachOCompactUnwindRecords *CompactCoverage = nullptr);

/// Copy the regenerated `__eh_frame` in \p Compiled into \p Region and grow the
/// section header to cover it.  The input and regenerated records pass the
/// same strict DWARF decoder, and their half-open FDE ranges must be globally
/// nonoverlapping.  Fails closed without changing \p Binary when \p Mod needs
/// registered records but \p Region is absent, malformed, ambiguous, or too
/// small to hold them.
llvm::Error installMachOEHFrame(std::vector<uint8_t> &Binary,
                                const std::optional<MachOEHFrameRegion> &Region,
                                const CompiledImage &Compiled,
                                const llvm::Module &Mod);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H
