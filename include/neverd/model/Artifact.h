//===- Artifact.h - Offline model artifact inspection ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Offline model artifact inspection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MODEL_ARTIFACT_H
#define NEVERD_MODEL_ARTIFACT_H
#include "neverd/model/Limits.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace neverd::model {
struct TensorRange {
  std::string Name;
  uint64_t Offset, Length;
};
struct Options {
  std::optional<TensorRange> Range;
  std::filesystem::path Baseline;
  bool ExternalData = false;
  uint64_t GapBytes = limits::kGapBytes;
  uint64_t ScaleBytes = 0;
  uint64_t CompareBytes = limits::kCompareBytes;
  uint64_t Offset = 0;
  unsigned Limit = limits::kPage;
  std::function<bool()> Cancel;
};
/// JSON contains stable package-relative identities and scoped coverage.
std::string scan(const std::filesystem::path &Input, const Options &Opts = {});
std::string scanJSON(const char *Path, const char *Settings,
                     std::function<bool()> Cancel);
} // namespace neverd::model
#endif // NEVERD_MODEL_ARTIFACT_H
