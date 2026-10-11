//===- MachO.cpp - SEA Mach-O section location ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SEA Mach-O section location.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

namespace neverd::web::sea_detail {
Location locateMachO(const Reader &R) {
  const auto H = R.read({0, 32});
  const auto CPU = number(H, 4, 4), Count = number(H, 16, 4);
  if (number(H, 0, 4) != 0xfeedfacf || number(H, 12, 4) != 2 ||
      (CPU != 0x1000007 && CPU != 0x100000c))
    throw Error("sea_unsupported_container");
  const auto Size = number(H, 20, 4);
  if (!Count || Count > 4096 || Size > 1024 * 1024)
    throw Error("sea_native_table_budget_exceeded");
  const auto Commands = R.read({32, Size});
  std::vector<Mapping> Loads, Sections;
  std::optional<Mapping> Resource;
  uint64_t At = 0, SectionCount = 0, SEASegments = 0;
  for (uint64_t I = 0; I < Count; ++I) {
    within({At, 8}, Size);
    const auto Kind = number(Commands, At, 4),
               Length = number(Commands, At + 4, 4);
    if (Length < 8 || Length % 8)
      throw Error("sea_invalid_macho_command");
    within({At, Length}, Size);
    const auto C = std::string_view(Commands).substr(At, Length);
    if (Kind == 0x19) {
      if (Length < 72)
        throw Error("sea_invalid_macho_command");
      const auto NS = number(C, 64, 4);
      // getsectdata uses strncmp: a matching NUL ends the comparison even
      // when the remaining fixed-width field contains nonzero bytes.
      if (C.substr(8, 9) == std::string_view("NODE_SEA\0", 9)) {
        if (!fixedName(C, 8, 16, "NODE_SEA"))
          throw Error("sea_ambiguous_resource_name");
        if (++SEASegments > 1)
          throw Error("sea_duplicate_resource");
      }
      if (NS > 4096 - SectionCount)
        throw Error("sea_native_table_budget_exceeded");
      SectionCount += NS;
      if (Length != 72 + NS * 80)
        throw Error("sea_invalid_macho_command");
      const Mapping M{{number(C, 40, 8), number(C, 48, 8)},
                      {number(C, 24, 8), number(C, 32, 8)}};
      within(M.File, R.Bytes.size());
      within(M.Virtual, UINT64_MAX);
      if (M.File.Size > M.Virtual.Size)
        throw Error("sea_invalid_load_mapping");
      Loads.push_back(M);
      for (uint64_t J = 0; J < NS; ++J) {
        const auto S = 72 + J * 80;
        const auto N = number(C, S + 40, 8), Type = number(C, S + 64, 4) & 255;
        Mapping V{{number(C, S + 48, 4), N}, {number(C, S + 32, 8), N}};
        within(V.Virtual, UINT64_MAX);
        const bool ZeroFill = Type == 1 || Type == 0xc || Type == 0x12;
        if (!contains(M.Virtual, V.Virtual))
          throw Error("sea_invalid_load_mapping");
        if (!ZeroFill) {
          within(V.File, R.Bytes.size());
          if (!contains(M.File, V.File))
            throw Error("sea_invalid_load_mapping");
        } else {
          V.File.Size = 0;
        }
        const bool Selected = fixedName(C, 8, 16, "NODE_SEA") &&
                              fixedName(C, S, 16, "__NODE_SEA_BLOB");
        if (Selected) {
          if (Resource)
            throw Error("sea_duplicate_resource");
          if (!fixedName(C, S + 16, 16, "NODE_SEA") || Type != 0 || !N ||
              overlaps(V.File, {0, 32 + Size}))
            throw Error("sea_invalid_resource_section");
          Resource = V;
        } else {
          Sections.push_back(V);
        }
      }
    }
    At += Length;
  }
  if (At != Size)
    throw Error("sea_invalid_macho_command");
  if (!Resource)
    throw Error("sea_resource_not_found");
  requireMapping(Resource->File, Resource->Virtual, Loads);
  for (const auto &S : Sections)
    if (overlaps(S.File, Resource->File) ||
        overlaps(S.Virtual, Resource->Virtual))
      throw Error("sea_ambiguous_mapping");
  return {Resource->File, "macho", CPU == 0x100000c ? "arm64" : "x64"};
}
} // namespace neverd::web::sea_detail
