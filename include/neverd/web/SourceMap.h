#pragma once

#include "neverd/web/SourceLocation.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::web {
inline constexpr std::string_view SourceMapProfile = "source-map-v3-offline-v1";
inline constexpr uint64_t MaxSourceMapBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxSourceMapSegments = 100000;

/// Storage locators are offsets in the immutable container, not host paths.
/// Decoded text has its own identity and never reuses compressed byte offsets.
struct MappedSourceStorage {
  uint64_t NameOffset = 0, NameSize = 0;
  uint64_t ContentOffset = 0, ContentSize = 0;
  std::string ContentHash;
};

struct SourceMapEmbedding {
  std::string ContainerArtifactID, ExtractionID, ModuleID, GeneratedArtifactID;
  uint64_t Offset = 0, Size = 0, MappingsOffset = 0, MappingsSize = 0;
};

struct MappedSource {
  std::string ID;
  std::optional<std::string> Name;
  // Share a root rather than amplifying its bytes once per source entry.
  std::shared_ptr<const std::string> Root;
  std::optional<std::string> Contents;
  std::optional<MappedSourceStorage> Storage;
  bool Ignored = false;
};

struct SourceMapSegment {
  TextPosition Generated;
  std::optional<uint64_t> SourceIndex;
  TextPosition Original;
  std::optional<uint64_t> NameIndex;
};

/// Untrusted mapping metadata. Names, URLs and embedded contents remain
/// private evidence; they are not host paths or permissions to read files.
struct SourceMap {
  std::string ID;
  std::string ArtifactID;
  std::string BlobHash;
  std::string Profile = std::string(SourceMapProfile);
  std::optional<SourceMapEmbedding> Embedding;
  std::optional<std::string> File;
  std::vector<MappedSource> Sources;
  std::vector<std::string> Names;
  std::vector<SourceMapSegment> Segments;
  bool Indexed = false;
  // Bun's serializer discards names and one-field unmapped segments. Such
  // a map describes retained anchors, not intervening mapped/unmapped spans.
  bool MappedAnchorsOnly = false;
};

/// Decode basic or embedded-section maps, without resolving any URL. Throws
/// on malformed structure, duplicate keys, invalid indexes and budget limits.
SourceMap decodeSourceMap(std::string_view ArtifactID, std::string_view Bytes);

/// Returns all anchors at the nearest preceding mapped/unmapped position on
/// this line. Empty means no anchor on the line. Does not interpolate columns
/// or turn an asserted source-map relationship into verified provenance.
std::vector<uint64_t> sourceMapAnchors(const SourceMap &Map,
                                       TextPosition Position);
} // namespace neverd::web
