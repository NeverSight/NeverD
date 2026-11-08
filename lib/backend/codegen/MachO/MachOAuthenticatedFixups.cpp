//===- MachOAuthenticatedFixups.cpp - Install dyld signing recipes
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Extend authenticated chains without changing existing slot identities.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOAuthenticatedFixups.h"

#include "MachODependencyState.h"

#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

using namespace neverd;
using namespace llvm::MachO;
namespace arm64e = neverd::macho_loader::arm64e;

namespace {
#define MACHO_AUTH_FIXUP_VALUE(Type, Name, Value) constexpr Type Name = Value;
#define MACHO_AUTH_FIXUP_TEXT(Name, Text) constexpr char Name[] = Text;
#include "MachOAuthenticatedFixups.def"

llvm::Error fail(llvm::StringRef Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 llvm::Twine(kErrorPrefix) + Message);
}

uint64_t maximum(arm64e::PointerField Field) {
  return llvm::maskTrailingOnes<uint64_t>(Field.Width);
}

using SegmentIndex = std::vector<std::pair<uint64_t, unsigned>>;
SegmentIndex segmentIndex(const MachOChainedFixups &Chains) {
  SegmentIndex Index;
  Index.reserve(Chains.Segments.size());
  for (unsigned I = 0; I < Chains.Segments.size(); ++I)
    if (Chains.Segments[I].VMSize)
      Index.emplace_back(Chains.Segments[I].VMAddr, I);
  llvm::sort(Index);
  return Index;
}

std::optional<unsigned> segmentFor(const MachOChainedFixups &Chains,
                                   const SegmentIndex &Index, uint64_t VA) {
  auto It = std::upper_bound(Index.begin(), Index.end(), VA,
                             [](uint64_t Address, const auto &Entry) {
                               return Address < Entry.first;
                             });
  if (It == Index.begin())
    return std::nullopt;
  const auto &[Start, I] = *std::prev(It);
  if (VA - Start >= Chains.Segments[I].VMSize || I == Chains.LinkeditIndex)
    return std::nullopt;
  return I;
}

llvm::Error validateRecipe(const MachOAuthenticatedPointer &Recipe,
                           const MachOChainedFixups &Chains) {
  if (Recipe.Key > maximum(arm64e::kKey) || Recipe.EncodedValue)
    return fail(kRecipe);
  if (Recipe.Symbol.empty()) {
    if (Recipe.Target == InvalidVA || Recipe.LibraryOrdinal ||
        !Recipe.Module.empty() || Recipe.Addend || Recipe.WeakImport)
      return fail(kRecipe);
    return llvm::Error::success();
  }
  if (Recipe.Target != InvalidVA || !Recipe.LibraryOrdinal ||
      llvm::StringRef(Recipe.Symbol).contains('\0'))
    return fail(kRecipe);
  const auto Ordinal = *Recipe.LibraryOrdinal;
  if (Ordinal < kMinLibraryOrdinal || Ordinal > kMaxLibraryOrdinal ||
      Ordinal > int64_t(Chains.Libraries.size()) ||
      (Ordinal > 0 ? Recipe.Module != Chains.Libraries[Ordinal - 1]
                   : !Recipe.Module.empty()))
    return fail(kImport);
  return llvm::Error::success();
}

using ImportIdentity = std::tuple<int64_t, int64_t, unsigned, std::string>;
ImportIdentity identity(const MachOChainedImport &Import) {
  return {Import.Ordinal, Import.Addend, Import.Flags, Import.Name};
}

llvm::Expected<uint64_t> encode(const MachOAuthenticatedPointer &Recipe,
                                MachOChainedFixups &Chains,
                                const SegmentIndex &Index,
                                const arm64e::ChainFormat &Format,
                                std::map<ImportIdentity, uint64_t> &Imports) {
  if (auto Error = validateRecipe(Recipe, Chains))
    return Error;
  uint64_t Word = uint64_t(1) << arm64e::kAuthentication.Low |
                  uint64_t(Recipe.Key) << arm64e::kKey.Low |
                  uint64_t(Recipe.Diversity) << arm64e::kDiversity.Low |
                  uint64_t(Recipe.AddressDiversity)
                      << arm64e::kAddressDiversity.Low;
  if (!Recipe.Symbol.empty()) {
    if (Format.Segmented)
      return fail(kImport);
    MachOChainedImport Import{*Recipe.LibraryOrdinal, Recipe.Addend,
                              unsigned(Recipe.WeakImport), Recipe.Symbol};
    auto [It, Added] = Imports.emplace(identity(Import), Chains.Imports.size());
    if (It->second > llvm::maskTrailingOnes<uint64_t>(Format.OrdinalBits))
      return fail(kImport);
    if (Added)
      Chains.Imports.push_back(std::move(Import));
    return Word | uint64_t(1) << arm64e::kBind.Low | It->second;
  }
  const auto Target = segmentFor(Chains, Index, Recipe.Target);
  if (!Target || !(Chains.Segments[*Target].Protections & VM_PROT_READ))
    return fail(kTarget);
  if (Format.Segmented) {
    const auto Offset = Recipe.Target - Chains.Segments[*Target].VMAddr;
    if (*Target > maximum(arm64e::kSegmentedTargetIndex) ||
        Offset > maximum(arm64e::kSegmentedTargetOffset))
      return fail(kTarget);
    return Word | Offset |
           uint64_t(*Target) << arm64e::kSegmentedTargetIndex.Low;
  }
  if (Recipe.Target < Chains.HeaderVA ||
      Recipe.Target - Chains.HeaderVA > maximum(arm64e::kAuthenticatedTarget))
    return fail(kTarget);
  return Word | (Recipe.Target - Chains.HeaderVA);
}

llvm::Error replay(const MachOChainedFixups &Chains,
                   llvm::ArrayRef<MachOAuthenticatedFixup> Fixups) {
  // Build once: chain traversal order need not match address order on a
  // multi-start page. Replays then query each requested slot logarithmically.
  std::vector<std::pair<uint64_t, std::pair<unsigned, uint64_t>>> Slots;
  for (unsigned I = 0; I < Chains.Segments.size(); ++I)
    for (const auto &Slot : Chains.Segments[I].Slots)
      Slots.push_back({Chains.Segments[I].VMAddr + Slot.Offset, {I, Slot.Raw}});
  llvm::sort(Slots);
  for (const auto &Fixup : Fixups) {
    if (auto Error = validateRecipe(Fixup.Recipe, Chains))
      return Error;
    auto It = std::lower_bound(
        Slots.begin(), Slots.end(), Fixup.Address,
        [](const auto &Slot, uint64_t VA) { return Slot.first < VA; });
    if (It == Slots.end() || It->first != Fixup.Address)
      return fail(kReplay);
    const auto Format =
        arm64e::chainFormat(Chains.Segments[It->second.first].PointerFormat);
    if (!Format)
      return fail(kPointerFormat);
    const auto Word = arm64e::decodePointerWord(It->second.second, *Format);
    const auto &Recipe = Fixup.Recipe;
    if (!Word || !Word->Authenticated || Word->Key != Recipe.Key ||
        Word->Diversity != Recipe.Diversity ||
        Word->AddressDiversity != Recipe.AddressDiversity ||
        Word->Bind != !Recipe.Symbol.empty())
      return fail(kReplay);
    if (Word->Bind) {
      const auto &Import = Chains.Imports[Word->Target];
      if (Import.Ordinal != *Recipe.LibraryOrdinal ||
          Import.Name != Recipe.Symbol || Import.Addend != Recipe.Addend ||
          bool(Import.Flags) != Recipe.WeakImport)
        return fail(kReplay);
    } else {
      const auto Base = Format->Segmented
                            ? Chains.Segments[Word->SegmentIndex].VMAddr
                            : Chains.HeaderVA;
      if (Base + Word->Target != Recipe.Target)
        return fail(kReplay);
    }
  }
  return llvm::Error::success();
}
} // namespace

llvm::Error neverd::installMachOAuthenticatedFixups(
    std::vector<uint8_t> &Binary,
    llvm::ArrayRef<MachOAuthenticatedFixup> Fixups) {
  if (Fixups.empty())
    return llvm::Error::success();
  if (Binary.size() > UINT32_MAX)
    return fail(kExtent);
  MachOChainedFixups Chains;
  auto Before = readMachODependencyState(Binary, &Chains);
  if (!Before)
    return Before.takeError();
  if (Before->CPUType != CPU_TYPE_ARM64 ||
      !arm64e::isSubtype(Before->CPUSubtype) || !Chains.CommandOffset)
    return fail(kImageABI);
  // A staged image may already have invalidated signature hashes/code limits.
  // Only own the opaque extent here; the publication owner validates the
  // original signing policy and replaces the signature after all mutations.
  std::optional<std::pair<uint32_t, linkedit_data_command>> Signature;
  bool BadSignature = false;
  forEachMachOLoadCommand(
      Binary.data(), Binary.size(),
      [&](const uint8_t *Command, uint32_t ID, uint32_t Size, bool) {
        if (ID != LC_CODE_SIGNATURE)
          return;
        if (Signature || Size != sizeof(linkedit_data_command)) {
          BadSignature = true;
          return;
        }
        const auto Data =
            *reinterpret_cast<const linkedit_data_command *>(Command);
        if (!Data.datasize ||
            Data.dataoff < Chains.Segments[Chains.LinkeditIndex].FileOff ||
            uint64_t(Data.dataoff) + Data.datasize != Binary.size())
          BadSignature = true;
        Signature.emplace(Command - Binary.data(), Data);
      });
  if (BadSignature)
    return fail(kSignature);
  uint16_t PointerFormat = kDefaultPointerFormat;
  for (const auto &Segment : Chains.Segments)
    if (Segment.PointerFormat)
      PointerFormat = Segment.PointerFormat;
  const auto Format = arm64e::chainFormat(PointerFormat);
  if (!Format)
    return fail(kPointerFormat);
  const auto Index = segmentIndex(Chains);
  std::map<ImportIdentity, uint64_t> Imports;
  for (size_t I = 0; I < Chains.Imports.size(); ++I)
    Imports.emplace(identity(Chains.Imports[I]), I);
  std::vector<const MachOAuthenticatedFixup *> Requests;
  Requests.reserve(Fixups.size());
  for (const auto &Fixup : Fixups)
    Requests.push_back(&Fixup);
  llvm::sort(Requests, [](const auto *Left, const auto *Right) {
    return Left->Address < Right->Address;
  });
  for (const auto *Fixup : Requests) {
    const auto Owner = segmentFor(Chains, Index, Fixup->Address);
    if (!Owner)
      return fail(kStorage);
    auto &Segment = Chains.Segments[*Owner];
    const auto Offset = Fixup->Address - Segment.VMAddr;
    const auto Page =
        Segment.PageSize ? Segment.PageSize : arm64e::kLargePageBytes;
    if (Segment.VMAddr < Chains.HeaderVA ||
        (Segment.Protections &
         (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) !=
            (VM_PROT_READ | VM_PROT_WRITE) ||
        Offset % Format->Stride ||
        !rangeInBounds(Offset, arm64e::kPointerBytes, Segment.FileSize) ||
        !rangeInBounds(Offset % Page, arm64e::kPointerBytes, Page))
      return fail(kStorage);
    auto Word = encode(Fixup->Recipe, Chains, Index, *Format, Imports);
    if (!Word)
      return Word.takeError();
    Segment.PointerFormat = PointerFormat;
    Segment.PageSize = Page;
    Segment.Slots.push_back({Offset, *Word});
  }

  auto Candidate = Binary;
  constexpr size_t StartHeader =
      offsetof(dyld_chained_starts_in_segment, page_start);
  const auto Next = Format->Segmented ? arm64e::kSegmentedNext : arm64e::kNext;
  const auto NextMask = maximum(Next) << Next.Low;
  std::vector<uint8_t> Starts(sizeof(uint32_t) * (Chains.Segments.size() + 1));
  if (Starts.size() > UINT32_MAX)
    return fail(kExtent);
  writeLE<uint32_t>(Starts.data(), Chains.Segments.size());
  for (size_t I = 0; I < Chains.Segments.size(); ++I) {
    auto &Segment = Chains.Segments[I];
    if (Segment.Slots.empty())
      continue;
    llvm::sort(Segment.Slots, [](const auto &Left, const auto &Right) {
      return Left.Offset < Right.Offset;
    });
    const auto PageCount = Segment.Slots.back().Offset / Segment.PageSize + 1;
    if (PageCount > UINT16_MAX)
      return fail(kExtent);
    std::vector<uint16_t> PageStarts(PageCount, DYLD_CHAINED_PTR_START_NONE);
    std::vector<uint16_t> Roots;
    for (size_t First = 0; First < Segment.Slots.size();) {
      const auto Page = Segment.Slots[First].Offset / Segment.PageSize;
      Roots.clear();
      bool ChainStart = true;
      size_t End = First;
      for (; End < Segment.Slots.size() &&
             Segment.Slots[End].Offset / Segment.PageSize == Page;
           ++End) {
        auto &Slot = Segment.Slots[End];
        if (ChainStart)
          Roots.push_back(Slot.Offset % Segment.PageSize);
        uint64_t Delta = 0;
        if (End + 1 < Segment.Slots.size()) {
          const auto &Following = Segment.Slots[End + 1];
          if (Following.Offset - Slot.Offset < arm64e::kPointerBytes)
            return fail(kOverlap);
          if (Following.Offset / Segment.PageSize == Page &&
              (Following.Offset - Slot.Offset) / Format->Stride <=
                  maximum(Next))
            Delta = (Following.Offset - Slot.Offset) / Format->Stride;
        }
        Slot.Raw = (Slot.Raw & ~NextMask) | Delta << Next.Low;
        writeLE<uint64_t>(Candidate.data() + Segment.FileOff + Slot.Offset,
                          Slot.Raw);
        ChainStart = Delta == 0;
      }
      if (Roots.size() == 1) {
        PageStarts[Page] = Roots.front();
      } else {
        if (PageStarts.size() >= DYLD_CHAINED_PTR_START_MULTI ||
            Roots.size() > DYLD_CHAINED_PTR_START_MULTI - PageStarts.size())
          return fail(kExtent);
        PageStarts[Page] = DYLD_CHAINED_PTR_START_MULTI | PageStarts.size();
        Roots.back() |= DYLD_CHAINED_PTR_START_LAST;
        PageStarts.insert(PageStarts.end(), Roots.begin(), Roots.end());
      }
      First = End;
    }
    Starts.resize(llvm::alignTo(Starts.size(), arm64e::kPointerBytes), 0);
    const auto At = Starts.size();
    const auto Size =
        llvm::alignTo(StartHeader + PageStarts.size() * sizeof(uint16_t),
                      arm64e::kPointerBytes);
    if (At > UINT32_MAX || Size > UINT32_MAX - At)
      return fail(kExtent);
    writeLE<uint32_t>(Starts.data() + sizeof(uint32_t) * (I + 1), At);
    Starts.resize(At + Size, 0);
    dyld_chained_starts_in_segment Header{};
    Header.size = Size;
    Header.page_size = Segment.PageSize;
    Header.pointer_format = Segment.PointerFormat;
    Header.segment_offset = Segment.VMAddr - Chains.HeaderVA;
    Header.page_count = PageCount;
    std::memcpy(Starts.data() + At, &Header, StartHeader);
    for (size_t P = 0; P < PageStarts.size(); ++P)
      writeLE<uint16_t>(Starts.data() + At + StartHeader + P * sizeof(uint16_t),
                        PageStarts[P]);
  }
  std::vector<uint8_t> Names, Table;
  for (const auto &Import : Chains.Imports) {
    if (Names.size() > UINT32_MAX ||
        Import.Name.size() >= UINT32_MAX - Names.size())
      return fail(kExtent);
    const auto At = Table.size();
    Table.resize(At + sizeof(dyld_chained_import_addend64));
    writeLE<uint64_t>(Table.data() + At,
                      uint64_t(uint16_t(Import.Ordinal)) |
                          uint64_t(Import.Flags) << kImportWeakBit |
                          uint64_t(Names.size()) << kImportNameBit);
    writeLE<uint64_t>(Table.data() + At + sizeof(uint64_t), Import.Addend);
    Names.insert(Names.end(), Import.Name.begin(), Import.Name.end());
    Names.push_back(0);
  }
  dyld_chained_fixups_header Header{};
  std::vector<uint8_t> Metadata(
      llvm::alignTo(sizeof(Header), arm64e::kPointerBytes), 0);
  Header.starts_offset = Metadata.size();
  Metadata.insert(Metadata.end(), Starts.begin(), Starts.end());
  Metadata.resize(llvm::alignTo(Metadata.size(), arm64e::kPointerBytes), 0);
  if (Metadata.size() > UINT32_MAX ||
      Table.size() > UINT32_MAX - Metadata.size() ||
      Names.size() > UINT32_MAX - Metadata.size() - Table.size())
    return fail(kExtent);
  Header.imports_offset = Metadata.size();
  Metadata.insert(Metadata.end(), Table.begin(), Table.end());
  Header.symbols_offset = Metadata.size();
  Metadata.insert(Metadata.end(), Names.begin(), Names.end());
  Header.imports_count = Chains.Imports.size();
  Header.imports_format = DYLD_CHAINED_IMPORT_ADDEND64;
  std::memcpy(Metadata.data(), &Header, sizeof(Header));
  Candidate.resize(llvm::alignTo(Candidate.size(), arm64e::kPointerBytes), 0);
  if (Candidate.size() > UINT32_MAX ||
      Metadata.size() > UINT32_MAX - Candidate.size())
    return fail(kExtent);
  auto *Command = reinterpret_cast<linkedit_data_command *>(
      Candidate.data() + Chains.CommandOffset);
  Command->dataoff = Candidate.size();
  Command->datasize = Metadata.size();
  Candidate.insert(Candidate.end(), Metadata.begin(), Metadata.end());
  if (Signature) {
    // codesign requires the old signature to terminate the file. Retain its
    // bytes at the new end without deleting any potentially shared old bytes.
    const auto At = llvm::alignTo(Candidate.size(), kCodeSignatureAlignment);
    const auto &Old = Signature->second;
    if (At > UINT32_MAX || Old.datasize > UINT32_MAX - At)
      return fail(kExtent);
    Candidate.resize(At, 0);
    Candidate.insert(Candidate.end(), Binary.begin() + Old.dataoff,
                     Binary.begin() + Old.dataoff + Old.datasize);
    auto *Relocated = reinterpret_cast<linkedit_data_command *>(
        Candidate.data() + Signature->first);
    Relocated->dataoff = At;
  }
  const auto &Linkedit = Chains.Segments[Chains.LinkeditIndex];
  auto Segment =
      readMachOSegment(Candidate.data() + Linkedit.CommandOffset, true);
  Segment.FileSize = Candidate.size() - Segment.FileOff;
  Segment.VMSize = std::max(
      Segment.VMSize, llvm::alignTo(Segment.FileSize, arm64e::kLargePageBytes));
  writeMachOSegment(Candidate.data() + Linkedit.CommandOffset, true, Segment);
  MachOChainedFixups Installed;
  auto After = readMachODependencyState(Candidate, &Installed);
  if (!After)
    return After.takeError();
  if (auto Error = preserveMachODependencyState(*Before, *After))
    return Error;
  if (auto Error = replay(Installed, Fixups))
    return Error;
  Binary.swap(Candidate);
  return llvm::Error::success();
}

llvm::Error neverd::verifyMachOAuthenticatedFixups(
    llvm::ArrayRef<uint8_t> Binary,
    llvm::ArrayRef<MachOAuthenticatedFixup> Fixups) {
  if (Fixups.empty())
    return llvm::Error::success();
  MachOChainedFixups Chains;
  auto State = readMachODependencyState(Binary, &Chains);
  if (!State)
    return State.takeError();
  if (State->CPUType != CPU_TYPE_ARM64 ||
      !arm64e::isSubtype(State->CPUSubtype) || !Chains.CommandOffset)
    return fail(kImageABI);
  return replay(Chains, Fixups);
}
