//===- MachOExceptionPatch.h - Mach-O unwind-record rewrite -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace llvm {
class Module;
}

namespace neverd {

struct CompiledImage;

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
