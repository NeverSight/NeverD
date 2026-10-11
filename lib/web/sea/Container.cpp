//===- Container.cpp - Explicit SEA container profiles --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit SEA container profiles.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

namespace neverd::web {
std::vector<std::string> seaProfiles() {
  std::vector<std::string> P{std::string(SEABlobProfile)};
  for (const auto *Format : {"elf", "macho", "pe"})
    for (const auto *Arch : {"x64", "arm64"})
      P.push_back(std::string("node-sea-22.15.0-") + Format + "-" + Arch +
                  "-v1");
  return P;
}
namespace sea_detail {
Location locate(const Reader &R, std::string_view Profile) {
  const auto Profiles = seaProfiles();
  if (std::find(Profiles.begin(), Profiles.end(), Profile) == Profiles.end())
    throw Error("sea_unsupported_profile");
  if (Profile == SEABlobProfile)
    return {{0, R.Bytes.size()}, "preparation_blob", "not_encoded"};
  Location L;
  if (Profile.starts_with("node-sea-22.15.0-elf-"))
    L = locateELF(R);
  else if (Profile.starts_with("node-sea-22.15.0-macho-"))
    L = locateMachO(R);
  else
    L = locatePE(R);
  const auto Expected =
      "node-sea-22.15.0-" + L.Format + "-" + L.Architecture + "-v1";
  if (Profile != Expected)
    throw Error("sea_profile_target_mismatch");
  return L;
}
} // namespace sea_detail
} // namespace neverd::web
