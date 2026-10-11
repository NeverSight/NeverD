#pragma once

#include "neverd/sdk/NeverDCAPITypes.h"

#include <cstdint>
#include <string>

namespace neverd {
struct BinaryImage;
namespace sdk {
/// Same-build SDK seam. Publish an already validated, fully owned image into
/// a fresh session using the normal decoder/image publication transaction.
/// No file or debug/annotation companion discovery is performed.
int loadNativeSnapshotSession(neverd_session_t Session, BinaryImage Image,
                              uint64_t InputBytes, std::string Provenance);
} // namespace sdk
} // namespace neverd
