//===- HTMLFiles.h - Captured HTML file index --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Captured HTML file index.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "neverd/web/Error.h"
#include "neverd/web/HTML.h"

#include <algorithm>
#include <functional>
#include <map>

namespace neverd::web {
/// Exact captured-file comparison shared by HTML entry and module consumers.
/// The caller owns the snapshot and charges work in its own budget domain.
class HTMLFiles {
  std::function<void(uint64_t)> Charge;
  struct Less {
    const std::function<void(uint64_t)> *Charge;
    bool operator()(std::string_view A, std::string_view B) const {
      (*Charge)(std::min(A.size(), B.size()) + 1);
      return A < B;
    }
  };
  std::map<std::string_view, const Artifact *, Less> Members;
  const Artifact *Origin = nullptr;
  bool Root = false;

public:
  HTMLFiles(const Snapshot &Input, std::string_view OriginID,
            std::string_view Hash, std::function<void(uint64_t)> Charge)
      : Charge(std::move(Charge)), Members(Less{&this->Charge}) {
    if (Input.Artifacts.size() > MaxHTMLNamespaceEntries)
      throw Error("invalid_html_link_model");
    for (const auto &A : Input.Artifacts) {
      this->Charge(A.MemberPath.size() + A.ID.size() + 1);
      if (!Members.emplace(A.MemberPath, &A).second)
        throw Error("invalid_artifact_snapshot");
      if (A.MemberPath.empty())
        Root = A.Directory;
      if (A.ID == OriginID) {
        if (Origin)
          throw Error("invalid_artifact_snapshot");
        Origin = &A;
      }
    }
    if (Origin && (Origin->Directory || Origin->BlobHash != Hash))
      throw Error("html_artifact_mismatch");
  }
  HTMLFiles(const HTMLFiles &) = delete;
  HTMLFiles &operator=(const HTMLFiles &) = delete;
  HTMLLocalURL documentBase() const {
    if (!Root || !Origin || Origin->MemberPath.empty())
      return {{}, "directory_origin_unavailable"};
    Charge(Origin->MemberPath.size());
    return resolveHTMLLocalURL(Origin->MemberPath, false, "");
  }
  const Artifact *find(std::string_view Path) const {
    const auto I = Members.find(Path);
    return I == Members.end() ? nullptr : I->second;
  }
};

inline HTMLLocalURL
htmlDeclaredBase(const HTMLLocalURL &Default, const HTMLBase &B,
                 const std::function<void(uint64_t)> &Charge) {
  Charge(B.Reference.size() + Default.Path.size() + 1);
  if (B.Selected && Default.Status == "local_url_candidate")
    return resolveHTMLLocalURL(Default.Path, Default.Directory, B.Reference);
  return Default;
}
} // namespace neverd::web
