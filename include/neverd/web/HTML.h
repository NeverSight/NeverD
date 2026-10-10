#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/ImportMap.h"

namespace neverd::web {
inline constexpr std::string_view HTMLProfile =
    "html-utf8-script-candidates-v1";
inline constexpr uint64_t MaxHTMLBytes = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLSteps = 16 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLRecords = 10000;
inline constexpr uint64_t MaxHTMLDecodedBytes = 1024 * 1024;
inline constexpr uint64_t MaxHTMLAttributeBytes = 16384;
inline constexpr uint64_t MaxHTMLPathBytes = 4096;
inline constexpr uint64_t MaxHTMLLinkSteps = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLNamespaceEntries = 10001;
inline constexpr uint64_t MaxHTMLImportMaps = 64;
inline constexpr uint32_t NoHTMLIndex = UINT32_MAX;

struct HTMLScript {
  std::string ID, Kind, SourceType, Context, BodyStatus;
  std::string InlineArtifactID, BodyHash;
  // Private decoded reference, never returned by ordinary metadata APIs.
  std::string Reference;
  uint64_t TagStart = 0, TagEnd = 0, BodyStart = 0, BodyEnd = 0, End = 0;
  uint64_t ReferenceStart = 0, ReferenceLength = 0;
  uint32_t Base = NoHTMLIndex;
  bool HasSource = false, Closed = false;
  bool Async = false, Defer = false, NoModule = false;
  bool Integrity = false, CrossOrigin = false, DuplicateAttributes = false;
};
struct HTMLBase {
  std::string ID, Context, Reference; // Reference stays private.
  uint64_t Start = 0, End = 0, ValueStart = 0, ValueLength = 0;
  bool Selected = false, DuplicateAttributes = false;
};
struct HTMLDocument {
  std::string ID, ArtifactID, BlobHash, Status, Reason;
  std::vector<HTMLScript> Scripts;
  std::vector<HTMLBase> Bases;
  uint64_t Steps = 0, DecodedBytes = 0, TokenCount = 0;
};

/// UTF-8 source-visible script candidates. No DOM, script execution, encoding
/// sniffing or fetch. HTML input preprocessing and browser tree construction
/// are not claimed. Unsupported contexts never become recovered JS sources.
HTMLDocument inspectHTML(std::string_view ArtifactID, std::string_view Bytes);

/// HTML attribute character references, with the pinned WHATWG name table and
/// numeric-reference rules. Steps and decoded-byte counters are shared with
/// the containing document. No raw content is published by this helper.
std::string decodeHTMLAttribute(std::string_view Bytes, uint64_t &Steps,
                                uint64_t &DecodedBytes);

struct HTMLLocalURL {
  std::string Path, Status;
  bool Directory = false, Query = false, Fragment = false;
};
/// A portable file-URL candidate relative to an admitted occurrence. Literal
/// absolute paths/URLs and encoded separators cannot escape to the host.
HTMLLocalURL resolveHTMLLocalURL(std::string_view BasePath, bool BaseDirectory,
                                 std::string_view Reference);
struct HTMLScriptLink {
  std::string Status, ArtifactID, BaseStatus;
  HTMLLocalURL Base; // Private context for a later inline-module consumer.
  bool Query = false, Fragment = false;
};
struct HTMLLinks {
  std::string ID, Status;
  std::vector<HTMLScriptLink> Scripts;
  uint64_t Steps = 0;
};
HTMLLinks linkHTMLScripts(const HTMLDocument &Document, const Snapshot &Input);

struct HTMLImportMap {
  uint32_t Script = NoHTMLIndex;
  std::string BaseStatus;
  HTMLLocalURL LocalBase;
  ImportMap Model;
};
struct HTMLImportMaps {
  std::string ID, Status, Reason;
  std::string DocumentURL; // Private synthetic capture URL, never a host path.
  std::vector<HTMLImportMap> Declarations;
  uint64_t Steps = 0, EligibleCount = 0;
};
HTMLImportMaps inspectHTMLImportMaps(const HTMLDocument &Document,
                                     std::string_view Bytes,
                                     const Snapshot &Input);
} // namespace neverd::web
