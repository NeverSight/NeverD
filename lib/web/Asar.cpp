#include "neverd/web/Asar.h"

#include "Internal.h"
#include "JsonReader.h"
#include "PathPolicy.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <charconv>
#include <map>
#include <set>

namespace neverd::web {
namespace {
void fields(const llvm::json::Object &O,
            std::initializer_list<std::string_view> Allowed) {
  for (const auto &F : O)
    if (std::find(Allowed.begin(), Allowed.end(), F.first.str()) ==
        Allowed.end())
      throw Error("asar_unsupported_metadata");
}

bool flag(const llvm::json::Object &O, llvm::StringRef Key) {
  if (const auto *V = O.get(Key)) {
    const auto B = V->getAsBoolean();
    if (!B)
      throw Error("asar_invalid_metadata");
    return *B;
  }
  return false;
}

uint64_t decimal(llvm::StringRef S) {
  if (S.empty())
    throw Error("asar_invalid_offset");
  uint64_t V = 0;
  auto R = std::from_chars(S.data(), S.data() + S.size(), V);
  if (R.ec != std::errc{} || R.ptr != S.data() + S.size() ||
      std::to_string(V) != S)
    throw Error("asar_invalid_offset");
  return V;
}

std::string digestText(const llvm::json::Value &V) {
  const auto S = V.getAsString();
  if (!S || S->size() != 64 ||
      S->find_first_not_of("0123456789abcdef") != llvm::StringRef::npos)
    throw Error("asar_invalid_integrity");
  return S->str();
}

struct Integrity {
  bool Present = false, Supported = false;
  std::string Hash;
  uint64_t BlockSize = 0;
  std::vector<std::string> Blocks;
};

Integrity integrity(const llvm::json::Object &O, uint64_t Size) {
  Integrity I;
  const auto *V = O.get("integrity");
  if (!V)
    return I;
  I.Present = true;
  const auto *Meta = V->getAsObject();
  if (!Meta)
    throw Error("asar_invalid_integrity");
  fields(*Meta, {"algorithm", "hash", "blockSize", "blocks"});
  const auto Algorithm = Meta->getString("algorithm");
  if (!Algorithm)
    throw Error("asar_invalid_integrity");
  if (*Algorithm != "SHA256")
    return I;
  I.Supported = true;
  const auto *Hash = Meta->get("hash");
  const auto BlockSize = Meta->getInteger("blockSize");
  const auto *Blocks = Meta->getArray("blocks");
  if (!Hash || !BlockSize || *BlockSize <= 0 ||
      *BlockSize > int64_t(MaxBlobReadBytes) || !Blocks ||
      Blocks->size() > 10000 ||
      Blocks->size() != Size / uint64_t(*BlockSize) + 1)
    throw Error("asar_invalid_integrity");
  I.Hash = digestText(*Hash);
  I.BlockSize = uint64_t(*BlockSize);
  for (const auto &B : *Blocks)
    I.Blocks.push_back(digestText(B));
  return I;
}

struct Reader {
  AsarExtraction Result;
  const Artifact &Archive;
  std::map<std::string, const Artifact *> Unpacked;
  std::vector<std::pair<uint64_t, uint64_t>> Ranges;
  std::vector<Integrity> Integrities;
  uint64_t PathBytes = 0, PayloadBytes = 0;

  void directory(const llvm::json::Object &Files, std::string_view Parent,
                 std::string_view ParentID, bool Inherited, uint64_t Depth) {
    if (Depth > MaxAsarDepth)
      throw Error("asar_depth_budget_exceeded");
    if (Files.size() > MaxAsarMembers - Result.Members.size())
      throw Error("asar_member_budget_exceeded");
    std::vector<std::string> Names;
    std::set<std::string> Keys;
    for (const auto &F : Files) {
      const auto Name = F.first.str();
      // A JSON property is exactly one component, not a relative path.
      validateMemberName(Name);
      if (!Keys.insert(archivePathKey(Name)).second)
        throw Error("asar_member_collision");
      Names.push_back(Name);
    }
    std::sort(Names.begin(), Names.end());
    for (const auto &Name : Names) {
      if (Result.Members.size() >= MaxAsarMembers)
        throw Error("asar_member_budget_exceeded");
      const auto Path =
          Parent.empty() ? Name : std::string(Parent) + '/' + Name;
      if (Path.size() > 4096 || Path.size() > MaxAsarPathBytes - PathBytes)
        throw Error("asar_path_budget_exceeded");
      PathBytes += Path.size();
      const auto *O = Files.getObject(Name);
      if (!O)
        throw Error("asar_invalid_metadata");
      AsarMember M;
      M.Path = Path;
      M.ParentID = ParentID;
      M.ID = identity("asar-member", {Result.ID, Path});
      const bool ExplicitUnpacked = flag(*O, "unpacked");
      if (Inherited && O->get("unpacked") && !ExplicitUnpacked)
        throw Error("asar_conflicting_storage");
      M.Unpacked = Inherited || ExplicitUnpacked;
      if (O->get("files")) {
        fields(*O, {"files", "unpacked"});
        const auto *Children = O->getObject("files");
        if (!Children)
          throw Error("asar_invalid_metadata");
        M.Kind = "directory";
        M.Status = "metadata_only";
        const auto ID = M.ID;
        Result.Members.push_back(std::move(M));
        Integrities.emplace_back();
        directory(*Children, Path, ID, Inherited || ExplicitUnpacked,
                  Depth + 1);
        continue;
      }
      if (O->get("link")) {
        fields(*O, {"link", "unpacked"});
        const auto Link = O->getString("link");
        if (!Link)
          throw Error("asar_invalid_metadata");
        (void)archivePathKey(*Link);
        M.Kind = "link";
        M.Link = Link->str();
        M.Status = "link_not_followed";
        Result.Members.push_back(std::move(M));
        Integrities.emplace_back();
        continue;
      }
      fields(*O, {"size", "offset", "unpacked", "executable", "integrity"});
      const auto Size = O->getInteger("size");
      if (!Size || *Size < 0)
        throw Error("asar_invalid_size");
      M.Size = uint64_t(*Size);
      if (M.Size > Limits::HardMemberBytes ||
          M.Size > MaxAsarPayloadBytes - PayloadBytes)
        throw Error("asar_payload_budget_exceeded");
      PayloadBytes += M.Size;
      M.Kind = "file";
      M.Executable = flag(*O, "executable");
      const auto I = integrity(*O, M.Size);
      M.IntegrityStatus = !I.Present    ? "missing"
                          : I.Supported ? "not_checked"
                                        : "unsupported_algorithm";
      if (M.Unpacked) {
        if (O->get("offset"))
          throw Error("asar_conflicting_storage");
        const auto It = Unpacked.find(Path);
        if (Result.UnpackedDirectoryID.empty())
          M.Status = "unpacked_not_selected";
        else if (It == Unpacked.end())
          M.Status = "missing_member";
        else if (It->second->Directory)
          M.Status = "member_type_mismatch";
        else {
          const auto &A = *It->second;
          M.StorageArtifactID = A.ID;
          M.Status = A.Content.size() == M.Size ? "available" : "size_mismatch";
          if (M.available())
            M.Content = A.Content;
        }
      } else {
        const auto Offset = O->getString("offset");
        if (!Offset)
          throw Error("asar_invalid_offset");
        const auto At = decimal(*Offset);
        const auto Available = Archive.Content.size() - Result.DataOffset;
        if (At > Available || M.Size > Available - At)
          throw Error("asar_range_out_of_bounds");
        M.Offset = Result.DataOffset + At;
        M.StorageArtifactID = Archive.ID;
        M.Content = Archive.Content.slice(M.Offset, M.Size);
        M.Status = "available";
        if (M.Size)
          Ranges.emplace_back(M.Offset, M.Offset + M.Size);
      }
      Result.Members.push_back(std::move(M));
      Integrities.push_back(I);
    }
  }

  void payloads() {
    std::sort(Ranges.begin(), Ranges.end());
    uint64_t End = Result.DataOffset;
    uint64_t Claimed = 0;
    for (const auto &[Start, Stop] : Ranges) {
      if (Start < End)
        throw Error("asar_overlapping_members");
      End = Stop;
      Claimed += Stop - Start;
    }
    Result.UnreferencedPayloadBytes =
        Archive.Content.size() - Result.DataOffset - Claimed;
    // Structure/ranges are complete before touching any member payload.
    for (size_t N = 0; N < Result.Members.size(); ++N) {
      auto &M = Result.Members[N];
      if (!M.available())
        continue;
      const auto &I = Integrities[N];
      if (I.Present && !I.Supported) {
        M.Status = "unsupported_integrity_algorithm";
        M.Content = {};
        continue;
      }
      llvm::SHA256 Whole, Block;
      uint64_t BlockUsed = 0, BlockIndex = 0;
      bool Match = true;
      for (uint64_t At = 0; At < M.Size;) {
        auto Count = std::min(BlobTransferBytes, M.Size - At);
        if (I.Present)
          Count = std::min(Count, I.BlockSize - BlockUsed);
        const auto Bytes = M.Content.read(At, Count);
        Whole.update(llvm::StringRef(Bytes));
        if (I.Present) {
          Block.update(llvm::StringRef(Bytes));
          BlockUsed += Count;
          if (BlockUsed == I.BlockSize) {
            Match &= llvm::toHex(Block.final(), true) == I.Blocks[BlockIndex++];
            Block = llvm::SHA256();
            BlockUsed = 0;
          }
        }
        At += Count;
      }
      M.BlobHash = llvm::toHex(Whole.final(), true);
      if (I.Present) {
        // This pinned writer flushes a final (possibly empty) block.
        Match &= llvm::toHex(Block.final(), true) == I.Blocks[BlockIndex];
        Match &= M.BlobHash == I.Hash;
        M.IntegrityStatus = Match ? "verified_bytes" : "mismatch";
        if (!Match) {
          M.Status = "integrity_mismatch";
          M.Content = {};
        }
      }
    }
  }
};
} // namespace

bool asarAvailable() { return archivePathsAvailable(); }

AsarExtraction extractAsar(const Snapshot &Input, std::string_view ArtifactID,
                           std::string_view UnpackedID) {
  if (!asarAvailable())
    throw Error("archive_path_policy_unavailable");
  const Artifact *Archive = nullptr, *Directory = nullptr;
  for (const auto &A : Input.Artifacts) {
    if (A.ID == ArtifactID)
      Archive = &A;
    if (!UnpackedID.empty() && A.ID == UnpackedID)
      Directory = &A;
  }
  if (!Archive)
    throw Error("unknown_artifact");
  if (Archive->Directory)
    throw Error("artifact_has_no_bytes");
  if (!UnpackedID.empty() && (!Directory || !Directory->Directory))
    throw Error("asar_invalid_unpacked_directory");
  if (Archive->Content.size() < 16)
    throw Error("asar_truncated_header");
  const auto Prefix = Archive->Content.read(0, 16);
  auto U32 = [&](unsigned At) {
    return llvm::support::endian::read32le(Prefix.data() + At);
  };
  const uint64_t Header = U32(4), Length = U32(12);
  if (U32(0) != 4 || Header < 8 || Header % 4 || U32(8) != Header - 4 ||
      Length > INT32_MAX || Header != 8 + ((Length + 3) & ~uint64_t(3)))
    throw Error("asar_invalid_pickle");
  if (Header > MaxAsarHeaderBytes)
    throw Error("asar_header_budget_exceeded");
  if (Header > Archive->Content.size() - 8)
    throw Error("asar_truncated_header");
  const auto Text = Archive->Content.read(16, Header - 8);
  if (std::any_of(Text.begin() + Length, Text.end(), [](char C) { return C; }))
    throw Error("asar_invalid_padding");
  auto JSON = parseBoundedJSON(std::string_view(Text).substr(0, Length),
                               {MaxAsarHeaderBytes, 64, 200000, 4096});
  const auto *Root = JSON.getAsObject();
  if (!Root || !Root->getObject("files"))
    throw Error("asar_invalid_metadata");
  fields(*Root, {"files"});
  Reader R{{}, *Archive, {}, {}, {}};
  R.Result.ArtifactID = Archive->ID;
  R.Result.UnpackedDirectoryID = std::string(UnpackedID);
  R.Result.DataOffset = 8 + Header;
  R.Result.ID = identity("asar-extraction", {Input.ID, ArtifactID, UnpackedID,
                                             AsarProfile, ArchivePathProfile});
  if (Directory) {
    const auto Prefix = Directory->MemberPath.empty()
                            ? std::string()
                            : Directory->MemberPath + '/';
    std::set<std::string> Keys;
    for (const auto &A : Input.Artifacts) {
      if (A.ID == Directory->ID || !A.MemberPath.starts_with(Prefix))
        continue;
      const auto Relative =
          std::string_view(A.MemberPath).substr(Prefix.size());
      // The selected scope index retains private paths too, including entries
      // that the archive does not declare. Charge them before allocating keys.
      if (Relative.size() > MaxAsarPathBytes - R.PathBytes)
        throw Error("asar_path_budget_exceeded");
      R.PathBytes += Relative.size();
      if (!Keys.insert(archivePathKey(Relative)).second ||
          !R.Unpacked.emplace(Relative, &A).second)
        throw Error("asar_unpacked_collision");
    }
  }
  R.directory(*Root->getObject("files"), {}, R.Result.ID, false, 0);
  R.payloads();
  return std::move(R.Result);
}
} // namespace neverd::web
