//===- SourceInterfaces.cpp - Source request construction candidates ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Direct lexical fetch/WebSocket candidates, never runtime API claims.
///
//===----------------------------------------------------------------------===//

#include "../SourceModel.h"

#include "neverd/web/Artifact.h"
#include "neverd/web/Error.h"
#include "neverd/web/Interfaces.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/ConvertUTF.h"

#include <set>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;

std::optional<std::string> utf8(std::u16string_view Text) {
  if (Text.size() > MaxInterfaceURLBytes)
    throw Error("interface_url_budget_exceeded");
  const std::vector<llvm::UTF16> Units(Text.begin(), Text.end());
  std::string R;
  if (!llvm::convertUTF16ToUTF8String(Units, R))
    return {};
  return R;
}

struct Inspector {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceValueAnalysis &V;
  SourceInterfaces A;
  std::vector<uint32_t> References;
  std::set<std::u16string> Written;

  void step(uint64_t N = 1) {
    if (N > MaxInterfaceSteps - A.Steps)
      throw Error("interface_work_budget_exceeded");
    A.Steps += N;
  }
  uint32_t child(uint32_t I, std::string_view Field, uint32_t Ordinal = 0) {
    if (I == None)
      return None;
    for (const auto &C : S.Nodes.at(I).Children) {
      step();
      if (C.Field == Field && C.Ordinal == Ordinal)
        return C.Index;
    }
    return None;
  }
  const std::u16string *constant(uint32_t I) const {
    if (I == None)
      return nullptr;
    const auto &Value = V.Nodes.at(I);
    return Value.Status == "constant" && Value.Value &&
                   Value.Value->Kind == PrimitiveKind::String
               ? &Value.Value->String
               : nullptr;
  }
  void url(SourceInterface &R) {
    if (R.URLNode == None)
      return;
    R.URLStatus = "dynamic_or_unavailable";
    R.Endpoint.Status = "dynamic_or_unavailable";
    const auto &N = S.Nodes[R.URLNode];
    if (N.Kind == "TemplateLiteral") {
      for (const auto &C : N.Children) {
        step();
        R.TemplateHoles += C.Field == "expressions";
      }
      R.URLStatus = "unresolved_template";
      R.Endpoint.Status = "unresolved_template";
    }
    const auto *Text = constant(R.URLNode);
    if (!Text)
      return;
    const auto UTF8 = utf8(*Text);
    if (!UTF8) {
      R.URLStatus = "unsupported_utf16";
      return;
    }
    R.URLStatus = "constant_string";
    R.Endpoint = inspectInterfaceEndpoint(*UTF8);
    const auto Size = R.Endpoint.Origin.size() + R.Endpoint.Path.size();
    if (Size > MaxInterfacePrivateBytes - A.PrivateBytes)
      throw Error("interface_private_budget_exceeded");
    A.PrivateBytes += Size;
    // Preserve invalidity that would be lost by ordinary URL serialization.
    if (R.Kind == "fetch" && R.Endpoint.CredentialsPresent)
      R.Endpoint.Status = "fetch_credentials_rejected";
    if (R.Kind == "websocket" && R.Endpoint.FragmentPresent)
      R.Endpoint.Status = "websocket_fragment_rejected";
  }
  void options(SourceInterface &R) {
    if (R.OptionsNode == None) {
      R.Method = "GET";
      R.MethodStatus = "omitted_init_default_candidate";
      return;
    }
    R.OptionsStatus = "unresolved";
    if (S.Nodes[R.OptionsNode].Kind != "ObjectExpression")
      return;
    std::map<std::u16string, uint32_t> Properties;
    for (const auto &C : S.Nodes[R.OptionsNode].Children) {
      step();
      const auto &P = S.Nodes[C.Index];
      if (C.Field != "properties" || P.Kind != "Property" ||
          P.flag("computed") || P.flag("method") || !P.text("kind") ||
          *P.text("kind") != u"init")
        return;
      const auto Key = child(C.Index, "key");
      if (Key == None)
        return;
      const auto &K = S.Nodes[Key];
      const auto *Name = K.text(K.Kind == "Identifier" ? "name" : "value");
      if (!Name || (K.Kind != "Identifier" && K.Kind != "StringLiteral") ||
          *Name == u"__proto__" ||
          !Properties.emplace(*Name, child(C.Index, "value")).second)
        return;
    }
    R.OptionsStatus = "explicit_data_properties";
    auto Property = [&](std::u16string_view Name) {
      const auto Found = Properties.find(std::u16string(Name));
      return Found == Properties.end() ? None : Found->second;
    };
    R.MethodNode = Property(u"method");
    R.HeadersNode = Property(u"headers");
    R.BodyNode = Property(u"body");
    if (R.MethodNode == None) {
      // WebIDL dictionary conversion can observe inherited properties.
      R.MethodStatus = "inherited_method_unknown";
      return;
    }
    if (const auto *Text = constant(R.MethodNode)) {
      if (Text->size() > 16) {
        R.Method = "other";
      } else if (auto UTF8 = utf8(*Text)) {
        R.Method = interfaceMethod(*UTF8, true);
      }
      R.MethodStatus = R.Method == "other" || R.Method == "unknown"
                           ? "unrecognized_redacted"
                           : "explicit_literal_candidate";
      if (R.Method == "CONNECT" || R.Method == "TRACE")
        R.MethodStatus = "fetch_forbidden_method";
    }
  }
  void run() {
    step(validateSourceModel(S));
    if (B.SourceID != S.ID || V.SourceID != S.ID ||
        V.Nodes.size() != S.Nodes.size() ||
        (B.Status != "ok" && B.Status != "partial") ||
        B.References.size() > MaxJavaScriptNodes)
      throw Error("interface_source_evidence_mismatch");
    for (const auto &Scope : B.Scopes) {
      step();
      if (Scope.PossibleDirectEval)
        throw Error("interface_source_dynamic_eval");
    }
    References.assign(S.Nodes.size(), None);
    for (uint32_t I = 0; I < B.References.size(); ++I) {
      step();
      const auto &R = B.References[I];
      if (R.Node >= S.Nodes.size() || References[R.Node] != None ||
          (R.Binding != None && R.Binding >= B.Bindings.size()))
        throw Error("invalid_interface_binding_model");
      References[R.Node] = I;
      const auto *Name = S.Nodes[R.Node].text("name");
      if (R.Binding == None && Name &&
          (*Name == u"fetch" || *Name == u"WebSocket") &&
          (R.Access == "write" || R.Access == "read_write"))
        Written.insert(*Name);
    }
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      const auto &N = S.Nodes[I];
      if (N.Kind != "CallExpression" && N.Kind != "OptionalCallExpression" &&
          N.Kind != "NewExpression")
        continue;
      const auto Callee = child(I, "callee");
      if (Callee == None || S.Nodes[Callee].Kind != "Identifier")
        continue;
      const auto *Name = S.Nodes[Callee].text("name");
      if (!Name || !((*Name == u"fetch" && N.Kind != "NewExpression") ||
                     (*Name == u"WebSocket" && N.Kind == "NewExpression")))
        continue;
      if (References[Callee] == None ||
          B.References[References[Callee]].Resolution != "external" ||
          Written.count(*Name)) {
        ++A.ExcludedDispatches;
        continue;
      }
      if (A.Records.size() >= MaxInterfaceRecords)
        throw Error("interface_record_budget_exceeded");
      SourceInterface R;
      R.Kind = *Name == u"fetch" ? "fetch" : "websocket";
      R.ID = identity("source-interface", {A.ID, N.ID, R.Kind});
      R.Node = I;
      R.URLNode = child(I, "arguments");
      R.OptionsNode = child(I, "arguments", 1);
      R.Optional = N.Kind == "OptionalCallExpression" || N.flag("optional");
      if (R.Kind == "fetch")
        options(R);
      else
        R.MethodStatus = "websocket_not_http_evidence";
      url(R);
      A.Records.push_back(std::move(R));
    }
    A.Status = "partial";
    A.Reason = "external_callee_syntax_not_runtime_identity";
  }
};
} // namespace

SourceInterfaces analyzeSourceInterfaces(const SourceAnalysis &Source,
                                         const SourceBindingAnalysis &Bindings,
                                         const SourceValueAnalysis &Values) {
  Inspector I{Source, Bindings, Values};
  I.A.SourceID = Source.ID;
  I.A.BindingID = Bindings.ID;
  I.A.ValueID = Values.ID;
  I.A.ID = identity("source-interfaces", {Source.ID, Bindings.ID, Values.ID,
                                          SourceInterfaceProfile});
  try {
    I.run();
  } catch (const Error &E) {
    I.A.Records.clear();
    I.A.PrivateBytes = 0;
    I.A.Reason = E.what();
    I.A.Status = I.A.Reason.find("budget_exceeded") != std::string::npos
                     ? "budget_exceeded"
                     : "unavailable";
  }
  return std::move(I.A);
}
} // namespace neverd::web
