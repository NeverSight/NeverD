//===- Streams.h - Passive transcript evidence --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded offline framing and metadata without publishing input values.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

#include <optional>

namespace neverd::web {

inline constexpr std::string_view StreamRedactionPolicy =
    "passive-stream-metadata-v1";
inline constexpr uint64_t MaxStreamBytes = 8 * 1024 * 1024;
inline constexpr uint64_t MaxStreamFragmentBytes = 256 * 1024;
inline constexpr uint64_t MaxStreamRecords = 4096;
inline constexpr uint64_t MaxStreamJSONWork = 200000;
inline constexpr uint64_t MaxStreamPrivateBytes = 1024 * 1024;

/// All strings here are fixed vocabulary or occurrence identities. Input
/// payloads, arbitrary method/event names and recorded identifiers are absent.
struct StreamRecord {
  std::string ID;
  uint64_t Offset = 0, Length = 0, FirstLine = 0, LineCount = 0;
  std::string Kind, JSONKind = "not_inspected", RPC = "not_selected";
  std::string Method = "absent", IDKind = "absent";
  std::string Direction = "absent", Timestamp = "absent";
  std::string Relation = "not_applicable", Event = "absent";
  std::optional<uint32_t> Peer;
  uint64_t DataLines = 0, DataBytes = 0, Comments = 0, IgnoredFields = 0;
  uint64_t IDFields = 0, InvalidIDFields = 0, RetryFields = 0;
  bool Terminated = false, Dispatched = false;
  bool SessionPresent = false, EventIDPresent = false, EventIDReset = false;
  bool ParamsPresent = false, ResultPresent = false, ErrorPresent = false;
};

struct StreamCapture {
  std::string ID, ArtifactID, BlobHash, Profile;
  std::vector<StreamRecord> Records;
  uint64_t JSONWork = 0, PairCount = 0;
  bool BOM = false;
  std::string RelationStatus = "not_requested";
};

std::vector<std::string_view> streamProfiles();
/// The selected profile determines framing. No protocol auto-detection,
/// execution or fetching occurs. Callers must preview/commit before
/// publication.
StreamCapture inspectStream(std::string_view ArtifactID, std::string_view Bytes,
                            std::string_view Profile);

} // namespace neverd::web
