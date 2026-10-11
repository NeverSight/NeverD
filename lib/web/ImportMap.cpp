//===- ImportMap.cpp - Bounded import map resolution -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded import map resolution.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/ImportMap.h"

#include "JsonReader.h"
#include "ada.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <set>

namespace neverd::web {
namespace {
bool relative(std::string_view S) {
  return S.starts_with("./") || S.starts_with("../");
}
std::string urlLike(std::string_view S, std::string_view Base,
                    const ImportMapCharge &Charge) {
  return normalizeModuleURL(S, relative(S) || S.starts_with('/') ? Base : "",
                            Charge);
}
bool same(std::string_view A, std::string_view B,
          const ImportMapCharge &Charge) {
  Charge(std::min(A.size(), B.size()) + 1);
  return A == B;
}
bool prefix(std::string_view Text, std::string_view Prefix,
            const ImportMapCharge &Charge) {
  Charge(std::min(Text.size(), Prefix.size()) + 1);
  return Text.starts_with(Prefix);
}
struct Less {
  const ImportMapCharge &Charge;
  bool operator()(std::string_view A, std::string_view B) const {
    Charge(std::min(A.size(), B.size()) + 1);
    return A < B;
  }
};
} // namespace

std::string normalizeModuleURL(std::string_view Reference,
                               std::string_view BaseURL,
                               const ImportMapCharge &Charge) {
  Charge(Reference.size() + BaseURL.size() + 1);
  if (Reference.size() > MaxImportMapURLBytes ||
      BaseURL.size() > MaxImportMapURLBytes || !validUtf8(Reference) ||
      !validUtf8(BaseURL))
    return {};
  auto Base = ada::parse<ada::url_aggregator>(BaseURL);
  if (!BaseURL.empty() && !Base)
    return {};
  auto Parsed = ada::parse<ada::url_aggregator>(
      Reference, BaseURL.empty() ? nullptr : &*Base);
  if (!Parsed || Parsed->get_href().size() > MaxImportMapURLBytes)
    return {};
  Charge(Parsed->get_href().size());
  return std::string(Parsed->get_href());
}

ImportMap inspectImportMap(std::string_view DeclarationID,
                           std::string_view Bytes, std::string_view BaseURL) {
  ImportMap R;
  if (Bytes.size() > MaxImportMapBytes) {
    R.ID = identity("import-map-refused", {ImportMapProfile, DeclarationID});
    R.Status = "budget_exceeded";
    R.Reason = "import_map_byte_budget_exceeded";
    return R;
  }
  R.ID = identity("import-map",
                  {ImportMapProfile, DeclarationID, sha256(Bytes), BaseURL});
  const ImportMapCharge Charge = [&](uint64_t N) {
    if (N > MaxImportMapSteps - R.Steps)
      throw Error("import_map_work_budget_exceeded");
    R.Steps += N;
  };
  try {
    Charge(Bytes.size());
    R.BaseURL = normalizeModuleURL(BaseURL, {}, Charge);
    if (R.BaseURL.empty())
      throw Error("import_map_base_unavailable");
    const auto Value =
        parseBoundedJSON(Bytes, {MaxImportMapBytes, 16, MaxImportMapRecords * 4,
                                 MaxImportMapURLBytes});
    const auto *Object = Value.getAsObject();
    if (!Object)
      throw Error("import_map_not_object");
    const auto Member = [&](llvm::StringRef Name) {
      const auto *V = Object->get(Name);
      if (V && !V->getAsObject())
        throw Error("invalid_import_map_member");
      return V ? V->getAsObject() : nullptr;
    };
    const auto *Imports = Member("imports"), *Scopes = Member("scopes"),
               *Integrity = Member("integrity");
    const auto Count = [&] {
      Charge(1);
      if (++R.EntryCount > MaxImportMapRecords)
        throw Error("import_map_record_budget_exceeded");
    };
    const auto Entries = [&](const llvm::json::Object &O, std::string_view ID) {
      std::vector<ImportMapEntry> Result;
      std::set<std::string, Less> Keys(Less{Charge});
      for (const auto &[K, V] : O) {
        Count();
        const std::string Key = K.str();
        if (Key.empty()) {
          ++R.IgnoredKeys;
          continue;
        }
        auto Normalized = urlLike(Key, R.BaseURL, Charge);
        if (!Normalized.empty() && !relative(Key))
          R.OriginDependentKeys = true;
        if (Normalized.empty())
          Normalized = Key;
        if (!Keys.insert(Normalized).second)
          throw Error("import_map_normalized_key_collision");
        ImportMapEntry E;
        E.Key = std::move(Normalized);
        E.RawKey = Key;
        E.BlockReason = "non_string_address";
        if (const auto Address = V.getAsString()) {
          E.RawAddress = Address->str();
          E.AddressRelative = relative(E.RawAddress);
          E.Address = urlLike(E.RawAddress, R.BaseURL, Charge);
          E.BlockReason = "invalid_address";
          if (Key.ends_with('/') && !E.Address.ends_with('/')) {
            E.Address.clear();
            E.BlockReason = "invalid_prefix_address";
          }
        }
        if (!E.Address.empty())
          E.BlockReason.clear();
        Result.push_back(std::move(E));
      }
      std::sort(Result.begin(), Result.end(),
                [&](const auto &A, const auto &B) {
                  return Less{Charge}(A.Key, B.Key);
                });
      for (size_t I = 0; I < Result.size(); ++I)
        Result[I].ID = identity("import-map-entry", {ID, std::to_string(I)});
      return Result;
    };
    if (Imports)
      R.Imports = Entries(*Imports, R.ID);
    if (Scopes) {
      std::set<std::string, Less> Keys(Less{Charge});
      for (const auto &[Key, Value] : *Scopes) {
        Count();
        const auto *O = Value.getAsObject();
        if (!O)
          throw Error("invalid_import_map_scope");
        ImportMapScope S;
        S.RawPrefix = Key.str();
        S.Prefix = normalizeModuleURL(Key.str(), R.BaseURL, Charge);
        if (S.Prefix.empty()) {
          ++R.IgnoredKeys;
          continue;
        }
        // A local capture has no proven origin. Absolute/network/root scope
        // keys can shadow local requests under a different real origin.
        const std::string Raw = Key.str();
        if (Raw.starts_with('/') ||
            !normalizeModuleURL(Raw, {}, Charge).empty())
          R.OriginDependentKeys = true;
        if (!Keys.insert(S.Prefix).second)
          throw Error("import_map_normalized_scope_collision");
        S.Entries = Entries(*O, {});
        R.Scopes.push_back(std::move(S));
      }
      std::sort(R.Scopes.begin(), R.Scopes.end(),
                [&](const auto &A, const auto &B) {
                  return Less{Charge}(A.Prefix, B.Prefix);
                });
      for (size_t I = 0; I < R.Scopes.size(); ++I) {
        auto &S = R.Scopes[I];
        S.ID = identity("import-map-scope", {R.ID, std::to_string(I)});
        for (size_t J = 0; J < S.Entries.size(); ++J)
          S.Entries[J].ID =
              identity("import-map-entry", {S.ID, std::to_string(J)});
      }
    }
    R.IntegrityPresent = Integrity;
    if (Integrity)
      for (const auto &[Key, Value] : *Integrity) {
        Count();
        if (Value.getAsString() &&
            !urlLike(Key.str(), R.BaseURL, Charge).empty())
          ++R.IntegrityCount;
        else
          ++R.IgnoredKeys;
      }
    for (const auto &[Key, Value] : *Object) {
      Charge(Key.str().size() + 1);
      if (Key != "imports" && Key != "scopes" && Key != "integrity")
        ++R.IgnoredKeys;
    }
    R.Status = "ok";
  } catch (const Error &E) {
    R.Reason = E.what();
    R.Status = R.Reason.find("budget_exceeded") != std::string::npos
                   ? "budget_exceeded"
                   : "refused";
    R.Imports.clear();
    R.Scopes.clear();
  }
  return R;
}

ImportMapResolution resolveImportMaps(std::span<const ImportMap *const> Maps,
                                      std::string_view Specifier,
                                      std::string_view ScriptBaseURL,
                                      const ImportMapCharge &Charge) {
  ImportMapResolution R;
  if (Specifier.size() > MaxImportMapURLBytes || !validUtf8(Specifier)) {
    R.Status = "unsupported_specifier";
    return R;
  }
  const auto Base = normalizeModuleURL(ScriptBaseURL, {}, Charge);
  if (Base.empty()) {
    R.Status = "import_map_base_unavailable";
    return R;
  }
  const auto URL = urlLike(Specifier, Base, Charge);
  const auto Normalized = URL.empty() ? Specifier : std::string_view(URL);
  auto Parsed = ada::parse<ada::url_aggregator>(URL);
  const bool PrefixAllowed = URL.empty() || (Parsed && Parsed->is_special());
  std::set<std::string_view, Less> Scopes(Less{Charge});
  uint64_t Count = 0;
  for (const auto *M : Maps) {
    Charge(1);
    if (!M || M->Status != "ok" || M->EntryCount > MaxImportMapRecords - Count)
      throw Error("invalid_import_map_model");
    Count += M->EntryCount;
    for (const auto &S : M->Scopes)
      if (same(Base, S.Prefix, Charge) ||
          (S.Prefix.ends_with('/') && prefix(Base, S.Prefix, Charge)))
        Scopes.insert(S.Prefix);
  }
  const auto Match = [&](std::string_view Scope, bool Scoped) {
    const ImportMapEntry *Best = nullptr;
    for (const auto *M : Maps) {
      const auto Visit = [&](const std::vector<ImportMapEntry> &Entries,
                             std::string_view ScopeID) {
        for (const auto &E : Entries) {
          const bool Exact = same(Normalized, E.Key, Charge);
          if (!Exact && !(PrefixAllowed && E.Key.ends_with('/') &&
                          prefix(Normalized, E.Key, Charge)))
            continue;
          if (Best && Best->Key.size() >= E.Key.size())
            continue; // Earliest map owns an equal normalized definition.
          Best = &E;
          R.Map = M;
          R.MapID = M->ID;
          R.ScopeID = ScopeID;
          R.EntryID = E.ID;
          R.MatchKind = Exact ? "exact" : "prefix";
        }
      };
      if (!Scoped)
        Visit(M->Imports, {});
      else
        for (const auto &S : M->Scopes)
          if (same(S.Prefix, Scope, Charge))
            Visit(S.Entries, S.ID);
    }
    R.Entry = Best;
    if (!Best)
      return false;
    if (Best->Address.empty())
      R.Status = "blocked_import_map";
    else if (R.MatchKind == "exact") {
      R.URL = Best->Address;
      R.Status = "url_candidate";
    } else {
      R.URL = normalizeModuleURL(Normalized.substr(Best->Key.size()),
                                 Best->Address, Charge);
      if (R.URL.empty() || !prefix(R.URL, Best->Address, Charge)) {
        R.URL.clear();
        R.Status = "blocked_import_map_backtracking";
      } else
        R.Status = "url_candidate";
    }
    return true;
  };
  // Matching scope prefixes form a chain. Reverse lexical order visits the
  // most specific prefix first, independent of unrelated Unicode key order.
  for (auto I = Scopes.rbegin(); I != Scopes.rend(); ++I)
    if (Match(*I, true))
      return R;
  if (Match({}, false))
    return R;
  R.URL = URL;
  R.Status = URL.empty() ? "unmapped_bare_specifier" : "url_candidate";
  R.MatchKind = "default_url";
  return R;
}
} // namespace neverd::web
