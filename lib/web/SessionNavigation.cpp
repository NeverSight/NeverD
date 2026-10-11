//===- SessionNavigation.cpp - Source navigation publication -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source navigation publication.
///
//===----------------------------------------------------------------------===//

#include "SessionInternal.h"

namespace neverd::web {
namespace {
llvm::json::Object summary(const SourceNavigation &A, uint64_t Revision) {
  llvm::json::Array Diagnostics;
  for (const auto &D : A.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{{"code", D.Code}});
  return llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", A.SourceID},
      {"navigation_id", A.ID},
      {"navigation_profile", std::string(JavaScriptNavigationProfile)},
      {"navigation_status", A.Status},
      {"binding_analysis_id", A.BindingID},
      {"binding_status", A.BindingStatus},
      {"function_count", A.Functions.size()},
      {"call_count", A.Calls.size()},
      {"reference_count", A.References.size()},
      {"syntax_inventory_complete", A.Status == "ok" || A.Status == "partial"},
      {"reference_inventory_available",
       (A.Status == "ok" || A.Status == "partial") &&
           (A.BindingStatus == "ok" || A.BindingStatus == "partial")},
      {"steps", A.Steps},
      {"diagnostics", std::move(Diagnostics)},
      {"function_containment", "source_syntax"},
      {"runtime_call_graph", "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}};
}
} // namespace

std::string Session::analyzeSourceNavigation(std::string_view Revision,
                                             std::string_view SourceID) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Source = State->Sources.find(std::string(SourceID));
  if (Source == State->Sources.end())
    throw Error("unknown_source");
  const auto Existing = State->Navigation.find(Source->first);
  if (Existing != State->Navigation.end())
    return json(summary(Existing->second, State->Revision));
  auto Bindings = State->Bindings.find(Source->first);
  if (Bindings == State->Bindings.end())
    Bindings =
        State->Bindings
            .emplace(Source->first, web::analyzeSourceBindings(Source->second))
            .first;
  auto Navigation =
      web::analyzeSourceNavigation(Source->second, Bindings->second);
  auto Reply = json(summary(Navigation, State->Revision));
  State->Navigation.emplace(Source->first, std::move(Navigation));
  return Reply;
#endif
}

std::string Session::sourceNavigationRecords(std::string_view Revision,
                                             std::string_view SourceID,
                                             std::string_view Kind,
                                             uint64_t Offset,
                                             uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(Revision);
  const auto Source = State->Sources.find(std::string(SourceID));
  if (Source == State->Sources.end())
    throw Error("unknown_source");
  const auto Existing = State->Navigation.find(Source->first);
  if (Existing == State->Navigation.end())
    throw Error("source_navigation_not_analyzed");
  const auto &S = Source->second;
  const auto &A = Existing->second;
  const auto &B = State->Bindings.at(Source->first);
  auto NodeID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(S.Nodes.at(I).ID);
  };
  auto BindingID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(B.Bindings.at(I).ID);
  };
  auto FunctionID = [&](uint32_t I) -> llvm::json::Value {
    return I == NoSourceIndex ? llvm::json::Value(nullptr)
                              : llvm::json::Value(A.Functions.at(I).ID);
  };
  uint64_t Count;
  if (Kind == "functions")
    Count = A.Functions.size();
  else if (Kind == "calls")
    Count = A.Calls.size();
  else if (Kind == "references")
    Count = A.References.size();
  else
    throw Error("unsupported_navigation_record_kind");
  if (!Limit || Limit > 512 || Offset > Count)
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Count, Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    uint32_t Node;
    llvm::json::Object Item;
    if (Kind == "functions") {
      const auto &F = A.Functions[I];
      Node = F.Node;
      Item = llvm::json::Object{
          {"function_id", F.ID},
          {"parent_function_id", FunctionID(F.ParentFunction)},
          {"name_node_id", NodeID(F.Name)},
          {"body_node_id", NodeID(F.Body)},
          {"name_binding_id", BindingID(F.NameBinding)},
          {"initializer_binding_id", BindingID(F.InitializerBinding)},
          {"binding_link_scope", "source_declaration_or_initializer"},
          {"parameter_count", F.Parameters},
          {"async", S.Nodes[Node].flag("async")},
          {"generator", S.Nodes[Node].flag("generator")},
          {"name_redacted", true}};
    } else if (Kind == "calls") {
      const auto &C = A.Calls[I];
      Node = C.Node;
      const auto *R = C.Reference == NoSourceIndex
                          ? nullptr
                          : &B.References.at(C.Reference);
      Item = llvm::json::Object{
          {"call_id", C.ID},
          {"call_kind", C.Kind},
          {"enclosing_function_id", FunctionID(C.EnclosingFunction)},
          {"callee_node_id", NodeID(C.Callee)},
          {"argument_node_id", NodeID(C.Argument)},
          {"callee_syntax_kind",
           C.Callee == NoSourceIndex
               ? llvm::json::Value(nullptr)
               : llvm::json::Value(S.Nodes[C.Callee].Kind)},
          {"callee_reference_id",
           R ? llvm::json::Value(R->ID) : llvm::json::Value(nullptr)},
          {"callee_binding_id", R && R->Resolution == "lexical_binding"
                                    ? BindingID(R->Binding)
                                    : llvm::json::Value(nullptr)},
          {"binding_resolution",
           R ? R->Resolution : "not_applicable_or_unavailable"},
          {"syntactic_function_id", FunctionID(C.SyntacticFunction)},
          {"runtime_target", "not_proven"},
          {"optional", S.Nodes[Node].flag("optional")}};
    } else {
      const auto &R = B.References.at(A.References[I].Reference);
      Node = R.Node;
      Item = llvm::json::Object{{"reference_id", R.ID},
                                {"enclosing_function_id",
                                 FunctionID(A.References[I].EnclosingFunction)},
                                {"binding_id", BindingID(R.Binding)},
                                {"resolution", R.Resolution},
                                {"access", R.Access},
                                {"name_redacted", true}};
    }
    const auto &N = S.Nodes[Node];
    Item["node_id"] = N.ID;
    Item["syntax_kind"] = N.Kind;
    Item["byte_offset"] = std::to_string(N.Start);
    Item["byte_length"] = std::to_string(N.End - N.Start);
    Items.emplace_back(std::move(Item));
  }
  auto Reply = summary(A, State->Revision);
  Reply["record_kind"] = std::string(Kind);
  Reply["items"] = std::move(Items);
  Reply["offset"] = Offset;
  Reply["page_complete"] = End == Count;
  Reply["next_offset"] =
      End == Count ? llvm::json::Value(nullptr) : llvm::json::Value(End);
  return json(std::move(Reply));
#endif
}
} // namespace neverd::web
