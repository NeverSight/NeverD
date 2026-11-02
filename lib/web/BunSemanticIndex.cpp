//===- BunSemanticIndex.cpp - Evidence-backed source navigation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "BunSemanticIndex.h"

#include "ExportDirectory.h"
#include "features/Publication.h"

#include "neverd/web/SourceFeatures.h"
#include "neverd/web/SourceModules.h"
#include "neverd/web/SourceNavigation.h"
#include "neverd/web/SourceRecovery.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;

struct Budget {
  BunSemanticLimits Limits;
  uint64_t Records = 0, Units = 0, Bytes = 0, MetadataBytes = 0;
  explicit Budget(BunSemanticLimits Value) : Limits(Value) {
    if (Limits.Records > BunSemanticLimits::HardRecords ||
        Limits.Units > BunSemanticLimits::HardUnits ||
        Limits.OutputBytes > BunSemanticLimits::HardOutputBytes)
      throw Error("bun_semantic_invalid_limits");
  }
  void bytes(uint64_t N) {
    if (N > Limits.OutputBytes - Bytes)
      throw Error("bun_semantic_output_budget_exceeded");
    Bytes += N;
  }
  void record(uint64_t N = 1) {
    if (N > Limits.Records - Records)
      throw Error("bun_semantic_record_budget_exceeded");
    // Fixed fields, IDs and the <=160-byte source excerpt fit this upper
    // bound even with JSON escaping. Exact name arrays are charged separately.
    bytes(N * 2048);
    MetadataBytes += N * 2048;
    Records += N;
  }
  void units(uint64_t N) {
    if (N > Limits.Units - Units)
      throw Error("bun_semantic_name_budget_exceeded");
    // UTF-16 JSON integers plus bounded escaped display strings.
    bytes(N * 24);
    MetadataBytes += N * 24;
    Units += N;
  }
  BunSemanticLimits remaining() const {
    return {Limits.Records - Records, Limits.Units - Units,
            Limits.OutputBytes - Bytes};
  }
};

std::u16string utf16(std::string_view Text) {
  llvm::SmallVector<llvm::UTF16, 64> Units;
  if (!llvm::convertUTF8ToUTF16String(llvm::StringRef(Text), Units))
    throw Error("bun_semantic_invalid_utf8");
  return {Units.begin(), Units.end()};
}

std::string display(std::u16string_view Text) {
  constexpr char Hex[] = "0123456789abcdef";
  std::string Result;
  const auto Length = std::min<size_t>(Text.size(), 128);
  for (size_t I = 0; I < Length; ++I) {
    const auto Unit = Text[I];
    if (Unit >= 32 && Unit < 127 && Unit != u'\\')
      Result += char(Unit);
    else {
      Result += "\\u";
      for (int Shift = 12; Shift >= 0; Shift -= 4)
        Result += Hex[(Unit >> Shift) & 15];
    }
  }
  if (Text.size() > Length)
    Result += "...";
  return Result;
}

llvm::json::Array units(std::u16string_view Text, Budget &B) {
  B.units(Text.size());
  llvm::json::Array Result;
  for (const auto Unit : Text)
    Result.emplace_back(uint64_t(Unit));
  return Result;
}

void name(llvm::json::Object &Row, std::string_view Prefix,
          std::u16string_view Value, Budget &B) {
  Row[std::string(Prefix) + "_utf16"] = units(Value, B);
  Row[std::string(Prefix) + "_display"] = display(Value);
  Row[std::string(Prefix) + "_display_truncated"] = Value.size() > 128;
}

std::string excerpt(std::string_view Text, uint64_t Start, uint64_t End) {
  if (Start > End || End > Text.size())
    throw Error("bun_semantic_invalid_source_span");
  auto Part = Text.substr(Start, std::min<uint64_t>(End - Start, 160));
  while (!Part.empty() && !llvm::json::isUTF8(llvm::StringRef(Part)))
    Part.remove_suffix(1);
  return std::string(Part);
}

uint32_t child(const SourceAnalysis &S, uint32_t I, std::string_view Field) {
  if (I == None)
    return None;
  for (const auto &C : S.Nodes.at(I).Children)
    if (C.Field == Field && C.Ordinal == 0)
      return C.Index;
  return None;
}

const std::u16string *spelling(const SourceAnalysis &S, uint32_t I) {
  if (I == None)
    return nullptr;
  const auto &N = S.Nodes.at(I);
  return N.Kind == "Identifier"      ? N.text("name")
         : N.Kind == "StringLiteral" ? N.text("value")
                                     : nullptr;
}

llvm::json::Object anchor(const SourceAnalysis &S, std::string_view Text,
                          uint32_t I, Budget &B) {
  B.record();
  const auto &N = S.Nodes.at(I);
  return llvm::json::Object{{"node_id", N.ID},
                            {"syntax_kind", N.Kind},
                            {"start", std::to_string(N.Start)},
                            {"end", std::to_string(N.End)},
                            {"coordinate_space", "decoded_original_utf8_bytes"},
                            {"exact_excerpt", excerpt(Text, N.Start, N.End)},
                            {"excerpt_truncated", N.End - N.Start > 160}};
}

void diagnostics(llvm::json::Array &Out,
                 std::span<const SourceDiagnostic> Input,
                 std::string_view Phase, Budget &B) {
  for (const auto &D : Input) {
    B.record();
    Out.emplace_back(llvm::json::Object{{"phase", std::string(Phase)},
                                        {"code", D.Code},
                                        {"byte_offset", D.ByteOffset}});
  }
}

std::string html(std::string_view Text) {
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
    default:
      Result += C;
    }
  }
  return Result;
}

std::string file(uint32_t I, std::string_view Suffix) {
  auto Number = std::to_string(I);
  Number.insert(0, 5 - std::min<size_t>(5, Number.size()), '0');
  return "m" + Number + std::string(Suffix);
}

std::string text(const llvm::json::Object &Row, llvm::StringRef Key) {
  const auto Value = Row.getString(Key);
  return Value ? Value->str() : "";
}

std::string pageStart(std::string_view Title) {
  return "<!doctype html><meta charset=\"utf-8\"><meta "
         "http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
         "style-src 'unsafe-inline'; script-src 'unsafe-inline'\"><title>" +
         html(Title) +
         "</title><style>body{font:15px system-ui;max-width:1100px;"
         "margin:32px auto;padding:0 20px}article{border-top:1px solid #ccc;"
         "padding:14px 0}pre{white-space:pre-wrap;overflow-wrap:anywhere;"
         "background:#f4f5f6;padding:12px}code{overflow-wrap:anywhere}"
         "input{padding:10px;width:70%}small{color:#555}a{margin-right:12px}"
         "</style><h1>" +
         html(Title) + "</h1>";
}

// These are navigation cues, explicitly weaker than the shared feature
// owner's shape matches. They inspect AST spelling, never raw-text regexes.
std::string_view cue(std::u16string_view Name, bool Literal) {
  if (Name == u"sessionId" || Name == u"session_id" || Name == u"sessionUUID" ||
      Name == u"sessionPath" || Name == u"resumeSession" ||
      Name == u"forkSession" || Name == u"createSession" ||
      Name == u"transcriptPath" || Name == u"conversationId")
    return "session";
  if (!Literal && (Name == u"createElement" || Name == u"useState" ||
                   Name == u"useEffect" || Name == u"useMemo" ||
                   Name == u"jsx" || Name == u"jsxs"))
    return "ui";
  if (Literal &&
      (Name == u"Bash" || Name == u"Read" || Name == u"Write" ||
       Name == u"Edit" || Name == u"Glob" || Name == u"Grep" ||
       Name == u"WebFetch" || Name == u"WebSearch" || Name == u"TodoWrite"))
    return "tool";
  if (Literal && (Name == u"settings.json" || Name == u"settings.local.json" ||
                  Name == u"managed-settings.json" || Name == u".claude.json" ||
                  Name == u"CLAUDE.md"))
    return "configuration";
  if (Literal && (Name == u"bypassPermissions" || Name == u"acceptEdits"))
    return "permission";
  return {};
}

bool uiAsset(std::u16string_view Name) {
  return Name.ends_with(u".css") || Name.ends_with(u".svg") ||
         Name.ends_with(u".png") || Name.ends_with(u".html") ||
         Name.ends_with(u".woff") || Name.ends_with(u".woff2") ||
         Name.ends_with(u".woff2.asset") || Name.ends_with(u".ttf") ||
         Name.find(u"mermaid") != Name.npos ||
         Name.find(u"chart.umd") != Name.npos ||
         Name.find(u"hljsBundle") != Name.npos;
}
} // namespace

BunSemanticSource
inspectBunSemanticSource(std::string_view ArtifactID, std::string_view Text,
                         std::string_view SourceType,
                         std::span<const BunSemanticModuleName> Names,
                         BunSemanticLimits Limits) {
  BunSemanticSource Result;
  Budget B(Limits);
  try {
    const auto S = inspectLargeJavaScript(ArtifactID, Text, SourceType);
    Result.ParseStatus = S.ParseStatus;
    diagnostics(Result.Diagnostics, S.Diagnostics, "parser", B);
    if (S.ParseStatus != "parsed") {
      Result.Status = S.ParseStatus;
    } else {
      const auto Bindings = analyzeSourceBindings(S);
      Result.BindingStatus = Bindings.Status;
      diagnostics(Result.Diagnostics, Bindings.Diagnostics, "bindings", B);
      const auto Modules = analyzeSourceModules(S, Bindings);
      Result.ModuleStatus = Modules.Status;
      diagnostics(Result.Diagnostics, Modules.Diagnostics, "modules", B);
      const auto Navigation = analyzeSourceNavigation(S, Bindings);
      Result.NavigationStatus = Navigation.Status;
      diagnostics(Result.Diagnostics, Navigation.Diagnostics, "navigation", B);
      const auto Features =
          analyzeClientSourceFeatures(S, Bindings, Navigation);
      Result.FeatureStatus = Features.Status;
      diagnostics(Result.Diagnostics, Features.Diagnostics, "features", B);
      std::map<std::u16string_view, uint32_t> ModuleNames;
      if (Names.size() > MaxBunModules)
        throw Error("bun_semantic_module_budget_exceeded");
      uint64_t ContextUnits = 0;
      for (const auto &Name : Names) {
        if (Name.Index >= MaxBunModules ||
            Name.Name.size() > MaxBunNameBytes - ContextUnits)
          throw Error("bun_semantic_module_name_budget_exceeded");
        ContextUnits += Name.Name.size();
        if (!ModuleNames.emplace(Name.Name, Name.Index).second)
          throw Error("bun_semantic_ambiguous_module_name");
      }
      for (const auto &Request : Modules.Requests) {
        auto Row = anchor(S, Text, Request.Node, B);
        Row["domain"] = "modules";
        Row["role"] = Request.Kind;
        Row["basis"] = "shared_source_module_request";
        Row["attribution"] = "captured_syntax";
        Row["request_id"] = Request.ID;
        Row["specifier_status"] = Request.SpecifierStatus;
        if (Request.Specifier != None) {
          const auto &Name = Modules.Names.at(Request.Specifier);
          name(Row, "original_name", Name, B);
          const auto Found = ModuleNames.find(Name);
          if (Found != ModuleNames.end())
            Row["target_module_index"] = Found->second;
        }
        Row["runtime_resolution"] = "not_proven";
        Result.Records.emplace_back(std::move(Row));
      }
      for (const auto &Export : Modules.Exports) {
        auto Row = anchor(S, Text, Export.Node, B);
        Row["domain"] = "modules";
        Row["role"] = "export";
        Row["basis"] = "shared_source_module_export";
        Row["attribution"] = "captured_syntax";
        Row["export_id"] = Export.ID;
        Row["link_status"] = Export.LinkStatus;
        if (Export.ExportedName != None)
          name(Row, "original_name", Modules.Names.at(Export.ExportedName), B);
        if (Export.Binding != None) {
          const auto &Binding = Bindings.Bindings.at(Export.Binding);
          Row["binding_id"] = Binding.ID;
          if (Binding.Name != None)
            name(Row, "original_binding", Bindings.Names.at(Binding.Name), B);
        }
        Result.Records.emplace_back(std::move(Row));
      }
      for (const auto &Feature : Features.Records) {
        auto Row = anchor(S, Text, Feature.Node, B);
        const auto Shared =
            client_features::record(Feature, S, Bindings, Navigation);
        Row["domain"] = text(Shared, "domain");
        Row["role"] = text(Shared, "pattern");
        Row["basis"] = Feature.Basis;
        Row["recognition_status"] = Feature.Status;
        Row["attribution"] = "semantic_alias_inference";
        Row["semantic_alias"] = text(Shared, "pattern");
        Row["feature_id"] = Feature.ID;
        Row["api_identity"] = "not_authenticated";
        const auto NameNode =
            Feature.Name != None ? Feature.Name : Feature.Trigger;
        if (const auto *Name = spelling(S, NameNode)) {
          name(Row, "original_name", *Name, B);
          Row["name_kind"] = S.Nodes[NameNode].Kind == "Identifier"
                                 ? "original_identifier"
                                 : "original_literal";
          Row["name_start"] = std::to_string(S.Nodes[NameNode].Start);
          Row["name_end"] = std::to_string(S.Nodes[NameNode].End);
        }
        if (Feature.Function != None) {
          const auto &Function = Navigation.Functions.at(Feature.Function);
          const auto &Node = S.Nodes.at(Function.Node);
          Row["function_id"] = Function.ID;
          Row["function_start"] = std::to_string(Node.Start);
          Row["function_end"] = std::to_string(Node.End);
          Row["parameter_count"] = Function.Parameters;
          if (const auto *Name = spelling(S, Function.Name))
            name(Row, "original_function", *Name, B);
        }
        if (Feature.CallbackBinding != None)
          Row["callback_binding_id"] =
              Bindings.Bindings.at(Feature.CallbackBinding).ID;
        Result.Records.emplace_back(std::move(Row));
      }
      std::set<uint32_t> Cues;
      for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
        const auto &N = S.Nodes[I];
        uint32_t NameNode = None;
        if (N.Kind == "StringLiteral")
          NameNode = I;
        else if (N.Kind == "Property" && !N.flag("computed"))
          NameNode = child(S, I, "key");
        else if (N.Kind == "CallExpression") {
          NameNode = child(S, I, "callee");
          if (NameNode != None &&
              S.Nodes[NameNode].Kind == "MemberExpression" &&
              !S.Nodes[NameNode].flag("computed"))
            NameNode = child(S, NameNode, "property");
        }
        const auto *Name = spelling(S, NameNode);
        if (!Name)
          continue;
        const bool Literal = S.Nodes[NameNode].Kind == "StringLiteral";
        const auto Domain = cue(*Name, Literal);
        if (Domain.empty() || !Cues.insert(NameNode).second)
          continue;
        auto Row = anchor(S, Text, NameNode, B);
        Row["domain"] = std::string(Domain);
        Row["role"] = "ast_spelling_navigation_cue";
        Row["basis"] = "exact_ast_name_in_closed_navigation_vocabulary";
        Row["attribution"] = "semantic_alias_inference";
        Row["semantic_alias"] = std::string(Domain) + " navigation candidate";
        Row["name_kind"] = Literal ? "original_literal" : "original_identifier";
        name(Row, "original_name", *Name, B);
        Result.Records.emplace_back(std::move(Row));
      }
      Result.Status = Bindings.Status == "ok" && Modules.Status == "ok" &&
                              (Navigation.Status == "ok" ||
                               Navigation.Status == "partial") &&
                              Features.Status == "partial"
                          ? "bounded_syntax_inventory"
                          : "partial_analysis";
    }
  } catch (const Error &E) {
    Result.Status = E.what();
    // Keep earlier bounded evidence, but never describe this refusal as an
    // absence of features. A fixed status is available even if budget is full.
  }
  Result.RecordsUsed = B.Records;
  Result.UnitsUsed = B.Units;
  Result.ReservedBytes = B.Bytes;
  return Result;
}

std::string exportBunSemanticIndex(const BunExtraction &E,
                                   std::string_view OutputDirectory,
                                   BunSemanticLimits Limits) {
  Budget B(Limits);
  if (E.Modules.empty() || E.Modules.size() > MaxBunModules ||
      E.EntryPoint >= E.Modules.size() || E.StartupCount > E.Modules.size())
    throw Error("bun_semantic_invalid_extraction");
  std::vector<BunSemanticModuleName> Names;
  std::vector<std::string> RawNames;
  for (uint32_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    if (M.Name >= E.Regions.size() || M.Contents >= E.Regions.size())
      throw Error("bun_semantic_invalid_extraction");
    const auto &R = E.Regions[M.Name];
    // A UTF-8 byte is an upper bound for one decoded UTF-16 unit. Reserve
    // before reading/retaining the complete name inventory, not at the later
    // per-module JSON insertion. Evidence names use their exact unit counts.
    B.units(R.Content.size());
    auto Name = R.Content.read(0, R.Content.size(), MaxBunNameBytes);
    RawNames.push_back(Name);
    Names.push_back({I, utf16(Name)});
  }
  ExportDirectory Output(OutputDirectory);
  auto Write = [&](std::string_view Name, std::string_view Text) {
    B.bytes(Text.size());
    Output.write(Name, Text);
  };
  auto Append = [&](std::string &Into, std::string_view Part) {
    B.bytes(Part.size());
    Into += Part;
  };
  std::string Main;
  Append(Main, pageStart("Bun source structure navigation"));
  Append(Main, "<p>Original spellings and byte spans are exact evidence. Role "
               "labels are syntax-based inferences. Runtime call targets, "
               "original TypeScript names and erased types remain unknown.</p>"
               "<p><a href=\"manifest.json\">Complete evidence JSON</a></p>"
               "<label>Filter modules or roles <input id=\"q\" "
               "type=\"search\"></label>");
  Append(Main, "<p>Domains: entry · tool · configuration · permission · hook · "
               "mcp · session · ui. Search a domain or an original spelling. "
               "Module pages include import/export and binding evidence.</p>");
  std::map<std::string, uint64_t> Domains;
  llvm::json::Array Modules;
  uint64_t Analyzed = 0, Refused = 0, EvidenceRecords = 0;
  for (uint32_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    const auto &Storage = E.Regions[M.Contents];
    B.record();
    llvm::json::Object Row{
        {"module_index", I},
        {"module_id", M.ID},
        {"captured_virtual_name", RawNames[I]},
        {"storage_sha256", Storage.BlobHash},
        {"storage_container_offset", std::to_string(Storage.Offset)},
        {"entry", I == E.EntryPoint},
        {"startup", I < E.StartupCount},
        {"loader_id", M.Loader},
        {"format_id", M.Format}};
    llvm::json::Array Records;
    std::string Original, ReadableFile, SourceHash;
    std::string Status = "asset_not_parsed";
    if (!M.SourceArtifactID.empty()) {
      const auto Source = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
      SourceHash = sha256(Source);
      Original = file(I, ".original.js");
      Write(Original, Source);
      Row["original_file"] = Original;
      Row["original_sha256"] = SourceHash;
      Row["source_artifact_id"] = M.SourceArtifactID;
      auto Analysis = inspectBunSemanticSource(
          M.SourceArtifactID, Source, M.Format == 1 ? "module" : "commonjs",
          Names, B.remaining());
      // The child received exactly the remaining budget before allocating any
      // records. Transfer its reservations without charging them twice.
      B.Records += Analysis.RecordsUsed;
      B.Units += Analysis.UnitsUsed;
      B.Bytes += Analysis.ReservedBytes;
      B.MetadataBytes += Analysis.ReservedBytes;
      Status = Analysis.Status;
      Row["parse_status"] = Analysis.ParseStatus;
      Row["binding_status"] = Analysis.BindingStatus;
      Row["module_status"] = Analysis.ModuleStatus;
      Row["navigation_status"] = Analysis.NavigationStatus;
      Row["feature_status"] = Analysis.FeatureStatus;
      Row["diagnostics"] = std::move(Analysis.Diagnostics);
      Records = std::move(Analysis.Records);
      ++Analyzed;
      Refused += Status != "bounded_syntax_inventory";
      bool Interesting = I == E.EntryPoint || I < E.StartupCount;
      for (const auto &Record : Records)
        if (const auto *R = Record.getAsObject())
          Interesting |= text(*R, "domain") != "modules";
      if (Interesting) {
        const auto Readable = recoverReadableJavaScript(
            M.SourceArtifactID, Source, M.Format == 1 ? "module" : "commonjs");
        Row["readable_status"] = Readable.Status;
        if (Readable.Status == "verified_same_parser_tree") {
          ReadableFile = file(I, ".readable.js");
          Write(ReadableFile, Readable.Text);
          Row["readable_file"] = ReadableFile;
          Row["readable_sha256"] = sha256(Readable.Text);
        }
      } else
        Row["readable_status"] = "not_selected_for_readable_view";
      if (I == E.EntryPoint || I < E.StartupCount) {
        B.record();
        Records.emplace_back(llvm::json::Object{
            {"domain", "entry"},
            {"role", I == E.EntryPoint ? "captured_entry_module"
                                       : "captured_startup_module"},
            {"attribution", "captured_graph_metadata"},
            {"basis", I == E.EntryPoint
                          ? "BunExtraction.EntryPoint equals module index"
                          : "module index is below BunExtraction.StartupCount"},
            {"start", "0"},
            {"end", std::to_string(Source.size())},
            {"coordinate_space", "decoded_original_utf8_bytes"}});
      }
    } else if (uiAsset(Names[I].Name)) {
      Original = file(I, ".asset.bin");
      B.bytes(Storage.Content.size());
      Output.write(Original, Storage.Content, Storage.BlobHash);
      Row["original_file"] = Original;
      Row["original_sha256"] = Storage.BlobHash;
      B.record();
      llvm::json::Object Evidence{
          {"domain", "ui"},
          {"role", "captured_resource_name_candidate"},
          {"attribution", "semantic_alias_inference"},
          {"basis", "captured virtual name matches a closed UI resource "
                    "suffix/name vocabulary"},
          {"start", "0"},
          {"end", std::to_string(Storage.Content.size())},
          {"coordinate_space", "original_asset_storage_bytes"},
          {"name_container_offset", std::to_string(E.Regions[M.Name].Offset)}};
      name(Evidence, "original_name", Names[I].Name, B);
      Records.emplace_back(std::move(Evidence));
    }
    Row["status"] = Status;
    std::string Page;
    Append(Page,
           pageStart("Module " + std::to_string(I) + " — " + RawNames[I]));
    Append(Page, "<p><a href=\"index.html\">Domain index</a>" +
                     (Original.empty()
                          ? "Original asset not selected for disclosure"
                          : "<a href=\"" + Original + "\">Exact original</a>") +
                     (ReadableFile.empty() ? ""
                                           : "<a href=\"" + ReadableFile +
                                                 "\">Readable view</a>") +
                     "</p><p>Status: <code>" + html(Status) +
                     "</code></p>"
                     "<p>Original SHA-256: <code>" +
                     html(SourceHash.empty() ? Storage.BlobHash : SourceHash) +
                     "</code></p>");
    std::map<std::string, uint64_t> LocalDomains;
    for (size_t R = 0; R < Records.size(); ++R) {
      const auto *Evidence = Records[R].getAsObject();
      if (!Evidence)
        throw Error("bun_semantic_invalid_record");
      const auto Domain = text(*Evidence, "domain");
      ++Domains[Domain];
      ++LocalDomains[Domain];
      std::string Label = text(*Evidence, "original_name_display");
      if (Label.empty())
        Label = text(*Evidence, "role");
      if (Domain != "modules")
        Append(Main,
               "<article><strong>" + html(Domain) + "</strong> · <a href=\"" +
                   file(I, ".html") + "#r" + std::to_string(R) + "\">" +
                   html(Label) + "</a><p>" + html(text(*Evidence, "role")) +
                   " · module " + std::to_string(I) + " · original bytes [" +
                   html(text(*Evidence, "start")) + ", " +
                   html(text(*Evidence, "end")) + ")</p><small>" +
                   html(text(*Evidence, "attribution")) + "</small><pre>" +
                   html(text(*Evidence, "exact_excerpt")) + "</pre></article>");
      Append(Page, "<article id=\"r" + std::to_string(R) + "\"><strong>" +
                       html(Domain + " · " + Label) + "</strong><p>" +
                       html(text(*Evidence, "role")) + " · " +
                       html(text(*Evidence, "attribution")) +
                       "</p><small>Original byte span [" +
                       html(text(*Evidence, "start")) + ", " +
                       html(text(*Evidence, "end")) + ") · " +
                       html(text(*Evidence, "basis")) + "</small><pre>" +
                       html(text(*Evidence, "exact_excerpt")) +
                       "</pre></article>");
      if (const auto Target = Evidence->getInteger("target_module_index")) {
        if (*Target < 0 || uint64_t(*Target) >= E.Modules.size())
          throw Error("bun_semantic_invalid_module_link");
        Append(Page, "<p><a href=\"" + file(uint32_t(*Target), ".html") +
                         "\">Captured target module " +
                         std::to_string(*Target) +
                         "</a>"
                         "<small>Exact captured name match; runtime resolution "
                         "not proven.</small></p>");
      }
    }
    const auto PageFile = file(I, ".html");
    Output.write(PageFile, Page);
    Row["navigation_file"] = PageFile;
    Row["navigation_sha256"] = sha256(Page);
    std::string DomainText;
    for (const auto &[Domain, Count] : LocalDomains)
      DomainText += Domain + " (" + std::to_string(Count) + ") ";
    Append(Main, "<article><a href=\"" + PageFile + "\">Module " +
                     std::to_string(I) + " · " + html(RawNames[I]) + "</a><p>" +
                     html(DomainText) + "</p><small>" + html(Status) +
                     "</small></article>");
    EvidenceRecords += Records.size();
    Row["records"] = std::move(Records);
    Modules.emplace_back(std::move(Row));
  }
  Append(Main,
         "<script>document.getElementById('q').addEventListener('input',"
         "function(){"
         "const q=this.value.toLowerCase();for(const a of "
         "document.querySelectorAll('article'))"
         "a.hidden=!a.textContent.toLowerCase().includes(q);});</script>");
  Output.write("index.html", Main);
  llvm::json::Array Coverage;
  for (const auto Domain : {"entry", "tool", "configuration", "permission",
                            "hook", "mcp", "session", "ui", "modules"}) {
    B.record();
    const auto Count = Domains[Domain];
    Coverage.emplace_back(llvm::json::Object{
        {"domain", Domain},
        {"record_count", Count},
        {"status",
         Count ? "evidence_candidates" : "no_candidate_in_analyzed_subset"}});
  }
  B.record();
  llvm::json::Object Manifest{
      {"schema_version", 1},
      {"profile", "bun-semantic-navigation-v1"},
      {"status", Refused ? "partial_analysis" : "bounded_syntax_inventory"},
      {"extraction_id", E.ID},
      {"entry_point_index", E.EntryPoint},
      {"analyzed_source_count", Analyzed},
      {"refused_or_partial_source_count", Refused},
      {"evidence_record_count", EvidenceRecords},
      {"metadata_record_count", B.Records},
      {"reserved_name_units", B.Units},
      {"name_unit_accounting", "exact UTF-16 evidence units plus UTF-8 byte "
                               "upper bounds for retained module names"},
      {"source_execution", false},
      {"binding_rewrites", false},
      {"erased_types", "unavailable"},
      {"feature_inventory_complete", false},
      {"index_file", "index.html"},
      {"index_sha256", sha256(Main)},
      {"coverage", std::move(Coverage)},
      {"modules", std::move(Modules)}};
  const auto Text = json(std::move(Manifest));
  // Reservation bounds are charged before JSON objects/arrays are built.
  // The writer independently checks actual aggregate bytes and read-back
  // hashes.
  if (Text.size() > B.MetadataBytes)
    throw Error("bun_semantic_metadata_bound_mismatch");
  Output.complete(Text);
  return json(llvm::json::Object{
      {"status", Refused ? "partial_analysis" : "bounded_syntax_inventory"},
      {"analyzed_source_count", Analyzed},
      {"refused_or_partial_source_count", Refused},
      {"evidence_record_count", EvidenceRecords},
      {"metadata_record_count", B.Records},
      {"reserved_name_units", B.Units},
      {"manifest_sha256", sha256(Text)},
      {"files_written", Output.files()},
      {"bytes_written", std::to_string(Output.bytes())}});
}
} // namespace neverd::web
