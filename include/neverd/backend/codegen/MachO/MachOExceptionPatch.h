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
/// `__eh_frame`, in which case the caller keeps codegen's in-image placement.
std::optional<MachOEHFrameRegion>
findMachOEHFrameRegion(llvm::ArrayRef<uint8_t> Binary);

/// True when \p Mod carries an exception contract that cannot run without
/// registered unwind records — a personality, an invoke, a landing pad, or a
/// resume.  Purely descriptive CFI records may be dropped; these may not.
bool requiresRegisteredMachOEHFrame(const llvm::Module &Mod);

/// Copy the regenerated `__eh_frame` in \p Compiled into \p Region and grow the
/// section header to cover it.  Fails closed when \p Mod needs registered
/// records but \p Region is absent or too small to hold them.
llvm::Error installMachOEHFrame(std::vector<uint8_t> &Binary,
                                const std::optional<MachOEHFrameRegion> &Region,
                                const CompiledImage &Compiled,
                                const llvm::Module &Mod);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_MACHO_MACHOEXCEPTIONPATCH_H
