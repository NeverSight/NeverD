//===- SourceModel.cpp - Private development syntax and binding export --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit local source-model disclosure using the shared analysis owners.
///
//===----------------------------------------------------------------------===//

#include "ExportDirectory.h"

#include "neverd/web/SourceBindings.h"
#include "neverd/web/SourceLimits.h"

#include <bit>
#include <charconv>
#include <iostream>
#include <span>

namespace {
using namespace neverd::web;

struct Budget {
  uint64_t Limit = 256ULL * 1024 * 1024;
  uint64_t Reserved = 4096;
  void add(uint64_t Bytes) {
    if (Bytes > Limit - Reserved)
      throw Error("source_model_export_budget_exceeded");
    Reserved += Bytes;
  }
  void row() { add(512); }
  llvm::json::Array units(std::u16string_view Value) {
    add(Value.size() * 12 + 64);
    llvm::json::Array Result;
    for (const auto Unit : Value)
      Result.emplace_back(uint64_t(Unit));
    return Result;
  }
};

llvm::json::Value index(uint32_t Value) {
  return Value == NoSourceIndex ? llvm::json::Value(nullptr)
                                : llvm::json::Value(uint64_t(Value));
}

llvm::json::Array diagnostics(std::span<const SourceDiagnostic> Values,
                              Budget &B) {
  llvm::json::Array Result;
  for (const auto &D : Values) {
    B.row();
    B.add(D.Code.size() * 8);
    Result.emplace_back(
        llvm::json::Object{{"code", D.Code}, {"byte_offset", D.ByteOffset}});
  }
  return Result;
}

llvm::json::Object model(const SourceAnalysis &S,
                         const SourceBindingAnalysis &Bindings, Budget &B) {
  llvm::json::Array Nodes, Names, Scopes, Symbols, Declarations, References;
  for (const auto &N : S.Nodes) {
    B.row();
    B.add(N.Kind.size() * 8);
    llvm::json::Array Children, Attributes;
    for (const auto &C : N.Children) {
      B.row();
      B.add(C.Field.size() * 8);
      Children.emplace_back(llvm::json::Object{
          {"field", C.Field}, {"node", C.Index}, {"ordinal", C.Ordinal}});
    }
    for (const auto &A : N.Attributes) {
      B.row();
      B.add(A.Field.size() * 8);
      llvm::json::Object Attribute{{"field", A.Field}};
      if (const auto *Value = std::get_if<std::u16string>(&A.Value))
        Attribute["utf16"] = B.units(*Value);
      else if (const auto *Value = std::get_if<bool>(&A.Value))
        Attribute["boolean"] = *Value;
      else
        Attribute["f64_bits"] = llvm::utohexstr(
            std::bit_cast<uint64_t>(std::get<double>(A.Value)), true);
      Attributes.emplace_back(std::move(Attribute));
    }
    Nodes.emplace_back(
        llvm::json::Object{{"kind", N.Kind},
                           {"start", std::to_string(N.Start)},
                           {"end", std::to_string(N.End)},
                           {"strict", N.Strict},
                           {"children", std::move(Children)},
                           {"attributes", std::move(Attributes)}});
  }
  for (const auto &Name : Bindings.Names)
    Names.emplace_back(B.units(Name));
  for (const auto &S : Bindings.Scopes) {
    B.row();
    B.add(S.Kind.size() * 8);
    Scopes.emplace_back(llvm::json::Object{
        {"kind", S.Kind},
        {"parent", index(S.Parent)},
        {"node", index(S.Node)},
        {"variable_scope", index(S.VariableScope)},
        {"strict", S.Strict},
        {"per_iteration", S.PerIteration},
        {"object_environment", S.ObjectEnvironment},
        {"possible_eval_declarations", S.PossibleEvalDeclarations},
        {"possible_direct_eval", S.PossibleDirectEval}});
  }
  for (const auto &S : Bindings.Bindings) {
    B.row();
    B.add(S.Kind.size() * 8);
    Symbols.emplace_back(
        llvm::json::Object{{"kind", S.Kind},
                           {"scope", index(S.Scope)},
                           {"name", index(S.Name)},
                           {"conflicting", S.Conflicting},
                           {"immutable", S.Immutable},
                           {"temporal_dead_zone", S.HasTemporalDeadZone},
                           {"implicit", S.Implicit}});
  }
  for (const auto &D : Bindings.Declarations) {
    B.row();
    B.add(D.Kind.size() * 8);
    Declarations.emplace_back(
        llvm::json::Object{{"node", index(D.Node)},
                           {"binding", index(D.Binding)},
                           {"scope", index(D.OccurrenceScope)},
                           {"kind", D.Kind}});
  }
  for (const auto &R : Bindings.References) {
    B.row();
    B.add((R.Access.size() + R.Resolution.size()) * 8);
    References.emplace_back(llvm::json::Object{{"node", index(R.Node)},
                                               {"scope", index(R.Scope)},
                                               {"name", index(R.Name)},
                                               {"binding", index(R.Binding)},
                                               {"access", R.Access},
                                               {"resolution", R.Resolution}});
  }
  return llvm::json::Object{
      {"nodes", std::move(Nodes)},
      {"names_utf16", std::move(Names)},
      {"scopes", std::move(Scopes)},
      {"bindings", std::move(Symbols)},
      {"declarations", std::move(Declarations)},
      {"references", std::move(References)},
      {"parser_diagnostics", diagnostics(S.Diagnostics, B)},
      {"binding_diagnostics", diagnostics(Bindings.Diagnostics, B)}};
}
} // namespace

int main(int Argc, char **Argv) {
  using namespace neverd::web;
  if (Argc != 4 && Argc != 5) {
    std::cerr << "usage: neverd-web-source-model <script|module|commonjs> "
                 "<input> <new-output-directory> [export-budget-mib]\n";
    return 2;
  }
  try {
    uint64_t ExportBudgetMiB = 256;
    if (Argc == 5) {
      const std::string_view Value = Argv[4];
      const auto [End, Error] = std::from_chars(
          Value.data(), Value.data() + Value.size(), ExportBudgetMiB);
      if (Error != std::errc{} || End != Value.data() + Value.size() ||
          ExportBudgetMiB == 0 || ExportBudgetMiB > 1024)
        throw neverd::web::Error("source_model_export_budget_invalid");
    }
    const std::string_view SourceType = Argv[1];
    if (SourceType != "script" && SourceType != "module" &&
        SourceType != "commonjs")
      throw Error("unsupported_source_type");
    const auto Limits = sourceLimits(SourceAnalysisMode::Large);
    neverd::web::Limits Bound;
    Bound.MaxInputBytes = Bound.MaxMemberBytes = Limits.Bytes;
    Bound.MaxEntries = 1;
    const auto Input = capture(Argv[2], Bound);
    if (Input.Artifacts.size() != 1 || Input.Artifacts[0].Directory)
      throw Error("source_model_requires_single_file");
    const auto &A = Input.Artifacts[0];
    const auto Text = A.Content.read(0, A.Content.size(), Limits.Bytes);
    if (sha256(Text) != A.BlobHash)
      throw Error("source_model_hash_mismatch");
    const auto Source = inspectLargeJavaScript(A.ID, Text, SourceType);
    if (Source.ParseStatus != "parsed")
      throw Error("source_model_parse_refused");
    const auto Bindings = analyzeSourceBindings(Source);
    if (Bindings.Status != "ok" && Bindings.Status != "partial")
      throw Error("source_model_binding_refused");
    Budget B{ExportBudgetMiB * 1024 * 1024};
    const auto Serialized = json(model(Source, Bindings, B));
    if (Serialized.size() > B.Reserved)
      throw Error("source_model_export_accounting_exceeded");
    ExportDirectory Output(Argv[3]);
    Output.write("original.js", Text);
    Output.write("model.json", Serialized);
    const auto Manifest = json(llvm::json::Object{
        {"schema_version", 1},
        {"profile", "development-source-model-v1"},
        {"parser_profile", std::string(LargeJavaScriptParserProfile)},
        {"binding_profile", std::string(JavaScriptBindingProfile)},
        {"source_type", std::string(SourceType)},
        {"status", "exported"},
        {"parse_status", Source.ParseStatus},
        {"binding_status", Bindings.Status},
        {"source_sha256", A.BlobHash},
        {"source_bytes", std::to_string(Text.size())},
        {"model_sha256", sha256(Serialized)},
        {"node_count", Source.Nodes.size()},
        {"binding_count", Bindings.Bindings.size()},
        {"export_reserved_bytes", B.Reserved},
        {"export_limit_bytes", B.Limit},
        {"executes_input", false},
        {"semantic_equivalence", "not_established"}});
    Output.complete(Manifest);
    std::cout << Manifest << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
