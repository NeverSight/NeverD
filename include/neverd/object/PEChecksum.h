//===- PEChecksum.h - PE image checksum ------------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_OBJECT_PECHECKSUM_H
#define NEVERD_OBJECT_PECHECKSUM_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>

namespace neverd {
/// Sum the complete file, including any overlay, with the four-byte checksum
/// field excluded. The file length is added after end-around carry folding.
inline std::optional<uint32_t> computePEChecksum(llvm::ArrayRef<uint8_t> File,
                                                 uint64_t ChecksumOffset) {
  if (File.size() > UINT32_MAX || ChecksumOffset > File.size() ||
      File.size() - ChecksumOffset < sizeof(uint32_t))
    return std::nullopt;
  auto Byte = [&](uint64_t At) -> uint32_t {
    return At >= File.size() ||
                   (At >= ChecksumOffset && At - ChecksumOffset < 4)
               ? 0
               : File[At];
  };
  uint32_t Sum = 0;
  for (uint64_t I = 0; I < File.size(); I += 2) {
    Sum += Byte(I) | (Byte(I + 1) << 8);
    Sum = (Sum & 0xffff) + (Sum >> 16);
  }
  Sum = (Sum & 0xffff) + (Sum >> 16);
  return Sum + uint32_t(File.size());
}
} // namespace neverd
#endif
