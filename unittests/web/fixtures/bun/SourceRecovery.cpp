//===- SourceRecovery.cpp - Development source recovery driver ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit local parser/readable export; never compiles or executes input.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/SourceRecovery.h"

#include "ExportDirectory.h"
#include "RecoveryParser.h"

#include <iostream>

int main(int Argc, char **Argv) {
  using namespace neverd::web;
  if (Argc != 5) {
    std::cerr << "usage: neverd-web-source-recovery <check|readable> "
                 "<script|module|commonjs> <input> <new-output-directory>\n";
    return 2;
  }
  try {
    const std::string_view Mode = Argv[1], SourceType = Argv[2];
    if (Mode != "check" && Mode != "readable")
      throw Error("unsupported_recovery_mode");
    if (SourceType != "script" && SourceType != "module" &&
        SourceType != "commonjs")
      throw Error("unsupported_source_type");
    Limits Bound;
    Bound.MaxInputBytes = Bound.MaxMemberBytes = MaxRecoverySourceBytes;
    Bound.MaxEntries = 1;
    const auto Input = capture(Argv[3], Bound);
    if (Input.Artifacts.size() != 1 || Input.Artifacts[0].Directory)
      throw Error("source_recovery_requires_single_file");
    const auto &A = Input.Artifacts[0];
    const auto Text =
        A.Content.read(0, A.Content.size(), MaxRecoverySourceBytes);
    if (sha256(Text) != A.BlobHash)
      throw Error("source_recovery_hash_mismatch");

    std::string Status, ParseStatus, Readable;
    uint64_t Nodes = 0;
    std::vector<SourceDiagnostic> Diagnostics;
    if (Mode == "readable") {
      auto R = recoverReadableJavaScript(A.ID, Text, SourceType);
      Status = R.Status;
      ParseStatus = R.ParseStatus;
      Nodes = R.Nodes;
      Diagnostics = std::move(R.Diagnostics);
      Readable = std::move(R.Text);
    } else {
      auto R = inspectRecoveryJavaScript(A.ID, Text, SourceType);
      ParseStatus = R.ParseStatus;
      Status = ParseStatus == "parsed" && R.LexemeStatus == "ok"
                   ? "syntax_valid"
                   : "syntax_refused";
      Nodes = R.Nodes.size();
      Diagnostics = std::move(R.Diagnostics);
    }
    const bool Accepted =
        Status == "syntax_valid" || Status == "verified_same_parser_tree";
    llvm::json::Array DS;
    for (const auto &D : Diagnostics)
      DS.emplace_back(llvm::json::Object{
          {"code", D.Code}, {"original_byte_offset", D.ByteOffset}});
    llvm::json::Object Receipt{
        {"schema_version", 1},
        {"profile", "development-source-recovery-v1"},
        {"parser_profile", std::string(JavaScriptRecoveryProfile)},
        {"mode", std::string(Mode)},
        {"source_type", std::string(SourceType)},
        {"status", Status},
        {"parse_status", ParseStatus},
        {"syntax_nodes", Nodes},
        {"original_sha256", A.BlobHash},
        {"original_bytes", std::to_string(Text.size())},
        {"diagnostics", std::move(DS)},
        {"executes_input", false},
        {"semantic_equivalence", "not_established"}};
    ExportDirectory Output(Argv[4]);
    Output.write("original.js", Text);
    if (Status == "verified_same_parser_tree") {
      Output.write("readable.js", Readable);
      Receipt["readable_file"] = "readable.js";
      Receipt["readable_sha256"] = sha256(Readable);
      Receipt["transformation"] = "whitespace_insertion_only";
    }
    const auto Serialized = json(std::move(Receipt));
    Output.complete(Serialized);
    std::cout << Serialized << '\n';
    return Accepted ? 0 : 1;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
