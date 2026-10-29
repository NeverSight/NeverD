//===- MappedSectionFlags.h - Section flags a loader maps -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Change a section's flags as a loader maps them. The segment that holds a
/// section owns its runtime protection (BinaryImage::readOnlyAfterRelocations),
/// so a writable section lies in a writable mapping.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORE_MAPPEDSECTIONFLAGS_H
#define NEVERD_UNITTESTS_LIFT_CORE_MAPPEDSECTIONFLAGS_H

#include "neverd/loader/BinaryImage.h"

namespace neverd {

/// Give \p Changed the \p Flags, and the mapping that holds it in \p Image
/// the write permission they grant.
inline void setMappedSectionFlags(BinaryImage &Image, Section &Changed,
                                  SegmentFlags Flags) {
  Changed.Flags = Flags;
  if (!hasFlag(Flags, SegmentFlags::Writable))
    return;
  for (Segment &Mapping : Image.Segments)
    if (Mapping.contains(Changed.VA))
      Mapping.Flags = Mapping.Flags | SegmentFlags::Writable;
}

} // namespace neverd

#endif // NEVERD_UNITTESTS_LIFT_CORE_MAPPEDSECTIONFLAGS_H
