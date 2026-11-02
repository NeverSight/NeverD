//===- BunProjectRequire.h - Bound Bun require alias candidates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private cross-module evidence for exact import.meta.require aliases.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_WEB_PRIVATE_BUNPROJECTREQUIRE_H
#define NEVERD_WEB_PRIVATE_BUNPROJECTREQUIRE_H

#include "BunProjectSource.h"

#include <utility>

namespace neverd::web {
inline constexpr std::string_view BunProjectRequireProfile =
    "bun-import-meta-require-alias-candidates-v1";
inline constexpr uint64_t MaxBunProjectRequireRecords = 4096;

struct BunProjectRequireProviders;
struct BunProjectRequireCalls;
class BunProjectRequireCall;

/// A unique, unwritten module binding initialized directly from
/// import.meta.require and exported by an exact local named export.
/// Only the lexical/module evidence owner can construct a nonempty provider.
class BunProjectRequireProvider {
public:
  struct Data {
    std::string ID, SourceArtifactID, SourceID, SourceHash;
    std::string BindingID, ExportID;
    std::u16string VirtualName, ExportedName;
    uint64_t InitializerStart = 0, InitializerEnd = 0;
  };
  BunProjectRequireProvider() = default;
  const Data &data() const { return Evidence; }

private:
  Data Evidence;
  explicit BunProjectRequireProvider(Data Value) : Evidence(std::move(Value)) {}
  friend BunProjectRequireProviders
      collectBunProjectRequireProviders(std::string_view, std::string_view,
                                        std::string_view, std::string_view);
};

/// Immutable private owner output, never deserialized from external data.
/// A relocation consumer must ALSO compare the source hash and exact AST
/// call/callee/argument spans and the decoded literal value. IDs name evidence;
/// they are not an authentication mechanism for arbitrary C++ callers.
class BunProjectRequireCall {
public:
  struct Data {
    std::string ID, SourceArtifactID, SourceHash;
    std::string ProviderID, ProviderSourceHash, ProviderBindingID,
        ProviderExportID;
    std::string ConsumerBindingID, ConsumerImportID;
    uint64_t CallStart = 0, CallEnd = 0, CalleeStart = 0, CalleeEnd = 0;
    uint64_t OriginalStart = 0, OriginalEnd = 0;
    std::u16string Target;
  };
  BunProjectRequireCall() = default;
  const Data &data() const { return Evidence; }

private:
  Data Evidence;
  explicit BunProjectRequireCall(Data Value) : Evidence(std::move(Value)) {}
  friend BunProjectRequireCalls
      proveBunProjectRequireCalls(std::string_view, std::string_view,
                                  std::string_view,
                                  std::span<const BunProjectRequireProvider>,
                                  std::span<const BunProjectSourceTarget>);
};

struct BunProjectRequireProviders {
  std::string Status = "not_run";
  std::vector<BunProjectRequireProvider> Providers;
  std::vector<SourceDiagnostic> Diagnostics;
};
struct BunProjectRequireCalls {
  std::string Status = "not_run";
  std::vector<BunProjectRequireCall> Calls;
  // The same proven imported callee with a literal outside Bun's virtual
  // namespaces. Preserve these unchanged requests without treating them as
  // missing modules or relocation proofs. Spans are original source bytes.
  std::vector<BunProjectSourceReference> ExternalReferences;
  std::vector<SourceDiagnostic> Diagnostics;
};

/// Each operation performs one explicit Large parse and uses the shared
/// lexical/module owners. Call only for selected candidates, never reuse a
/// Recovery model as semantic evidence. No target is executed.
BunProjectRequireProviders collectBunProjectRequireProviders(
    std::string_view ArtifactID, std::string_view Text,
    std::string_view SourceType, std::string_view VirtualName);
BunProjectRequireCalls proveBunProjectRequireCalls(
    std::string_view ArtifactID, std::string_view Text,
    std::string_view SourceType,
    std::span<const BunProjectRequireProvider> Providers,
    std::span<const BunProjectSourceTarget> Targets);

/// Structural check for an immutable private owner result. Default is invalid.
bool validBunProjectRequireCall(const BunProjectRequireCall &Call);
} // namespace neverd::web

#endif // NEVERD_WEB_PRIVATE_BUNPROJECTREQUIRE_H
