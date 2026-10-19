//===- DriverSectionsJSON.cpp - Strict named section scenario parser ------===//
// NeverD Decompiler
///
/// \file
/// Strict named section scenario parser.
///

#include "DriverSectionsJSON.h"

#include "llvm/ADT/StringExtras.h"
namespace neverd::emulation {
llvm::Expected<DriverSectionNamespace>
parseDriverSectionNamespace(const llvm::json::Value &V) {
  auto Invalid = [] {
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "section_namespace requires directories and sections arrays; section "
        "records require name, size, optional bytes_hex and protection");
  };
  const auto *O = V.getAsObject();
  if (!O || O->size() < 2 || O->size() > 3)
    return Invalid();
  for (const auto &[Key, Value] : *O)
    if (Key != "directories" && Key != "sections" && Key != "aliases")
      return Invalid();
  const auto *Dirs = O->getArray("directories"),
             *Sections = O->getArray("sections");
  if (!Dirs || !Sections || Dirs->size() > 64 || Sections->size() > 64)
    return Invalid();
  DriverSectionNamespace R;
  if (const auto *Aliases = O->get("aliases")) {
    const auto *Map = Aliases->getAsObject();
    if (!Map || Map->size() > 64)
      return Invalid();
    for (const auto &[Key, Value] : *Map) {
      auto Target = Value.getAsString();
      if (!Target)
        return Invalid();
      R.Aliases.emplace(Key.str(), Target->str());
    }
  }
  for (const auto &D : *Dirs) {
    auto S = D.getAsString();
    if (!S)
      return Invalid();
    R.Directories.push_back(S->str());
  }
  for (const auto &Entry : *Sections) {
    const auto *S = Entry.getAsObject();
    if (!S)
      return Invalid();
    auto Name = S->getString("name");
    auto Size = S->get("size");
    if (!Name || !Size || !Size->getAsUINT64())
      return Invalid();
    DriverNamedSection N;
    N.Name = Name->str();
    N.Size = *Size->getAsUINT64();
    for (const auto &[Key, Value] : *S) {
      if (Key == "name" || Key == "size")
        continue;
      if (Key == "protection") {
        auto P = Value.getAsUINT64();
        if (!P || *P > UINT32_MAX)
          return Invalid();
        N.Protection = uint32_t(*P);
      } else if (Key == "bytes_hex") {
        auto H = Value.getAsString();
        std::string Bytes;
        if (!H || H->size() > 2 * 1048576 || !llvm::tryGetFromHex(*H, Bytes))
          return Invalid();
        N.Bytes.assign(Bytes.begin(), Bytes.end());
      } else
        return Invalid();
    }
    R.Sections.push_back(std::move(N));
  }
  if (auto E = validateDriverSectionNamespace(R))
    return std::move(E);
  return R;
}
llvm::json::Value driverSectionNamespaceJSON(const DriverSectionNamespace &N) {
  llvm::json::Array Dirs, Sections;
  llvm::json::Object Aliases;
  for (const auto &D : N.Directories)
    Dirs.push_back(D);
  for (const auto &S : N.Sections)
    Sections.push_back(llvm::json::Object{{"name", S.Name},
                                          {"size", S.Size},
                                          {"protection", S.Protection},
                                          {"bytes_hex", llvm::toHex(S.Bytes)}});
  for (const auto &[A, T] : N.Aliases)
    Aliases[A] = T;
  return llvm::json::Object{{"directories", std::move(Dirs)},
                            {"sections", std::move(Sections)},
                            {"aliases", std::move(Aliases)}};
}
} // namespace neverd::emulation
