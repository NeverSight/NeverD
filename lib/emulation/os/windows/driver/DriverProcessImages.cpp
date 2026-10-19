//===- DriverProcessImages.cpp - Concrete ordinary image-section snapshots ===//
// NeverD Decompiler
///
/// \file
/// Concrete ordinary image-section snapshots.
///

#include "DriverProcessImages.h"

#include "../WindowsUnicode.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process image: " + Text);
}
uint64_t pageSize(uint64_t N) {
  return (N + profile::PageSize - 1) & ~(profile::PageSize - 1);
}
} // namespace
llvm::Expected<std::optional<DriverProcessImageFileIdentity>>
driverProcessImageFileIdentity(
    const DriverProcessImageMapping &Source,
    const std::optional<std::vector<DriverFiniteFileSystemVolume>> &Volumes) {
  if (Volumes)
    for (size_t I = 0; I < Volumes->size(); ++I) {
      const auto &Volume = (*Volumes)[I];
      if (Volume.DeviceName != Source.DeviceName)
        continue;
      auto Key = windows_unicode::key(llvm::StringRef(Source.MappedFile)
                                          .drop_front(Source.DeviceName.size()),
                                      Volume.UpcaseTable);
      if (!Key)
        return Key.takeError();
      for (const auto &Entry : Volume.Entries) {
        auto EntryKey = windows_unicode::key(Entry.Path, Volume.UpcaseTable);
        if (!EntryKey)
          return EntryKey.takeError();
        if (*EntryKey != *Key)
          continue;
        if (Entry.Directory || Entry.Data != Source.FileBytes)
          return invalid(
              "snapshot disagrees with its declared filesystem source");
        return std::optional<DriverProcessImageFileIdentity>{
            {I, std::move(*Key)}};
      }
      return invalid("snapshot file is absent from its declared source volume");
    }
  return std::optional<DriverProcessImageFileIdentity>{};
}
llvm::Expected<DriverProcessImagePlan>
planDriverProcessImage(const DriverProcessImageMapping &Source) {
  const auto &B = Source.FileBytes;
  if (B.size() < 64 || B.size() > MaxDriverProcessImageFileBytes)
    return invalid("requires a bounded complete PE file snapshot");
  if (Source.MappedFile.find('\0') != std::string::npos ||
      !Source.MappedFile.starts_with("\\Device\\") ||
      Source.MappedFile.find('/') != std::string::npos ||
      Source.MappedFile.ends_with('\\'))
    return invalid("mapped_file requires a guest NT device file path");
  if (!Source.DeviceName.starts_with("\\Device\\") ||
      Source.DeviceName.find('\0') != std::string::npos ||
      Source.DeviceName.find('/') != std::string::npos ||
      Source.DeviceName.ends_with('\\') ||
      !Source.MappedFile.starts_with(Source.DeviceName + "\\"))
    return invalid(
        "device_name requires the mapped file's observed volume device");
  auto Name = windows_unicode::bytes(Source.MappedFile, true);
  if (!Name)
    return Name.takeError();
  if (Name->size() > 1026)
    return invalid("mapped_file exceeds 512 UTF-16 units");
  auto U16 = [&](size_t O) {
    return llvm::support::endian::read16le(B.data() + O);
  };
  auto U32 = [&](size_t O) {
    return llvm::support::endian::read32le(B.data() + O);
  };
  auto U64 = [&](size_t O) {
    return llvm::support::endian::read64le(B.data() + O);
  };
  const uint64_t PE = U32(60);
  if (U16(0) != 0x5a4d || PE < 64 || PE > B.size() - 24 || U32(PE) != 0x4550 ||
      U16(PE + 4) != 0x8664 || !(U16(PE + 22) & 2))
    return invalid("requires an executable AMD64 PE image");
  const uint64_t Opt = PE + 24, OptSize = U16(PE + 20), Count = U16(PE + 6);
  if (OptSize < 240 || OptSize > B.size() - Opt || U16(Opt) != 0x20b ||
      !Count || Count > 64 || Count * 40 > B.size() - Opt - OptSize)
    return invalid("truncated or unsupported PE optional/section headers");
  const uint64_t ImageSize = U32(Opt + 56), Headers = U32(Opt + 60),
                 Preferred = U64(Opt + 24), FileAlignment = U32(Opt + 36);
  if (U32(Opt + 32) != profile::PageSize || FileAlignment < 512 ||
      FileAlignment > profile::PageSize ||
      (FileAlignment & (FileAlignment - 1)) || !ImageSize ||
      ImageSize > MaxDriverProcessImageFileBytes ||
      ImageSize % profile::PageSize || !Headers || Headers > B.size() ||
      Headers > ImageSize || Headers % FileAlignment ||
      Opt + OptSize + Count * 40 > Headers || U32(Opt + 108) > 16)
    return invalid(
        "unsupported or malformed ordinary page-aligned image layout");
  const uint64_t Base = Source.Base ? Source.Base : Preferred;
  if (Base != Preferred)
    return invalid(
        "rebased image-section snapshots require relocation support");
  if (Base < profile::ProcessMemoryBase || Base >= profile::ProcessMemoryEnd ||
      Base % 0x10000 || ImageSize > profile::ProcessMemoryEnd - Base)
    return invalid("image allocation is outside the process memory window");
  DriverProcessImagePlan Out;
  Out.Base = Base;
  Out.MappedFile = Source.MappedFile;
  Out.DeviceName = Source.DeviceName;
  Out.Bytes.resize(ImageSize);
  Out.PageProtection.resize(ImageSize / profile::PageSize);
  std::copy_n(B.begin(), Headers, Out.Bytes.begin());
  std::fill_n(Out.PageProtection.begin(), pageSize(Headers) / profile::PageSize,
              2);
  std::vector<bool> UsedRaw(B.size());
  std::fill_n(UsedRaw.begin(), Headers, true);
  for (uint64_t I = 0; I < Count; ++I) {
    const uint64_t S = Opt + OptSize + 40 * I;
    const uint64_t VA = U32(S + 12), Raw = U32(S + 20), RawSize = U32(S + 16),
                   Virtual = U32(S + 8),
                   Size = pageSize(Virtual ? Virtual : RawSize);
    const uint32_t Flags = U32(S + 36);
    if (!Size || VA % profile::PageSize || VA > ImageSize ||
        Size > ImageSize - VA || RawSize > Size ||
        (RawSize && (Raw % FileAlignment || RawSize % FileAlignment ||
                     Raw > B.size() || RawSize > B.size() - Raw)) ||
        (Flags & 0x14000000))
      return invalid("malformed, shared, or unsupported image section");
    if (RawSize) {
      if (std::any_of(UsedRaw.begin() + Raw, UsedRaw.begin() + Raw + RawSize,
                      [](bool V) { return V; }))
        return invalid("overlapping image file ranges");
      std::fill_n(UsedRaw.begin() + Raw, RawSize, true);
    }
    const bool Read = Flags & 0x40000000, Write = Flags & 0x80000000,
               Execute = Flags & 0x20000000;
    const uint8_t Protect = Write     ? (Execute ? 0x80 : 8)
                            : Read    ? (Execute ? 0x20 : 2)
                            : Execute ? 0x10
                                      : 1;
    for (uint64_t P = VA / profile::PageSize;
         P < (VA + Size) / profile::PageSize; ++P) {
      if (Out.PageProtection[P])
        return invalid("overlapping image virtual ranges");
      Out.PageProtection[P] = Protect;
    }
    if (RawSize)
      std::copy_n(B.begin() + Raw, RawSize, Out.Bytes.begin() + VA);
  }
  // Unknown image holes have native reservation/commit semantics of their own.
  // Refuse them rather than synthesizing MEM_COMMIT/PAGE_NOACCESS metadata.
  if (std::find(Out.PageProtection.begin(), Out.PageProtection.end(), 0) !=
      Out.PageProtection.end())
    return invalid("image layout contains unsupported allocation holes");
  if (U32(Opt + 16) >= ImageSize)
    return invalid("image entry is outside its allocation");
  return Out;
}
} // namespace neverd::emulation
