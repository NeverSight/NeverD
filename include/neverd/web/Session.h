//===- Session.h - Offline analysis session ----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline analysis session.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Error.h"
#include "neverd/web/NativeInput.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace neverd::web {

class Session {
public:
  Session();
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;

  static std::string capabilities();
  /// Prepare immutable evidence, without replacing the published snapshot.
  std::string preview(std::string_view Path, std::string_view Options);
  /// Revalidate the selected input before atomically publishing the preview.
  std::string commit(std::string_view Token);
  std::string metadata() const;
  std::string previewStream(std::string_view ExpectedRevision,
                            std::string_view ArtifactID,
                            std::string_view Profile);
  std::string commitStream(std::string_view ExpectedRevision,
                           std::string_view PreviewToken);
  std::string streamRecords(std::string_view ExpectedRevision,
                            std::string_view CaptureID, uint64_t Offset,
                            uint64_t Limit) const;
  std::string previewHAR(std::string_view ExpectedRevision,
                         std::string_view ArtifactID);
  std::string commitHAR(std::string_view ExpectedRevision,
                        std::string_view PreviewToken);
  std::string harRecords(std::string_view ExpectedRevision,
                         std::string_view CaptureID, uint64_t Offset,
                         uint64_t Limit) const;
  std::string analyzeInterfaces(std::string_view ExpectedRevision,
                                std::string_view SourceID);
  std::string interfaceRecords(std::string_view ExpectedRevision,
                               std::string_view AnalysisID, uint64_t Offset,
                               uint64_t Limit) const;
  std::string compareInterfaces(std::string_view ExpectedRevision,
                                std::string_view AnalysisID,
                                std::string_view CaptureID);
  std::string interfaceCorrelationRecords(std::string_view ExpectedRevision,
                                          std::string_view CorrelationID,
                                          uint64_t Offset,
                                          uint64_t Limit) const;
  std::string extractPackageArchive(std::string_view ExpectedRevision,
                                    std::string_view ArtifactID,
                                    std::string_view Format);
  std::string verifyPackageIntegrity(std::string_view ExpectedRevision,
                                     std::string_view ArtifactID,
                                     std::string_view DeclarationID,
                                     std::string_view PackageID);
  std::string packageArchiveRecords(std::string_view ExpectedRevision,
                                    std::string_view ArchiveID, uint64_t Offset,
                                    uint64_t Limit) const;
  std::string analyzePackages(std::string_view ExpectedRevision,
                              std::string_view ArtifactID,
                              std::string_view InputKind);
  std::string packageRecords(std::string_view ExpectedRevision,
                             std::string_view AnalysisID,
                             std::string_view RecordKind, uint64_t Offset,
                             uint64_t Limit) const;
  /// Both analyses belong to this published revision. A new import revokes
  /// both sides and their comparisons; no stale ID is rebound to new bytes.
  std::string comparePackages(std::string_view ExpectedRevision,
                              std::string_view BeforeID,
                              std::string_view AfterID);
  std::string packageDiffRecords(std::string_view ExpectedRevision,
                                 std::string_view DiffID, uint64_t Offset,
                                 uint64_t Limit) const;
  std::string analyzeHTML(std::string_view ExpectedRevision,
                          std::string_view ArtifactID);
  std::string htmlRecords(std::string_view ExpectedRevision,
                          std::string_view HTMLID, std::string_view RecordKind,
                          uint64_t Offset, uint64_t Limit) const;
  std::string analyzeElectronManifest(std::string_view ExpectedRevision,
                                      std::string_view ArtifactID);
  std::string analyzeElectronSource(std::string_view ExpectedRevision,
                                    std::string_view SourceID);
  std::string electronSourceRecords(std::string_view ExpectedRevision,
                                    std::string_view SourceID, uint64_t Offset,
                                    uint64_t Limit) const;
  std::string analyzeElectronIPC(std::string_view ExpectedRevision,
                                 std::string_view ManifestArtifactID,
                                 std::string_view Options);
  std::string electronIPCRecords(std::string_view ExpectedRevision,
                                 std::string_view AnalysisID,
                                 std::string_view RecordKind, uint64_t Offset,
                                 uint64_t Limit) const;
  std::string analyzeElectronEntries(std::string_view ExpectedRevision,
                                     std::string_view ManifestArtifactID,
                                     std::string_view Options);
  std::string electronEntryRecords(std::string_view ExpectedRevision,
                                   std::string_view AnalysisID,
                                   std::string_view RecordKind, uint64_t Offset,
                                   uint64_t Limit) const;
  std::string extractAsar(std::string_view ExpectedRevision,
                          std::string_view ArtifactID,
                          std::string_view UnpackedDirectoryID = {});
  std::string asarRecords(std::string_view ExpectedRevision,
                          std::string_view ExtractionID, uint64_t Offset,
                          uint64_t Limit) const;
  NativeInput nativeInput(std::string_view ExpectedRevision,
                          std::string_view SelectionID) const;
  std::string extractBun(std::string_view ExpectedRevision,
                         std::string_view ArtifactID);
  std::string extractSEA(std::string_view ExpectedRevision,
                         std::string_view ArtifactID, std::string_view Profile);
  std::string seaRecords(std::string_view ExpectedRevision,
                         std::string_view ExtractionID, uint64_t Offset,
                         uint64_t Limit) const;
  /// Explicit local raw disclosure into a new directory. Fixed output names;
  /// captured names are manifest data only. Never overwrites or executes.
  std::string exportBun(std::string_view ExpectedRevision,
                        std::string_view ExtractionID,
                        std::string_view OutputDirectory);
  std::string bunRecords(std::string_view ExpectedRevision,
                         std::string_view ExtractionID,
                         std::string_view RecordKind, uint64_t Offset,
                         uint64_t Limit) const;
  /// Continuations must bind to the revision received with the first page.
  std::string artifacts(std::string_view ExpectedRevision, uint64_t Offset,
                        uint64_t Limit) const;
  /// Analyzes immutable bytes already admitted into this session. Syntax
  /// inventory does not imply whole-program semantic validity or execution.
  std::string analyzeSource(std::string_view ExpectedRevision,
                            std::string_view ArtifactID,
                            std::string_view SourceType);
  std::string sourceNodes(std::string_view ExpectedRevision,
                          std::string_view SourceID, uint64_t Offset,
                          uint64_t Limit) const;
  std::string analyzeSourceNavigation(std::string_view ExpectedRevision,
                                      std::string_view SourceID);
  std::string sourceNavigationRecords(std::string_view ExpectedRevision,
                                      std::string_view SourceID,
                                      std::string_view RecordKind,
                                      uint64_t Offset, uint64_t Limit) const;
  /// Source UTF-8 coordinates, original storage precision and optional display
  /// projection. A supplied view must be committed for the same source.
  std::string sourceAnchor(std::string_view ExpectedRevision,
                           std::string_view SourceID, uint64_t Offset,
                           uint64_t Length, std::string_view ViewID) const;
  /// Metadata-only review followed by explicit publication of a source view.
  /// Nonempty reviewed ranges assert local caller review, not remote consent.
  std::string previewSourceView(std::string_view ExpectedRevision,
                                std::string_view SourceID,
                                std::string_view Options);
  std::string commitSourceView(std::string_view ExpectedRevision,
                               std::string_view PreviewToken);
  /// Range metadata can be inspected before publication; text cannot.
  std::string sourceViewRecords(std::string_view ExpectedRevision,
                                std::string_view ViewID, uint64_t Offset,
                                uint64_t Limit) const;
  std::string sourceViewChunk(std::string_view ExpectedRevision,
                              std::string_view ViewID, uint64_t ByteOffset,
                              uint64_t ByteLimit) const;
  std::string analyzeSourceMap(std::string_view ExpectedRevision,
                               std::string_view ArtifactID);
  std::string analyzeSourceBindings(std::string_view ExpectedRevision,
                                    std::string_view SourceID);
  std::string sourceBindingRecords(std::string_view ExpectedRevision,
                                   std::string_view SourceID,
                                   std::string_view RecordKind, uint64_t Offset,
                                   uint64_t Limit) const;
  std::string analyzeSourceSemantics(std::string_view ExpectedRevision,
                                     std::string_view SourceID);
  std::string sourceSemanticRecords(std::string_view ExpectedRevision,
                                    std::string_view SourceID, uint64_t Offset,
                                    uint64_t Limit) const;
  std::string analyzeSourceModules(std::string_view ExpectedRevision,
                                   std::string_view SourceID);
  std::string sourceModuleRecords(std::string_view ExpectedRevision,
                                  std::string_view SourceID,
                                  std::string_view RecordKind, uint64_t Offset,
                                  uint64_t Limit) const;
  std::string analyzeSourceBundles(std::string_view ExpectedRevision,
                                   std::string_view SourceID);
  std::string sourceBundleRecords(std::string_view ExpectedRevision,
                                  std::string_view SourceID,
                                  std::string_view RecordKind, uint64_t Offset,
                                  uint64_t Limit) const;
  std::string sourceMapSources(std::string_view ExpectedRevision,
                               std::string_view MapID, uint64_t Offset,
                               uint64_t Limit) const;
  std::string sourceMapSegments(std::string_view ExpectedRevision,
                                std::string_view MapID, uint64_t Offset,
                                uint64_t Limit) const;
  std::string lookupSourceMap(std::string_view ExpectedRevision,
                              std::string_view MapID,
                              std::string_view GeneratedSourceID,
                              uint64_t ByteOffset) const;

private:
  struct Impl;
  std::unique_ptr<Impl> State;
};

std::string failure(std::string_view Code);

} // namespace neverd::web
