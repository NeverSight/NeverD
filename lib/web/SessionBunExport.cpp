#include "ExportDirectory.h"
#include "RecoveryParser.h"
#include "SessionInternal.h"

#include "neverd/web/SourceRecovery.h"

namespace neverd::web {
namespace {
std::string filename(char Prefix, size_t Index, std::string_view Extension) {
  auto Number = std::to_string(Index);
  Number.insert(0, 5 - std::min<size_t>(5, Number.size()), '0');
  return std::string(1, Prefix) + Number + std::string(Extension);
}
std::string escapeHTML(std::string_view Text) {
  std::string Result;
  for (char C : Text) {
    switch (C) {
    case '&':
      Result += "&amp;";
      break;
    case '<':
      Result += "&lt;";
      break;
    case '>':
      Result += "&gt;";
      break;
    case '"':
      Result += "&quot;";
      break;
    case '\'':
      Result += "&#39;";
      break;
    default:
      Result += C;
    }
  }
  return Result;
}
} // namespace

std::string Session::exportBun(std::string_view ExpectedRevision,
                               std::string_view ExtractionID,
                               std::string_view OutputDirectory) {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->BunExtractions.find(std::string(ExtractionID));
  if (Found == State->BunExtractions.end())
    throw Error("unknown_bun_extraction");
  const auto &E = Found->second;
  const auto Original = std::find_if(
      State->Published.Artifacts.begin(), State->Published.Artifacts.end(),
      [&](const auto &A) { return A.ID == E.ArtifactID; });
  if (Original == State->Published.Artifacts.end())
    throw Error("unknown_artifact");
  ExportDirectory Output(OutputDirectory);
  Output.write("original.bin", Original->Content, Original->BlobHash);
  llvm::json::Array Regions, Modules;
  std::string IndexRows;
  for (size_t I = 0; I < E.Regions.size(); ++I) {
    const auto &R = E.Regions[I];
    const auto Name = filename('r', I, ".bin");
    Output.write(Name, R.Content, R.BlobHash);
    Regions.emplace_back(
        llvm::json::Object{{"region_id", R.ID},
                           {"kind", R.Kind},
                           {"file", Name},
                           {"original_offset", std::to_string(R.Offset)},
                           {"bytes", std::to_string(R.Content.size())},
                           {"sha256", R.BlobHash}});
  }
  uint64_t Sources = 0, SourceBytes = 0, Maps = 0, Caches = 0, Assets = 0;
  uint64_t Unavailable = 0, Readable = 0, Parsed = 0;
  for (size_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    const auto &R = E.Regions[M.Contents];
    auto Ref = [&](uint32_t Region) -> llvm::json::Value {
      return Region == NoBunIndex
                 ? llvm::json::Value(nullptr)
                 : llvm::json::Value(filename('r', Region, ".bin"));
    };
    const auto &Name = E.Regions[M.Name].Content;
    const auto NameText = Name.read(0, Name.size());
    llvm::json::Object Row{{"module_index", I},
                           {"module_id", M.ID},
                           {"virtual_name", validUtf8(NameText)
                                                ? llvm::json::Value(NameText)
                                                : llvm::json::Value(nullptr)},
                           {"name_file", Ref(M.Name)},
                           {"storage_file", Ref(M.Contents)},
                           {"source_map_file", Ref(M.SourceMap)},
                           {"bytecode_file", Ref(M.Bytecode)},
                           {"module_info_file", Ref(M.ModuleInfo)},
                           {"bytecode_origin_file", Ref(M.BytecodeOrigin)},
                           {"encoding_id", M.Encoding},
                           {"loader_id", M.Loader},
                           {"format_id", M.Format},
                           {"storage_sha256", R.BlobHash},
                           {"original_offset", std::to_string(R.Offset)}};
    Maps += M.SourceMap != NoBunIndex;
    Caches += M.Bytecode != NoBunIndex;
    if (M.SourceArtifactID.empty()) {
      ++Assets;
      Row["source_status"] = "asset";
    } else {
      std::string Text;
      try {
        Text = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
      } catch (const Error &Failure) {
        // A failed projection never substitutes empty source. Exact raw bytes
        // have already been preserved, and the fixed failure remains visible.
        Row["source_status"] = std::string(Failure.what());
        ++Unavailable;
      }
      if (!Row.get("source_status")) {
        const auto File = filename('m', I, ".js");
        Output.write(File, Text);
        Row["source_status"] = "decoded_exact";
        Row["source_file"] = File;
        Row["source_sha256"] = sha256(Text);
        Row["source_bytes"] = std::to_string(Text.size());
        ++Sources;
        SourceBytes += Text.size();
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
        const auto Recovered = recoverReadableJavaScript(
            M.SourceArtifactID, Text, M.Format == 1 ? "module" : "commonjs");
        Row["parse_status"] = Recovered.ParseStatus;
        Row["readable_status"] = Recovered.Status;
        Row["syntax_nodes"] = Recovered.Nodes;
        llvm::json::Array Diagnostics;
        for (const auto &D : Recovered.Diagnostics)
          Diagnostics.emplace_back(llvm::json::Object{
              {"code", D.Code},
              {"original_utf8_byte_offset",
               D.ByteOffset < 0
                   ? llvm::json::Value(nullptr)
                   : llvm::json::Value(std::to_string(D.ByteOffset))}});
        Row["parse_diagnostics"] = std::move(Diagnostics);
        Parsed += Recovered.ParseStatus == "parsed";
        if (Recovered.Status == "verified_same_parser_tree") {
          const auto ReadableFile = filename('m', I, ".readable.js");
          Output.write(ReadableFile, Recovered.Text);
          Row["readable_file"] = ReadableFile;
          Row["readable_sha256"] = sha256(Recovered.Text);
          ++Readable;
        }
#else
        Row["readable_status"] = "javascript_parser_unavailable";
#endif
      }
    }
    IndexRows += "<tr id=\"m" + std::to_string(I) + "\"><td>" +
                 std::to_string(I) + (I == E.EntryPoint ? " (entry)" : "") +
                 "</td><td><code>" +
                 escapeHTML(validUtf8(NameText) ? NameText : "[binary name]") +
                 "</code></td><td>";
    for (const auto Key : {"source_file", "readable_file", "storage_file"})
      if (const auto File = Row.getString(Key))
        IndexRows += "<a href=\"" + File->str() + "\">" + Key + "</a> ";
    IndexRows += "</td><td>" + Row.getString("source_status")->str();
    if (const auto Status = Row.getString("readable_status"))
      IndexRows += " / " + escapeHTML(Status->str());
    IndexRows += "</td></tr>\n";
    Modules.emplace_back(std::move(Row));
  }
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"export_profile", "bun-local-evidence-export-v1"},
      {"layout_profile", E.Profile},
      {"extraction_id", E.ID},
      {"original_sha256", Original->BlobHash},
      {"original_bytes", std::to_string(Original->Content.size())},
      {"module_count", E.Modules.size()},
      {"region_count", E.Regions.size()},
      {"entry_point_index", E.EntryPoint},
      {"source_count", Sources},
      {"source_bytes", std::to_string(SourceBytes)},
      {"unavailable_source_count", Unavailable},
      {"asset_count", Assets},
      {"parsed_source_count", Parsed},
      {"readable_source_count", Readable},
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
      {"readable_source_profile", std::string(JavaScriptRecoveryProfile)},
#else
      {"readable_source_profile", nullptr},
#endif
      {"source_map_count", Maps},
      {"bytecode_count", Caches},
      {"bytecode_decoding", "opaque"},
      {"executes_input", false},
      {"verification", "all_exported_files_reread_sha256_matches"},
      {"original_source_restoration", "not_claimed"}};
  auto Manifest = Summary;
  const std::string Index =
      "<!doctype html><meta charset=\"utf-8\"><meta "
      "http-equiv=\"Content-Security-Policy\" "
      "content=\"default-src 'none'; style-src 'unsafe-inline'\">"
      "<title>NeverD recovered Bun modules</title><style>"
      "body{font:15px system-ui;margin:32px;color:#182332;background:#fafbfc}"
      "td,th{padding:8px;text-align:left;border-bottom:1px solid #d8dde5}"
      "code{font-size:12px}a{color:#1858a0}table{border-collapse:collapse}"
      "</style><h1>NeverD recovered Bun modules</h1><p>SHA-256: <code>" +
      Original->BlobHash + "</code></p><p>" + std::to_string(Sources) +
      " decoded JavaScript modules, " + std::to_string(Readable) +
      " readable modules verified by reparsing, " + std::to_string(Assets) +
      " assets, " + std::to_string(Unavailable) +
      " unavailable text projections.</p>"
      "<p>Sources and assets were extracted offline. Raw bytes and names are "
      "retained. "
      "Readable files add whitespace only. Original TypeScript, deleted names "
      "and "
      "comments, and native/bytecode decompilation are not claimed.</p>"
      "<p><a href=\"manifest.json\">Full evidence manifest</a> · <a href=\"#m" +
      std::to_string(E.EntryPoint) +
      "\">Entry module</a></p>"
      "<table><thead><tr><th>Index</th><th>Captured virtual "
      "name</th><th>Files</th>"
      "<th>Recovery status</th></tr></thead><tbody>" +
      IndexRows + "</tbody></table>";
  Output.write("index.html", Index);
  Manifest["index_file"] = "index.html";
  Manifest["index_sha256"] = sha256(Index);
  Manifest["original_file"] = "original.bin";
  Manifest["virtual_names_are_host_paths"] = false;
  Manifest["modules"] = std::move(Modules);
  Manifest["regions"] = std::move(Regions);
  const auto ManifestText = json(std::move(Manifest));
  Output.complete(ManifestText);
  Summary["manifest_sha256"] = sha256(ManifestText);
  Summary["file_count"] = Output.files();
  Summary["bytes_written"] = std::to_string(Output.bytes());
  return json(std::move(Summary));
}
} // namespace neverd::web
