//===- Blob.cpp - Node SEA serialized regions -----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Node SEA serialized regions.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include <set>

// Independently implemented from Node v22.15.0 node_sea.cc and
// blob_serializer_deserializer-inl.h; no Node/V8 code is linked or executed.
namespace neverd::web {
namespace {
using namespace sea_detail;
class BlobReader {
  const Artifact &Input;
  Reader R;
  SEAExtraction Out;
  uint64_t At = 0, PrivateBytes = 0;

  uint64_t integer(unsigned Width) {
    const auto V = R.integer(At, Width);
    At += Width;
    return V;
  }
  void region(Range V, const char *Kind, bool Private = false) {
    SEARegion S;
    S.Offset = V.Offset;
    S.Kind = Kind;
    S.Private = Private;
    S.Content = Input.Content.slice(V.Offset, V.Size);
    S.ID = identity("sea-region", {Out.ID, S.Kind, std::to_string(S.Offset),
                                   std::to_string(V.Size)});
    if (!Private)
      S.BlobHash = S.Content.digest();
    if (S.Kind == "javascript_storage")
      Out.SourceArtifactID = S.ID;
    Out.Regions.push_back(std::move(S));
  }
  Range string(const char *Kind, bool Private = false) {
    const auto Prefix = At;
    const auto Size = integer(8);
    within({At, Size}, R.Bytes.size());
    if (Private) {
      if (Size > MaxSEANameBytes || Size > MaxSEAPrivateBytes - PrivateBytes)
        throw Error("sea_name_budget_exceeded");
      PrivateBytes += Size;
    }
    region({Out.ResourceOffset + Prefix, 8}, "serialization");
    const Range V{At, Size};
    region({Out.ResourceOffset + At, Size}, Kind, Private);
    At += Size;
    return V;
  }

public:
  BlobReader(const Artifact &Input, const Location &L, std::string_view Profile)
      : Input(Input),
        R{Input.Content.slice(L.Resource.Offset, L.Resource.Size)} {
    Out.ArtifactID = Input.ID;
    Out.BlobHash = Input.BlobHash;
    Out.Profile = Profile;
    Out.ContainerFormat = L.Format;
    Out.Architecture = L.Architecture;
    Out.ResourceOffset = L.Resource.Offset;
    Out.ResourceSize = L.Resource.Size;
    Out.ID = identity("sea-extraction", {Input.ID, Input.BlobHash, Profile});
  }
  SEAExtraction run() {
    if (integer(4) != 0x143da20)
      throw Error("sea_invalid_magic");
    Out.Flags = integer(4);
    if ((Out.Flags & ~15U) || (Out.Flags & 6U) == 6U)
      throw Error("sea_unsupported_flags");
    region({0, Out.ResourceOffset}, "container_prefix");
    region({Out.ResourceOffset, 8}, "serialization");
    string("code_path", true);
    string(Out.Flags & 2 ? "v8_snapshot" : "javascript_storage");
    if (Out.Flags & 4)
      string("v8_code_cache");
    if (Out.Flags & 8) {
      const auto CountOffset = At;
      const auto Count = integer(8);
      if (!Count || Count > MaxSEAAssets)
        throw Error("sea_asset_budget_exceeded");
      Out.AssetCount = Count;
      region({Out.ResourceOffset + CountOffset, 8}, "serialization");
      std::set<std::string> Keys;
      for (uint64_t I = 0; I < Count; ++I) {
        const auto Key = string("asset_key", true);
        if (!Keys.insert(R.read(Key)).second)
          throw Error("sea_duplicate_asset_key");
        string("asset");
      }
    }
    if (At != R.Bytes.size())
      throw Error("sea_trailing_bytes");
    const auto End = Out.ResourceOffset + Out.ResourceSize;
    region({End, Input.Content.size() - End}, "container_suffix");
    return std::move(Out);
  }
};
} // namespace
SEAExtraction extractSEA(const Artifact &Input, std::string_view Profile) {
  if (Input.Content.size() > MaxSEAInputBytes)
    throw Error("sea_input_budget_exceeded");
  const auto L = sea_detail::locate({Input.Content}, Profile);
  return BlobReader(Input, L, Profile).run();
}
} // namespace neverd::web
