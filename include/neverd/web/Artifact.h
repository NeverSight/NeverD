#pragma once

#include "neverd/web/Blob.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::web {

/// A content identity never substitutes for an occurrence's container origin.
struct Artifact {
  std::string ID;
  std::string BlobHash;
  std::string ParentID;
  std::string MemberPath;
  std::string Kind;
  Blob Content;
  bool Directory = false;
};

struct Snapshot {
  std::string ID;
  std::vector<Artifact> Artifacts;
  uint64_t InputBytes = 0;
};

std::string sha256(std::string_view Bytes);
/// Shared strict UTF-8 admission, kept outside the embedded parser's headers.
bool validUtf8(std::string_view Bytes);
/// Domain-separated, length-framed UTF-8/byte fields. No delimiter ambiguity.
std::string identity(std::string_view Domain,
                     const std::vector<std::string_view> &Fields);

} // namespace neverd::web
