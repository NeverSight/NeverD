//===- ARMEHABI.h - ARM EHABI exception recovery driver -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives ARM EHABI exception recovery for a loaded image: walks the sorted
/// `.ARM.exidx` index, decodes each entry's unwinding description -- inline in
/// the index or out of line in `.ARM.extab` -- and normalizes the result into
/// the image's \ref ExceptionInfo.
///
/// EHABI is the one table model in wide use that gives a C++ frame's language
/// data no section of its own.  On every other Itanium target the LSDA lives
/// in `.gcc_except_table` and an FDE points at it; here it is appended to the
/// frame's `.ARM.extab` entry, immediately after the unwind opcodes.  A reader
/// that looks only for `.gcc_except_table` therefore finds nothing at all in a
/// 32-bit ARM image, which is most of the C++ ever shipped on Android and on
/// embedded Linux.
///
/// The index is also the most complete function table such an image has: it
/// covers every function the linker placed, in address order, whether or not
/// the image kept its symbols. \ref parseARMEHABIExceptions therefore seeds
/// function discovery from it.
///
/// This runs after symbols, imports, and import veneers have been discovered,
/// because a generic-model entry names its personality routine through a PLT
/// veneer whose address alone identifies nothing.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_ELF_ARMEHABI_H
#define NEVERD_LOADER_ELF_ARMEHABI_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::arm_ehabi {

/// Decode `.ARM.exidx` and every `.ARM.extab` entry it names into
/// `Img.ExceptionMetadata`.  A no-op for an image without an index, and for
/// an image whose architecture is not 32-bit ARM: the section name is not
/// reserved, and a table read against the wrong pointer size is worse than no
/// table at all.
///
/// Existing records are preserved.  An image can legitimately carry both an
/// index and DWARF frame information -- `-fasynchronous-unwind-tables` emits
/// `.eh_frame` beside `.ARM.exidx` -- and each describes the same frames from
/// a different direction.
void parseARMEHABIExceptions(BinaryImage &Img);

} // namespace neverd::arm_ehabi

#endif // NEVERD_LOADER_ELF_ARMEHABI_H
