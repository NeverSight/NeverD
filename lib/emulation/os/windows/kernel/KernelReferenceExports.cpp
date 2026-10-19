//===- KernelReferenceExports.cpp - Bounded PE export metadata ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded PE export metadata.
///

#include "KernelReferenceExports.h"

#include "neverd/emulation/DriverSession.h"
#include "neverd/loader/BinaryImageModel.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "kernel export metadata: " + Message);
}
} // namespace

llvm::Expected<KernelReferenceExports>
KernelReferenceExports::load(const BinaryImage &Image,
                             const llvm::object::COFFObjectFile &Object,
                             uint64_t GuestBase, uint64_t MemoryLimit) {
  KernelReferenceExports Result;
  const auto *Header = Object.getPE32PlusHeader();
  const auto *Directory = Object.getDataDirectory(llvm::COFF::EXPORT_TABLE);
  if (!Header || !Directory || !Directory->RelativeVirtualAddress ||
      Directory->Size < 40 ||
      uint64_t(Directory->RelativeVirtualAddress) + Directory->Size >
          Header->SizeOfImage ||
      GuestBase > UINT64_MAX - Header->SizeOfImage)
    return invalid("invalid directory extent");

  // A span must have original bytes in a readable, non-writable data section.
  // Neither section names nor the directory's declared size grant visibility.
  auto Original =
      [&](uint32_t RVA,
          uint64_t Length) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
    if (!RVA || !Length || uint64_t(RVA) + Length > Header->SizeOfImage)
      return invalid("invalid RVA or table length");
    for (const auto &Section : Object.sections()) {
      const auto *Raw = Object.getCOFFSection(Section);
      const uint32_t Flags = Raw->Characteristics;
      if (!(Flags & llvm::COFF::IMAGE_SCN_MEM_READ) ||
          (Flags & (llvm::COFF::IMAGE_SCN_MEM_WRITE |
                    llvm::COFF::IMAGE_SCN_MEM_EXECUTE)) ||
          RVA < Raw->VirtualAddress)
        continue;
      const uint64_t Offset = uint64_t(RVA) - Raw->VirtualAddress;
      const uint64_t FileOffset = uint64_t(Raw->PointerToRawData) + Offset;
      if (Offset > Raw->SizeOfRawData ||
          Length > uint64_t(Raw->SizeOfRawData) - Offset ||
          FileOffset > Image.Raw.size() ||
          Length > Image.Raw.size() - FileOffset)
        continue;
      return llvm::ArrayRef(Image.Raw).slice(FileOffset, Length);
    }
    return invalid("metadata requires immutable file-backed data");
  };
  uint64_t ObservedBytes = 0;
  auto Include =
      [&](uint32_t RVA,
          uint64_t Length) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
    if (Length > MemoryLimit - ObservedBytes)
      return invalid("metadata observation budget exceeded");
    auto Bytes = Original(RVA, Length);
    if (!Bytes)
      return Bytes.takeError();
    ObservedBytes += Length;
    Result.Ranges.emplace_back(GuestBase + RVA, GuestBase + RVA + Length);
    return *Bytes;
  };
  auto String = [&](uint32_t RVA, uint64_t End) -> llvm::Expected<std::string> {
    const uint64_t Limit = std::min<uint64_t>(
        profile::MaxKernelExportNameSize + 1, End > RVA ? End - RVA : 0);
    std::string Name;
    for (uint64_t I = 0; I < Limit; ++I) {
      auto Byte = Original(RVA + I, 1);
      if (!Byte)
        return Byte.takeError();
      if (!(*Byte)[0]) {
        if (Name.empty())
          return invalid("empty export string");
        auto Bytes = Include(RVA, I + 1);
        if (!Bytes)
          return Bytes.takeError();
        return Name;
      }
      if ((*Byte)[0] < '!' || (*Byte)[0] > '~')
        return invalid("export string is not printable ASCII");
      Name.push_back((*Byte)[0]);
    }
    return invalid("unterminated or oversized export string");
  };
  auto Table = Include(Directory->RelativeVirtualAddress, 40);
  if (!Table)
    return Table.takeError();
  auto Field = [&](unsigned Offset) {
    return llvm::support::endian::read32le(Table->data() + Offset);
  };
  const uint32_t Functions = Field(20), Names = Field(24);
  if (Field(0) || !Functions || Functions > 65536 || Names > 65536 ||
      Names > profile::MaxImports ||
      uint64_t(Field(16)) + Functions - 1 > UINT32_MAX)
    return invalid("invalid or unsupported export count or ordinal base");
  auto Addresses = Include(Field(28), uint64_t(Functions) * 4);
  if (!Addresses)
    return Addresses.takeError();
  auto DLLName = String(Field(12), Header->SizeOfImage);
  if (!DLLName)
    return DLLName.takeError();
  const uint64_t ForwarderEnd =
      uint64_t(Directory->RelativeVirtualAddress) + Directory->Size;
  for (uint32_t I = 0; I < Functions; ++I) {
    const uint32_t RVA =
        llvm::support::endian::read32le(Addresses->data() + I * 4);
    if (RVA >= Header->SizeOfImage)
      return invalid("export target exceeds the image");
    if (RVA >= Directory->RelativeVirtualAddress && RVA < ForwarderEnd) {
      auto Forwarder = String(RVA, ForwarderEnd);
      if (!Forwarder)
        return Forwarder.takeError();
      const auto Dot = Forwarder->rfind('.');
      if (Dot == std::string::npos || !Dot || Dot + 1 == Forwarder->size())
        return invalid("malformed forwarder string");
    }
  }
  if (Names) {
    auto Pointers = Include(Field(32), uint64_t(Names) * 4);
    if (!Pointers)
      return Pointers.takeError();
    auto Ordinals = Include(Field(36), uint64_t(Names) * 2);
    if (!Ordinals)
      return Ordinals.takeError();
    std::string Previous;
    for (uint32_t I = 0; I < Names; ++I) {
      const auto Ordinal =
          llvm::support::endian::read16le(Ordinals->data() + I * 2);
      if (Ordinal >= Functions)
        return invalid("name ordinal exceeds the address table");
      auto Name =
          String(llvm::support::endian::read32le(Pointers->data() + I * 4),
                 Header->SizeOfImage);
      if (!Name)
        return Name.takeError();
      if (I && *Name <= Previous)
        return invalid("name table is not strictly sorted");
      Previous = *Name;
      Result.Names.emplace(
          std::move(*Name),
          llvm::support::endian::read32le(Addresses->data() + Ordinal * 4));
    }
  }
  std::sort(Result.Ranges.begin(), Result.Ranges.end());
  std::vector<std::pair<uint64_t, uint64_t>> Merged;
  for (auto [Begin, End] : Result.Ranges) {
    if (!Merged.empty() && Begin <= Merged.back().second)
      Merged.back().second = std::max(Merged.back().second, End);
    else
      Merged.emplace_back(Begin, End);
  }
  Result.Ranges = std::move(Merged);
  // Export tables contain RVAs, never relocated absolute pointers. Unrelated
  // relocated data sharing the page stays opaque or belongs to another owner.
  std::map<uint64_t, std::vector<uint8_t>> Pages;
  for (auto [Begin, End] : Result.Ranges) {
    auto Bytes = Original(Begin - GuestBase, End - Begin);
    if (!Bytes)
      return Bytes.takeError();
    for (uint64_t Address = Begin; Address < End;) {
      const uint64_t Page = Address & ~uint64_t(4095);
      auto [It, New] = Pages.try_emplace(Page);
      if (New) {
        if (Pages.size() > MemoryLimit / 4096)
          return invalid("metadata page budget exceeded");
        It->second.resize(4096);
      }
      const uint64_t Count = std::min(End - Address, Page + 4096 - Address);
      std::copy_n(Bytes->begin() + (Address - Begin), Count,
                  It->second.begin() + (Address - Page));
      Address += Count;
    }
  }
  for (const auto &Relocation : Image.BaseRelocations) {
    if (Relocation.Type == llvm::COFF::IMAGE_REL_BASED_ABSOLUTE)
      continue;
    if (Relocation.Address < Image.Base)
      return invalid("invalid relocation address");
    const uint64_t RVA = Relocation.Address - Image.Base;
    if (RVA >= Header->SizeOfImage)
      return invalid("relocation exceeds the image");
    const uint64_t Address = GuestBase + RVA;
    // Eight bytes is conservative for the supported x64 relocation family.
    if (Result.overlapsBytes(Address, 8))
      return invalid("relocation overlaps immutable export metadata bytes");
  }
  for (auto &[Address, Bytes] : Pages) {
    if (!Result.Pages.empty() &&
        Result.Pages.back().Address + Result.Pages.back().Bytes.size() ==
            Address)
      Result.Pages.back().Bytes.insert(Result.Pages.back().Bytes.end(),
                                       Bytes.begin(), Bytes.end());
    else
      Result.Pages.push_back({Address, Read, std::move(Bytes)});
  }
  return Result;
}

bool KernelReferenceExports::permitsRead(uint64_t Address,
                                         uint32_t Size) const {
  if (!Size || Size > UINT64_MAX - Address)
    return false;
  auto It = std::upper_bound(Ranges.begin(), Ranges.end(),
                             std::pair{Address, UINT64_MAX});
  return It != Ranges.begin() && Address >= std::prev(It)->first &&
         Address + Size <= std::prev(It)->second;
}
bool KernelReferenceExports::overlapsPage(uint64_t Address,
                                          uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Address)
    return false;
  for (const auto &Page : Pages)
    if (Address < Page.Address + Page.Bytes.size() &&
        Page.Address < Address + Size)
      return true;
  return false;
}
bool KernelReferenceExports::overlapsBytes(uint64_t Address,
                                           uint64_t Size) const {
  if (!Size || Size > UINT64_MAX - Address)
    return false;
  auto It = std::upper_bound(Ranges.begin(), Ranges.end(),
                             std::pair{Address, UINT64_MAX});
  if (It != Ranges.begin() && std::prev(It)->second > Address)
    return true;
  return It != Ranges.end() && It->first < Address + Size;
}
} // namespace neverd::emulation
