//===- WindowsProcessMemory.cpp - Windows user virtual memory ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcess.h"

#include "neverd/emulation/CPU.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation::windows_process {
namespace {
using namespace value;
MemoryResult rejected(uint32_t Code) { return {0, Code}; }
MemoryResult unmodeled() { return {0, 0, true}; }

std::optional<unsigned> permissions(uint32_t Protection) {
  switch (Protection) {
#define NEVERD_WINDOWS_MEMORY_PROTECTION(Name, Rights)                         \
  case Name:                                                                   \
    return UserAccessible | (Rights);
#include "WindowsProcessMemory.def"
#undef NEVERD_WINDOWS_MEMORY_PROTECTION
  default:
    return std::nullopt;
  }
}
std::optional<uint32_t> protection(unsigned Permissions) {
  switch (Permissions) {
#define NEVERD_WINDOWS_MEMORY_PROTECTION(Name, Rights)                         \
  case UserAccessible | (Rights):                                              \
    return Name;
#include "WindowsProcessMemory.def"
#undef NEVERD_WINDOWS_MEMORY_PROTECTION
  default:
    return std::nullopt;
  }
}
MemoryResult protectionFailure(uint32_t Protection) {
  const uint32_t Base = Protection & PageBaseMask;
  if (!Base || (Base & (Base - 1)))
    return rejected(ErrorInvalidParameter);
  if (Protection & ~PageBaseMask || Base == PageExecuteOnly)
    return unmodeled();
  return rejected(ErrorInvalidParameter);
}
// Align both ends of a nonempty user range without ever adding unchecked
// guest values. Reservation starts use allocation granularity, ends use pages.
bool roundRange(uint64_t &Address, uint64_t &Size, uint64_t Alignment) {
  if (!Size || Address >= UserLimit || Size > UserLimit - Address)
    return false;
  const uint64_t End = (Address + Size + PageSize - 1) & ~(PageSize - 1);
  Address &= ~(Alignment - 1);
  Size = End - Address;
  return true;
}
} // namespace

VirtualMemory::VirtualMemory(AddressSpace &Space, const Image &Image,
                             const ProcessOptions &Options)
    : VirtualMemory(Space, Options) {
  llvm::cantFail(reserveImage(Image.Base, Image.Size, false));
}
VirtualMemory::VirtualMemory(AddressSpace &Space, const ProcessOptions &Options)
    : Space(Space) {
  auto Add = [&](uint64_t Base, uint64_t Size, uint32_t Protection,
                 uint32_t Type, Owner Kind) {
    Ranges.emplace(Base, Reservation{Size, Protection, Type, Kind});
  };
  Add(TEB, EnvironmentEnd - TEB, PageReadWrite, MemPrivate, Owner::Runtime);
  Add(GateBase, GateSize, PageExecuteRead, MemPrivate, Owner::Runtime);
  Add(HeapHandle, PageSize, PageNoAccess, MemPrivate, Owner::Runtime);
  Add(HeapBase, HeapLimit - HeapBase, PageReadWrite, MemPrivate, Owner::Heap);
  Add(StackTop - Options.StackSize, Options.StackSize, PageReadWrite,
      MemPrivate, Owner::Stack);
  Add(StackTop - Options.StackSize - PageSize, PageSize, PageNoAccess,
      MemPrivate, Owner::Runtime);
  Add(StackTop, PageSize, PageNoAccess, MemPrivate, Owner::Runtime);
}
llvm::Expected<uint64_t> VirtualMemory::reserveImage(uint64_t Preferred,
                                                     uint64_t Size,
                                                     bool Relocatable) {
  if (Preferred < ImageAlignment || Preferred % ImageAlignment ||
      Preferred >= UserLimit || !Size || Size % PageSize ||
      Size > UserLimit - Preferred)
    return failure(text::Layout);
  auto Next = Ranges.lower_bound(Preferred);
  if ((Next != Ranges.end() && Next->first < Preferred + Size) ||
      (Next != Ranges.begin() &&
       std::prev(Next)->first + std::prev(Next)->second.Size > Preferred)) {
    if (!Relocatable)
      return failure(text::Layout);
    auto Gap = findGap(Size, false);
    if (!Gap)
      return failure(text::Layout);
    Preferred = *Gap;
  }
  Ranges.emplace(Preferred, Reservation{Size, PageExecuteWriteCopy, MemImage,
                                        Owner::Image});
  return Preferred;
}
llvm::Error VirtualMemory::releaseImage(uint64_t Base) {
  auto I = Ranges.find(Base);
  if (I == Ranges.end() || I->second.Kind != Owner::Image)
    return failure(text::Layout);
  if (auto E = decommit(Base, I->second.Size))
    return E;
  Ranges.erase(I);
  return llvm::Error::success();
}
VirtualMemory::Reservations::iterator
VirtualMemory::containing(uint64_t Address, uint64_t Size) {
  auto I = Ranges.upper_bound(Address);
  if (I == Ranges.begin())
    return Ranges.end();
  --I;
  const uint64_t Offset = Address - I->first;
  return Offset < I->second.Size && Size <= I->second.Size - Offset
             ? I
             : Ranges.end();
}
std::optional<uint64_t> VirtualMemory::findGap(uint64_t Size,
                                               bool TopDown) const {
  auto Fit = [&](uint64_t Begin, uint64_t End) -> std::optional<uint64_t> {
    Begin = std::max(Begin, ImageAlignment);
    if (Begin >= End || Size > End - Begin)
      return std::nullopt;
    const uint64_t Candidate =
        TopDown ? (End - Size) & ~(ImageAlignment - 1)
                : (Begin + ImageAlignment - 1) & ~(ImageAlignment - 1);
    if (Candidate < Begin || Candidate >= End || Size > End - Candidate)
      return std::nullopt;
    return Candidate;
  };
  uint64_t Cursor = TopDown ? UserLimit : ImageAlignment;
  if (TopDown) {
    for (auto I = Ranges.rbegin(); I != Ranges.rend(); ++I) {
      if (auto Gap = Fit(I->first + I->second.Size, Cursor))
        return Gap;
      Cursor = I->first;
    }
    return Fit(ImageAlignment, Cursor);
  }
  for (const auto &[Base, R] : Ranges) {
    if (auto Gap = Fit(Cursor, Base))
      return Gap;
    Cursor = Base + R.Size;
  }
  return Fit(Cursor, UserLimit);
}
llvm::Expected<bool> VirtualMemory::commit(uint64_t Address, uint64_t Size,
                                           unsigned Permissions) {
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  uint64_t Needed = Size;
  for (const auto &M : *Mappings) {
    const uint64_t Begin = std::max(Address, M.Address);
    const uint64_t End = std::min(Address + Size, M.Address + M.Size);
    if (Begin < End)
      Needed -= End - Begin;
  }
  auto RAM = Space.physicalMemory();
  if (Needed > RAM->limit() - RAM->allocatedBytes())
    return false;
  // Independently owned pages allow partial decommit to return its capacity.
  // Stage every allocation before publishing a single new virtual mapping.
  std::vector<std::pair<uint64_t, std::shared_ptr<MemoryRegion>>> Pages;
  Pages.reserve(Needed / PageSize);
  size_t Index = 0;
  for (uint64_t Page = Address; Page < Address + Size; Page += PageSize) {
    while (Index < Mappings->size() &&
           (*Mappings)[Index].Address + (*Mappings)[Index].Size <= Page)
      ++Index;
    if (Index < Mappings->size() && (*Mappings)[Index].Address <= Page)
      continue;
    auto Region = RAM->allocate(PageSize);
    if (!Region) {
      auto E = Region.takeError();
      if (!E.isA<GuestMemoryLimitError>())
        return std::move(E);
      llvm::consumeError(std::move(E));
      return false;
    }
    Pages.emplace_back(Page, std::move(*Region));
  }
  size_t Published = 0;
  auto Rollback = [&](llvm::Error E) {
    for (size_t I = 0; I < Published; ++I)
      E = llvm::joinErrors(std::move(E), Space.unmap(Pages[I].first, PageSize));
    return E;
  };
  for (auto &[Page, Region] : Pages) {
    if (auto E = Space.mapRegion(Page, Region, 0, PageSize, Permissions)) {
      const bool Exhausted = E.isA<GuestMemoryLimitError>();
      if (!Exhausted)
        return Rollback(std::move(E));
      llvm::consumeError(std::move(E));
      if (auto Undo = Rollback(llvm::Error::success()))
        return std::move(Undo);
      return false;
    }
    ++Published;
  }
  // Recommit preserves existing data but installs the requested protection.
  if (auto E = Space.protect(Address, Size, Permissions))
    return Rollback(std::move(E));
  return true;
}
llvm::Error VirtualMemory::decommit(uint64_t Address, uint64_t Size) {
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  for (const auto &M : *Mappings) {
    const uint64_t Begin = std::max(Address, M.Address);
    const uint64_t End = std::min(Address + Size, M.Address + M.Size);
    if (Begin < End)
      if (auto E = Space.unmap(Begin, End - Begin))
        return E;
  }
  return llvm::Error::success();
}
llvm::Expected<MemoryResult> VirtualMemory::allocate(uint64_t Address,
                                                     uint64_t Size,
                                                     uint32_t Type,
                                                     uint32_t Protection) {
  if (Type & ~(MemReserve | MemCommit | MemTopDown))
    return unmodeled();
  if (!(Type & (MemReserve | MemCommit)) || !Size)
    return rejected(ErrorInvalidParameter);
  auto Rights = permissions(Protection);
  if (!Rights)
    return protectionFailure(Protection);
  const bool Reserve = (Type & MemReserve) || !Address;
  const bool Automatic = !Address;
  if (!roundRange(Address, Size, Reserve ? ImageAlignment : PageSize))
    return rejected(ErrorNotEnoughMemory);
  if (Reserve) {
    if (Ranges.size() >= MaxReservations)
      return rejected(ErrorNotEnoughMemory);
    if (Automatic) {
      auto Gap = findGap(Size, Type & MemTopDown);
      if (!Gap)
        return rejected(ErrorNotEnoughMemory);
      Address = *Gap;
    }
    auto Next = Ranges.lower_bound(Address);
    if (Address < ImageAlignment ||
        (Next != Ranges.end() && Next->first < Address + Size) ||
        (Next != Ranges.begin() &&
         std::prev(Next)->first + std::prev(Next)->second.Size > Address))
      return rejected(ErrorInvalidAddress);
  } else {
    auto I = containing(Address, Size);
    if (I == Ranges.end())
      return rejected(ErrorInvalidAddress);
    if (I->second.Kind != Owner::Virtual)
      return unmodeled();
  }
  if (Type & MemCommit) {
    auto Committed = commit(Address, Size, *Rights);
    if (!Committed)
      return Committed.takeError();
    if (!*Committed)
      return rejected(ErrorNotEnoughMemory);
  }
  if (Reserve)
    Ranges.emplace(Address,
                   Reservation{Size, Protection, MemPrivate, Owner::Virtual});
  return MemoryResult{Address};
}
llvm::Expected<MemoryResult> VirtualMemory::free(uint64_t Address,
                                                 uint64_t Size, uint32_t Type) {
  if (Type != MemRelease && Type != MemDecommit)
    return Type & ~(MemRelease | MemDecommit) ? unmodeled()
                                              : rejected(ErrorInvalidParameter);
  if (Type == MemRelease && Size)
    return rejected(ErrorInvalidParameter);
  auto I = containing(Address, Size);
  if (I == Ranges.end())
    return rejected(ErrorInvalidAddress);
  if (I->second.Kind != Owner::Virtual)
    return unmodeled();
  if (Type == MemRelease || !Size) {
    if (Address != I->first)
      return rejected(ErrorInvalidAddress);
    Size = I->second.Size;
  } else if (!roundRange(Address, Size, PageSize) ||
             containing(Address, Size) != I)
    return rejected(ErrorInvalidAddress);
  if (auto E = decommit(Address, Size))
    return std::move(E);
  if (Type == MemRelease)
    Ranges.erase(I);
  return MemoryResult{1};
}
llvm::Expected<MemoryResult>
VirtualMemory::protect(uint64_t Address, uint64_t Size, uint32_t Protection) {
  if (!roundRange(Address, Size, PageSize))
    return rejected(ErrorInvalidParameter);
  auto I = containing(Address, Size);
  if (I == Ranges.end())
    return rejected(ErrorInvalidAddress);
  if (I->second.Kind == Owner::Runtime)
    return unmodeled();
  auto Rights = permissions(Protection);
  if (!Rights) {
    if (I->second.Kind == Owner::Image &&
        (Protection == PageWriteCopy || Protection == PageExecuteWriteCopy))
      return unmodeled();
    return protectionFailure(Protection);
  }
  auto Mapped = Space.canAccess(Address, Size, UserAccessible);
  if (!Mapped)
    return Mapped.takeError();
  if (!*Mapped)
    return MemoryResult{PageNoAccess, ErrorInvalidAddress};
  auto Old = query(Address);
  if (!Old)
    return Old.takeError();
  if (!*Old)
    return failure(text::MemoryState);
  if (auto E = Space.protect(Address, Size, *Rights))
    return std::move(E);
  return MemoryResult{(**Old).Protection};
}
llvm::Expected<std::optional<MemoryInformation>>
VirtualMemory::query(uint64_t Address) {
  if (Address >= UserLimit)
    return std::nullopt;
  Address &= ~(PageSize - 1);
  auto I = containing(Address, 1);
  if (I == Ranges.end()) {
    auto Next = Ranges.upper_bound(Address);
    uint64_t End = Next == Ranges.end() ? UserLimit : Next->first;
    if (Address < ImageAlignment)
      End = ImageAlignment;
    return MemoryInformation{Address,      0, End - Address, 0, MemFree,
                             PageNoAccess, 0};
  }
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  uint64_t End = I->first + I->second.Size;
  uint32_t State = MemReserve, Protection = 0;
  for (const auto &M : *Mappings) {
    if (M.Address + M.Size <= Address)
      continue;
    if (M.Address > Address) {
      End = std::min(End, M.Address);
      break;
    }
    auto P = protection(M.Permissions);
    if (!P || M.Device)
      return failure(text::MemoryState);
    State = MemCommit;
    Protection = *P;
    End = std::min(End, M.Address + M.Size);
    break;
  }
  return MemoryInformation{
      Address, I->first,   End - Address, I->second.Protection,
      State,   Protection, I->second.Type};
}

llvm::Expected<std::optional<uint64_t>>
Services::memory(const Service &S, const NativeCallEvent &Event) {
  const auto &A = Event.Arguments;
  auto WinError =
      [&](uint32_t Code) -> llvm::Expected<std::optional<uint64_t>> {
    auto V = error(Code);
    if (!V)
      return V.takeError();
    return std::optional<uint64_t>(*V);
  };
  if (S.Kind == API::FlushInstructionCache) {
    if (A[0] != CurrentProcess)
      return WinError(ErrorInvalidHandle);
    if (A[1] && A[2]) {
      auto Valid = access(A[1], A[2], Read);
      if (!Valid)
        return Valid.takeError();
      if (!*Valid)
        return unsupported(S);
    }
    // Checked CPU transports invalidate code translations on every entry;
    // the stopped service boundary already guarantees instruction coherence.
    return std::optional<uint64_t>(1);
  }
  if (S.Kind == API::VirtualQuery) {
    if (A[2] < MemoryInformationSize)
      return WinError(ErrorBadLength);
    auto Info = Virtual.query(A[0]);
    if (!Info)
      return Info.takeError();
    if (!*Info)
      return WinError(ErrorInvalidParameter);
    auto Writable = access(A[1], MemoryInformationSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return WinError(ErrorNoAccess);
    const auto &V = **Info;
    // Capture before writing: the query result may reside in its own region.
    std::array<uint8_t, MemoryInformationSize> Bytes{};
    using namespace llvm::support::endian;
    write64le(Bytes.data() + MemoryInfoBase, V.Base);
    write64le(Bytes.data() + MemoryInfoAllocationBase, V.AllocationBase);
    write32le(Bytes.data() + MemoryInfoAllocationProtection,
              V.AllocationProtection);
    write64le(Bytes.data() + MemoryInfoRegionSize, V.Size);
    write32le(Bytes.data() + MemoryInfoState, V.State);
    write32le(Bytes.data() + MemoryInfoProtection, V.Protection);
    write32le(Bytes.data() + MemoryInfoType, V.Type);
    if (auto E = CPU.write(A[1], Bytes))
      return std::move(E);
    return std::optional<uint64_t>(MemoryInformationSize);
  }
  if (S.Kind == API::VirtualProtect) {
    auto Writable = access(A[3], DWordSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return WinError(ErrorNoAccess);
  }
  auto V = [&]() -> llvm::Expected<MemoryResult> {
    switch (S.Kind) {
    case API::VirtualAlloc:
      return Virtual.allocate(A[0], A[1], uint32_t(A[2]), uint32_t(A[3]));
    case API::VirtualFree:
      return Virtual.free(A[0], A[1], uint32_t(A[2]));
    case API::VirtualProtect:
      return Virtual.protect(A[0], A[1], uint32_t(A[2]));
    default:
      return failure(text::MemoryState);
    }
  }();
  if (!V)
    return V.takeError();
  if (V->Unsupported)
    return unsupported(S);
  if (S.Kind == API::VirtualProtect && V->Value) {
    auto Writable = access(A[3], DWordSize, Write);
    if (!Writable)
      return Writable.takeError();
    // Native Windows retains the successful protection change when it makes
    // the previously writable output inaccessible. The output stays unchanged.
    if (*Writable)
      if (auto E = CPU.writeInteger(A[3], V->Value, DWordSize))
        return std::move(E);
  }
  if (V->Error)
    return WinError(V->Error);
  if (S.Kind == API::VirtualProtect)
    return std::optional<uint64_t>(1);
  return std::optional<uint64_t>(V->Value);
}
} // namespace neverd::emulation::windows_process
