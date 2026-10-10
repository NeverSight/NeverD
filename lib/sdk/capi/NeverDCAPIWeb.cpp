//===- NeverDCAPIWeb.cpp - Offline web analysis C interface ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Offline web analysis C interface.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPIWeb.h"

#ifdef NEVERD_ENABLE_WEB_ANALYSIS
#include "NativeSnapshotSession.h"
#include "SessionImpl.h"

#include "neverd/loader/ObjectFileUtils.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/web/Session.h"
#endif

#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

namespace {
const char *owned(std::string_view Text) noexcept {
  auto *Result = static_cast<char *>(std::malloc(Text.size() + 1));
  if (Result) {
    std::memcpy(Result, Text.data(), Text.size());
    Result[Text.size()] = 0;
  }
  return Result;
}

const char *unavailable() noexcept {
  return owned(
      R"({"schema_version":1,"status":"error","error":{"code":"capability_unavailable"}})");
}

#ifdef NEVERD_ENABLE_WEB_ANALYSIS
template <typename F> const char *guarded(F Call) {
  try {
    return owned(Call());
  } catch (const neverd::web::Error &Error) {
    try {
      return owned(neverd::web::failure(Error.what()));
    } catch (...) {
      return nullptr;
    }
  } catch (...) {
    return owned(
        R"({"schema_version":1,"status":"error","error":{"code":"internal_error"}})");
  }
}

template <typename F> const char *invoke(neverd_web_session_t Handle, F Call) {
  return guarded([&] {
    if (!Handle)
      throw neverd::web::Error("invalid_session");
    return Call(*static_cast<neverd::web::Session *>(Handle));
  });
}

std::string_view buffer(const char *Data, size_t Size, size_t Limit) {
  if ((!Data && Size) || Size > Limit)
    throw neverd::web::Error("invalid_buffer");
  return Size ? std::string_view(Data, Size) : std::string_view();
}

neverd::sdk::Session &nativeSession(neverd_session_t Handle) {
  if (!Handle)
    throw neverd::web::Error("invalid_session");
  auto &S = *neverd::sdk::toSession(Handle);
  if (!S.Loaded || !S.MemoryInputBytes || S.WebNativeProvenance.empty())
    throw neverd::web::Error("native_handoff_required");
  return S;
}

std::string nativeReport(neverd::sdk::Session &S) {
  auto Value = llvm::json::parse(S.WebNativeProvenance);
  if (!Value || !Value->getAsObject()) {
    if (!Value)
      llvm::consumeError(Value.takeError());
    throw neverd::web::Error("invalid_native_provenance");
  }
  S.synchronizeFunctions();
  auto &Report = *Value->getAsObject();
  Report["format"] = S.Img.getFormatName();
  Report["architecture"] = neverd::getArchName(S.Img.Arch);
  Report["instruction_mode"] = neverd::getInstructionModeName(S.Img.Mode);
  Report["bits"] = S.Img.is64Bit() ? 64 : 32;
  Report["platform"] = "not_inferred";
  Report["base_address"] = std::to_string(S.Img.Base);
  Report["entry_address"] = std::to_string(S.Img.Entry);
  Report["segment_count"] = S.Img.Segments.size();
  Report["section_count"] = S.Img.Sections.size();
  Report["import_count"] = S.Img.Imports.size();
  Report["export_count"] = S.Img.Exports.size();
  Report["symbol_count"] = S.Img.Symbols.size();
  Report["function_count"] = S.Functions.size();
  Report["pipeline_status"] = !S.PipeRan             ? "not_run"
                              : S.PipeResult.Success ? "succeeded"
                                                     : "failed";
  Report["analysis_scope"] = "selected_native_image";
  Report["original_source_recovered"] = false;
  return neverd::sdk::jsonToString(*Value);
}
#endif
} // namespace

neverd_web_session_t neverd_web_session_create(void) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  try {
    return new neverd::web::Session();
  } catch (...) {
    return nullptr;
  }
#else
  return nullptr;
#endif
}

void neverd_web_session_destroy(neverd_web_session_t Session) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  delete static_cast<neverd::web::Session *>(Session);
#endif
}

const char *neverd_web_capabilities_json(void) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  try {
    return owned(neverd::web::Session::capabilities());
  } catch (...) {
    return nullptr;
  }
#else
  return unavailable();
#endif
}

const char *neverd_web_native_open_json(neverd_web_session_t Session,
                                        const char *ExpectedRevision,
                                        size_t RevisionSize,
                                        const char *SelectionID,
                                        size_t SelectionIDSize,
                                        neverd_session_t *OutSession) {
  if (OutSession)
    *OutSession = nullptr;
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  std::unique_ptr<neverd::sdk::Session> Native;
  const auto *Reply = invoke(Session, [&](auto &Web) {
    if (!OutSession)
      throw neverd::web::Error("invalid_output");
    auto Input = Web.nativeInput(buffer(ExpectedRevision, RevisionSize, 20),
                                 buffer(SelectionID, SelectionIDSize, 64));
    auto Bytes = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(
        Input.Content.size(), "<web-native>");
    if (!Bytes)
      throw neverd::web::Error("native_allocation_failed");
    for (uint64_t At = 0; At < Input.Content.size();) {
      const auto Part =
          Input.Content.read(At, std::min(neverd::web::BlobTransferBytes,
                                          Input.Content.size() - At));
      std::memcpy(Bytes->getBufferStart() + At, Part.data(), Part.size());
      At += Part.size();
    }
    // A PE signature can follow a long DOS stub. Classify the complete admitted
    // buffer, exactly as file loading does, rather than a guessed header
    // window.
    const auto Magic =
        llvm::identify_magic(Bytes->getMemBufferRef().getBuffer());
    if (Magic == llvm::file_magic::macho_universal_binary)
      throw neverd::web::Error("native_slice_selection_required");
    if (neverd::magicToFormat(Magic) == neverd::BinaryFormat::Unknown)
      throw neverd::web::Error("unsupported_native_format");
    auto Image = neverd::loadBinaryBuffer(Bytes->getMemBufferRef());
    if (!Image) {
      llvm::consumeError(Image.takeError());
      throw neverd::web::Error("native_load_failed");
    }
    if (Image->Arch != neverd::Arch::X86 && Image->Arch != neverd::Arch::X64 &&
        Image->Arch != neverd::Arch::ARM &&
        Image->Arch != neverd::Arch::AArch64)
      throw neverd::web::Error("unsupported_native_architecture");
    if (!Image->InputFileSHA256 ||
        llvm::toHex(*Image->InputFileSHA256, true) != Input.BlobHash)
      throw neverd::web::Error("native_input_hash_mismatch");
    auto Candidate = std::make_unique<neverd::sdk::Session>();
    if (!neverd::sdk::loadNativeSnapshotSession(
            Candidate.get(), std::move(*Image), Input.Content.size(),
            std::move(Input.Provenance)))
      throw neverd::web::Error("native_decoder_unavailable");
    auto Result = nativeReport(*Candidate);
    Native = std::move(Candidate);
    return Result;
  });
  if (Reply && Native)
    *OutSession = Native.release();
  return Reply;
#else
  return unavailable();
#endif
}

const char *neverd_web_native_metadata_json(neverd_session_t Native) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return guarded([&] { return nativeReport(nativeSession(Native)); });
#else
  return unavailable();
#endif
}

const char *neverd_web_native_analyze_json(neverd_session_t Native) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return guarded([&] {
    auto &S = nativeSession(Native);
    S.clearError();
    if (!S.ensurePipeline())
      throw neverd::web::Error("native_analysis_failed");
    return nativeReport(S);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_import_preview_json(neverd_web_session_t Session,
                                           const char *Path, size_t PathSize,
                                           const char *OptionsJSON,
                                           size_t OptionsSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.preview(buffer(Path, PathSize, 32768),
                     buffer(OptionsJSON, OptionsSize, 4096));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_import_commit_json(neverd_web_session_t Session,
                                          const char *Token, size_t TokenSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(
      Session, [&](auto &S) { return S.commit(buffer(Token, TokenSize, 64)); });
#else
  return unavailable();
#endif
}

const char *neverd_web_metadata_json(neverd_web_session_t Session) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [](auto &S) { return S.metadata(); });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_manifest_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeElectronManifest(buffer(ExpectedRevision, RevisionSize, 20),
                                     buffer(ArtifactID, ArtifactIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_packages_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *InputKind, size_t InputKindSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzePackages(buffer(ExpectedRevision, RevisionSize, 20),
                             buffer(ArtifactID, ArtifactIDSize, 64),
                             buffer(InputKind, InputKindSize, 32));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_har_preview_json(neverd_web_session_t Session,
                                        const char *ExpectedRevision,
                                        size_t RevisionSize,
                                        const char *ArtifactID,
                                        size_t ArtifactIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.previewHAR(buffer(ExpectedRevision, RevisionSize, 20),
                        buffer(ArtifactID, ArtifactIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_har_commit_json(neverd_web_session_t Session,
                                       const char *ExpectedRevision,
                                       size_t RevisionSize,
                                       const char *PreviewToken,
                                       size_t PreviewTokenSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.commitHAR(buffer(ExpectedRevision, RevisionSize, 20),
                       buffer(PreviewToken, PreviewTokenSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_har_records_json(neverd_web_session_t Session,
                                        const char *ExpectedRevision,
                                        size_t RevisionSize,
                                        const char *CaptureID,
                                        size_t CaptureIDSize, uint64_t Offset,
                                        uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.harRecords(buffer(ExpectedRevision, RevisionSize, 20),
                        buffer(CaptureID, CaptureIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_interfaces_analyze_json(neverd_web_session_t Session,
                                               const char *ExpectedRevision,
                                               size_t RevisionSize,
                                               const char *SourceID,
                                               size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeInterfaces(buffer(ExpectedRevision, RevisionSize, 20),
                               buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_interface_records_json(neverd_web_session_t Session,
                                              const char *ExpectedRevision,
                                              size_t RevisionSize,
                                              const char *AnalysisID,
                                              size_t AnalysisIDSize,
                                              uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.interfaceRecords(buffer(ExpectedRevision, RevisionSize, 20),
                              buffer(AnalysisID, AnalysisIDSize, 64), Offset,
                              Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_interfaces_compare_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *CaptureID, size_t CaptureIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.compareInterfaces(buffer(ExpectedRevision, RevisionSize, 20),
                               buffer(AnalysisID, AnalysisIDSize, 64),
                               buffer(CaptureID, CaptureIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_interface_correlation_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *CorrelationID, size_t CorrelationIDSize,
    uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.interfaceCorrelationRecords(
        buffer(ExpectedRevision, RevisionSize, 20),
        buffer(CorrelationID, CorrelationIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_package_integrity_verify_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *DeclarationID, size_t DeclarationIDSize, const char *PackageID,
    size_t PackageIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.verifyPackageIntegrity(
        buffer(ExpectedRevision, RevisionSize, 20),
        buffer(ArtifactID, ArtifactIDSize, 64),
        buffer(DeclarationID, DeclarationIDSize, 64),
        buffer(PackageID, PackageIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_package_archive_extract_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *Format, size_t FormatSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.extractPackageArchive(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(ArtifactID, ArtifactIDSize, 64),
                                   buffer(Format, FormatSize, 16));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_package_archive_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArchiveID, size_t ArchiveIDSize,
    uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.packageArchiveRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(ArchiveID, ArchiveIDSize, 64), Offset,
                                   Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_package_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.packageRecords(buffer(ExpectedRevision, RevisionSize, 20),
                            buffer(AnalysisID, AnalysisIDSize, 64),
                            buffer(RecordKind, RecordKindSize, 32), Offset,
                            Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_packages_compare_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *BeforeID, size_t BeforeIDSize,
    const char *AfterID, size_t AfterIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.comparePackages(buffer(ExpectedRevision, RevisionSize, 20),
                             buffer(BeforeID, BeforeIDSize, 64),
                             buffer(AfterID, AfterIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_package_diff_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *DiffID, size_t DiffIDSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.packageDiffRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                buffer(DiffID, DiffIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_source_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeElectronSource(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_source_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.electronSourceRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(SourceID, SourceIDSize, 64), Offset,
                                   Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_ipc_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *OptionsJSON, size_t OptionsSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeElectronIPC(buffer(ExpectedRevision, RevisionSize, 20),
                                buffer(ArtifactID, ArtifactIDSize, 64),
                                buffer(OptionsJSON, OptionsSize, 4096));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_ipc_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.electronIPCRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                buffer(AnalysisID, AnalysisIDSize, 64),
                                buffer(RecordKind, RecordKindSize, 16), Offset,
                                Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_entries_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *OptionsJSON, size_t OptionsSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeElectronEntries(buffer(ExpectedRevision, RevisionSize, 20),
                                    buffer(ArtifactID, ArtifactIDSize, 64),
                                    buffer(OptionsJSON, OptionsSize, 4096));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_electron_entry_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *AnalysisID, size_t AnalysisIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.electronEntryRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                  buffer(AnalysisID, AnalysisIDSize, 64),
                                  buffer(RecordKind, RecordKindSize, 16),
                                  Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_html_analyze_json(neverd_web_session_t Session,
                                         const char *ExpectedRevision,
                                         size_t RevisionSize,
                                         const char *ArtifactID,
                                         size_t ArtifactIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeHTML(buffer(ExpectedRevision, RevisionSize, 20),
                         buffer(ArtifactID, ArtifactIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_html_records_json(neverd_web_session_t Session,
                                         const char *ExpectedRevision,
                                         size_t RevisionSize,
                                         const char *HTMLID, size_t HTMLIDSize,
                                         const char *RecordKind,
                                         size_t RecordKindSize, uint64_t Offset,
                                         uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.htmlRecords(buffer(ExpectedRevision, RevisionSize, 20),
                         buffer(HTMLID, HTMLIDSize, 64),
                         buffer(RecordKind, RecordKindSize, 16), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *
neverd_web_asar_extract_json(neverd_web_session_t Session,
                             const char *ExpectedRevision, size_t RevisionSize,
                             const char *ArtifactID, size_t ArtifactIDSize,
                             const char *UnpackedID, size_t UnpackedSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.extractAsar(buffer(ExpectedRevision, RevisionSize, 20),
                         buffer(ArtifactID, ArtifactIDSize, 64),
                         buffer(UnpackedID, UnpackedSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_asar_records_json(neverd_web_session_t Session,
                                         const char *ExpectedRevision,
                                         size_t RevisionSize,
                                         const char *ExtractionID,
                                         size_t ExtractionIDSize,
                                         uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.asarRecords(buffer(ExpectedRevision, RevisionSize, 20),
                         buffer(ExtractionID, ExtractionIDSize, 64), Offset,
                         Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_bun_export_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ExtractionID, size_t ExtractionIDSize,
    const char *OutputDirectory, size_t OutputDirectorySize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.exportBun(buffer(ExpectedRevision, RevisionSize, 20),
                       buffer(ExtractionID, ExtractionIDSize, 64),
                       buffer(OutputDirectory, OutputDirectorySize, 32768));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_bun_extract_json(neverd_web_session_t Session,
                                        const char *ExpectedRevision,
                                        size_t RevisionSize,
                                        const char *ArtifactID,
                                        size_t ArtifactIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.extractBun(buffer(ExpectedRevision, RevisionSize, 20),
                        buffer(ArtifactID, ArtifactIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *
neverd_web_bun_records_json(neverd_web_session_t Session,
                            const char *ExpectedRevision, size_t RevisionSize,
                            const char *ExtractionID, size_t ExtractionIDSize,
                            const char *RecordKind, size_t RecordKindSize,
                            uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.bunRecords(buffer(ExpectedRevision, RevisionSize, 20),
                        buffer(ExtractionID, ExtractionIDSize, 64),
                        buffer(RecordKind, RecordKindSize, 16), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_artifacts_json(neverd_web_session_t Session,
                                      const char *ExpectedRevision,
                                      size_t RevisionSize, uint64_t Offset,
                                      uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.artifacts(buffer(ExpectedRevision, RevisionSize, 20), Offset,
                       Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ArtifactID, size_t ArtifactIDSize,
    const char *SourceType, size_t SourceTypeSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSource(buffer(ExpectedRevision, RevisionSize, 20),
                           buffer(ArtifactID, ArtifactIDSize, 64),
                           buffer(SourceType, SourceTypeSize, 16));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_bindings_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceBindings(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_binding_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceBindingRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                  buffer(SourceID, SourceIDSize, 64),
                                  buffer(RecordKind, RecordKindSize, 16),
                                  Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_modules_analyze_json(neverd_web_session_t Session,
                                                   const char *ExpectedRevision,
                                                   size_t RevisionSize,
                                                   const char *SourceID,
                                                   size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceModules(buffer(ExpectedRevision, RevisionSize, 20),
                                  buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_module_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceModuleRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                 buffer(SourceID, SourceIDSize, 64),
                                 buffer(RecordKind, RecordKindSize, 16), Offset,
                                 Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_bundles_analyze_json(neverd_web_session_t Session,
                                                   const char *ExpectedRevision,
                                                   size_t RevisionSize,
                                                   const char *SourceID,
                                                   size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceBundles(buffer(ExpectedRevision, RevisionSize, 20),
                                  buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_bundle_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceBundleRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                 buffer(SourceID, SourceIDSize, 64),
                                 buffer(RecordKind, RecordKindSize, 16), Offset,
                                 Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_semantics_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceSemantics(buffer(ExpectedRevision, RevisionSize, 20),
                                    buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_semantic_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    uint64_t Offset, uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceSemanticRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                   buffer(SourceID, SourceIDSize, 64), Offset,
                                   Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_map_analyze_json(neverd_web_session_t Session,
                                               const char *ExpectedRevision,
                                               size_t RevisionSize,
                                               const char *ArtifactID,
                                               size_t ArtifactIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceMap(buffer(ExpectedRevision, RevisionSize, 20),
                              buffer(ArtifactID, ArtifactIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_map_sources_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceMapSources(buffer(ExpectedRevision, RevisionSize, 20),
                              buffer(MapID, MapIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_map_segments_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceMapSegments(buffer(ExpectedRevision, RevisionSize, 20),
                               buffer(MapID, MapIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_map_lookup_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *MapID, size_t MapIDSize,
    const char *GeneratedSourceID, size_t GeneratedSourceIDSize,
    uint64_t ByteOffset) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.lookupSourceMap(
        buffer(ExpectedRevision, RevisionSize, 20),
        buffer(MapID, MapIDSize, 64),
        buffer(GeneratedSourceID, GeneratedSourceIDSize, 64), ByteOffset);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_nodes_json(neverd_web_session_t Session,
                                         const char *ExpectedRevision,
                                         size_t RevisionSize,
                                         const char *SourceID,
                                         size_t SourceIDSize, uint64_t Offset,
                                         uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceNodes(buffer(ExpectedRevision, RevisionSize, 20),
                         buffer(SourceID, SourceIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_navigation_analyze_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.analyzeSourceNavigation(buffer(ExpectedRevision, RevisionSize, 20),
                                     buffer(SourceID, SourceIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_navigation_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *RecordKind, size_t RecordKindSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceNavigationRecords(buffer(ExpectedRevision, RevisionSize, 20),
                                     buffer(SourceID, SourceIDSize, 64),
                                     buffer(RecordKind, RecordKindSize, 16),
                                     Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *
neverd_web_source_anchor_json(neverd_web_session_t Session,
                              const char *ExpectedRevision, size_t RevisionSize,
                              const char *SourceID, size_t SourceIDSize,
                              uint64_t ByteOffset, uint64_t ByteLength,
                              const char *ViewID, size_t ViewIDSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceAnchor(buffer(ExpectedRevision, RevisionSize, 20),
                          buffer(SourceID, SourceIDSize, 64), ByteOffset,
                          ByteLength, buffer(ViewID, ViewIDSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_view_preview_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *SourceID, size_t SourceIDSize,
    const char *OptionsJSON, size_t OptionsSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.previewSourceView(buffer(ExpectedRevision, RevisionSize, 20),
                               buffer(SourceID, SourceIDSize, 64),
                               buffer(OptionsJSON, OptionsSize, 16384));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_view_commit_json(neverd_web_session_t Session,
                                               const char *ExpectedRevision,
                                               size_t RevisionSize,
                                               const char *PreviewToken,
                                               size_t PreviewTokenSize) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.commitSourceView(buffer(ExpectedRevision, RevisionSize, 20),
                              buffer(PreviewToken, PreviewTokenSize, 64));
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_view_records_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ViewID, size_t ViewIDSize, uint64_t Offset,
    uint64_t Limit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceViewRecords(buffer(ExpectedRevision, RevisionSize, 20),
                               buffer(ViewID, ViewIDSize, 64), Offset, Limit);
  });
#else
  return unavailable();
#endif
}

const char *neverd_web_source_view_chunk_json(
    neverd_web_session_t Session, const char *ExpectedRevision,
    size_t RevisionSize, const char *ViewID, size_t ViewIDSize,
    uint64_t ByteOffset, uint64_t ByteLimit) {
#ifdef NEVERD_ENABLE_WEB_ANALYSIS
  return invoke(Session, [&](auto &S) {
    return S.sourceViewChunk(buffer(ExpectedRevision, RevisionSize, 20),
                             buffer(ViewID, ViewIDSize, 64), ByteOffset,
                             ByteLimit);
  });
#else
  return unavailable();
#endif
}
