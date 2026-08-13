//===- ELFExceptionPatch.h - ELF unwind-record rewrite ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_ELF_ELFEXCEPTIONPATCH_H
#define NEVERD_BACKEND_CODEGEN_ELF_ELFEXCEPTIONPATCH_H

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

/// The two file-backed regions that carry DWARF unwind on ELF.  Regenerated
/// records are appended to the tail of `.eh_frame`, and every appended function
/// is added to the sorted search table in `.eh_frame_hdr` -- the ELF unwinder
/// reaches an FDE only through that table, so a record the table does not name
/// is invisible however correctly it is written.  The `PT_GNU_EH_FRAME` segment
/// that publishes the table to the loader is grown alongside it.
struct ELFEHFrameRegion {
  bool Is64 = true;

  /// `.eh_frame`: the records themselves, and where another sequence fits after
  /// the ones the image already carries.
  uint64_t SectionVA = 0;
  uint64_t SectionFileOff = 0;
  uint64_t AppendVA = 0;
  uint64_t AppendFileOff = 0;
  uint64_t LimitFileOff = 0;
  uint64_t SectionHeaderOff = 0;

  /// `.eh_frame_hdr`: the binary-search table, its slack, and the program
  /// header that maps it.  Absent when the image ships no header, in which case
  /// appended records cannot be registered and a module that needs them fails
  /// closed rather than producing a binary that cannot unwind.
  bool HasHdr = false;
  uint64_t HdrVA = 0;
  uint64_t HdrFileOff = 0;
  uint64_t HdrSize = 0;
  uint64_t HdrLimitFileOff = 0;
  uint64_t HdrSectionHeaderOff = 0;
  uint64_t GnuEhFramePhdrOff = 0;
};

/// Locate where another `.eh_frame` sequence fits after the records \p Binary
/// already carries, together with the `.eh_frame_hdr` table that must name it.
/// Returns nullopt when the image declares no usable `.eh_frame`, in which case
/// the caller keeps codegen's in-image placement.
std::optional<ELFEHFrameRegion>
findELFEHFrameRegion(llvm::ArrayRef<uint8_t> Binary);

/// True when \p Mod carries an exception contract that cannot run without
/// registered unwind records -- a personality, an invoke, a landing pad, or a
/// resume.  Purely descriptive CFI records may be dropped; these may not.
bool requiresRegisteredELFEHFrame(const llvm::Module &Mod);

/// Copy the regenerated `.eh_frame` in \p Compiled into \p Region, add its
/// functions to the `.eh_frame_hdr` search table, and grow the covering section
/// and segment to match.  Fails closed when \p Mod needs registered records but
/// \p Region is absent, too small, or shaped in a way the table rewrite does
/// not model, so an image is never written that would fault while unwinding.
llvm::Error installELFEHFrame(std::vector<uint8_t> &Binary,
                              const std::optional<ELFEHFrameRegion> &Region,
                              const CompiledImage &Compiled,
                              const llvm::Module &Mod);

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_ELF_ELFEXCEPTIONPATCH_H
