//===- BinaryImageSection.h - Section within a segment ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The fine-grained region model (`.text`, `.data`, `.bss`, ...) that sits
/// inside the coarse load region \ref Segment describes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGESECTION_H
#define NEVERD_LOADER_BINARYIMAGESECTION_H

#include "neverd/Common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Section — fine-grained region within a segment
// ===--------------------------------------------------------------------===//

struct Section {
  std::string Name;
  std::string SegmentName;
  va_t VA = 0;
  uint64_t Size = 0;
  uint64_t FileOff = 0;
  uint64_t FileSz = 0;
  SegmentFlags Flags = SegmentFlags::None;
  uint32_t Type = 0;
  uint32_t Alignment = 1;
  std::vector<uint8_t> Data;

  bool isExecutable() const { return hasFlag(Flags, SegmentFlags::Executable); }
  bool isWritable() const { return hasFlag(Flags, SegmentFlags::Writable); }
  bool isReadable() const { return hasFlag(Flags, SegmentFlags::Readable); }
  bool contains(va_t Addr) const { return Addr >= VA && Addr - VA < Size; }
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGESECTION_H
