#include "HTMLImportMaps.h"

#include "HTMLFiles.h"

#include <algorithm>

namespace neverd::web {
namespace {
constexpr std::string_view CaptureOrigin = "https://neverd-capture.invalid/";

std::string documentURL(const HTMLLocalURL &Base,
                        const ImportMapCharge &Charge) {
  if (Base.Status != "local_url_candidate")
    return {};
  Charge(Base.Path.size() * 3 + 1);
  std::string URL(CaptureOrigin);
  constexpr char Hex[] = "0123456789ABCDEF";
  for (unsigned char C : Base.Path) {
    if ((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') ||
        (C >= '0' && C <= '9') || C == '/' || C == '.' || C == '_' ||
        C == '-' || C == '~')
      URL += C;
    else {
      URL += '%';
      URL += Hex[C >> 4];
      URL += Hex[C & 15];
    }
  }
  return normalizeModuleURL(URL, {}, Charge);
}
std::string precedingBase(const HTMLDocument &Document, const HTMLScript &S,
                          std::string_view Default,
                          const ImportMapCharge &Charge) {
  if (S.Base == NoHTMLIndex)
    return std::string(Default);
  if (S.Base >= Document.Bases.size() || !Document.Bases[S.Base].Selected ||
      Document.Bases[S.Base].Start >= S.TagStart)
    throw Error("invalid_html_module_context");
  return normalizeModuleURL(Document.Bases[S.Base].Reference, Default, Charge);
}
} // namespace

HTMLImportMaps inspectHTMLImportMaps(const HTMLDocument &Document,
                                     std::string_view Bytes,
                                     const Snapshot &Input) {
  HTMLImportMaps R;
  R.ID =
      identity("html-import-maps", {ImportMapProfile, Document.ID, Input.ID});
  R.Status = "unavailable";
  if (Document.Status != "partial")
    return R;
  if (Bytes.size() > MaxHTMLBytes || Document.BlobHash != sha256(Bytes) ||
      Document.Scripts.size() > MaxHTMLRecords ||
      Document.Bases.size() > MaxHTMLRecords - Document.Scripts.size())
    throw Error("html_import_map_context_mismatch");
  const ImportMapCharge Charge = [&](uint64_t N) {
    if (N > MaxImportMapSteps - R.Steps)
      throw Error("html_import_map_work_budget_exceeded");
    R.Steps += N;
  };
  try {
    const HTMLFiles Files(Input, Document.ArtifactID, Document.BlobHash,
                          Charge);
    const auto Default = Files.documentBase();
    R.DocumentURL = documentURL(Default, Charge);
    uint64_t BodyBytes = 0, Records = 0;
    for (uint32_t I = 0; I < Document.Scripts.size(); ++I) {
      Charge(1);
      const auto &S = Document.Scripts[I];
      if (S.Kind != "importmap")
        continue;
      if (R.Declarations.size() >= MaxHTMLImportMaps)
        throw Error("html_import_map_count_budget_exceeded");
      HTMLImportMap D;
      D.Script = I;
      D.Model.ID = identity("html-import-map-declaration", {R.ID, S.ID});
      D.Model.Status = "refused";
      D.BaseStatus = "not_analyzed";
      if (S.Context != "html_source_candidate")
        D.Model.Reason = "ineligible_html_context";
      else {
        ++R.EligibleCount;
        if (S.HasSource)
          D.Model.Reason = "external_import_map_unsupported";
        else if (!S.Closed)
          D.Model.Reason = "unclosed_import_map";
        else {
          if (S.BodyEnd < S.BodyStart || S.BodyEnd > Bytes.size())
            throw Error("html_import_map_context_mismatch");
          const auto Body = Bytes.substr(S.BodyStart, S.BodyEnd - S.BodyStart);
          if (Body.size() > MaxImportMapBytes - BodyBytes)
            throw Error("html_import_map_byte_budget_exceeded");
          BodyBytes += Body.size();
          if (S.BodyHash != sha256(Body))
            throw Error("html_import_map_context_mismatch");
          D.LocalBase = Default;
          if (S.Base != NoHTMLIndex) {
            if (S.Base >= Document.Bases.size())
              throw Error("html_import_map_context_mismatch");
            D.LocalBase =
                htmlDeclaredBase(Default, Document.Bases[S.Base], Charge);
          }
          D.BaseStatus = D.LocalBase.Status;
          const auto Base = precedingBase(Document, S, R.DocumentURL, Charge);
          if (D.BaseStatus != "local_url_candidate" || Base.empty())
            D.Model.Reason = "import_map_base_unavailable";
          else {
            D.Model = inspectImportMap(D.Model.ID, Body, Base);
            Charge(D.Model.Steps);
            if (D.Model.EntryCount > MaxImportMapRecords - Records)
              throw Error("html_import_map_record_budget_exceeded");
            Records += D.Model.EntryCount;
          }
        }
      }
      R.Declarations.push_back(std::move(D));
    }
    R.Status = "partial";
  } catch (const Error &E) {
    if (std::string_view(E.what()).find("budget_exceeded") == std::string::npos)
      throw;
    R.Status = "budget_exceeded";
    R.Reason = E.what();
    R.Declarations.clear();
    R.EligibleCount = 0;
  }
  return R;
}

HTMLImportMapSelection selectHTMLImportMaps(const HTMLDocument &Document,
                                            const HTMLImportMaps &Maps,
                                            uint32_t Script,
                                            const ImportMapCharge &Charge) {
  HTMLImportMapSelection R;
  if (Script >= Document.Scripts.size())
    throw Error("invalid_html_module_context");
  if (Maps.Status != "partial") {
    R.Status = Maps.Status == "budget_exceeded"
                   ? "html_import_map_budget_exceeded"
                   : "html_import_map_unavailable";
    return R;
  }
  R.BaseURL = precedingBase(Document, Document.Scripts[Script],
                            Maps.DocumentURL, Charge);
  uint64_t FirstScript = UINT64_MAX;
  for (const auto &S : Document.Scripts) {
    Charge(1);
    if (S.Context == "html_source_candidate" &&
        (S.Kind == "module" || S.Kind == "classic"))
      FirstScript = std::min(FirstScript, S.TagStart);
  }
  for (const auto &D : Maps.Declarations) {
    Charge(1);
    if (D.Script >= Document.Scripts.size())
      throw Error("invalid_html_module_context");
    const auto &S = Document.Scripts[D.Script];
    if (S.Context != "html_source_candidate")
      continue;
    if (S.TagStart > FirstScript) {
      R.Status = "html_import_map_timing_unverified";
      return R;
    }
    if (D.Model.Status != "ok") {
      R.Status = D.Model.Status == "budget_exceeded"
                     ? "html_import_map_budget_exceeded"
                     : "html_import_map_refused";
      return R;
    }
    if (D.Model.OriginDependentKeys) {
      R.Status = "html_import_map_origin_unverified";
      return R;
    }
    const auto LocalReference = [&](std::string_view Reference) {
      Charge(Reference.size() + D.LocalBase.Path.size() + 1);
      return resolveHTMLLocalURL(D.LocalBase.Path, D.LocalBase.Directory,
                                 Reference)
                 .Status == "local_url_candidate";
    };
    const auto KeysAreLocal = [&](const auto &Entries) {
      for (const auto &E : Entries) {
        Charge(1);
        if ((E.RawKey.starts_with("./") || E.RawKey.starts_with("../")) &&
            !LocalReference(E.RawKey))
          return false;
      }
      return true;
    };
    if (!KeysAreLocal(D.Model.Imports)) {
      R.Status = "html_import_map_key_context_unsupported";
      return R;
    }
    for (const auto &Scope : D.Model.Scopes)
      if (!LocalReference(Scope.RawPrefix) || !KeysAreLocal(Scope.Entries)) {
        R.Status = "html_import_map_key_context_unsupported";
        return R;
      }
    R.Maps.push_back(&D.Model);
  }
  R.Status = R.Maps.empty() ? "no_eligible_import_map_declaration"
                            : "source_order_import_map_candidate";
  return R;
}

HTMLLocalURL importMapFileCandidate(const ImportMapResolution &Resolution,
                                    const HTMLImportMaps &Maps,
                                    const HTMLLocalURL &ScriptBase,
                                    std::string_view Specifier,
                                    const ImportMapCharge &Charge) {
  HTMLLocalURL Refused{{}, "unsupported_local_url"};
  if (Resolution.Status != "url_candidate")
    return {{}, Resolution.Status};
  // Validate capture confinement from the unnormalized reference too. URL
  // normalization may clamp traversal at an origin root and erase evidence.
  if (Resolution.Entry) {
    if (!Resolution.Entry->AddressRelative)
      return {{}, "import_map_nonlocal_target"};
    const auto D = std::find_if(Maps.Declarations.begin(),
                                Maps.Declarations.end(), [&](const auto &D) {
                                  Charge(1);
                                  return &D.Model == Resolution.Map;
                                });
    if (D == Maps.Declarations.end())
      throw Error("invalid_import_map_model");
    Charge(D->LocalBase.Path.size() + Resolution.Entry->RawAddress.size() + 1);
    const auto Address =
        resolveHTMLLocalURL(D->LocalBase.Path, D->LocalBase.Directory,
                            Resolution.Entry->RawAddress);
    if (Address.Status != "local_url_candidate")
      return Address;
  } else {
    if (!(Specifier.starts_with("./") || Specifier.starts_with("../")))
      return Refused;
    Charge(ScriptBase.Path.size() + Specifier.size() + 1);
    const auto Address =
        resolveHTMLLocalURL(ScriptBase.Path, ScriptBase.Directory, Specifier);
    if (Address.Status != "local_url_candidate")
      return Address;
  }
  Charge(Resolution.URL.size());
  if (!Resolution.URL.starts_with(CaptureOrigin))
    return {{}, "import_map_nonlocal_target"};
  return resolveHTMLLocalURL(
      {}, true, "./" + Resolution.URL.substr(CaptureOrigin.size()));
}
} // namespace neverd::web
