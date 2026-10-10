#include "HermesLexemes.h"
#include "HermesModel.h"
#include "hermes/AST/Context.h"
#include "hermes/AST/ESTree.h"
#include "hermes/AST/SemValidate.h"
#include "hermes/Parser/JSParser.h"
#include "llvh/Support/MemoryBuffer.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/ParserBudget.h"
#include "neverd/web/Session.h"
#include "neverd/web/Source.h"

#include <cfenv>

namespace neverd::web {
namespace {
namespace AST = hermes::ESTree;

struct Diagnostics {
  SourceAnalysis &Result;
  const char *Base;
  size_t Size;
  static void receive(const llvh::SMDiagnostic &D, void *Opaque) {
    auto &Self = *static_cast<Diagnostics *>(Opaque);
    if (Self.Result.Diagnostics.size() >= 16)
      return;
    const auto Address = reinterpret_cast<uintptr_t>(D.getLoc().getPointer());
    const auto Begin = reinterpret_cast<uintptr_t>(Self.Base);
    const int64_t Offset = Address >= Begin && Address - Begin <= Self.Size
                               ? int64_t(Address - Begin)
                               : -1;
    // Neither the message nor its line/filename is safe for ordinary output.
    Self.Result.Diagnostics.push_back({D.getKind() == llvh::SourceMgr::DK_Error
                                           ? "syntax_error"
                                           : "parser_warning",
                                       Offset});
  }
};

} // namespace

SourceAnalysis inspectJavaScript(std::string_view ArtifactID,
                                 std::string_view Bytes,
                                 std::string_view SourceType) {
  if (SourceType != "script" && SourceType != "module" &&
      SourceType != "commonjs")
    throw Error("unsupported_source_type");
  if (Bytes.size() > MaxJavaScriptBytes)
    throw Error("source_byte_budget_exceeded");
  SourceAnalysis Result;
  Result.ArtifactID = ArtifactID;
  Result.BlobHash = sha256(Bytes);
  Result.SourceType = SourceType;
  Result.ID = identity("source-unit", {ArtifactID, Result.BlobHash,
                                       JavaScriptParserProfile, SourceType});
  Result.ParseStatus = "invalid_syntax";
  // The embedded numeric-literal converter assumes nearest rounding. Do not
  // silently inherit another host thread's floating-point mode or change it.
  if (std::fegetround() != FE_TONEAREST) {
    Result.ParseStatus = "unsupported_environment";
    Result.Diagnostics.push_back({"unsupported_host_rounding_mode", -1});
    return Result;
  }
  if (!validUtf8(Bytes) || Bytes.find('\0') != Bytes.npos) {
    Result.ParseStatus = "unsupported_encoding";
    Result.Diagnostics.push_back({"unsupported_encoding", -1});
    return Result;
  }
  ParserBudget Budget;
  auto Buffer = llvh::MemoryBuffer::getMemBufferCopy(
      llvh::StringRef(Bytes.data(), Bytes.size()), "source");
  const auto *Base = Buffer->getBufferStart();
  Diagnostics Diags{Result, Base, Bytes.size()};
  hermes::CodeGenerationSettings Settings;
  Settings.enableBlockScoping = false;
  hermes::Context Context(Settings);
  Context.setStrictMode(SourceType == "module");
  Context.setAllowReturnOutsideFunction(SourceType == "commonjs");
  // Hermes uses this validation flag for ES module declarations too. Only
  // parser validation is called below, never CJS lowering or code generation.
  Context.setTransformCJSModules(SourceType == "module");
  Context.getSourceErrorManager().setDiagHandler(Diagnostics::receive, &Diags);
  Context.getSourceErrorManager().setErrorLimit(16);
  hermes::parser::JSParser Parser(Context, std::move(Buffer));
  Parser.setStrictMode(SourceType == "module");
  Parser.setStoreComments(true);
  Parser.setStoreTokens(true);
  auto Program = Parser.parse();
  if (!Program)
    return Result;
  hermes::sem::SemContext Semantics;
  if (!hermes::sem::validateASTForParser(Context, Semantics, *Program))
    return Result;
  hermes_model::Collector Nodes(Result, Base, Bytes.size());
  try {
    Nodes.collect(*Program, UINT32_MAX, SourceType == "module");
  } catch (const Error &E) {
    Result.Nodes.clear();
    Result.Diagnostics.push_back({E.what(), -1});
    Result.ParseStatus = "unsupported";
    return Result;
  }
  Result.CommentCount = Parser.getStoredComments().size();
  try {
    hermes_model::collectLexemes(Result, Parser, Base, Bytes);
  } catch (const Error &E) {
    Result.Lexemes.clear();
    Result.LexemeStatus = E.what();
  }
  Result.ParseStatus = "parsed";
  return Result;
}

} // namespace neverd::web
