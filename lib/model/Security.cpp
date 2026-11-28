//===- Security.cpp - Static loader, dependency and provenance evidence ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Inspect local package declarations and source tokens as data.
//===----------------------------------------------------------------------===//

#include "ModelInternal.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"

namespace neverd::model {
namespace {
bool plain(llvm::StringRef S) {
  return !S.empty() && S.size() <= 160 && llvm::all_of(S, [](char C) {
    return llvm::isAlnum(C) || C == '-' || C == '_' || C == '.' || C == '+';
  });
}
void dependency(Package &P, const std::string &File, Json Edge) {
  P.B.entry();
  Edge["source"] = File;
  if (P.DependencyCount++ >= P.B.Opts.Offset &&
      P.Dependencies.size() < P.B.Opts.Limit)
    P.Dependencies.push_back(std::move(Edge));
}
void localReference(Package &P, const std::string &File,
                    const std::string &Field, llvm::StringRef Raw,
                    bool IsLoader) {
  Json E = {{"field", Field},
            {"kind", IsLoader ? "loader" : "base"},
            {"reference_sha256", digest(Raw.str())}};
  std::string Local;
  if (Raw.empty() || Raw.size() > limits::kNameBytes || Raw.contains(char(0))) {
    E["resolution"] = "invalid-reference";
    dependency(P, File, std::move(E));
    return;
  }
  if (Raw.contains("://") || (IsLoader && Raw.contains("--")))
    E["resolution"] = "remote-reference";
  else if (Raw.starts_with("/") || Raw.contains("..") || Raw.contains('\\') ||
           Raw.contains(':'))
    E["resolution"] = "unsafe-path";
  else if (IsLoader) {
    auto [Module, Class] = Raw.rsplit('.');
    if (!plain(Module) || !plain(Class))
      E["resolution"] = "unsupported-class-name";
    else {
      Local = Module.str();
      std::replace(Local.begin(), Local.end(), '.', '/');
      Local += ".py";
    }
  } else if (Raw.starts_with("./") || fs::exists(P.Root / utf8Path(Raw.str())))
    Local = Raw.str();
  else
    E["resolution"] = "unverified-model-identifier";
  if (!Local.empty()) {
    try {
      memberPath(P.Root, utf8Path(Local));
      E["resolution"] = "local";
      E["member"] = Local;
    } catch (const Error &Ex) {
      E["resolution"] =
          std::string_view(Ex.what()).find("escapes") != std::string_view::npos
              ? "unsafe-path"
              : "missing-local";
    }
  }
  if (E["resolution"] == "unsafe-path" || E["resolution"] == "missing-local")
    P.gap(File, "unavailable declared local dependency", E);
  dependency(P, File, std::move(E));
}
void pythonSource(Package &P, const std::string &Name, llvm::StringRef Text) {
  size_t Pos = 0, PreviousAt = 0;
  llvm::StringRef Previous;
  bool ModuleNext = false, FromClause = false;
  while (Pos < Text.size()) {
    P.B.tick();
    const size_t At = Pos;
    char C = Text[Pos++];
    if (C == '#') {
      Pos = Text.find('\n', Pos);
      if (Pos == Text.npos)
        break;
      continue;
    }
    if (C == '\'' || C == '"') {
      const bool Triple = Text.substr(Pos, 2) == std::string(2, C);
      if (Triple)
        Pos += 2;
      bool Closed = false;
      while (Pos < Text.size()) {
        if (Text[Pos] == '\\') {
          Pos += std::min<size_t>(2, Text.size() - Pos);
          continue;
        }
        if (Text[Pos] == C &&
            (!Triple || Text.substr(Pos, 3) == std::string(3, C))) {
          Pos += Triple ? 3 : 1;
          Closed = true;
          break;
        }
        if (!Triple && Text[Pos] == '\n')
          break;
        ++Pos;
      }
      if (!Closed) {
        P.gap(Name, "unterminated Python string", {{"offset", At}});
        return;
      }
      Previous = {};
      ModuleNext = false;
      continue;
    }
    if (llvm::isSpace(C)) {
      if (C == '\n') {
        Previous = {};
        ModuleNext = false;
        FromClause = false;
      }
      continue;
    }
    if (llvm::isAlpha(C) || C == '_') {
      while (Pos < Text.size() &&
             (llvm::isAlnum(Text[Pos]) || Text[Pos] == '_' || Text[Pos] == '.'))
        ++Pos;
      auto Word = Text.slice(At, Pos);
      if (ModuleNext && plain(Word)) {
        dependency(P, Name,
                   {{"kind", "import-token"},
                    {"name", Word.str()},
                    {"offset", At},
                    {"length", Word.size()},
                    {"resolution", "declared-module"}});
      }
      ModuleNext = Word == "from" || (Word == "import" && !FromClause);
      if (Word == "from")
        FromClause = true;
      if (Word == "import")
        FromClause = false;
      Previous = Word;
      PreviousAt = At;
      continue;
    }
    if (C == '(' &&
        (Previous == "__import__" || Previous == "importlib.import_module" ||
         Previous == "torch.ops.load_library" || Previous == "ctypes.CDLL" ||
         Previous == "ctypes.PyDLL")) {
      P.finding(
          "package.loader-call", Name, "review", "loader-source", "lexical",
          "Review the call target and its arguments. This source location "
          "identifies a call spelling.",
          {{"offset", PreviousAt}, {"end", Pos}, {"call", Previous.str()}});
    }
    Previous = {};
    ModuleNext = false;
  }
}
void manifest(Package &P, const std::string &Name, llvm::StringRef Text) {
  uint64_t Offset = 0;
  while (!Text.empty()) {
    P.B.tick();
    auto [Line, Rest] = Text.split('\n');
    auto Dep = Line.split('#').first.trim();
    if (!Dep.empty()) {
      Json E = {{"kind", "requirement"},
                {"offset", Offset},
                {"length", Line.size()},
                {"reference_sha256", digest(Dep.str())},
                {"resolution", "unresolved-expression"}};
      auto [Library, Version] = Dep.split("==");
      if (plain(Library) && plain(Version) && !Version.contains('*')) {
        E["name"] = Library.str();
        E["version"] = Version.str();
        E["resolution"] = "declared-exact-version";
      } else if (Dep.contains("://"))
        E["resolution"] = "remote-reference";
      else if (plain(Dep)) {
        E["name"] = Dep.str();
        E["resolution"] = "declared-name";
      }
      dependency(P, Name, std::move(E));
    }
    Offset += Line.size() + 1;
    Text = Rest;
  }
}
} // namespace

void inspectReferences(Package &P, const std::string &Name, const Json &J) {
  if (auto A = J.find("auto_map"); A != J.end() && A->is_object()) {
    for (auto &[Key, V] : A->items()) {
      if (V.is_string())
        localReference(P, Name, "auto_map/" + Key,
                       V.get_ref<const std::string &>(), true);
      else if (V.is_array()) {
        for (size_t I = 0; I < V.size(); ++I)
          if (V[I].is_string())
            localReference(P, Name, "auto_map/" + Key + "/" + std::to_string(I),
                           V[I].get_ref<const std::string &>(), true);
      } else
        P.gap(Name, "unsupported loader reference",
              {{"field", "auto_map/" + Key}});
    }
  }
  if (auto B = J.find("base_model_name_or_path");
      B != J.end() && B->is_string())
    localReference(P, Name, "base_model_name_or_path",
                   B->get_ref<const std::string &>(), false);
  for (auto Key :
       {"license", "revision", "author", "publisher", "_commit_hash"}) {
    auto V = J.find(Key);
    if (V == J.end() || !V->is_string())
      continue;
    auto &S = V->get_ref<const std::string &>();
    Json Claim = {{"source", Name},
                  {"field", Key},
                  {"value_sha256", digest(S)},
                  {"verification", "producer-claim"}};
    if (plain(S))
      Claim["value"] = S;
    P.Provenance.push_back(std::move(Claim));
  }
}

void inspectComponent(Package &P, const std::string &Name,
                      std::string_view Kind) {
  auto &F = P.file(Name);
  Json A = {{"id", Name}, {"format", Kind}, {"size", F.size()}};
  if (Kind == "native-library") {
    auto H = F.header(0, std::min<uint64_t>(4096, F.size()), P.B);
    const auto Magic = llvm::identify_magic(H);
    const char *Binary =
        Magic == llvm::file_magic::elf_shared_object   ? "ELF"
        : Magic == llvm::file_magic::pecoff_executable ? "PE"
        : (Magic == llvm::file_magic::macho_dynamically_linked_shared_lib ||
           Magic == llvm::file_magic::macho_bundle ||
           Magic == llvm::file_magic::macho_universal_binary)
            ? "Mach-O"
            : nullptr;
    A["native_analysis_available"] = Binary != nullptr;
    if (Binary) {
      A["observed_format"] = Binary;
      A["coverage"] = "native-header-identification";
      P.finding(
          "package.native-component", Name, "info", "native-component",
          "observed",
          "Open this component in native static analysis to inspect its code.",
          {{"offset", 0}, {"length", H.size()}, {"format", Binary}});
    } else
      P.gap(Name, "native filename hint with unrecognized header");
  } else if (Kind == "compiled-engine") {
    A["coverage"] = "filename-hint";
    P.gap(Name, "compiled engine deserialization and semantics unsupported");
  } else if (F.size() > limits::kTemplateBytes) {
    P.gap(Name, "component content exceeds static text byte limit");
  } else {
    auto Text = F.header(0, F.size(), P.B);
    A["file_sha256"] = digest(Text);
    if (Kind == "python-source") {
      A["coverage"] = "lexical imports and loader call spellings";
      pythonSource(P, Name, Text);
    } else if (Kind == "dependency-manifest") {
      if (llvm::StringRef(Name).ends_with("requirements.txt"))
        manifest(P, Name, Text);
      else
        P.gap(Name, "manifest inventoried; dependency syntax unsupported");
    } else {
      auto Base = utf8Path(Name).filename().string();
      const bool Signature = Base.ends_with(".sig") ||
                             Base.ends_with(".attestation") ||
                             Base.ends_with(".sigstore.json");
      P.Provenance.push_back(
          {{"source", Name},
           {"kind", Signature                     ? "signature-or-attestation"
                    : Base.starts_with("LICENSE") ? "license-file"
                                                  : "model-card"},
           {"sha256", A.at("file_sha256")},
           {"verification",
            Signature ? "present-unverified" : "content-hashed"}});
      llvm::StringRef Lines(Text);
      if (Base == "README.md" && Lines.consume_front("---\n")) {
        while (!Lines.empty()) {
          auto [Line, Rest] = Lines.split('\n');
          if (Line.trim() == "---")
            break;
          auto [Key, Value] = Line.split(':');
          if ((Key == "license" || Key == "author" || Key == "revision" ||
               Key == "publisher") &&
              plain(Value.trim()))
            P.Provenance.push_back(
                {{"source", Name},
                 {"field", Key.str()},
                 {"value", Value.trim().str()},
                 {"offset", size_t(Line.data() - Text.data())},
                 {"verification", "producer-claim"}});
          Lines = Rest;
        }
      }
    }
  }
  P.Artifacts.push_back(std::move(A));
}
} // namespace neverd::model
