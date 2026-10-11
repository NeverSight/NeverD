//===- ELF.cpp - SEA ELF note location ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SEA ELF note location.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

namespace neverd::web::sea_detail {
Location locateELF(const Reader &R) {
  const auto H = R.read({0, 64});
  const auto Machine = number(H, 18, 2);
  if (H.compare(0, 7, "\177ELF\2\1\1", 7) || (H[7] != 0 && H[7] != 3) ||
      (number(H, 16, 2) != 2 && number(H, 16, 2) != 3) ||
      (Machine != 62 && Machine != 183) || number(H, 20, 4) != 1 ||
      number(H, 52, 2) != 64 || number(H, 54, 2) != 56)
    throw Error("sea_unsupported_container");
  const auto Count = number(H, 56, 2);
  if (!Count || Count > 1024)
    throw Error("sea_native_table_budget_exceeded");
  const Range PH{number(H, 32, 8), Count * 56};
  if (PH.Offset < 64)
    throw Error("sea_overlapping_metadata");
  const auto P = R.read(PH);
  std::vector<Mapping> Loads, Notes;
  for (uint64_t I = 0; I < Count; ++I) {
    const auto At = I * 56;
    Mapping M{{number(P, At + 8, 8), number(P, At + 32, 8)},
              {number(P, At + 16, 8), number(P, At + 40, 8)}};
    within(M.File, R.Bytes.size());
    within(M.Virtual, UINT64_MAX);
    const auto Type = number(P, At, 4), Alignment = number(P, At + 48, 8);
    if (Type == 1) {
      if (M.File.Size > M.Virtual.Size ||
          (Alignment > 1 &&
           (!powerOfTwo(Alignment) ||
            M.File.Offset % Alignment != M.Virtual.Offset % Alignment)))
        throw Error("sea_invalid_load_mapping");
      Loads.push_back(M);
    } else if (Type == 4) {
      if (M.File.Size != M.Virtual.Size)
        throw Error("sea_non_file_backed_note");
      Notes.push_back(M);
    }
  }
  std::optional<Range> Resource;
  uint64_t Seen = 0, NameBytes = 0;
  for (const auto &N : Notes) {
    if (N.File.Size)
      requireMapping(N.File, N.Virtual, Loads);
    uint64_t At = N.File.Offset;
    while (At < N.File.end()) {
      if (++Seen > 8192)
        throw Error("sea_native_table_budget_exceeded");
      if (N.File.end() - At < 12)
        throw Error("sea_invalid_note");
      const auto H = R.read({At, 12});
      const auto NameSize = number(H, 0, 4), Size = number(H, 4, 4);
      const auto NamePadded = align4(NameSize), DataPadded = align4(Size);
      if (!contains(N.File, {At + 12, NamePadded + DataPadded}))
        throw Error("sea_invalid_note");
      if (NameSize > MaxSEANameBytes ||
          NameSize > MaxSEAPrivateBytes - NameBytes)
        throw Error("sea_name_budget_exceeded");
      NameBytes += NameSize;
      const auto Name = R.read({At + 12, NameSize});
      // Node's bundled postject compares sizeof(pointer) bytes here. Refuse
      // competing prefixes rather than selecting a different runtime resource.
      if (NameSize && Size &&
          (!contains(N.File, {At + 12, 8}) ||
           R.read({At + 12, 8}) == "NODE_SEA")) {
        if (Name != std::string_view("NODE_SEA_BLOB\0", 14))
          throw Error("sea_ambiguous_resource_name");
        if (Resource)
          throw Error("sea_duplicate_resource");
        const Range File{At + 12 + NamePadded, Size};
        if (!Size || overlaps(File, {0, 64}) || overlaps(File, PH))
          throw Error("sea_overlapping_metadata");
        const Range Virtual{N.Virtual.Offset + (File.Offset - N.File.Offset),
                            Size};
        requireMapping(File, Virtual, Loads);
        Resource = File;
      }
      At += 12 + NamePadded + DataPadded;
    }
  }
  if (!Resource)
    throw Error("sea_resource_not_found");
  return {*Resource, "elf", Machine == 183 ? "arm64" : "x64"};
}
} // namespace neverd::web::sea_detail
