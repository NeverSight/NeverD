//===- PackageArchive.cpp - Bounded tar and gzip admission -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private member slices with strict archive framing and name ownership.
///
//===----------------------------------------------------------------------===//

#include "neverd/web/PackageArchive.h"

#include "../BlobStore.h"
#include "../PathPolicy.h"
#include "../archives/PathIndex.h"
#include "../archives/ZipInternal.h"

#include "neverd/web/Error.h"
#include "neverd/web/Limits.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>

#ifdef NEVERD_WEB_ZLIB
#include <zlib.h>
#endif

namespace neverd::web {
namespace {

bool zeros(std::string_view Bytes) {
  return Bytes.find_first_not_of('\0') == std::string_view::npos;
}

uint64_t number(std::string_view Text, unsigned Base) {
  uint64_t Value = 0;
  if (Text.empty())
    throw Error("package_archive_invalid_number");
  for (const unsigned char C : Text) {
    if (C < '0' || C >= '0' + Base || Value > (UINT64_MAX - (C - '0')) / Base)
      throw Error("package_archive_invalid_number");
    Value = Value * Base + (C - '0');
  }
  return Value;
}

uint64_t octal(std::string_view Text) {
  const auto First = Text.find_first_not_of(" \0", 0, 2);
  if (First == std::string_view::npos)
    return 0;
  const auto Last = Text.find_last_not_of(" \0", std::string_view::npos, 2);
  return number(Text.substr(First, Last - First + 1), 8);
}

std::string field(std::string_view Text) {
  const auto End = Text.find('\0');
  if (End != std::string_view::npos) {
    if (!zeros(Text.substr(End)))
      throw Error("package_archive_ambiguous_header");
    Text = Text.substr(0, End);
  }
  if (!validUtf8(Text))
    throw Error("package_archive_invalid_utf8");
  return std::string(Text);
}

Blob decompress(const Blob &Input, uint64_t Budget) {
#ifndef NEVERD_WEB_ZLIB
  throw Error("package_gzip_unavailable");
#else
  struct Inflater {
    z_stream Stream{};
    Inflater() {
      if (inflateInit2(&Stream, 15 + 16) != Z_OK)
        throw Error("package_gzip_unavailable");
    }
    ~Inflater() { inflateEnd(&Stream); }
  } Decoder;
  auto &S = Decoder.Stream;
  BlobStore Store(Budget);
  std::string Chunk;
  std::array<char, BlobTransferBytes> Output;
  uint64_t Read = 0, Produced = 0, Steps = 0;
  for (;;) {
    if (++Steps > 2 * MaxPackageArchiveBytes / BlobTransferBytes + 1024)
      throw Error("package_archive_work_budget_exceeded");
    if (!S.avail_in && Read < Input.size()) {
      Chunk =
          Input.read(Read, std::min(BlobTransferBytes, Input.size() - Read));
      Read += Chunk.size();
      S.next_in = reinterpret_cast<Bytef *>(Chunk.data());
      S.avail_in = uInt(Chunk.size());
    }
    const auto Before = S.avail_in;
    S.next_out = reinterpret_cast<Bytef *>(Output.data());
    S.avail_out = uInt(Output.size());
    const int Status = inflate(&S, Z_NO_FLUSH);
    const uint64_t Count = Output.size() - S.avail_out;
    if (Count > Budget - Produced)
      throw Error("package_archive_expansion_budget_exceeded");
    Store.append(std::string_view(Output.data(), Count));
    Produced += Count;
    if (Status == Z_STREAM_END) {
      if (S.avail_in || Read != Input.size())
        throw Error("package_archive_trailing_gzip_data");
      break;
    }
    if (Status != Z_OK || (!Count && Before == S.avail_in))
      throw Error("package_archive_invalid_gzip");
  }
  // Seal only the private intermediate stream. No namespace or member bytes
  // are published until all tar records and terminal data validate below.
  Store.seal();
  return Store.whole();
#endif
}

class TarReader {
  PackageArchive &A;
  uint64_t Metadata = 0;
  std::map<std::string, std::string> Pax;
  ArchivePathIndex Paths{Metadata};

  void charge(uint64_t Size) {
    if (Size > MaxPackageArchiveMetadata - Metadata)
      throw Error("package_archive_metadata_budget_exceeded");
    Metadata += Size;
  }

  void extended(std::string_view Bytes) {
    if (!Pax.empty())
      throw Error("package_archive_repeated_pax_header");
    while (!Bytes.empty()) {
      const auto Space = Bytes.find(' ');
      if (Space == std::string_view::npos || Space > 20)
        throw Error("package_archive_invalid_pax");
      const auto Length = number(Bytes.substr(0, Space), 10);
      if (Length > Bytes.size() || Length < Space + 4 ||
          Bytes[Length - 1] != '\n')
        throw Error("package_archive_invalid_pax");
      const auto Record = Bytes.substr(Space + 1, Length - Space - 2);
      const auto Equal = Record.find('=');
      if (!Equal || Equal == std::string_view::npos ||
          Equal + 1 == Record.size() || !validUtf8(Record) ||
          Record.find('\0') != std::string_view::npos)
        throw Error("package_archive_invalid_pax");
      const auto Key = std::string(Record.substr(0, Equal));
      if (Key != "path" && Key != "linkpath" && Key != "size" &&
          Key != "mtime" && Key != "atime" && Key != "ctime" && Key != "uid" &&
          Key != "gid" && Key != "uname" && Key != "gname")
        throw Error("package_archive_unsupported_pax_key");
      if (!Pax.emplace(Key, Record.substr(Equal + 1)).second)
        throw Error("package_archive_duplicate_pax_key");
      Bytes.remove_prefix(Length);
    }
    if (Pax.empty())
      throw Error("package_archive_empty_pax_header");
  }

  void admit(PackageArchiveMember &M) {
    if (M.Path.ends_with('/') && M.Kind == "directory")
      M.Path.pop_back();
    charge(M.Link.size());
    Paths.admit(M.Path, M.Kind == "directory");
    M.ID = identity("package-archive-member",
                    {A.ID, std::to_string(A.Members.size())});
    M.ParentID = A.ID;
  }

public:
  explicit TarReader(PackageArchive &A) : A(A) {}

  void run() {
    if (A.ExpandedBytes < 1024 || A.ExpandedBytes % 512)
      throw Error("package_archive_invalid_tar_size");
    uint64_t Offset = 0, Headers = 0;
    bool End = false;
    while (Offset < A.ExpandedBytes) {
      if (++Headers > 2 * MaxPackageArchiveMembers + 2)
        throw Error("package_archive_member_budget_exceeded");
      const auto H = A.Expanded.read(Offset, 512);
      if (zeros(H)) {
        if (!Pax.empty() || A.ExpandedBytes - Offset < 1024)
          throw Error("package_archive_invalid_tar_termination");
        for (auto I = Offset; I < A.ExpandedBytes;) {
          const auto Count = std::min(BlobTransferBytes, A.ExpandedBytes - I);
          if (!zeros(A.Expanded.read(I, Count)))
            throw Error("package_archive_trailing_tar_data");
          I += Count;
        }
        End = true;
        break;
      }
      if (H.compare(257, 8,
                    std::string("ustar\0"
                                "00",
                                8)))
        throw Error("package_archive_unsupported_tar_format");
      uint64_t Sum = 8 * ' ';
      for (uint64_t I = 0; I < H.size(); ++I)
        if (I < 148 || I >= 156)
          Sum += uint8_t(H[I]);
      if (Sum != octal(std::string_view(H).substr(148, 8)))
        throw Error("package_archive_invalid_tar_checksum");
      auto Size = octal(std::string_view(H).substr(124, 12));
      const auto Mode = octal(std::string_view(H).substr(100, 8));
      const auto Type = H[156];
      if (Type != 'x' && Type != '0' && Type != '\0' && Type != '5' &&
          Type != '1' && Type != '2')
        throw Error("package_archive_unsupported_tar_type");
      if (Type != 'x' && Pax.contains("size"))
        Size = number(Pax.at("size"), 10);
      if (Size > MaxPackageArchiveFileBytes ||
          Size > A.ExpandedBytes - Offset - 512)
        throw Error("package_archive_invalid_member_size");
      const auto Padded = (Size + 511) / 512 * 512;
      if (Padded > A.ExpandedBytes - Offset - 512)
        throw Error("package_archive_truncated_member");
      if (Padded > Size &&
          !zeros(A.Expanded.read(Offset + 512 + Size, Padded - Size)))
        throw Error("package_archive_nonzero_padding");
      if (Type == 'x') {
        charge(Size);
        extended(
            A.Expanded.read(Offset + 512, Size, MaxPackageArchiveMetadata));
      } else {
        if (A.Members.size() >= MaxPackageArchiveMembers)
          throw Error("package_archive_member_budget_exceeded");
        PackageArchiveMember M;
        M.Path = field(std::string_view(H).substr(0, 100));
        const auto Prefix = field(std::string_view(H).substr(345, 155));
        if (!Prefix.empty())
          M.Path = Prefix + "/" + M.Path;
        if (Pax.contains("path"))
          M.Path = Pax.at("path");
        M.Link = field(std::string_view(H).substr(157, 100));
        if (Pax.contains("linkpath"))
          M.Link = Pax.at("linkpath");
        M.Kind = Type == '5'   ? "directory"
                 : Type == '1' ? "hardlink"
                 : Type == '2' ? "symlink"
                               : "file";
        if ((!M.available() && Size) ||
            ((M.Kind == "file" || M.Kind == "directory") && !M.Link.empty()) ||
            ((M.Kind == "symlink" || M.Kind == "hardlink") && M.Link.empty()))
          throw Error("package_archive_invalid_member_type");
        M.HeaderOffset = Offset;
        M.Offset = Offset + 512;
        M.Size = Size;
        M.Executable = (Mode & 0111) != 0;
        admit(M);
        if (M.available()) {
          M.Content = A.Expanded.slice(M.Offset, M.Size);
          M.BlobHash = M.Content.digest();
        }
        A.Members.push_back(std::move(M));
        Pax.clear();
      }
      Offset += 512 + Padded;
    }
    if (!End)
      throw Error("package_archive_missing_tar_termination");
  }
};

} // namespace

bool packageArchiveAvailable() { return archivePathsAvailable(); }
bool packageGzipAvailable() {
#ifdef NEVERD_WEB_ZLIB
  return true;
#else
  return false;
#endif
}

PackageArchive extractPackageArchive(const Artifact &Input,
                                     std::string_view Format,
                                     uint64_t ExpandedBudget) {
  if (!packageArchiveAvailable())
    throw Error("package_archive_path_policy_unavailable");
  if (Input.Directory || Input.Content.size() > Limits::HardInputBytes ||
      !ExpandedBudget || ExpandedBudget > MaxPackageArchiveBytes)
    throw Error("package_archive_invalid_input");
  if (Format != "tar" && Format != "tgz" && Format != "zip")
    throw Error("package_archive_unsupported_profile");
  if (Input.Content.digest() != Input.BlobHash)
    throw Error("package_archive_hash_mismatch");
  PackageArchive A;
  A.ArtifactID = Input.ID;
  A.BlobHash = Input.BlobHash;
  A.Original = Input.Content;
  A.Format = Format;
  if (Format == "zip")
    A.Profile = ZipArchiveProfile;
  A.ID = identity("package-archive",
                  {Input.ID, Input.BlobHash, Format, A.Profile});
  if (Format == "zip") {
    readZipArchive(A, ExpandedBudget);
    return A;
  }
  A.Expanded = Format == "tgz" ? decompress(Input.Content, ExpandedBudget)
                               : Input.Content;
  A.ExpandedBytes = A.Expanded.size();
  if (A.ExpandedBytes > ExpandedBudget)
    throw Error("package_archive_expansion_budget_exceeded");
  A.ExpandedHash = A.Expanded.digest();
  TarReader(A).run();
  return A;
}

Snapshot packageArchiveNamespace(const PackageArchive &A) {
  Snapshot S;
  S.ID = A.ID;
  S.InputBytes = A.ExpandedBytes;
  Artifact Root;
  Root.ID = A.ID;
  Root.Directory = true;
  S.Artifacts.push_back(std::move(Root));
  for (const auto &M : A.Members)
    if (M.available() || M.Kind == "directory")
      S.Artifacts.push_back({M.ID, M.BlobHash, M.ParentID, M.Path, M.Kind,
                             M.Content, M.Kind == "directory"});
  return S;
}

} // namespace neverd::web
