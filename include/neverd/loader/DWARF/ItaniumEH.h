//===- ItaniumEH.h - Itanium exception recovery driver --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Drives Itanium-model exception recovery for a loaded image: locates the
/// frame section, decodes its call frame information, resolves each frame's
/// personality and language-specific data area, and normalizes the result
/// into the image's \ref ExceptionInfo.
///
/// This runs after symbols, imports, and import veneers have been discovered,
/// because a personality routine is usually reached through a dynamically
/// bound slot whose file-image value names nothing.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_DWARF_ITANIUMEH_H
#define NEVERD_LOADER_DWARF_ITANIUMEH_H

#include "neverd/loader/BinaryImage.h"

namespace neverd::dwarf_eh {

/// Decode `.eh_frame` / `__eh_frame` and every LSDA it names into
/// `Img.ExceptionMetadata`.  A no-op for an image without a frame section.
///
/// Existing records are preserved: a MinGW PE legitimately carries both a
/// `.pdata` directory and Itanium tables, and each describes a different part
/// of the same image.
void parseItaniumExceptions(BinaryImage &Img);

/// Re-decode one function's LSDA after its personality has changed.
///
/// \p Function is a staging copy whose personality already carries the new
/// classification.  The replacement is accepted only when the original FDE
/// and LSDA provenance are present and the new decode is complete; otherwise
/// the function is left untouched and false is returned.
bool refreshItaniumLanguageData(const BinaryImage &Img,
                                ExceptionFunction &Function);

} // namespace neverd::dwarf_eh

#endif // NEVERD_LOADER_DWARF_ITANIUMEH_H
