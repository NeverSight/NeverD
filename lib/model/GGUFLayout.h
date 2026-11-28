//===- GGUFLayout.h - Serialized ranges for a verified GGUF copy ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Coordinates captured by the same directory reader used for inspection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_MODEL_GGUFLAYOUT_H
#define NEVERD_LIB_MODEL_GGUFLAYOUT_H

#include <cstdint>
#include <string>
#include <vector>

namespace neverd::model {
struct GGUFMetadataRange {
  std::string Key, ValueHash;
  uint64_t Begin, Value, End;
};
struct GGUFTensorRecord {
  std::string Name;
  uint64_t OffsetField;
};
struct GGUFLayout {
  std::vector<GGUFMetadataRange> Metadata;
  std::vector<GGUFTensorRecord> Records;
  uint64_t DirectoryEnd = 0, DataStart = 0, Alignment = 0;
  bool BigEndian = false;
};
} // namespace neverd::model
#endif // NEVERD_LIB_MODEL_GGUFLAYOUT_H
