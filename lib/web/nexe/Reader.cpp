//===- Reader.cpp - Offline nexe EOF envelope ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checked trailer framing and complete immutable byte partitions.
///
//===----------------------------------------------------------------------===//

#include "Reader.h"

#include "../PathPolicy.h"

#include "neverd/web/Error.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace neverd::web {
namespace {
constexpr std::string_view Sentinel = "<nexe~~sentinel>";
constexpr uint64_t FooterSize = 32, SearchWindow = 16000;

uint64_t length(std::string_view Bytes, uint64_t Bound) {
  uint64_t Bits = 0;
  for (unsigned I = 0; I < 8; ++I)
    Bits |= uint64_t(uint8_t(Bytes[I])) << (8 * I);
  const auto V = std::bit_cast<double>(Bits);
  // Input bounds are exactly representable. Check before any integer cast;
  // negative zero is outside this producer's canonical size encodings.
  if (!std::isfinite(V) || std::signbit(V) || V > double(Bound) ||
      std::trunc(V) != V)
    throw Error("nexe_invalid_length");
  return uint64_t(V);
}

void partition(const Artifact &Input, uint64_t BootstrapOffset,
               uint64_t BootstrapSize, NexeExtraction &E) {
  auto Add = [&](const char *Kind, uint64_t Offset, uint64_t Size) {
    NexeRegion R;
    R.Kind = Kind;
    R.Offset = Offset;
    R.Content = Input.Content.slice(Offset, Size);
    R.ID = identity("nexe-region",
                    {E.ID, Kind, std::to_string(Offset), std::to_string(Size)});
    E.Regions.push_back(std::move(R));
  };
  Add("opaque_prefix", 0, BootstrapOffset);
  Add("bootstrap_storage", BootstrapOffset, BootstrapSize);
  Add("resource_storage", E.ResourceOffset, E.ResourceSize);
  Add("footer", Input.Content.size() - FooterSize, FooterSize);
  // Verify the parent and hash all partition regions in the same read pass.
  // Alias member hashing is charged separately and deduplicated by readIndex.
  llvm::SHA256 Whole;
  for (auto &R : E.Regions) {
    llvm::SHA256 Hash;
    for (uint64_t At = 0; At < R.Content.size();) {
      const auto Count = std::min(BlobTransferBytes, R.Content.size() - At);
      const auto Bytes = R.Content.read(At, Count);
      Whole.update(Bytes);
      Hash.update(Bytes);
      At += Count;
      E.HashedBytes += Count;
    }
    R.BlobHash = llvm::toHex(Hash.final(), true);
  }
  if (llvm::toHex(Whole.final(), true) != Input.BlobHash)
    throw Error("nexe_hash_mismatch");
}
} // namespace

bool nexeAvailable() { return archivePathsAvailable(); }

NexeExtraction extractNexe(const Artifact &Input, std::string_view Profile) {
  if (Profile != NexeProfile)
    throw Error("nexe_unsupported_profile");
  if (!nexeAvailable())
    throw Error("nexe_path_policy_unavailable");
  const auto Size = Input.Content.size();
  if (Input.Directory || Size > MaxNexeInputBytes)
    throw Error("nexe_invalid_input");
  if (Size < FooterSize)
    throw Error("nexe_footer_not_found");
  const auto TailSize = std::min(Size, SearchWindow);
  const auto Tail = Input.Content.read(Size - TailSize, TailSize);
  const auto FooterAt = TailSize - FooterSize;
  if (std::string_view(Tail).substr(FooterAt, Sentinel.size()) != Sentinel)
    throw Error("nexe_footer_not_found");
  if (Tail.find(Sentinel) != FooterAt)
    throw Error("nexe_ambiguous_sentinel");
  const auto BootstrapSize = length(
      std::string_view(Tail).substr(FooterAt + 16, 8), Size - FooterSize);
  const auto ResourceSize = length(
      std::string_view(Tail).substr(FooterAt + 24, 8), Size - FooterSize);
  if (!BootstrapSize || BootstrapSize > MaxNexeBootstrapBytes)
    throw Error("nexe_bootstrap_budget_exceeded");
  if (BootstrapSize >= Size - FooterSize ||
      ResourceSize >= Size - FooterSize - BootstrapSize)
    throw Error("nexe_invalid_partition");
  const auto ResourceOffset = Size - FooterSize - ResourceSize;
  const auto BootstrapOffset = ResourceOffset - BootstrapSize;
  NexeExtraction E;
  E.ArtifactID = Input.ID;
  E.BlobHash = Input.BlobHash;
  E.ID = identity("nexe-extraction", {Input.ID, Input.BlobHash, Profile});
  E.ResourceOffset = ResourceOffset;
  E.ResourceSize = ResourceSize;
  E.ScannedBytes = TailSize + BootstrapSize;
  const auto Bootstrap = Input.Content.read(BootstrapOffset, BootstrapSize);
  if (!validUtf8(Bootstrap))
    throw Error("nexe_invalid_bootstrap_utf8");
  // Validate the entire namespace before exposing or hashing any member.
  nexe_detail::readIndex(Input, Bootstrap, E);
  partition(Input, BootstrapOffset, BootstrapSize, E);
  if (E.HashedBytes > 2 * Size)
    throw Error("nexe_hash_budget_exceeded");
  return E;
}

Snapshot nexeNamespace(const NexeExtraction &E) {
  Snapshot S;
  S.ID = E.ID;
  S.InputBytes = E.ReferencedBytes;
  Artifact Root;
  Root.ID = E.ID;
  Root.Directory = true;
  S.Artifacts.push_back(std::move(Root));
  for (const auto &M : E.Members)
    S.Artifacts.push_back(
        {M.ID, M.BlobHash, E.ID, M.Path, "file", M.Content, false});
  return S;
}
} // namespace neverd::web
