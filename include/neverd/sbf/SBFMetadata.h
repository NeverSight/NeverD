//===- SBFMetadata.h - Loader-to-frontend SBF metadata ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SBFMETADATA_H
#define NEVERD_SBF_SBFMETADATA_H

#include "neverd/sbf/image/SBFVersion.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd::sbf {

enum class DebugEnrichmentStatus : uint8_t {
  NotAttempted,
  Complete,
  Unavailable,
  Malformed,
};

inline llvm::StringRef debugEnrichmentStatusName(DebugEnrichmentStatus Status) {
  switch (Status) {
  case DebugEnrichmentStatus::NotAttempted:
    return "not-attempted";
  case DebugEnrichmentStatus::Complete:
    return "complete";
  case DebugEnrichmentStatus::Unavailable:
    return "unavailable";
  case DebugEnrichmentStatus::Malformed:
    return "malformed";
  }
  return "unknown";
}

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
  DebugEnrichmentStatus DebugEnrichment = DebugEnrichmentStatus::NotAttempted;
  FileRange TextFile;
  VMRange TextVM;
  FileRange RodataFile;
  VMRange RodataVM;
};

} // namespace neverd::sbf

#endif // NEVERD_SBF_SBFMETADATA_H
