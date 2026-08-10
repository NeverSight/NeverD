//===- SBFMetadata.h - Loader-to-frontend SBF metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFMETADATA_H
#define NEVERD_SBF_SBFMETADATA_H

#include "neverd/sbf/Version.h"

#include <cstdint>

namespace neverd::sbf {

struct FileRange {
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct VMRange {
  uint64_t Address = 0;
  uint64_t Size = 0;
};

struct Metadata {
  uint16_t Machine = 0;
  uint32_t ELFFlags = 0;
  neverd::sbf::Version Version = neverd::sbf::Version::Reserved;
  bool StrictLayout = false;
  FileRange TextFile;
  VMRange TextVM;
  FileRange RodataFile;
  VMRange RodataVM;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SBFMETADATA_H
