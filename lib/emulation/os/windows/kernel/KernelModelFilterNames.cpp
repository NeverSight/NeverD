//===- KernelModelFilterNames.cpp - Owned local Filter Manager names ----===//
///
/// \file
/// Owned local Filter Manager names.
///

#include "../WindowsUnicode.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
constexpr uint32_t InfoSize = 120;
constexpr uint64_t MaxReferences = INT32_MAX - 1;
constexpr uint32_t NotInitialized = 0xc01c0007, InvalidNameRequest = 0xc01c0005,
                   CacheMiss = 0xc01c0018;
llvm::Error nameError(const llvm::Twine &T) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "Filter Manager names: " + T);
}
bool overlaps(uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
  return N && M && A < B + M && B < A + N;
}
} // namespace
llvm::Expected<uint64_t>
KernelModel::callFilterName(llvm::StringRef API, llvm::ArrayRef<uint64_t> A) {
  using namespace windows;
  if (API != "FltGetFileNameInformationUnsafe") {
    auto R = FilterNames.find(A[0]);
    if (R == FilterNames.end() || !R->second.References)
      return nameError("operation requires a live caller-owned name reference");
    auto &Record = R->second;
    if (API == "FltReferenceFileNameInformation") {
      if (Record.References >= MaxReferences)
        return nameError("name reference count exhausted");
      ++Record.References;
      return 0;
    }
    if (API == "FltParseFileNameInformation") {
      if (auto E = parseFilterName(A[0]))
        return E;
      return 0;
    }
    if (API == "FltReleaseFileNameInformation") {
      if (Record.References == 1 && !Record.Cached) {
        if (auto E = freeFilterName(A[0]))
          return E;
      } else
        --Record.References;
      return 0;
    }
    return nameError("unknown name operation");
  }
  if (!FilterManagerFileNames)
    return nameError("requires explicit filter_manager_file_names observation");
  if (!*FilterManagerFileNames)
    return uint64_t(NotInitialized);
  if (A[1])
    return nameError("non-null minifilter instance is unsupported");
  const uint32_t Options = uint32_t(A[2]), Format = Options & 255,
                 Method = Options & 0xff00;
  if (!Format || Format > 3 || Method < 0x100 || Method > 0x400)
    return uint64_t(StatusInvalidParameter);
  if (Format == 3)
    return nameError("short names require an explicit 8.3 provider");
  if (Options & ~0xffffu)
    return nameError("additional file name query flags are unsupported");
  auto F = NativeFiles.find(A[0]);
  if (F == NativeFiles.end())
    return nameError("query requires a known FILE_OBJECT identity");
  if (NativeOpenFiles.at(F->second.ID).State != NativeFileState::Open)
    return uint64_t(InvalidNameRequest);
  if (!F->second.Node || F->second.RawVolume)
    return nameError("raw volume names are unsupported");
  if (!A[3])
    return uint64_t(StatusInvalidParameter);
  if (auto E = objectBuffer(A[3], 8, true))
    return E;
  const auto &Volume = FiniteFileSystem->at(F->second.Volume);
  auto Units = windows_unicode::utf16(
      Format == 1 ? Volume.DeviceName + F->second.Node->Path : F->second.Name);
  if (!Units)
    return Units.takeError();
  std::u16string Name(Units->begin(), Units->end());
  auto Key = std::pair{A[0], Format};
  auto Found = FilterNameCache.find(Key);
  if (Method != 0x300 && Found != FilterNameCache.end()) {
    auto &Record = FilterNames.at(Found->second);
    if (Record.Name != Name)
      return nameError("cached file name invalidation is unsupported");
    if (Record.References >= MaxReferences)
      return nameError("name reference count exhausted");
    if (auto E = Memory.writeInteger(A[3], Found->second, 8))
      return E;
    ++Record.References;
    return uint64_t(StatusSuccess);
  }
  if (Method == 0x200)
    return uint64_t(CacheMiss);
  // This provider is an explicit local finite filesystem. It contains no
  // streams, short aliases, reparses, mounted directories or network shares.
  auto VolumeUnits = windows_unicode::utf16(Volume.DeviceName);
  if (!VolumeUnits)
    return VolumeUnits.takeError();
  const uint64_t Size = InfoSize + Name.size() * 2 + 2;
  const bool PagedArena = bool(GuestPageTables);
  const auto Cursor = PagedArena ? NextPagedAllocation : NextAllocation;
  const auto Limit = PagedArena ? PagedAllocationEnd : AllocationEnd;
  if (Name.size() > 32766 || Size + PoolAlignment > Limit - Cursor)
    return uint64_t(StatusInsufficientResources);
  auto Storage =
      allocatePhysicalBuffer(Size, PoolAlignment, 0, Size, PagedArena);
  if (!Storage)
    return Storage.takeError();
  const uint64_t Data = *Storage + InfoSize;
  std::vector<uint8_t> Bytes(Size);
  auto Store = [&](size_t At, uint64_t V, unsigned W) {
    if (W == 2)
      llvm::support::endian::write16le(Bytes.data() + At, V);
    else if (W == 4)
      llvm::support::endian::write32le(Bytes.data() + At, V);
    else
      llvm::support::endian::write64le(Bytes.data() + At, V);
  };
  Store(0, InfoSize, 2);
  Store(4, Format, 4);
  Store(8, Name.size() * 2, 2);
  Store(10, Name.size() * 2 + 2, 2);
  Store(16, Data, 8);
  Store(24, VolumeUnits->size() * 2, 2);
  Store(26, VolumeUnits->size() * 2, 2);
  Store(32, Data, 8);
  for (size_t I = 0; I < Name.size(); ++I)
    Store(InfoSize + I * 2, Name[I], 2);
  if (auto E = Memory.write(*Storage, Bytes))
    return E;
  if (auto E = Memory.writeInteger(A[3], *Storage, 8))
    return E;
  FilterNames.emplace(*Storage,
                      FilterNameRecord{Size, 1, Data, Format, Method != 0x300,
                                       false, std::move(Name),
                                       uint32_t(VolumeUnits->size())});
  if (Method != 0x300)
    FilterNameCache.emplace(Key, *Storage);
  return uint64_t(StatusSuccess);
}
llvm::Error KernelModel::parseFilterName(uint64_t Info) {
  auto &R = FilterNames.at(Info);
  if (R.Parsed)
    return llvm::Error::success();
  const auto &Name = R.Name;
  auto Slice = [&](uint64_t Field, size_t First, size_t Last,
                   bool Null = false) -> llvm::Error {
    const uint64_t Pointer = Null ? 0 : R.NameAddress + First * 2;
    if (auto E = Memory.writeInteger(Info + Field, (Last - First) * 2, 2))
      return E;
    if (auto E = Memory.writeInteger(Info + Field + 2, (Last - First) * 2, 2))
      return E;
    return Memory.writeInteger(Info + Field + 8, Pointer, 8);
  };
  size_t End = Name.size();
  if (R.Format == 2 && End > R.VolumeUnits + 1 && Name.back() == u'\\')
    --End;
  size_t Final = End;
  if (End > R.VolumeUnits) {
    const auto Last = Name.rfind(u'\\', End - 1);
    if (Last == std::u16string::npos || Last < R.VolumeUnits)
      return nameError("local name lost its volume separator");
    Final = Last + 1;
  }
  size_t Extension = End;
  bool HasExtension = false;
  if (Final < End) {
    auto Dot = Name.rfind(u'.', End - 1);
    if (Dot != std::u16string::npos && Dot >= Final) {
      Extension = Dot + 1;
      HasExtension = true;
    }
  }
  if (auto E = Slice(56, Extension, End, !HasExtension))
    return E;
  if (auto E = Slice(72, 0, 0, true))
    return E;
  if (auto E = Slice(88, Final, End, Final == End))
    return E;
  if (auto E = Slice(104, R.VolumeUnits, Final, End == R.VolumeUnits))
    return E;
  if (auto E = Memory.writeInteger(Info + 2, 15, 2))
    return E;
  R.Parsed = true;
  return llvm::Error::success();
}
llvm::Error KernelModel::freeFilterName(uint64_t Info) {
  const auto Size = FilterNames.at(Info).Size;
  if (auto E = prepareReleaseRange(Info, Size))
    return E;
  if (auto E = Physical.retire(Info))
    return E;
  FreedRanges.emplace(Info, Size);
  FilterNames.erase(Info);
  return llvm::Error::success();
}
llvm::Error KernelModel::validateFilterNameAccess(uint64_t A, uint32_t N,
                                                  bool Write) const {
  for (const auto &[Info, R] : FilterNames) {
    if (!overlaps(A, N, Info, R.Size))
      continue;
    if (Write || !R.References)
      return nameError(
          "name storage requires a live read-only caller reference");
    if (CurrentIRQL > 1)
      return nameError("paged name storage requires IRQL <= APC_LEVEL");
    if (!R.Parsed && overlaps(A, N, Info + 56, 64))
      return nameError("name components require FltParseFileNameInformation");
  }
  return llvm::Error::success();
}
llvm::Error KernelModel::releaseFilterNameCache() {
  for (const auto &[Info, R] : FilterNames) {
    if (R.References)
      return nameError("unload returned with live name references");
    if (auto E = canReleaseRange(Info, R.Size))
      return E;
  }
  FilterNameCache.clear();
  while (!FilterNames.empty())
    if (auto E = freeFilterName(FilterNames.begin()->first))
      return E;
  return llvm::Error::success();
}
llvm::Error KernelModel::retireFilterNamesForFile(uint64_t File) {
  std::vector<std::pair<uint64_t, uint32_t>> Keys;
  for (const auto &[Key, Info] : FilterNameCache)
    if (Key.first == File) {
      const auto &R = FilterNames.at(Info);
      if (!R.References)
        if (auto E = canReleaseRange(Info, R.Size))
          return E;
      Keys.push_back(Key);
    }
  for (const auto &Key : Keys) {
    const auto Info = FilterNameCache.at(Key);
    auto &R = FilterNames.at(Info);
    R.Cached = false;
    if (!R.References)
      if (auto E = freeFilterName(Info))
        return E;
    FilterNameCache.erase(Key);
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
