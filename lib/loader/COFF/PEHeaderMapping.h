//===- PEHeaderMapping.h - Checked PE header storage -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checked original PE header mapping and snapshot identity.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_COFF_PEHEADERMAPPING_H
#define NEVERD_LOADER_COFF_PEHEADERMAPPING_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

namespace neverd {
struct BinaryImage;

namespace coff_loader {
struct PEHeaderInitializationWork;
struct PEHeaderMapping {
  va_t Base;
  uint32_t Size;
  uint32_t FileAlignment;
  uint32_t ImageBaseOffset;
};

/// File-backed header bytes on pages disjoint from every section. This
/// deliberately excludes sub-page layouts and grants no header execution or
/// zero-filled padding. Inspect the original bytes, never the COFF reader's
/// zero-VirtualSize projection.
llvm::Expected<PEHeaderMapping>
getPEHeaderMapping(llvm::ArrayRef<uint8_t> Bytes,
                   PEHeaderInitializationWork *Work = nullptr);

void appendPEHeaderMapping(BinaryImage &Image, const PEHeaderMapping &Mapping,
                           llvm::ArrayRef<uint8_t> Bytes);

/// BinaryImage::PEHeaderBytes of a linked PE file: SizeOfHeaders within
/// SizeOfImage, up to the first section that maps over it.  Zero without a
/// complete header or a nonzero 64 KiB-aligned base.  Unlike
/// getPEHeaderMapping, this asks no proof of the loader's writes: those
/// decide read authority over the header bytes, not where the headers are.
uint32_t getPEHeaderBytes(llvm::ArrayRef<uint8_t> Bytes);

/// Bind both normalized header regions to their current raw-file owner.
bool matchesPEHeaderMapping(const BinaryImage &Image, size_t Index,
                            const PEHeaderMapping &Mapping,
                            llvm::ArrayRef<uint8_t> Bytes);
} // namespace coff_loader
} // namespace neverd

#endif
