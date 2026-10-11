//===- Zip.cpp - ZIP32 container evidence ---------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded local/central record agreement before transactional publication.
///
//===----------------------------------------------------------------------===//

#include "../BlobStore.h"
#include "PathIndex.h"
#include "ZipInternal.h"

#include "neverd/web/Error.h"

#include <algorithm>
#include <bit>
#include <numeric>
#include <optional>
#include <set>

namespace neverd::web {
namespace {
uint32_t word(std::string_view Bytes, uint64_t Offset, unsigned Size) {
  if (Offset > Bytes.size() || Size > Bytes.size() - Offset)
    throw Error("zip_truncated_record");
  uint32_t Result = 0;
  for (unsigned I = 0; I < Size; ++I)
    Result |= uint32_t(uint8_t(Bytes[Offset + I])) << (8 * I);
  return Result;
}

void extra(std::string_view Bytes, bool Central) {
  std::set<uint16_t> Seen;
  while (!Bytes.empty()) {
    const auto Tag = word(Bytes, 0, 2), Length = word(Bytes, 2, 2);
    if (Length > Bytes.size() - 4)
      throw Error("zip_truncated_extra");
    if (!Seen.insert(Tag).second)
      throw Error("zip_duplicate_extra");
    // Timestamp/UID records do not affect content, paths or stream framing.
    // Alternate names, links, ZIP64 and encryption extensions need profiles.
    if (Tag != 0x5455 && Tag != 0x7875 && Tag != 0x000a)
      throw Error("zip_unsupported_extra");
    const auto Data = Bytes.substr(4, Length);
    if (Tag == 0x5455) {
      const auto Flags = word(Data, 0, 1);
      const auto Stored = Central ? Flags & 1 : Flags;
      if ((Flags & ~7U) || Length != 1 + 4 * std::popcount(Stored))
        throw Error("zip_invalid_timestamp_extra");
    } else if (Tag == 0x7875) {
      if (word(Data, 0, 1) != 1)
        throw Error("zip_unsupported_uid_version");
      const auto UID = word(Data, 1, 1);
      if (!UID || UID > 8)
        throw Error("zip_invalid_uid_extra");
      const auto GID = word(Data, 2 + UID, 1);
      if (!GID || GID > 8 || Length != 3 + UID + GID)
        throw Error("zip_invalid_uid_extra");
    } else if (Length != 32 || word(Data, 0, 4) != 0 || word(Data, 4, 2) != 1 ||
               word(Data, 6, 2) != 24) {
      throw Error("zip_unsupported_ntfs_extra");
    }
    Bytes.remove_prefix(4 + Length);
  }
}

struct Record {
  uint32_t CRC = 0;
  uint16_t Version = 0, Flags = 0, Time = 0, Date = 0;
};

class ZipReader {
  PackageArchive &A;
  uint64_t Budget, Metadata = 0, Declared = 0;
  ArchivePathIndex Paths{Metadata};
  uint64_t CentralOffset = 0, CentralSize = 0, Count = 0;
  std::vector<Record> Records;

  void charge(uint64_t Bytes) {
    if (Bytes > MaxPackageArchiveMetadata - Metadata)
      throw Error("package_archive_metadata_budget_exceeded");
    Metadata += Bytes;
  }

  void terminal() {
    if (A.Original.size() < 22)
      throw Error("zip_missing_end_record");
    const auto Size = std::min<uint64_t>(A.Original.size(), 65535 + 22);
    const auto Start = A.Original.size() - Size;
    const auto Tail = A.Original.read(Start, Size);
    std::optional<uint64_t> End;
    for (uint64_t I = 0; I + 22 <= Tail.size(); ++I)
      if (word(Tail, I, 4) == 0x06054b50 &&
          word(Tail, I + 20, 2) == Tail.size() - I - 22) {
        if (End)
          throw Error("zip_ambiguous_end_record");
        End = I;
      }
    if (!End)
      throw Error("zip_missing_end_record");
    const auto E = std::string_view(Tail).substr(*End, 22);
    Count = word(E, 10, 2);
    CentralSize = word(E, 12, 4);
    CentralOffset = word(E, 16, 4);
    if (word(E, 4, 2) || word(E, 6, 2) || word(E, 8, 2) != Count)
      throw Error("zip_multidisk_unsupported");
    if (Count == 0xffff || CentralSize == 0xffffffff ||
        CentralOffset == 0xffffffff)
      throw Error("zip64_unsupported");
    if (Count > MaxPackageArchiveMembers)
      throw Error("package_archive_member_budget_exceeded");
    if (CentralOffset > Start + *End ||
        CentralSize != Start + *End - CentralOffset)
      throw Error("zip_invalid_central_extent");
    charge(CentralSize + word(E, 20, 2));
  }

  void central() {
    const auto Bytes = A.Original.read(CentralOffset, CentralSize);
    uint64_t Offset = 0;
    for (uint64_t I = 0; I < Count; ++I) {
      if (Offset > Bytes.size() || Bytes.size() - Offset < 46)
        throw Error("zip_truncated_central");
      auto H = std::string_view(Bytes).substr(Offset);
      if (word(H, 0, 4) != 0x02014b50)
        throw Error("zip_invalid_central_signature");
      const auto NameSize = word(H, 28, 2), ExtraSize = word(H, 30, 2),
                 CommentSize = word(H, 32, 2);
      const auto Length = 46 + NameSize + ExtraSize + CommentSize;
      if (Length > H.size())
        throw Error("zip_truncated_central");
      H = H.substr(0, Length);
      Offset += Length;
      Record R{word(H, 16, 4), uint16_t(word(H, 6, 2)), uint16_t(word(H, 8, 2)),
               uint16_t(word(H, 12, 2)), uint16_t(word(H, 14, 2))};
      PackageArchiveMember M;
      M.Compression = word(H, 10, 2);
      M.StoredSize = word(H, 20, 4);
      M.Size = word(H, 24, 4);
      M.LocalHeaderOffset = word(H, 42, 4);
      if (M.StoredSize == 0xffffffff || M.Size == 0xffffffff ||
          M.LocalHeaderOffset == 0xffffffff)
        throw Error("zip64_unsupported");
      if (word(H, 34, 2))
        throw Error("zip_multidisk_unsupported");
      if (R.Version < 10 || R.Version > 20 ||
          (M.Compression == 8 && R.Version < 20))
        throw Error("zip_unsupported_version");
      if ((R.Flags & ~uint16_t(0x080f)) ||
          (M.Compression != 8 && (R.Flags & 6)))
        throw Error("zip_unsupported_flags");
      M.Path = std::string(H.substr(46, NameSize));
      if (!validUtf8(M.Path) ||
          (!(R.Flags & 0x800) &&
           std::any_of(M.Path.begin(), M.Path.end(),
                       [](unsigned char C) { return C >= 0x80; })))
        throw Error("zip_unsupported_name_encoding");
      extra(H.substr(46 + NameSize, ExtraSize), true);
      const auto Host = word(H, 4, 2) >> 8;
      const auto Attributes = word(H, 38, 4);
      if (Host != 0 && Host != 3 && Host != 19)
        throw Error("zip_unsupported_creator");
      const bool Directory = M.Path.ends_with('/');
      const auto Mode = Host == 0 ? 0 : Attributes >> 16;
      const auto Type = Mode & 0170000;
      if ((Type && Type != 0100000 && Type != 0040000) || (Attributes & 8))
        throw Error("zip_special_member_unsupported");
      if ((Type == 0040000 && !Directory) || (Type == 0100000 && Directory) ||
          ((Attributes & 0x10) && !Directory))
        throw Error("zip_ambiguous_member_kind");
      M.Kind = Directory ? "directory" : "file";
      M.Executable = !Directory && (Mode & 0111);
      if (Directory) {
        M.Path.pop_back();
        if (M.Size || M.StoredSize || R.CRC || M.Compression || (R.Flags & 9))
          throw Error("zip_invalid_directory");
      } else {
        if (M.Size > MaxPackageArchiveFileBytes || M.Size > Budget - Declared)
          throw Error("package_archive_expansion_budget_exceeded");
        if (M.Size > 1000 * std::max<uint64_t>(1, M.StoredSize))
          throw Error("zip_expansion_ratio_exceeded");
        Declared += M.Size;
        if (R.Flags & 1)
          M.UnavailableReason = "encrypted";
        else if (M.Compression != 0 && M.Compression != 8)
          M.UnavailableReason = "unsupported_compression";
        else if (M.Compression == 8 && !packageZipDeflateAvailable())
          M.UnavailableReason = "deflate_unavailable";
      }
      Paths.admit(M.Path, Directory);
      M.ID = identity("package-archive-member", {A.ID, std::to_string(I)});
      M.ParentID = A.ID;
      A.Members.push_back(std::move(M));
      Records.push_back(R);
    }
    if (Offset != Bytes.size())
      throw Error("zip_trailing_central_data");
  }

  void local() {
    std::vector<uint32_t> Order(A.Members.size());
    std::iota(Order.begin(), Order.end(), 0);
    std::sort(Order.begin(), Order.end(), [&](auto L, auto R) {
      return A.Members[L].LocalHeaderOffset < A.Members[R].LocalHeaderOffset;
    });
    uint64_t Next = 0;
    for (const auto I : Order) {
      auto &M = A.Members[I];
      const auto &R = Records[I];
      if (M.LocalHeaderOffset != Next || Next > CentralOffset ||
          CentralOffset - Next < 30)
        throw Error("zip_noncontiguous_local_records");
      const auto H = A.Original.read(Next, 30);
      if (word(H, 0, 4) != 0x04034b50 || word(H, 4, 2) != R.Version ||
          word(H, 6, 2) != R.Flags || word(H, 8, 2) != M.Compression ||
          word(H, 10, 2) != R.Time || word(H, 12, 2) != R.Date)
        throw Error("zip_local_central_mismatch");
      const auto NameSize = word(H, 26, 2), ExtraSize = word(H, 28, 2);
      Next += 30;
      if (NameSize + ExtraSize > CentralOffset - Next)
        throw Error("zip_truncated_local");
      charge(NameSize + ExtraSize);
      const auto Fields = A.Original.read(Next, NameSize + ExtraSize);
      const auto Name = M.Kind == "directory" ? M.Path + '/' : M.Path;
      if (std::string_view(Fields).substr(0, NameSize) != Name)
        throw Error("zip_local_central_mismatch");
      extra(std::string_view(Fields).substr(NameSize), false);
      Next += NameSize + ExtraSize;
      M.StoredOffset = Next;
      if (M.StoredSize > CentralOffset - Next)
        throw Error("zip_truncated_payload");
      Next += M.StoredSize;
      const auto CRC = word(H, 14, 4), Stored = word(H, 18, 4),
                 Size = word(H, 22, 4);
      if (!(R.Flags & 8)) {
        if (CRC != R.CRC || Stored != M.StoredSize || Size != M.Size)
          throw Error("zip_local_central_mismatch");
      } else {
        if ((CRC || Stored || Size) &&
            (CRC != R.CRC || Stored != M.StoredSize || Size != M.Size))
          throw Error("zip_local_central_mismatch");
        const auto Tail =
            A.Original.read(Next, std::min<uint64_t>(16, CentralOffset - Next));
        const bool Bare = Tail.size() >= 12 && word(Tail, 0, 4) == R.CRC &&
                          word(Tail, 4, 4) == M.StoredSize &&
                          word(Tail, 8, 4) == M.Size;
        const bool Signed =
            Tail.size() == 16 && word(Tail, 0, 4) == 0x08074b50 &&
            word(Tail, 4, 4) == R.CRC && word(Tail, 8, 4) == M.StoredSize &&
            word(Tail, 12, 4) == M.Size;
        if (Bare == Signed)
          throw Error("zip_invalid_data_descriptor");
        Next += Signed ? 16 : 12;
      }
      M.StoredFrameSize = Next - M.LocalHeaderOffset;
    }
    if (Next != CentralOffset)
      throw Error("zip_noncontiguous_local_records");
  }

  void payloads() {
    BlobStore Store(Budget);
    uint64_t Steps = 0;
    for (uint64_t I = 0; I < A.Members.size(); ++I) {
      auto &M = A.Members[I];
      if (!M.available())
        continue;
      M.Offset = A.ExpandedBytes;
      decodeZipMember(A.Original.slice(M.StoredOffset, M.StoredSize),
                      M.Compression, M.Size, Records[I].CRC, Store, Steps);
      A.ExpandedBytes += M.Size;
    }
    Store.seal();
    A.Expanded = Store.whole();
    A.ExpandedHash = A.Expanded.digest();
    for (auto &M : A.Members)
      if (M.available()) {
        M.Content = A.Expanded.slice(M.Offset, M.Size);
        M.BlobHash = M.Content.digest();
      }
  }

public:
  ZipReader(PackageArchive &A, uint64_t Budget) : A(A), Budget(Budget) {}
  void run() {
    terminal();
    central();
    local();
    payloads();
  }
};
} // namespace

void readZipArchive(PackageArchive &Archive, uint64_t ExpandedBudget) {
  ZipReader(Archive, ExpandedBudget).run();
}
} // namespace neverd::web
