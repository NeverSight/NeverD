#include "neverd/web/SourcePaths.h"

#include "Internal.h"
#include "SourceModel.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/ConvertUTF.h"

#include <algorithm>
#include <optional>

namespace neverd::web {
namespace {
constexpr auto None = NoSourceIndex;
using Kind = CapturedPathKind;
using Path = std::shared_ptr<const CapturedPathValue>;
std::u16string utf16(std::string_view Text) {
  llvm::SmallVector<llvm::UTF16, 64> Units;
  if (!llvm::convertUTF8ToUTF16String(llvm::StringRef(Text), Units))
    throw Error("invalid_captured_path_context");
  return std::u16string(Units.begin(), Units.end());
}
std::optional<std::string> utf8(std::u16string_view Text) {
  const std::vector<llvm::UTF16> Units(Text.begin(), Text.end());
  std::string Result;
  if (!llvm::convertUTF16ToUTF8String(Units, Result))
    return {};
  return Result;
}
// This is a virtual captured-root path, not the analyzer host's path syntax.
// Dot normalization occurs only for an explicit normalizing operation.
std::optional<std::u16string> normalize(std::u16string_view Text, bool Captured,
                                        bool Trailing) {
  if (Text.size() > MaxCapturedPathUnits || Text.find(u'\\') != Text.npos ||
      Text.find(u':') != Text.npos || Text.find(u'\0') != Text.npos ||
      (!Captured && Text.starts_with(u'/')))
    return {};
  std::vector<std::u16string_view> Parts;
  auto Remaining = Text;
  while (true) {
    const auto Slash = Remaining.find(u'/');
    const auto Part = Remaining.substr(0, Slash);
    if (Part == u"..") {
      if (!Parts.empty() && Parts.back() != u"..")
        Parts.pop_back();
      else if (Captured)
        return {};
      else
        Parts.push_back(Part);
    } else if (!Part.empty() && Part != u".") {
      const auto UTF8 = utf8(Part);
      if (!UTF8)
        return {};
      try {
        validateMemberName(*UTF8);
      } catch (const Error &) {
        return {};
      }
      Parts.push_back(Part);
    }
    if (Slash == Remaining.npos)
      break;
    Remaining.remove_prefix(Slash + 1);
  }
  std::u16string Result;
  for (const auto Part : Parts) {
    if (!Result.empty())
      Result += u'/';
    Result += Part;
  }
  if (Result.empty() && !Captured)
    Result = u".";
  if (Trailing && Text.ends_with(u'/') && !Result.empty())
    Result += u'/';
  return Result;
}

struct Resolver {
  const SourceAnalysis &S;
  const SourceBindingAnalysis &B;
  const SourceModuleAnalysis &M;
  const SourceOrigins &O;
  const SourceValueAnalysis &V;
  const CapturedPathContext &Context;
  SourcePaths A;
  std::vector<uint32_t> References, Declarations, Initializers;
  std::vector<bool> Written, Ambiguous, Declared;
  std::vector<uint8_t> Visiting;
  std::u16string SourceFile, SourceDirectory, AppDirectory;
  bool URLWritten = false;

  void step(uint64_t N = 1) {
    if (N > MaxCapturedPathSteps - A.Steps)
      throw Error("source_path_budget_exceeded");
    A.Steps += N;
  }
  Path refuse(std::string Reason) {
    auto P = std::make_shared<CapturedPathValue>();
    P->Reason = std::move(Reason);
    return P;
  }
  Path make(Kind K, std::u16string_view Text, uint32_t Root = None,
            uint32_t Operation = None) {
    step(Text.size() + 1);
    if (Text.size() > MaxCapturedPathUnits)
      return refuse("path_length_exceeded");
    if (Text.size() > MaxCapturedPathAllocatedUnits - A.AllocatedUnits)
      throw Error("source_path_budget_exceeded");
    A.AllocatedUnits += Text.size();
    auto P = std::make_shared<CapturedPathValue>();
    P->Kind = K;
    P->Reason.clear();
    P->Text = Text;
    P->RootNode = Root;
    P->OperationNode = Operation;
    return P;
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
  bool is(uint32_t I, std::string_view K) const {
    return I != None && S.Nodes.at(I).Kind == K;
  }
  std::u16string_view property(uint32_t I) {
    const auto P = child(I, "property");
    if (P == None)
      return {};
    const auto &N = S.Nodes[P];
    const auto *Name = !S.Nodes[I].flag("computed") && N.Kind == "Identifier"
                           ? N.text("name")
                       : N.Kind == "StringLiteral" ? N.text("value")
                                                   : nullptr;
    return Name ? std::u16string_view(*Name) : std::u16string_view();
  }
  bool meta(uint32_t I) {
    if (S.SourceType != "module" || !is(I, "MetaProperty"))
      return false;
    const auto Import = child(I, "meta"), Meta = child(I, "property");
    return is(Import, "Identifier") && is(Meta, "Identifier") &&
           S.Nodes[Import].text("name") &&
           *S.Nodes[Import].text("name") == u"import" &&
           S.Nodes[Meta].text("name") && *S.Nodes[Meta].text("name") == u"meta";
  }
  struct API {
    std::u16string_view Module;
    std::span<const std::u16string> Members;
  };
  std::optional<API> api(uint32_t I) {
    if (I == None || !O.Nodes.at(I) || O.Nodes[I]->Construction != None)
      return {};
    const auto &Origin = *O.Nodes[I];
    if (Origin.Request >= M.Requests.size())
      throw Error("invalid_source_origin_model");
    const auto Specifier = M.Requests[Origin.Request].Specifier;
    if (Specifier >= M.Names.size())
      throw Error("invalid_source_module_model");
    auto Members = std::span<const std::u16string>(Origin.Members);
    if (!Members.empty() && Members.front() == u"default")
      Members = Members.subspan(1);
    return API{M.Names[Specifier], Members};
  }
  Path concat(Path L, Path R, uint32_t I) {
    if (L->Kind == Kind::Unknown)
      return L;
    if (R->Kind == Kind::Unknown)
      return R;
    if ((L->Kind != Kind::Text && L->Kind != Kind::Path) ||
        R->Kind != Kind::Text) {
      if (L->Kind == Kind::Text && L->Text.empty() && R->Kind == Kind::Path)
        return R;
      return refuse("unsupported_path_concatenation");
    }
    if (L->Text.size() + R->Text.size() > MaxCapturedPathUnits)
      return refuse("path_length_exceeded");
    if (L->Kind == Kind::Path && L->Text.empty() && !R->Text.empty() &&
        !R->Text.starts_with(u'/'))
      return refuse("unknown_namespace_root_suffix");
    return make(L->Kind, L->Text + R->Text, L->RootNode, I);
  }
  Path pathCall(std::u16string_view Method, const std::vector<Path> &Args,
                uint32_t I) {
    for (const auto &P : Args)
      if (P->Kind == Kind::Unknown)
        return P;
      else if (P->Kind != Kind::Text && P->Kind != Kind::Path)
        return refuse("path_argument_not_string");
    if (Method == u"dirname") {
      if (Args.size() != 1)
        return refuse("unsupported_path_arity");
      auto Text = Args[0]->Text;
      if (Text.find_first_of(u"\\:\0", 0, 3) != Text.npos ||
          (Args[0]->Kind == Kind::Text && Text.starts_with(u'/')))
        return refuse("unsupported_path_syntax");
      while (!Text.empty() && Text.back() == u'/')
        Text.pop_back();
      if (Text.empty() && Args[0]->Kind == Kind::Path)
        return refuse("path_outside_captured_root");
      const auto Last = Text.rfind(u'/');
      Text = Last == Text.npos ? std::u16string() : Text.substr(0, Last);
      if (Text.empty() && Args[0]->Kind == Kind::Text)
        Text = u".";
      return make(Args[0]->Kind, Text, Args[0]->RootNode, I);
    }
    if (Method == u"normalize") {
      if (Args.size() != 1)
        return refuse("unsupported_path_arity");
      step(Args[0]->Text.size());
      const auto Text =
          normalize(Args[0]->Text, Args[0]->Kind == Kind::Path, true);
      return Text ? make(Args[0]->Kind, *Text, Args[0]->RootNode, I)
                  : refuse("path_normalization_refused");
    }
    if (Method != u"join" && Method != u"resolve")
      return refuse("unsupported_path_method");
    Kind K = Kind::Text;
    uint32_t Root = None;
    std::u16string Text;
    bool HasPart = false;
    for (const auto &P : Args) {
      step(P->Text.size() + 1);
      if (P->Kind == Kind::Path) {
        if (Method == u"join" && HasPart)
          return refuse("unsupported_path_root_position");
        Text = P->Text;
        K = Kind::Path;
        Root = P->RootNode;
        HasPart = true;
      } else if (!P->Text.empty()) {
        if (P->Text.starts_with(u'/') && (Method == u"resolve" || !HasPart))
          return refuse("external_absolute_path");
        if (Text.size() + P->Text.size() + 1 > MaxCapturedPathUnits)
          return refuse("path_length_exceeded");
        if (HasPart)
          Text += u'/';
        Text += P->Text;
        HasPart = true;
      }
    }
    if (Method == u"resolve" && K != Kind::Path)
      return refuse("unknown_working_directory");
    step(Text.size());
    const auto Normalized = normalize(Text, K == Kind::Path, Method == u"join");
    return Normalized ? make(K, *Normalized, Root, I)
                      : refuse("path_normalization_refused");
  }
  Path call(uint32_t I, unsigned Depth) {
    const auto Callee = child(I, "callee");
    std::vector<uint32_t> ArgNodes;
    for (const auto &C : S.Nodes[I].Children) {
      step();
      if (C.Field == "arguments")
        ArgNodes.push_back(C.Index);
    }
    if (S.Nodes[I].flag("optional") ||
        S.Nodes[I].Kind == "OptionalCallExpression")
      return refuse("optional_path_call");
    const auto API = api(Callee);
    bool URLConstructor = false;
    if (S.Nodes[I].Kind == "NewExpression") {
      if (API && (API->Module == u"url" || API->Module == u"node:url") &&
          API->Members.size() == 1 && API->Members[0] == u"URL")
        URLConstructor = true;
      else if (is(Callee, "Identifier") && References[Callee] != None &&
               !URLWritten) {
        const auto &R = B.References[References[Callee]];
        const auto *Name = S.Nodes[Callee].text("name");
        URLConstructor = R.Resolution == "external" && Name && *Name == u"URL";
      }
      if (!URLConstructor || ArgNodes.size() != 2)
        return refuse("unsupported_path_constructor");
      const auto Relative = node(ArgNodes[0], Depth + 1),
                 Base = node(ArgNodes[1], Depth + 1);
      if (Relative->Kind != Kind::Text || (Base->Kind != Kind::FileURLString &&
                                           Base->Kind != Kind::FileURLObject))
        return refuse("unsupported_file_url_base");
      if (Relative->Text.empty() || Relative->Text.starts_with(u'/') ||
          Relative->Text.find_first_of(u"%?#\\:") != Relative->Text.npos ||
          Relative->Text.front() <= u' ' || Relative->Text.back() <= u' ')
        return refuse("unsupported_file_url_reference");
      auto BaseDirectory = Base->Text;
      const auto Slash = BaseDirectory.rfind(u'/');
      BaseDirectory = Slash == BaseDirectory.npos
                          ? std::u16string()
                          : BaseDirectory.substr(0, Slash + 1);
      if (BaseDirectory.size() + Relative->Text.size() > MaxCapturedPathUnits)
        return refuse("path_length_exceeded");
      auto Text = normalize(BaseDirectory + Relative->Text, true, true);
      const auto RelativeText = std::u16string_view(Relative->Text);
      const bool DirectoryURL = RelativeText == u"." || RelativeText == u".." ||
                                RelativeText.ends_with(u"/.") ||
                                RelativeText.ends_with(u"/..");
      if (Text && DirectoryURL && !Text->empty() && !Text->ends_with(u'/'))
        *Text += u'/';
      return Text ? make(Kind::FileURLObject, *Text, Base->RootNode, I)
                  : refuse("file_url_path_refused");
    }
    if (API) {
      auto Members = API->Members;
      if (API->Module == u"path" || API->Module == u"node:path" ||
          API->Module == u"path/posix" || API->Module == u"node:path/posix" ||
          API->Module == u"path/win32" || API->Module == u"node:path/win32") {
        if ((Members.size() == 2 &&
             (Members[0] == u"posix" || Members[0] == u"win32")) ||
            (API->Module != u"path" && API->Module != u"node:path"))
          return refuse("path_flavor_requires_target_context");
        if (Members.size() != 1)
          return refuse("unsupported_path_callee");
        std::vector<Path> Args;
        for (auto Arg : ArgNodes)
          Args.push_back(node(Arg, Depth + 1));
        return pathCall(Members[0], Args, I);
      }
      if ((API->Module == u"electron" || API->Module == u"electron/main") &&
          Members.size() == 2 && Members[0] == u"app" &&
          Members[1] == u"getAppPath" && ArgNodes.empty())
        return make(Kind::Path, AppDirectory, I, I);
      if ((API->Module == u"url" || API->Module == u"node:url") &&
          Members.size() == 1 && Members[0] == u"fileURLToPath" &&
          ArgNodes.size() == 1) {
        const auto URL = node(ArgNodes[0], Depth + 1);
        if (URL->Kind == Kind::FileURLString ||
            URL->Kind == Kind::FileURLObject)
          return make(Kind::Path, URL->Text, URL->RootNode, I);
        return refuse("unsupported_file_url_value");
      }
    }
    if (is(Callee, "MemberExpression") && property(Callee) == u"toString" &&
        ArgNodes.empty()) {
      const auto URL = node(child(Callee, "object"), Depth + 1);
      if (URL->Kind == Kind::FileURLObject)
        return make(Kind::FileURLString, URL->Text, URL->RootNode, I);
    }
    return refuse("unsupported_path_callee");
  }
  Path node(uint32_t I, unsigned Depth = 0) {
    step();
    if (Depth > 64)
      throw Error("source_path_budget_exceeded");
    if (I == None)
      return refuse("missing_path_expression");
    if (I >= S.Nodes.size())
      throw Error("invalid_source_path_model");
    if (Visiting[I] == 2)
      return A.Nodes[I];
    if (Visiting[I] == 1)
      return refuse("cyclic_path_initializer");
    Visiting[I] = 1;
    Path P;
    const auto &N = S.Nodes[I];
    if (V.Nodes[I].Status == "constant" && V.Nodes[I].Value &&
        V.Nodes[I].Value->Kind == PrimitiveKind::String) {
      P = make(Kind::Text, V.Nodes[I].Value->String);
    } else if (N.Kind == "Identifier" && References[I] != None) {
      const auto &R = B.References[References[I]];
      if (R.Resolution == "lexical_binding" && R.Binding != None) {
        const auto &Binding = B.Bindings[R.Binding];
        if (Written[R.Binding] || Ambiguous[R.Binding] || Binding.Conflicting)
          P = refuse("written_or_conflicting_path_binding");
        else if (Binding.Kind == "commonjs_parameter" && Binding.Implicit &&
                 !Declared[R.Binding] && S.SourceType == "commonjs") {
          const auto &Name = B.Names.at(Binding.Name);
          if (Name == u"__dirname")
            P = make(Kind::Path, SourceDirectory, I);
          else if (Name == u"__filename")
            P = make(Kind::Path, SourceFile, I);
        } else if (Initializers[R.Binding] != None)
          P = node(Initializers[R.Binding], Depth + 1);
      }
    } else if (N.Kind == "MemberExpression" && meta(child(I, "object"))) {
      const auto Name = property(I);
      if (Name == u"url")
        P = make(Kind::FileURLString, SourceFile, I);
      else if (Name == u"dirname")
        P = make(Kind::Path, SourceDirectory, I);
      else if (Name == u"filename")
        P = make(Kind::Path, SourceFile, I);
    } else if (N.Kind == "MemberExpression" && property(I) == u"href") {
      const auto URL = node(child(I, "object"), Depth + 1);
      if (URL->Kind == Kind::FileURLObject)
        P = make(Kind::FileURLString, URL->Text, URL->RootNode, I);
    } else if (N.Kind == "BinaryExpression" && N.text("operator") &&
               *N.text("operator") == u"+") {
      P = concat(node(child(I, "left"), Depth + 1),
                 node(child(I, "right"), Depth + 1), I);
    } else if (N.Kind == "TemplateLiteral") {
      std::vector<uint32_t> Quasis, Expressions;
      for (const auto &C : N.Children) {
        step();
        (C.Field == "quasis" ? Quasis : Expressions).push_back(C.Index);
      }
      if (Quasis.size() != Expressions.size() + 1)
        throw Error("invalid_source_model");
      P = node(Quasis[0], Depth + 1);
      for (size_t J = 0; J < Expressions.size(); ++J)
        P = concat(concat(P, node(Expressions[J], Depth + 1), I),
                   node(Quasis[J + 1], Depth + 1), I);
    } else if (N.Kind == "ConditionalExpression") {
      const auto Test = child(I, "test");
      if (Test != None && V.Nodes[Test].Status == "constant" &&
          V.Nodes[Test].Value)
        P = node(child(I, sourcePrimitiveTruthy(*V.Nodes[Test].Value)
                              ? "consequent"
                              : "alternate"),
                 Depth + 1);
    } else if (N.Kind == "CallExpression" ||
               N.Kind == "OptionalCallExpression" ||
               N.Kind == "NewExpression") {
      P = call(I, Depth);
    }
    if (!P)
      P = refuse("unsupported_path_expression");
    A.Nodes[I] = P;
    Visiting[I] = 2;
    return P;
  }
  void run(std::span<const uint32_t> Requested) {
    step(validateSourceModel(S));
    if (B.SourceID != S.ID || M.SourceID != S.ID || O.SourceID != S.ID ||
        V.SourceID != S.ID || O.BindingID != B.ID || O.ModuleID != M.ID ||
        M.BindingID != B.ID || B.Bindings.size() > 2 * MaxJavaScriptNodes ||
        B.Declarations.size() > 2 * MaxJavaScriptNodes ||
        B.References.size() > MaxJavaScriptNodes || Requested.size() > 10000)
      throw Error("invalid_source_path_inputs");
    if (O.Status != "partial" || O.Nodes.size() != S.Nodes.size() ||
        V.Nodes.size() != S.Nodes.size())
      throw Error("source_path_evidence_unavailable");
    if (Context.SourceMemberPath.empty() ||
        Context.SourceMemberPath.size() > 4 * MaxCapturedPathUnits ||
        Context.ApplicationDirectory.size() > 4 * MaxCapturedPathUnits)
      throw Error("invalid_captured_path_context");
    SourceFile = utf16(Context.SourceMemberPath);
    AppDirectory = utf16(Context.ApplicationDirectory);
    if (!AppDirectory.empty() && AppDirectory.back() != u'/')
      throw Error("invalid_captured_path_context");
    if (!AppDirectory.empty())
      AppDirectory.pop_back();
    const auto CheckFile = normalize(SourceFile, true, false),
               CheckApp = normalize(AppDirectory, true, false);
    if (!CheckFile || !CheckApp || *CheckFile != SourceFile ||
        *CheckApp != AppDirectory || SourceFile.starts_with(u'/') ||
        AppDirectory.starts_with(u'/'))
      throw Error("invalid_captured_path_context");
    const auto Slash = SourceFile.rfind(u'/');
    SourceDirectory = Slash == SourceFile.npos ? std::u16string()
                                               : SourceFile.substr(0, Slash);
    References.assign(S.Nodes.size(), None);
    Declarations.assign(S.Nodes.size(), None);
    Visiting.resize(S.Nodes.size());
    A.Nodes.resize(S.Nodes.size());
    Initializers.assign(B.Bindings.size(), None);
    Written.resize(B.Bindings.size());
    Ambiguous.resize(B.Bindings.size());
    Declared.resize(B.Bindings.size());
    for (uint32_t RI = 0; RI < B.References.size(); ++RI) {
      step();
      const auto &R = B.References[RI];
      if (R.Node >= S.Nodes.size() ||
          (R.Binding != None && R.Binding >= B.Bindings.size()) ||
          References[R.Node] != None)
        throw Error("invalid_source_binding_model");
      References[R.Node] = RI;
      if (R.Access == "write" || R.Access == "read_write") {
        if (R.Binding != None)
          Written[R.Binding] = true;
        else if (S.Nodes[R.Node].text("name") &&
                 *S.Nodes[R.Node].text("name") == u"URL")
          URLWritten = true;
      }
    }
    for (const auto &D : B.Declarations) {
      step();
      if (D.Binding >= B.Bindings.size() ||
          (D.Node != None && D.Node >= S.Nodes.size()))
        throw Error("invalid_source_binding_model");
      if (D.Node != None) {
        Declared[D.Binding] = true;
        if (Declarations[D.Node] != None && Declarations[D.Node] != D.Binding)
          throw Error("invalid_source_binding_model");
        Declarations[D.Node] = D.Binding;
      }
    }
    for (uint32_t I = 0; I < S.Nodes.size(); ++I) {
      step();
      if (S.Nodes[I].Kind != "VariableDeclarator")
        continue;
      const auto ID = child(I, "id"), Init = child(I, "init");
      if (!is(ID, "Identifier") || Declarations[ID] == None || Init == None)
        continue;
      const auto Binding = Declarations[ID];
      if (Initializers[Binding] != None)
        Ambiguous[Binding] = true;
      Initializers[Binding] = Init;
    }
    for (auto I : Requested)
      node(I);
    A.Status = "partial";
    A.Reason = "captured_roots_are_syntactic_candidates";
  }
};
} // namespace

SourcePaths analyzeSourcePaths(const SourceAnalysis &S,
                               const SourceBindingAnalysis &B,
                               const SourceModuleAnalysis &M,
                               const SourceOrigins &O,
                               const SourceValueAnalysis &V,
                               const CapturedPathContext &Context,
                               std::span<const uint32_t> Requested) {
  Resolver R{S, B, M, O, V, Context};
  R.A.SourceID = S.ID;
  R.A.ID = identity("source-paths",
                    {S.ID, B.ID, M.ID, O.ID, V.ID, Context.NamespaceID,
                     Context.ManifestID, Context.SourceMemberPath,
                     Context.ApplicationDirectory, CapturedPathProfile});
  std::vector<uint32_t> Selection;
  if (Requested.size() <= 10000) {
    Selection.assign(Requested.begin(), Requested.end());
    std::sort(Selection.begin(), Selection.end());
    Selection.erase(std::unique(Selection.begin(), Selection.end()),
                    Selection.end());
    std::vector<std::string_view> Fields{R.A.ID};
    for (auto I : Selection)
      Fields.push_back(I < S.Nodes.size() ? std::string_view(S.Nodes[I].ID)
                                          : std::string_view("invalid-node"));
    R.A.ID = identity("source-path-selection", Fields);
  }
  try {
    if (Requested.size() > 10000)
      throw Error("invalid_source_path_inputs");
    R.run(Selection);
  } catch (const Error &E) {
    R.A.Nodes.clear();
    R.A.Reason = E.what();
    R.A.Status = R.A.Reason == "source_path_budget_exceeded" ? "budget_exceeded"
                                                             : "unavailable";
  }
  return std::move(R.A);
}

CapturedPathTarget capturedEntryTarget(const CapturedPathValue &V,
                                       std::string_view AppDirectory,
                                       std::string_view EntryKind) {
  if (V.Kind == Kind::Unknown)
    return {{}, V.Reason};
  if (V.Text.size() > MaxCapturedPathUnits ||
      AppDirectory.size() > 4 * MaxCapturedPathUnits)
    return {{}, "path_length_exceeded"};
  std::u16string Base;
  try {
    Base = utf16(AppDirectory);
  } catch (const Error &) {
    return {{}, "invalid_captured_path_context"};
  }
  if (!Base.empty()) {
    if (!Base.ends_with(u'/') || Base.starts_with(u'/'))
      return {{}, "invalid_captured_path_context"};
    auto Components = Base.substr(0, Base.size() - 1);
    const auto Normalized = normalize(Components, true, false);
    if (!Normalized || *Normalized != Components)
      return {{}, "invalid_captured_path_context"};
  }
  std::optional<std::u16string> Text;
  if (EntryKind == "renderer_url") {
    if (V.Kind != Kind::FileURLString)
      return {{}, "url_base_not_captured"};
    if (V.Text.empty() || V.Text.ends_with(u'/'))
      return {{}, "file_url_directory_reference"};
    Text = normalize(V.Text, true, false);
  } else if (EntryKind == "window_preload") {
    if (V.Kind != Kind::Path)
      return {{}, "preload_absolute_base_not_captured"};
    Text = normalize(V.Text, true, false);
    auto Original = std::u16string_view(V.Text);
    if (Original.starts_with(u'/'))
      Original.remove_prefix(1); // Virtual captured-root delimiter only.
    if (!Text || *Text != Original)
      return {{}, "preload_path_requires_runtime_normalization"};
  } else if (EntryKind == "renderer_file") {
    if (V.Kind != Kind::Text && V.Kind != Kind::Path)
      return {{}, "renderer_file_argument_not_path"};
    if (V.Kind == Kind::Text &&
        (V.Text.starts_with(u'/') || V.Text.find(u':') != V.Text.npos))
      return {{}, "external_absolute_path"};
    auto Path = V.Kind == Kind::Path ? V.Text : Base + V.Text;
    if (Path.size() > MaxCapturedPathUnits)
      return {{}, "path_length_exceeded"};
    Text = normalize(Path, true, false);
  } else {
    return {{}, "unsupported_entry_kind"};
  }
  if (!Text)
    return {{}, "entry_path_normalization_refused"};
  const auto Path = utf8(*Text);
  if (!Path)
    return {{}, "unsupported_path_encoding"};
  if (!Path->starts_with(AppDirectory) &&
      (AppDirectory.empty() ||
       *Path != AppDirectory.substr(0, AppDirectory.size() - 1)))
    return {{}, "entry_outside_manifest_scope"};
  return {*Path, "captured_path_candidate"};
}
} // namespace neverd::web
