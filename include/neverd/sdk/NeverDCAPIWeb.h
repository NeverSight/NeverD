//===- NeverDCAPIWeb.h - Offline web artifact analysis -----------*- C -*-===//
#ifndef NEVERD_SDK_CAPI_WEB_H
#define NEVERD_SDK_CAPI_WEB_H

#include "neverd/sdk/NeverDCAPISession.h" // Shared owned-string disposer.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Independent of neverd_session_t: source identities are not native addresses.
typedef void *neverd_web_session_t;

/// Returns null on allocation failure or when web analysis is disabled.
NEVERD_API neverd_web_session_t neverd_web_session_create(void);
/// No concurrent call may be active during destruction. Null is accepted.
NEVERD_API void neverd_web_session_destroy(neverd_web_session_t Session);

/// All JSON results are owned strings, released using neverd_free_string().
/// Null means allocation failure. Errors use schema_version/status/error.code;
/// no untrusted source text or input name is included in ordinary diagnostics.
NEVERD_API const char *neverd_web_capabilities_json(void);

/// Analyze a caller-selected manifest within its captured occurrence namespace.
/// Metadata only; main entry links are exact-file candidates, not runtime
/// resolution. Available ASAR members are valid selections. No host lookup.
NEVERD_API const char *neverd_web_electron_manifest_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize);
/// Source-visible Electron API candidates using lexical module origins.
/// No framework/runtime-target verification, execution or permission verdict.
NEVERD_API const char *neverd_web_electron_source_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// Page source boundaries with exact node/range links; literal channels,
/// paths, URLs and API names remain private. Limit is 1..512.
NEVERD_API const char *neverd_web_electron_source_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    uint64_t Offset, uint64_t Limit);

/// Compare private constant IPC channels within one explicitly selected
/// manifest directory namespace. Manifest and source evidence must already
/// exist in this revision. Options: {"schema_version":1,"source_ids":[...]}.
/// Select 1..16 sources, including the manifest's main candidate with its
/// source type. Matching channels are candidates, not verified runtime routes.
NEVERD_API const char *neverd_web_electron_ipc_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ManifestArtifactID, size_t ArtifactIDSize,
    const char *OptionsJSON, size_t OptionsSize);
/// RecordKind is sources, channels or endpoints. Limit is 1..512.
/// Channel IDs bind the selected evidence scope; raw values and standalone
/// channel hashes are never returned. Import replacement invalidates the ID.
NEVERD_API const char *neverd_web_electron_ipc_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);

/// Compare preload/renderer path candidates with exact captured members using
/// the IPC source-selection contract. Cached manifest and source evidence are
/// required; IPC analysis is not. This does not analyze HTML or run code.
NEVERD_API const char *neverd_web_electron_entries_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ManifestArtifactID, size_t ArtifactIDSize,
    const char *OptionsJSON, size_t OptionsSize);
/// RecordKind is sources or entries. Limit is 1..512. Paths remain private;
/// linked artifacts and selected target sources are unverified candidates.
NEVERD_API const char *neverd_web_electron_entry_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);

/// Inspect UTF-8 HTML source-visible script candidates in captured bytes.
/// Independent of the JS parser. No DOM construction, execution or fetching.
/// Inline IDs may be passed to source_analyze with the reported source_type;
/// their anchors retain the original HTML byte range and container origin.
NEVERD_API const char *
neverd_web_html_analyze_json(neverd_web_session_t Session,
                             const char *ExpectedRevision, size_t RevisionSize,
                             const char *ArtifactID, size_t ArtifactIDSize);
/// RecordKind is scripts or bases. Limit is 1..512. No paths, URLs, raw
/// attributes or body text are emitted. Import replacement invalidates IDs.
NEVERD_API const char *
neverd_web_html_records_json(neverd_web_session_t Session,
                             const char *ExpectedRevision, size_t RevisionSize,
                             const char *HTMLID, size_t HTMLIDSize,
                             const char *RecordKind, size_t RecordKindSize,
                             uint64_t Offset, uint64_t Limit);

/// Explicitly load an original artifact, extracted Bun asset, or available
/// ASAR member into a NEW
/// independent native session. Select by ID, never host path. OutSession is
/// required, is cleared on entry, and receives ownership only with a non-null
/// success response. Destroy it with neverd_session_destroy(). The caller must
/// dispose any prior handle before reusing its output variable.
/// The snapshot is hash-checked; no target execution, debug/sidecar discovery,
/// implicit full analysis or external helper occurs. Universal Mach-O needs a
/// selected slice and is currently refused. Native loader resources are
/// separate from JS budgets. Snapshot-backed sessions have no path:
/// path-dependent persistence/patching fails. Ordinary native APIs may expose
/// original names; the JSON below remains metadata-only. No concurrent native
/// calls are allowed.
NEVERD_API const char *
neverd_web_native_open_json(neverd_web_session_t Session,
                            const char *ExpectedRevision, size_t RevisionSize,
                            const char *SelectionID, size_t SelectionIDSize,
                            neverd_session_t *OutSession);
/// Query a handoff's captured provenance and native counts. It remains usable
/// after the originating web session is replaced or destroyed. A successful
/// ordinary native file reload clears this handoff identity.
NEVERD_API const char *neverd_web_native_metadata_json(neverd_session_t Native);
/// Explicitly run the existing static native pipeline, returning metadata only.
/// Pipeline success does not establish whole-app semantics or source recovery.
NEVERD_API const char *neverd_web_native_analyze_json(neverd_session_t Native);

/// Path/options are borrowed byte buffers for this synchronous call. A path
/// may not contain NUL. Empty options select the reported default limits;
/// nonempty options require schema_version:1. The preview contains only
/// metadata and does not replace the published project. No input is executed.
NEVERD_API const char *
neverd_web_import_preview_json(neverd_web_session_t Session, const char *Path,
                               size_t PathSize, const char *OptionsJSON,
                               size_t OptionsSize);

/// Consumes a preview token and verifies the input still has the previewed
/// bytes/tree. Failure preserves the previously published revision.
NEVERD_API const char *
neverd_web_import_commit_json(neverd_web_session_t Session, const char *Token,
                              size_t TokenSize);

NEVERD_API const char *neverd_web_metadata_json(neverd_web_session_t Session);

/// Bounded ASAR inspection with an explicitly selected captured directory.
/// Both IDs belong to this revision. Empty UnpackedDirectoryID reports absent
/// companions; it never reads archive-path + ".unpacked" from the host.
/// Member IDs with availability="available" can be used with source/map/native
/// operations. Names and link targets remain private. No link is followed.
NEVERD_API const char *neverd_web_asar_extract_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *UnpackedDirectoryID, size_t UnpackedDirectoryIDSize);
NEVERD_API const char *
neverd_web_asar_records_json(neverd_web_session_t Session,
                             const char *ExpectedRevision, size_t RevisionSize,
                             const char *ExtractionID, size_t ExtractionIDSize,
                             uint64_t Offset, uint64_t Limit);

/// Extract the explicitly qualified Bun ELF layout from one admitted original.
/// No execution or external tools; version/producer authentication is not
/// implied. Source maps and JSC caches retain their original byte ranges.
NEVERD_API const char *neverd_web_bun_extract_json(neverd_web_session_t Session,
                                                   const char *ExpectedRevision,
                                                   size_t RevisionSize,
                                                   const char *ArtifactID,
                                                   size_t ArtifactIDSize);
/// RecordKind is modules/regions; Limit is 1..512. A module's derived
/// source_artifact_id can be passed to source_analyze for a bounded UTF-8
/// projection. Original byte ranges and hashes retain separate identities.
/// source_map_region_id can be passed to source_map_analyze; availability of
/// the bounded, in-process Zstd decoder is reported by capabilities.
NEVERD_API const char *
neverd_web_bun_records_json(neverd_web_session_t Session,
                            const char *ExpectedRevision, size_t RevisionSize,
                            const char *ExtractionID, size_t ExtractionIDSize,
                            const char *RecordKind, size_t RecordKindSize,
                            uint64_t Offset, uint64_t Limit);

/// Every page binds to an exact published revision, including the first page.
/// Limit is 1..512. page_complete is unrelated to source-analysis coverage.
NEVERD_API const char *neverd_web_artifacts_json(neverd_web_session_t Session,
                                                 const char *ExpectedRevision,
                                                 size_t RevisionSize,
                                                 uint64_t Offset,
                                                 uint64_t Limit);

/// Parse an admitted artifact with an explicit script/module/commonjs profile.
/// Returns a source identity and bounded diagnostics; source contents and names
/// are never included. Results are cached within the published revision.
/// Parse acceptance is not a claim of complete semantic validation.
NEVERD_API const char *neverd_web_source_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *SourceType, size_t SourceTypeSize);

/// Enumerates syntax kinds, parent identities and original half-open byte
/// spans for a previously analyzed source. Limit is 1..512.
NEVERD_API const char *
neverd_web_source_nodes_json(neverd_web_session_t Session,
                             const char *ExpectedRevision, size_t RevisionSize,
                             const char *SourceID, size_t SourceIDSize,
                             uint64_t Offset, uint64_t Limit);

/// Analyze lexical declaration/reference identities for an already parsed
/// source. Results describe static scope families, not values, initialization
/// or runtime intrinsics. Dynamic/unsupported cases are explicit.
NEVERD_API const char *neverd_web_source_bindings_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// RecordKind is scopes/bindings/declarations/references; Limit is 1..512.
/// Identifier names remain private. Pages contain only identity links,
/// analyzer-owned categories and flags.
NEVERD_API const char *neverd_web_source_binding_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);

/// Analyze bounded literal primitives and conservative immediate/deferred
/// effects. Also admits the source's lexical binding analysis. No target
/// execution, runtime binding propagation or source rewriting occurs.
NEVERD_API const char *neverd_web_source_semantics_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// Limit is 1..512. Only value status/kind/reasons and effect categories are
/// returned. Actual strings, numbers, booleans and BigInts remain private.
NEVERD_API const char *neverd_web_source_semantic_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    uint64_t Offset, uint64_t Limit);

/// Inventory static ESM entries and dynamic import/require candidates, then
/// compare relative specifiers with admitted members under the reported
/// profile. Derived HTML inline sources retain document/script/base context and
/// support literal dynamic-import file candidates; ordinary files keep their
/// own profile. Import-map declarations remain explicit unresolved boundaries.
/// File candidates and CommonJS binding identities never verify runtime loads.
NEVERD_API const char *neverd_web_source_modules_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// RecordKind is requests/imports/exports/attributes; Limit is 1..512.
/// Specifiers, names, assertion values and paths remain private.
NEVERD_API const char *neverd_web_source_module_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);

/// Recover original factory partitions for an explicit qualified webpack
/// loader/table shape. Preserves spans/hashes and uncertain dependency links;
/// never authenticates a producer, reconstructs filenames or executes code.
NEVERD_API const char *neverd_web_source_bundles_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// RecordKind is bundles/modules/dependencies/regions; Limit is 1..512.
/// Module keys, names and source text remain private.
NEVERD_API const char *neverd_web_source_bundle_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);

/// Index function/call/reference source containment. Binding and initializer
/// links are source evidence only, never proven runtime call targets.
NEVERD_API const char *neverd_web_source_navigation_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize);
/// RecordKind is functions/calls/references; Limit is 1..512. Text stays
/// private.
NEVERD_API const char *neverd_web_source_navigation_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit);
/// Map a UTF-8 source span to line/UTF-16 coordinates and original storage.
/// Bun transcoding preserves scalar boundaries; compressed source content
/// identifies its containing frame, never fabricated per-character offsets.
/// Optional ViewID must name a committed view for this source. Hidden interiors
/// map to whole display regions, including zero-width source positions.
NEVERD_API const char *
neverd_web_source_anchor_json(neverd_web_session_t Session,
                              const char *ExpectedRevision, size_t RevisionSize,
                              const char *SourceID, size_t SourceIDSize,
                              uint64_t ByteOffset, uint64_t ByteLength,
                              const char *ViewID, size_t ViewIDSize);

/// Preview a display projection, returning metadata only. Options are empty
/// for the default structural policy, or schema_version:1 with reviewed_ranges
/// (canonical decimal-string byte_offset/byte_length) and
/// locally_reviewed:true. The latter is a local caller assertion, never proof
/// of remote permission. Replaces any pending preview. No target code is
/// evaluated or rewritten.
NEVERD_API const char *neverd_web_source_view_preview_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *OptionsJSON, size_t OptionsSize);
/// Consumes the current preview on every current-revision attempt. Publishing
/// a new policy for a source revokes its previous view. At most two sources'
/// views are published in a session; import commit clears all projections.
NEVERD_API const char *neverd_web_source_view_commit_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *PreviewToken, size_t PreviewTokenSize);
/// Metadata-only original/projected range mapping; accepts pending previews.
/// Limit is 1..512. Replacement regions do not claim bytewise equivalence.
NEVERD_API const char *neverd_web_source_view_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ViewID, size_t ViewIDSize, uint64_t Offset,
    uint64_t Limit);
/// Text is available only for a committed view. ByteLimit is 1..65536;
/// UTF-8 and CRLF boundaries are preserved. Text remains untrusted source data.
NEVERD_API const char *neverd_web_source_view_chunk_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ViewID, size_t ViewIDSize,
    uint64_t ByteOffset, uint64_t ByteLimit);

/// Decode an admitted basic or embedded-section map. Format validity is
/// independent of association to generated code; no URL/path is resolved.
/// Also accepts source_map_region_id from an extracted Bun module. Its fixed
/// binary profile retains only mapped anchors (the producer discarded names
/// and unmapped boundaries); raw and compressed storage ranges stay distinct.
NEVERD_API const char *neverd_web_source_map_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize);
/// Embedded sources have private derived artifact IDs that source_analyze
/// can inspect. Missing content is never fetched.
NEVERD_API const char *neverd_web_source_map_sources_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize, uint64_t Offset,
    uint64_t Limit);
NEVERD_API const char *neverd_web_source_map_segments_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize, uint64_t Offset,
    uint64_t Limit);
/// Association is a caller assertion, or an unverified container assertion
/// when a Bun map names this generated artifact. Only anchor boundaries are
/// checked; no verified provenance, interpolation or whole-map validity claim.
NEVERD_API const char *neverd_web_source_map_lookup_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize,
    const char *GeneratedSourceID, size_t GeneratedSourceIDSize,
    uint64_t ByteOffset);

#ifdef __cplusplus
}
#endif
#endif
