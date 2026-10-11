//===- ImportMap.h - Bounded import map resolution ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded import map resolution.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Artifact.h"

#include <functional>
#include <span>

namespace neverd::web {
inline constexpr std::string_view ImportMapProfile =
    "import-map-url-evidence-v1";
inline constexpr uint64_t MaxImportMapBytes = 1024 * 1024;
inline constexpr uint64_t MaxImportMapRecords = 10000;
inline constexpr uint64_t MaxImportMapURLBytes = 4096;
inline constexpr uint64_t MaxImportMapSteps = 4 * 1024 * 1024;
using ImportMapCharge = std::function<void(uint64_t)>;

struct ImportMapEntry {
  std::string ID;
  // Private normalized URL/specifier text. An empty Address is a blocking
  // entry, never permission to fall back to another scope or map.
  std::string Key, RawKey, Address, RawAddress, BlockReason;
  bool AddressRelative = false;
};
struct ImportMapScope {
  std::string ID, Prefix, RawPrefix; // URL text stays private.
  std::vector<ImportMapEntry> Entries;
};
struct ImportMap {
  std::string ID, Status, Reason, BaseURL;
  std::vector<ImportMapEntry> Imports;
  std::vector<ImportMapScope> Scopes;
  uint64_t Steps = 0, EntryCount = 0, IgnoredKeys = 0, IntegrityCount = 0;
  bool IntegrityPresent = false, OriginDependentKeys = false;
};

/// Parse captured JSON under an explicit base URL. Duplicate decoded or
/// normalized keys and lone surrogates are explicit profile refusals. All
/// URL work is in process; this API performs no I/O and executes no input.
ImportMap inspectImportMap(std::string_view DeclarationID,
                           std::string_view Bytes, std::string_view BaseURL);

struct ImportMapResolution {
  std::string Status, URL, MatchKind, MapID, EntryID, ScopeID;
  const ImportMapEntry *Entry = nullptr;
  const ImportMap *Map = nullptr;
};
/// Candidate composition with an empty prior resolved-module set. Earlier
/// maps win duplicate definitions. The caller must establish its timing
/// profile separately; this does not reconstruct browser preparation order.
ImportMapResolution resolveImportMaps(std::span<const ImportMap *const> Maps,
                                      std::string_view Specifier,
                                      std::string_view ScriptBaseURL,
                                      const ImportMapCharge &Charge);

/// Shared native URL owner. Returns empty for a failed or oversized URL;
/// serialized query, fragment and percent spelling are retained privately.
std::string normalizeModuleURL(std::string_view Reference,
                               std::string_view BaseURL,
                               const ImportMapCharge &Charge);
} // namespace neverd::web
