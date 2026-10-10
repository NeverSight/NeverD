#pragma once

#include "neverd/web/Artifact.h"
#include "neverd/web/SourceBindings.h"

#include <optional>

namespace neverd::web {

inline constexpr std::string_view JavaScriptModuleProfile =
    "javascript-module-evidence-v1";
inline constexpr std::string_view JavaScriptModuleLinkProfile =
    "admitted-relative-file-v1";
inline constexpr uint64_t MaxJavaScriptModuleSteps = 2000000;
inline constexpr uint64_t MaxJavaScriptModuleNameUnits = 1048576;
inline constexpr uint64_t MaxJavaScriptModulePathUnits = 4096;

struct SourceModuleRequest {
  std::string ID;
  std::string Kind;
  uint32_t Node = NoSourceIndex;
  uint32_t SpecifierNode = NoSourceIndex;
  uint32_t Specifier = NoSourceIndex;
  uint32_t AttributesNode = NoSourceIndex;
  uint32_t CalleeNode = NoSourceIndex;
  uint32_t CalleeBinding = NoSourceIndex;
  std::string CalleeEvidence;
  std::string SpecifierStatus;
  uint32_t ArgumentCount = 0;
  bool Optional = false;
};

struct SourceModuleImport {
  std::string ID;
  std::string Kind;
  uint32_t Node = NoSourceIndex;
  uint32_t Request = NoSourceIndex;
  uint32_t ImportedName = NoSourceIndex;
  uint32_t Binding = NoSourceIndex;
};

struct SourceModuleExport {
  std::string ID;
  std::string Kind;
  std::string LinkStatus;
  uint32_t Node = NoSourceIndex;
  uint32_t Request = NoSourceIndex;
  uint32_t ExportedName = NoSourceIndex;
  uint32_t LocalName = NoSourceIndex;
  uint32_t Binding = NoSourceIndex;
  bool Conflicting = false;
};

struct SourceModuleAttribute {
  std::string ID;
  uint32_t Node = NoSourceIndex;
  uint32_t Request = NoSourceIndex;
  uint32_t Key = NoSourceIndex;
  uint32_t Value = NoSourceIndex;
  bool Conflicting = false;
};

struct SourceModuleAnalysis {
  std::string ID;
  std::string SourceID;
  std::string BindingID;
  std::string Status;
  std::vector<SourceModuleRequest> Requests;
  std::vector<SourceModuleImport> Imports;
  std::vector<SourceModuleExport> Exports;
  std::vector<SourceModuleAttribute> Attributes;
  // Exact private UTF-16 strings. Never emit specifiers, names or assertion
  // values through ordinary metadata, diagnostics or transport errors.
  std::vector<std::u16string> Names;
  std::vector<SourceDiagnostic> Diagnostics;
  uint64_t DiagnosticCount = 0;
  uint64_t Steps = 0;
};

struct SourceModuleLink {
  // An exact admitted-file candidate is NOT a resolved runtime module.
  std::string Status;
  std::string ArtifactID;
  std::string URLCandidateID, ImportMapID, ImportMapEntryID, ImportMapScopeID;
  std::string ImportMapMatch;
  std::optional<bool> Query, Fragment;
};

struct SourceModuleLinkContext {
  std::string ID, Kind, HTMLID, ScriptID, BaseID, Status, ImportMapStatus;
  std::string ImportMapAnalysisID;
  std::string ModuleURLIdentity = "not_modeled";
};

struct SourceModuleLinks {
  std::string ID;
  std::string Profile = std::string(JavaScriptModuleLinkProfile);
  std::optional<SourceModuleLinkContext> Context;
  std::string Status;
  std::vector<SourceModuleLink> Requests;
  uint64_t Steps = 0;
};

/// Syntactic requests/imports/exports with lexical evidence. Neither binding
/// identity nor caller-selected CommonJS mode proves a runtime require value.
/// No target code, identifier propagation, module loader or filesystem access.
SourceModuleAnalysis
analyzeSourceModules(const SourceAnalysis &Source,
                     const SourceBindingAnalysis &Bindings);

/// Compare literal static ESM relative paths with already admitted members.
/// No extension/index/package/URL lookup, external I/O or dynamic-import link.
/// Derived source-map paths and single-file imports provide no directory base.
SourceModuleLinks linkSourceModules(const SourceAnalysis &Source,
                                    const SourceModuleAnalysis &Modules,
                                    const Snapshot &Input);

struct HTMLDocument;
struct HTMLLinks;
struct HTMLImportMaps;
inline constexpr std::string_view HTMLModuleLinkProfile =
    "html-inline-module-file-candidates-v2";
inline constexpr uint64_t MaxHTMLModuleLinkSteps = 4 * 1024 * 1024;
inline constexpr uint64_t MaxHTMLModuleRequests = 10000;
/// Explicit inline occurrence context, never a fabricated captured member.
/// Literal ESM imports/reexports and dynamic import expressions use the HTML
/// URL owners. Import-map composition requires the source-order candidate
/// profile and never establishes browser activation or runtime identity.
SourceModuleLinks linkHTMLSourceModules(
    const SourceAnalysis &Source, const SourceModuleAnalysis &Modules,
    const HTMLDocument &Document, const HTMLLinks &Links, uint32_t ScriptIndex,
    const Snapshot &Input, const HTMLImportMaps &ImportMaps);

} // namespace neverd::web
