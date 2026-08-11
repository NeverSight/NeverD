//===- MachOExceptions.h - Darwin exception recovery driver ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives exception recovery for a Mach-O image.
///
/// Darwin splits the job across two sections that must be read together:
/// `__unwind_info` holds a compact entry for nearly every function, including
/// its personality index and LSDA pointer, and `__eh_frame` holds a full FDE
/// only for the frames whose shape no compact encoding can express.  Reading
/// either alone misses most of the image.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_MACHO_MACHOEXCEPTIONS_H
#define NEVERD_LOADER_MACHO_MACHOEXCEPTIONS_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::macho_unwind {

/// Decode `__unwind_info`, `__eh_frame`, and every LSDA they name into
/// `Img.ExceptionMetadata`.  A no-op for an image with neither section.
void parseDarwinExceptions(BinaryImage &Img);

} // namespace neverd::macho_unwind

#endif // NEVERD_LOADER_MACHO_MACHOEXCEPTIONS_H
