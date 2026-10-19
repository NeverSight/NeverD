//===- X64SEHMetadata.h - Retained x64 metadata byte ranges -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Retained x64 metadata byte ranges.
///

#ifndef NEVERD_EMULATION_X64SEHMETADATA_H
#define NEVERD_EMULATION_X64SEHMETADATA_H
#include "neverd/loader/COFF/PEProgramExports.h"
#include "neverd/loader/ExceptionTable.h"

#include "llvm/ADT/StringRef.h"
namespace neverd::emulation {
struct X64SEHMetadataRanges {
  std::vector<PEMetadataRange> Ranges;
  uint64_t Records = 0;
};
/// Original-image RVAs for the exact directory/unwind/scope/GS facts, headers,
/// and executable regions used to classify personalities. No memory reads.
llvm::Expected<X64SEHMetadataRanges>
x64SEHMetadataRanges(const ExceptionInfo &, uint64_t PreferredBase,
                     uint64_t ImageSize, uint64_t HeaderSize,
                     llvm::ArrayRef<PEMetadataRange> CodeRegions,
                     uint64_t RecordLimit);
/// Availability only; these identities do not declare a callable DDI ABI.
llvm::ArrayRef<llvm::StringLiteral> x64SEHPersonalityExports();
} // namespace neverd::emulation
#endif
