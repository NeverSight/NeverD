//===- PE.cpp - SEA PE resource directory location ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// SEA PE resource directory location.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include <set>

namespace neverd::web::sea_detail {
namespace {
class Resources {
  const Reader &R;
  const std::vector<Mapping> &Sections;
  Range Root;
  uint64_t Entries = 0, Names = 0;
  std::vector<Range> Metadata;
  std::set<uint32_t> Visited;

  uint64_t file(uint64_t RVA, uint64_t Size) const {
    const Range V{RVA, Size};
    within(V, uint64_t(UINT32_MAX) + 1);
    std::optional<Range> F;
    for (const auto &S : Sections) {
      if (!overlaps(V, S.Virtual))
        continue;
      if (!contains(S.Virtual, V) || F)
        throw Error("sea_ambiguous_mapping");
      const Range Candidate{S.File.Offset + RVA - S.Virtual.Offset, Size};
      if (!contains(S.File, Candidate))
        throw Error("sea_non_file_backed_resource");
      F = Candidate;
    }
    if (!F)
      throw Error("sea_non_file_backed_resource");
    requireMapping(*F, V, Sections);
    return F->Offset;
  }
  std::string metadata(Range V) {
    within(V, Root.Size);
    Metadata.push_back({Root.Offset + V.Offset, V.Size});
    return R.read(Metadata.back());
  }
  std::string name(uint32_t Key) {
    if (!(Key & 0x80000000U))
      return "#" + std::to_string(Key);
    const auto At = Key & 0x7fffffff;
    const auto Count = number(metadata({At, 2}), 0, 2);
    if (Count * 2 > MaxSEANameBytes || Count * 2 > MaxSEAPrivateBytes - Names)
      throw Error("sea_name_budget_exceeded");
    Names += Count * 2;
    auto Bytes = metadata({At + 2, Count * 2});
    // Keep exact UTF-16 code units. The runtime's query is already uppercase;
    // a lowercase table name must not be invented as a matching resource.
    return "s" + Bytes;
  }
  std::vector<std::pair<std::string, uint32_t>> directory(uint32_t At) {
    if (At % 4 || !Visited.insert(At).second)
      throw Error("sea_invalid_resource_directory");
    const auto H = metadata({At, 16});
    const auto Named = number(H, 12, 2), IDs = number(H, 14, 2),
               N = Named + IDs;
    if (N > 4096 - Entries)
      throw Error("sea_native_table_budget_exceeded");
    Entries += N;
    const auto D = metadata({uint64_t(At) + 16, N * 8});
    std::set<std::string> Seen;
    std::vector<std::pair<std::string, uint32_t>> Result;
    uint32_t PreviousKey = 0;
    for (uint64_t I = 0; I < N; ++I) {
      const uint32_t Key = number(D, I * 8, 4), Value = number(D, I * 8 + 4, 4);
      if (bool(Key & 0x80000000U) != (I < Named))
        throw Error("sea_invalid_resource_directory");
      if (I >= Named && Key > 65535)
        throw Error("sea_invalid_resource_directory");
      auto Name = name(Key);
      if (!Seen.insert(Name).second)
        throw Error("sea_duplicate_resource_key");
      if (I && I != Named) {
        bool Ordered = false;
        if (I >= Named) {
          Ordered = PreviousKey < Key;
        } else {
          const auto &Previous = Result.back().first;
          size_t J = 1;
          for (; J < Previous.size() && J < Name.size(); J += 2) {
            const auto A = number(Previous, J, 2), B = number(Name, J, 2);
            if (A != B) {
              Ordered = A < B;
              break;
            }
          }
          if (J == Previous.size() || J == Name.size())
            Ordered = Previous.size() < Name.size();
        }
        if (!Ordered)
          throw Error("sea_unsorted_resource_directory");
      }
      PreviousKey = Key;
      Result.emplace_back(std::move(Name), Value);
    }
    return Result;
  }
  static uint32_t branch(uint32_t Value) {
    if (!(Value & 0x80000000U))
      throw Error("sea_invalid_resource_directory");
    return Value & 0x7fffffff;
  }

public:
  Resources(const Reader &R, const std::vector<Mapping> &Sections, uint64_t RVA,
            uint64_t Size)
      : R(R), Sections(Sections) {
    if (!RVA || Size < 16)
      throw Error("sea_resource_not_found");
    Root = {file(RVA, Size), Size};
  }
  Range locate() {
    std::string SEAName = "s";
    for (char C : std::string_view("NODE_SEA_BLOB")) {
      SEAName += C;
      SEAName += '\0';
    }
    std::optional<Range> Found;
    for (const auto &[Type, Names] : directory(0)) {
      if (Type != "#10")
        continue;
      for (const auto &[Name, Languages] : directory(branch(Names))) {
        if (Name != SEAName)
          continue;
        const auto Leaves = directory(branch(Languages));
        if (Leaves.size() != 1 || !Leaves[0].first.starts_with("#") ||
            (Leaves[0].second & 0x80000000U))
          throw Error("sea_ambiguous_resource_language");
        const auto Leaf = metadata({Leaves[0].second, 16});
        const auto RVA = number(Leaf, 0, 4), Size = number(Leaf, 4, 4);
        if (!Size || number(Leaf, 12, 4))
          throw Error("sea_invalid_resource_data");
        if (Found)
          throw Error("sea_duplicate_resource");
        Found = Range{file(RVA, Size), Size};
        if (!contains(Root, *Found))
          throw Error("sea_invalid_resource_data");
      }
    }
    if (!Found)
      throw Error("sea_resource_not_found");
    for (const auto &M : Metadata)
      if (overlaps(M, *Found))
        throw Error("sea_overlapping_metadata");
    return *Found;
  }
};
} // namespace
Location locatePE(const Reader &R) {
  const auto DOS = R.read({0, 64});
  if (!DOS.starts_with("MZ"))
    throw Error("sea_unsupported_container");
  const auto At = number(DOS, 60, 4);
  if (At < 64 || At > 1024 * 1024)
    throw Error("sea_unsupported_container");
  const auto H = R.read({At, 24});
  const auto Machine = number(H, 4, 2), Count = number(H, 6, 2);
  if (H.compare(0, 4, "PE\0\0", 4) || (Machine != 0x8664 && Machine != 0xaa64))
    throw Error("sea_unsupported_container");
  const auto OptionalSize = number(H, 20, 2);
  if (!Count || Count > 512 || OptionalSize < 136 || OptionalSize > 4096)
    throw Error("sea_native_table_budget_exceeded");
  const auto O = R.read({At + 24, OptionalSize});
  const auto Directories = number(O, 108, 4);
  if (number(O, 0, 2) != 0x20b || Directories < 3 || Directories > 16 ||
      112 + Directories * 8 > OptionalSize)
    throw Error("sea_unsupported_container");
  const auto HeaderSize = number(O, 60, 4);
  const Range Table{At + 24 + OptionalSize, Count * 40};
  if (HeaderSize < Table.end() || HeaderSize > R.Bytes.size())
    throw Error("sea_invalid_pe_headers");
  const auto S = R.read(Table);
  std::vector<Mapping> Sections;
  for (uint64_t I = 0; I < Count; ++I) {
    const auto P = I * 40;
    const Mapping M{{number(S, P + 20, 4), number(S, P + 16, 4)},
                    {number(S, P + 12, 4), number(S, P + 8, 4)}};
    within(M.File, R.Bytes.size());
    within(M.Virtual, uint64_t(UINT32_MAX) + 1);
    if (overlaps(M.File, {0, HeaderSize}) ||
        overlaps(M.Virtual, {0, HeaderSize}))
      throw Error("sea_overlapping_metadata");
    Sections.push_back(M);
  }
  const auto Resource =
      Resources(R, Sections, number(O, 128, 4), number(O, 132, 4)).locate();
  return {Resource, "pe", Machine == 0xaa64 ? "arm64" : "x64"};
}
} // namespace neverd::web::sea_detail
