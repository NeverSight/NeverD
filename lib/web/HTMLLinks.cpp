//===- HTMLLinks.cpp - Confined HTML file candidates -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Confined HTML file candidates.
///
//===----------------------------------------------------------------------===//

#include "HTMLFiles.h"
#include "Internal.h"

#include "neverd/web/HTML.h"

#include <algorithm>

namespace neverd::web {
namespace {
bool space(char C) {
  return C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f';
}
std::string_view trim(std::string_view S) {
  while (!S.empty() && space(S.front()))
    S.remove_prefix(1);
  while (!S.empty() && space(S.back()))
    S.remove_suffix(1);
  return S;
}
unsigned hex(char C) {
  if (C >= '0' && C <= '9')
    return C - '0';
  if (C >= 'a' && C <= 'f')
    return C - 'a' + 10;
  if (C >= 'A' && C <= 'F')
    return C - 'A' + 10;
  return 16;
}
bool canonicalBase(std::string_view Path, bool Directory) {
  if (Path.size() > MaxHTMLPathBytes || (Path.empty() && !Directory))
    return false;
  while (!Path.empty()) {
    const auto Slash = Path.find('/');
    validateMemberName(Path.substr(0, Slash));
    if (Slash == Path.npos)
      return true;
    Path.remove_prefix(Slash + 1);
    if (Path.empty())
      return false;
  }
  return Directory;
}
} // namespace

HTMLLocalURL resolveHTMLLocalURL(std::string_view BasePath, bool BaseDirectory,
                                 std::string_view Reference) {
  HTMLLocalURL R;
  R.Status = "unsupported_local_url";
  if (Reference.size() > MaxHTMLAttributeBytes || !validUtf8(Reference))
    return R;
  try {
    if (!canonicalBase(BasePath, BaseDirectory))
      return R;
    Reference = trim(Reference);
    // The portable subset does not silently apply URL control stripping or
    // platform-dependent backslash, drive-letter or network-path rules.
    for (unsigned char C : Reference)
      if (C < 32 || C == 127)
        return R;
    const auto Fragment = Reference.find('#');
    R.Fragment = Fragment != Reference.npos;
    Reference = Reference.substr(0, Fragment);
    const auto Query = Reference.find('?');
    R.Query = Query != Reference.npos;
    Reference = Reference.substr(0, Query);
    if (Reference.empty()) {
      R.Path = BasePath;
      R.Directory = BaseDirectory;
      R.Status = "local_url_candidate";
      return R;
    }
    if (Reference.front() == '/' || Reference.find(':') != Reference.npos ||
        Reference.find('\\') != Reference.npos)
      return R;
    std::string Decoded;
    for (size_t I = 0; I < Reference.size(); ++I) {
      auto C = Reference[I];
      if (C == '%') {
        if (I + 2 >= Reference.size() || hex(Reference[I + 1]) >= 16 ||
            hex(Reference[I + 2]) >= 16)
          return R;
        C = char(hex(Reference[I + 1]) * 16 + hex(Reference[I + 2]));
        I += 2;
        if (C == '/' || C == '\\')
          return R;
      }
      if (Decoded.size() == MaxHTMLPathBytes)
        return R;
      Decoded += C;
    }
    std::vector<std::string_view> Parts;
    auto Prefix = BaseDirectory
                      ? BasePath
                      : BasePath.substr(0, BasePath.rfind('/') == BasePath.npos
                                               ? 0
                                               : BasePath.rfind('/'));
    while (!Prefix.empty()) {
      const auto Slash = Prefix.find('/');
      Parts.push_back(Prefix.substr(0, Slash));
      if (Slash == Prefix.npos)
        break;
      Prefix.remove_prefix(Slash + 1);
    }
    std::string_view Tail(Decoded);
    while (true) {
      const auto Slash = Tail.find('/');
      const auto Part = Tail.substr(0, Slash);
      const bool Last = Slash == Tail.npos;
      if (Part == "..") {
        if (Parts.empty()) {
          R.Status = "outside_snapshot_root";
          return R;
        }
        Parts.pop_back();
      } else if (Part != ".") {
        if (Part.empty()) {
          if (!Last)
            return R;
        } else {
          validateMemberName(Part);
          Parts.push_back(Part);
        }
      }
      if (Last) {
        R.Directory = Part.empty() || Part == "." || Part == "..";
        break;
      }
      Tail.remove_prefix(Slash + 1);
    }
    for (const auto Part : Parts) {
      if (Part.size() + (R.Path.empty() ? 0 : 1) >
          MaxHTMLPathBytes - R.Path.size()) {
        R.Path.clear();
        return R;
      }
      if (!R.Path.empty())
        R.Path += '/';
      R.Path += Part;
    }
    R.Status = "local_url_candidate";
  } catch (const Error &E) {
    if (std::string_view(E.what()) != "unsafe_member_name")
      throw;
    R.Path.clear();
  }
  return R;
}

HTMLLinks linkHTMLScripts(const HTMLDocument &Document, const Snapshot &Input) {
  HTMLLinks R;
  R.ID = identity("html-script-links", {Document.ID, Input.ID, HTMLProfile});
  R.Status = "partial";
  if (Document.Status != "partial") {
    R.Status = "unavailable";
    return R;
  }
  const auto Step = [&](uint64_t N = 1) {
    if (N > MaxHTMLLinkSteps - R.Steps)
      throw Error("html_link_work_budget_exceeded");
    R.Steps += N;
  };
  try {
    if (Input.Artifacts.size() > MaxHTMLNamespaceEntries ||
        Document.Scripts.size() > MaxHTMLRecords ||
        Document.Bases.size() > MaxHTMLRecords - Document.Scripts.size())
      throw Error("invalid_html_link_model");
    const HTMLFiles Members(Input, Document.ArtifactID, Document.BlobHash,
                            Step);
    const auto Default = Members.documentBase();
    std::vector<HTMLLocalURL> Bases;
    for (const auto &B : Document.Bases)
      Bases.push_back(htmlDeclaredBase(Default, B, Step));
    for (const auto &S : Document.Scripts) {
      Step();
      HTMLScriptLink L;
      if (S.HasSource) {
        const auto Fragment = S.Reference.find('#');
        L.Fragment = Fragment != S.Reference.npos;
        L.Query = std::string_view(S.Reference).substr(0, Fragment).find('?') !=
                  std::string_view::npos;
      }
      if (S.Base != NoHTMLIndex &&
          (S.Base >= Bases.size() || !Document.Bases[S.Base].Selected))
        throw Error("invalid_html_link_model");
      const auto &Base = S.Base == NoHTMLIndex ? Default : Bases[S.Base];
      Step(Base.Path.size() + S.Reference.size() + 1);
      L.Base = Base;
      L.BaseStatus = L.Base.Status;
      if (S.Context != "html_source_candidate")
        L.Status = "non_entry_context";
      else if (S.SourceType.empty())
        L.Status = "non_javascript_data";
      else if (!S.Closed)
        L.Status = "unterminated_script";
      else if (!S.HasSource) {
        L.Status = S.BodyStatus;
        L.ArtifactID = S.InlineArtifactID;
      } else if (trim(S.Reference).empty())
        L.Status = "empty_source_reference";
      else if (L.Base.Status != "local_url_candidate")
        L.Status = "base_url_unavailable";
      else {
        Step(L.Base.Path.size() + S.Reference.size() + 1);
        const auto Target =
            resolveHTMLLocalURL(L.Base.Path, L.Base.Directory, S.Reference);
        if (Target.Status != "local_url_candidate")
          L.Status = Target.Status;
        else if (Target.Directory)
          L.Status = "directory_target";
        else if (const auto *TargetArtifact = Members.find(Target.Path);
                 !TargetArtifact)
          L.Status = "not_in_snapshot";
        else if (TargetArtifact->Directory)
          L.Status = "directory_target";
        else {
          L.Status = "exact_admitted_file_candidate";
          L.ArtifactID = TargetArtifact->ID;
        }
      }
      R.Scripts.push_back(std::move(L));
    }
  } catch (const Error &E) {
    if (std::string_view(E.what()) != "html_link_work_budget_exceeded")
      throw;
    R.Status = "budget_exceeded";
    R.Scripts.clear();
  }
  return R;
}
} // namespace neverd::web
