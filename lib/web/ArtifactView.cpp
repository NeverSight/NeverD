//===- ArtifactView.cpp - Immutable occurrence selection ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Immutable occurrence selection.
///
//===----------------------------------------------------------------------===//

#include "ArtifactView.h"

#include "SessionInternal.h"

namespace neverd::web {
std::optional<Session::Impl::HTMLSourceSelection>
Session::Impl::htmlSource(std::string_view SelectionID) const {
  for (const auto &[ID, H] : HTMLDocuments)
    for (uint32_t I = 0; I < H.Document.Scripts.size(); ++I) {
      const auto &S = H.Document.Scripts[I];
      if (!S.InlineArtifactID.empty() && S.InlineArtifactID == SelectionID)
        return HTMLSourceSelection{&H, I};
    }
  return std::nullopt;
}

std::optional<ArtifactView>
Session::Impl::artifactView(std::string_view SelectionID) const {
  for (const auto &A : Published.Artifacts)
    if (A.ID == SelectionID) {
      if (A.Directory)
        throw Error("artifact_has_no_bytes");
      return ArtifactView{
          A.Content, A.BlobHash, 0,
          llvm::json::Object{{"kind", "original_artifact"},
                             {"artifact_id", A.ID},
                             {"storage_artifact_id", A.ID},
                             {"parent_artifact_id", A.ParentID}}};
    }
  for (const auto &[ID, E] : BunExtractions)
    for (const auto &R : E.Regions)
      if (R.ID == SelectionID && R.Kind == "asset") {
        auto Parent = artifactView(E.ArtifactID);
        if (!Parent)
          throw Error("bun_parent_bytes_unavailable");
        return ArtifactView{
            R.Content, R.BlobHash, Parent->StorageOffset + R.Offset,
            llvm::json::Object{
                {"kind", "bun_asset"},
                {"container_artifact_id", E.ArtifactID},
                {"storage_artifact_id",
                 Parent->Origin.getString("storage_artifact_id")
                     .value_or("")
                     .str()},
                {"extraction_id", E.ID},
                {"region_id", R.ID},
                {"module_id",
                 R.Module == NoBunIndex
                     ? llvm::json::Value(nullptr)
                     : llvm::json::Value(E.Modules.at(R.Module).ID)},
                {"container_byte_offset", std::to_string(R.Offset)},
                {"byte_offset_basis", Parent->DirectStorage
                                          ? "storage_artifact"
                                          : "expanded_stream"},
                {"byte_offset", Parent->DirectStorage
                                    ? llvm::json::Value(std::to_string(
                                          Parent->StorageOffset + R.Offset))
                                    : llvm::json::Value(nullptr)},
                {"expanded_byte_offset",
                 !Parent->DirectStorage ? llvm::json::Value(std::to_string(
                                              Parent->StorageOffset + R.Offset))
                                        : llvm::json::Value(nullptr)},
                {"byte_length", std::to_string(R.Content.size())},
                {"container_origin", std::move(Parent->Origin)}},
            Parent->DirectStorage};
      }
  for (const auto &[ID, E] : AsarExtractions)
    for (const auto &M : E.Members)
      if (M.ID == SelectionID) {
        if (!M.available())
          throw Error("artifact_bytes_unavailable");
        return ArtifactView{
            M.Content, M.BlobHash, M.Offset,
            llvm::json::Object{{"kind", M.Unpacked ? "asar_unpacked_member"
                                                   : "asar_packed_member"},
                               {"container_artifact_id", E.ArtifactID},
                               {"extraction_id", E.ID},
                               {"member_id", M.ID},
                               {"storage_artifact_id", M.StorageArtifactID},
                               {"unpacked_directory_id",
                                M.Unpacked
                                    ? llvm::json::Value(E.UnpackedDirectoryID)
                                    : llvm::json::Value(nullptr)},
                               {"byte_offset", std::to_string(M.Offset)},
                               {"byte_length", std::to_string(M.Size)},
                               {"integrity_status", M.IntegrityStatus},
                               {"authenticates_publisher", false}}};
      }
  for (const auto &[ID, E] : PackageArchives)
    for (uint64_t I = 0; I < E.Members.size(); ++I) {
      const auto &M = E.Members[I];
      if (M.ID != SelectionID)
        continue;
      if (!M.available())
        throw Error("artifact_bytes_unavailable");
      return ArtifactView{
          M.Content, M.BlobHash, M.Offset,
          llvm::json::Object{
              {"kind", "package_archive_member"},
              {"container_artifact_id", E.ArtifactID},
              {"storage_artifact_id", E.ArtifactID},
              {"byte_offset_basis",
               E.Format == "tar" ? "storage_artifact" : "expanded_stream"},
              {"container_sha256", E.BlobHash},
              {"archive_id", E.ID},
              {"member_id", M.ID},
              {"member_index", I},
              {"profile", std::string(PackageArchiveProfile)},
              {"format", E.Format},
              {"expanded_stream_sha256", E.ExpandedHash},
              {"expanded_byte_offset", std::to_string(M.Offset)},
              {"expanded_byte_length", std::to_string(M.Size)},
              {"container_frame_offset", "0"},
              {"container_frame_length", std::to_string(E.Original.size())}},
          E.Format == "tar"};
    }
  if (const auto Inline = htmlSource(SelectionID)) {
    const auto &H = *Inline->Analysis;
    const auto &S = H.Document.Scripts[Inline->Script];
    auto Parent = artifactView(H.Document.ArtifactID);
    if (!Parent)
      throw Error("html_parent_bytes_unavailable");
    return ArtifactView{
        Parent->Content.slice(S.BodyStart, S.BodyEnd - S.BodyStart), S.BodyHash,
        Parent->StorageOffset + S.BodyStart,
        llvm::json::Object{
            {"kind", "html_inline_script"},
            {"html_id", H.Document.ID},
            {"html_artifact_id", H.Document.ArtifactID},
            {"storage_artifact_id",
             Parent->Origin.getString("storage_artifact_id")
                 .value_or("")
                 .str()},
            {"byte_offset_basis",
             Parent->DirectStorage ? "storage_artifact" : "expanded_stream"},
            {"script_id", S.ID},
            {"document_byte_offset", std::to_string(S.BodyStart)},
            {"byte_length", std::to_string(S.BodyEnd - S.BodyStart)},
            {"byte_preprocessing", "none_raw_source_candidate"},
            {"parent_origin", std::move(Parent->Origin)}},
        Parent->DirectStorage};
  }
  return std::nullopt;
}

std::optional<Snapshot>
Session::Impl::memberNamespace(std::string_view SelectionID) const {
  for (const auto &[ID, E] : PackageArchives)
    if (std::any_of(E.Members.begin(), E.Members.end(), [&](const auto &M) {
          return M.ID == SelectionID && M.available();
        }))
      return packageArchiveNamespace(E);
  for (const auto &[ID, E] : AsarExtractions) {
    if (std::none_of(E.Members.begin(), E.Members.end(), [&](const auto &M) {
          return M.ID == SelectionID && M.available();
        }))
      continue;
    Snapshot S;
    S.ID = E.ID;
    Artifact Root;
    Root.ID = E.ID;
    Root.Directory = true;
    S.Artifacts.push_back(std::move(Root));
    for (const auto &M : E.Members) {
      if (!M.available() && M.Kind != "directory")
        continue;
      S.Artifacts.push_back(Artifact{M.ID, M.BlobHash, M.ParentID, M.Path,
                                     M.Kind, M.Content, M.Kind == "directory"});
    }
    return S;
  }
  return std::nullopt;
}
} // namespace neverd::web
