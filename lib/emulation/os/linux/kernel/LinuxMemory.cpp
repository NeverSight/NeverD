//===- LinuxMemory.cpp - Linux anonymous memory services -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LinuxMemory.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::emulation::linux_model {
namespace {
uint64_t errorValue(uint64_t Number) { return uint64_t(0) - Number; }
bool freeRange(llvm::ArrayRef<AddressMapping> Ranges, uint64_t Address,
               uint64_t Size) {
  for (const auto &Range : Ranges)
    if (Address <= Range.Address ? Range.Address - Address < Size
                                 : Address - Range.Address < Range.Size)
      return false;
  return true;
}
bool supportedProtection(uint64_t Protection) {
  // Write-only and execute-only differ across hardware/personality/pkey
  // policies. Admit only the explicit common readable combinations or NONE.
  return !(Protection & ~(ProtRead | ProtWrite | ProtExecute)) &&
         (!Protection || (Protection & ProtRead));
}
} // namespace

unsigned LinuxMemory::permissions(uint64_t Protection) {
  return UserAccessible | (Protection & ProtRead ? Read : 0u) |
         (Protection & ProtWrite ? Write : 0u) |
         (Protection & ProtExecute ? Execute : 0u);
}

LinuxMemory::LinuxMemory(AddressSpace &Space, const MemoryLayout &Layout,
                         uint64_t InitialBreak, const ProcessOptions &Options)
    : Space(Space), Options(Options), PageSize(Layout.PageSize),
      UserLimit(Layout.UserLimit), Limit(Options.MemoryLimit),
      MinimumBreak(InitialBreak),
      GuardBase(StackTop - Options.StackSize - PageSize),
      GuardEnd(StackTop + PageSize), ProgramBreak(InitialBreak) {}

std::optional<uint64_t> LinuxMemory::roundSize(uint64_t Size) const {
  if (Size > UINT64_MAX - (PageSize - 1))
    return std::nullopt;
  return (Size + PageSize - 1) & ~(PageSize - 1);
}
bool LinuxMemory::validRange(uint64_t Address, uint64_t Size) const {
  return Address < UserLimit && Size <= UserLimit - Address;
}
llvm::Expected<std::vector<AddressMapping>>
LinuxMemory::reservedRanges() const {
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  // Stack guard policy is an OS reservation, not a second page table.
  Ranges->push_back({GuardBase, GuardEnd - GuardBase, 0, false});
  std::sort(Ranges->begin(), Ranges->end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  return Ranges;
}
std::optional<uint64_t>
LinuxMemory::findGap(llvm::ArrayRef<AddressMapping> Ranges, uint64_t Start,
                     uint64_t Size) const {
  uint64_t Candidate = std::max(Start, MinimumAddress);
  for (const auto &Range : Ranges) {
    if (!validRange(Candidate, Size))
      return std::nullopt;
    if (Range.Address > Candidate && Size <= Range.Address - Candidate)
      return Candidate;
    if (Range.Address <= Candidate && Candidate - Range.Address >= Range.Size)
      continue;
    if (!validRange(Range.Address, Range.Size))
      return std::nullopt;
    Candidate = Range.Address + Range.Size;
  }
  return validRange(Candidate, Size) ? std::optional<uint64_t>(Candidate)
                                     : std::nullopt;
}

void LinuxMemory::setAllocationOwnership(uint64_t Address, uint64_t Size,
                                         bool Owned) {
  const uint64_t End = Address + Size;
  std::vector<std::pair<uint64_t, uint64_t>> Updated;
  for (const auto &[Begin, Limit] : AnonymousAllocationRanges) {
    if (Limit <= Address || Begin >= End) {
      Updated.emplace_back(Begin, Limit);
      continue;
    }
    if (Begin < Address)
      Updated.emplace_back(Begin, Address);
    if (Limit > End)
      Updated.emplace_back(End, Limit);
  }
  if (Owned)
    Updated.emplace_back(Address, End);
  std::sort(Updated.begin(), Updated.end());
  AnonymousAllocationRanges.clear();
  for (const auto &Range : Updated) {
    if (!AnonymousAllocationRanges.empty() &&
        AnonymousAllocationRanges.back().second >= Range.first)
      AnonymousAllocationRanges.back().second =
          std::max(AnonymousAllocationRanges.back().second, Range.second);
    else
      AnonymousAllocationRanges.push_back(Range);
  }
}

llvm::Expected<bool> LinuxMemory::mapPages(uint64_t Address, uint64_t Size,
                                           unsigned Permissions) {
  auto RAM = Space.physicalMemory();
  if (Space.mappedBytes() > Limit || Size > Limit - Space.mappedBytes() ||
      RAM->allocatedBytes() > Limit || Size > Limit - RAM->allocatedBytes())
    return false;
  // Each anonymous page has an independent allocation lifetime. A partial
  // munmap must not retain the entire original mmap allocation. Allocate all
  // owners before publishing mappings; predictable shortage has no effects.
  std::vector<std::shared_ptr<MemoryRegion>> Pages;
  Pages.reserve(Size / PageSize);
  for (uint64_t Offset = 0; Offset < Size; Offset += PageSize) {
    auto Region = RAM->allocate(PageSize);
    if (!Region) {
      auto E = Region.takeError();
      if (!E.isA<GuestMemoryLimitError>())
        return std::move(E);
      llvm::consumeError(std::move(E));
      return false;
    }
    Pages.push_back(std::move(*Region));
  }
  uint64_t Mapped = 0;
  for (auto &Page : Pages) {
    if (auto E =
            Space.mapRegion(Address + Mapped, Page, 0, PageSize, Permissions)) {
      if (Mapped)
        E = llvm::joinErrors(std::move(E), Space.unmap(Address, Mapped));
      return std::move(E);
    }
    Mapped += PageSize;
  }
  setAllocationOwnership(Address, Size, true);
  return true;
}
llvm::Expected<bool> LinuxMemory::unmapPages(uint64_t Address, uint64_t Size) {
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  bool Removed = false;
  for (const auto &Range : *Ranges) {
    const uint64_t Begin = std::max(Address, Range.Address);
    const uint64_t End = std::min(Address + Size, Range.Address + Range.Size);
    if (Begin >= End)
      continue;
    if (Range.Device)
      return failure(MemoryState);
    if (auto E = Space.unmap(Begin, End - Begin))
      return std::move(E);
    setAllocationOwnership(Begin, End - Begin, false);
    setMergeable(Begin, End - Begin, false);
    retireDeviceRanges(Begin, End - Begin);
    retireProcMappings(Begin, End - Begin);
    retireAnonymousVMANames(Begin, End - Begin);
    Removed = true;
  }
  return Removed;
}

bool LinuxMemory::isMergeable(uint64_t Address) const {
  return std::any_of(MergeableRanges.begin(), MergeableRanges.end(),
                     [Address](const auto &Range) {
                       return Address >= Range.first && Address < Range.second;
                     });
}
void LinuxMemory::setMergeable(uint64_t Address, uint64_t Size,
                               bool Mergeable) {
  const uint64_t End = Address + Size;
  std::vector<std::pair<uint64_t, uint64_t>> Updated;
  for (const auto &[Begin, Limit] : MergeableRanges) {
    if (Limit <= Address || Begin >= End) {
      Updated.emplace_back(Begin, Limit);
      continue;
    }
    if (Begin < Address)
      Updated.emplace_back(Begin, Address);
    if (Limit > End)
      Updated.emplace_back(End, Limit);
  }
  if (Mergeable)
    Updated.emplace_back(Address, End);
  std::sort(Updated.begin(), Updated.end());
  MergeableRanges.clear();
  for (const auto &Range : Updated) {
    if (!MergeableRanges.empty() &&
        MergeableRanges.back().second >= Range.first)
      MergeableRanges.back().second =
          std::max(MergeableRanges.back().second, Range.second);
    else
      MergeableRanges.push_back(Range);
  }
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::discard(const ProcessServiceEvent &Event, ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  auto Unsupported = [&](const char *Text) -> Reply {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Text;
    return std::nullopt;
  };
  const uint64_t Address = Event.Arguments[0];
  auto Size = roundSize(Event.Arguments[1]);
  if (Address % PageSize || !Size || Address > UINT64_MAX - *Size)
    return Reply(errorValue(InvalidArgument));
  if (!*Size)
    return Reply(0);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  const uint64_t End = Address + *Size;
  uint64_t Covered = 0;
  auto Owner = AnonymousAllocationRanges.begin();
  for (const auto &R : *Ranges) {
    const uint64_t Begin = std::max(Address, R.Address);
    const uint64_t Limit = std::min(End, R.Address + R.Size);
    for (uint64_t At = Begin; At < Limit; At += PageSize) {
      while (Owner != AnonymousAllocationRanges.end() && Owner->second <= At)
        ++Owner;
      if (R.Device || Owner == AnonymousAllocationRanges.end() ||
          Owner->first > At || PageSize > Owner->second - At)
        return Unsupported("Linux madvise DONTNEED requires owned private "
                           "anonymous mmap or brk backing");
      // Dropping a private PTE must not overwrite retained physical aliases or
      // pinned views. Allocate a fresh zero page before removing this mapping.
      // The OS owns mutations for the whole stopped operation; unexpected map
      // failures are terminal. AddressSpace owns permissions and generations.
      auto Page = Space.physicalMemory()->allocate(PageSize);
      if (!Page) {
        auto E = Page.takeError();
        if (!E.isA<GuestMemoryLimitError>())
          return std::move(E);
        llvm::consumeError(std::move(E));
        return Unsupported("Linux madvise DONTNEED replacement backing "
                           "exceeds model memory limit");
      }
      if (auto E = Space.unmap(At, PageSize))
        return std::move(E);
      if (auto E = Space.mapRegion(At, *Page, 0, PageSize, R.Permissions))
        return std::move(E);
      Covered += PageSize;
    }
  }
  // Kernel traversal still discards mapped pages on both sides of a hole.
  // VMA names, protections, break position and KSM eligibility stay intact.
  return Reply(Covered == *Size ? 0 : errorValue(NoMemory));
}

llvm::Expected<uint64_t> LinuxMemory::advise(const ProcessServiceEvent &Event) {
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  auto Size = roundSize(Length);
  if (Address % PageSize || !Size || Address > UINT64_MAX - *Size)
    return errorValue(InvalidArgument);
  if (!*Size)
    return 0;
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  const uint64_t End = Address + *Size;
  uint64_t Covered = 0;
  for (const auto &Range : *Ranges) {
    const uint64_t Begin = std::max(Address, Range.Address);
    const uint64_t Limit = std::min(End, Range.Address + Range.Size);
    if (Begin >= Limit)
      continue;
    if (Range.Device)
      return failure(MemoryState);
    setMergeable(Begin, Limit - Begin,
                 uint32_t(Event.Arguments[2]) == AdviceMergeable);
    Covered += Limit - Begin;
  }
  // Linux visits mapped ranges on both sides of a hole and still reports
  // ENOMEM. Eligibility never changes guest bytes, permissions or ownership;
  // physical deduplication needs a scanner, which this profile does not run.
  return Covered == *Size ? 0 : errorValue(NoMemory);
}

llvm::Expected<uint64_t> LinuxMemory::map(const ProcessServiceEvent &Event) {
  const auto [Hint, Length, Protection, Flags, FD, Offset] = Event.Arguments;
  if (!Length || Offset % PageSize)
    return errorValue(InvalidArgument);
  auto Size = roundSize(Length);
  if (!Size || *Size > Limit)
    return errorValue(NoMemory);
  auto Address = placement(Hint, *Size);
  if (!Address)
    return Address.takeError();
  if (!*Address)
    return errorValue(NoMemory);
  auto Mapped = mapPages(**Address, *Size, permissions(Protection));
  if (!Mapped)
    return Mapped.takeError();
  if (*Mapped)
    setMergeable(**Address, *Size, false);
  return *Mapped ? **Address : errorValue(NoMemory);
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::placement(uint64_t Hint, uint64_t Size) const {
  auto Ranges = reservedRanges();
  if (!Ranges)
    return Ranges.takeError();
  uint64_t Address = Hint & ~(PageSize - 1);
  if (Address < MinimumAddress || !validRange(Address, Size) ||
      !freeRange(*Ranges, Address, Size)) {
    auto Gap = findGap(*Ranges, MmapBase, Size);
    if (!Gap)
      Gap = findGap(*Ranges, MinimumAddress, Size);
    return Gap;
  }
  return std::optional<uint64_t>(Address);
}

llvm::Expected<uint64_t>
LinuxMemory::protect(const ProcessServiceEvent &Event) {
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  if (Address % PageSize)
    return errorValue(InvalidArgument);
  if (!Length)
    return 0;
  auto Size = roundSize(Length);
  if (!Size || !validRange(Address, *Size))
    return errorValue(NoMemory);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  uint64_t Cursor = Address;
  const uint64_t End = Address + *Size;
  for (const auto &Range : *Ranges) {
    if (Range.Address > Cursor)
      break;
    if (Cursor - Range.Address >= Range.Size)
      continue;
    if (Range.Device)
      return failure(MemoryState);
    uint64_t Count =
        std::min(End - Cursor, Range.Size - (Cursor - Range.Address));
    // Mapping snapshots can coalesce adjacent VMAs. Split at the independent
    // maximum-protection boundary to preserve any successfully changed prefix.
    const auto Restricted = firstDeviceByte(Cursor, Cursor + Count);
    const bool Denied = Restricted && (Event.Arguments[2] & ~ProtRead);
    if (Denied && *Restricted == Cursor)
      return errorValue(AccessDenied);
    if (Denied)
      Count = *Restricted - Cursor;
    if (auto E = Space.protect(Cursor, Count, permissions(Event.Arguments[2])))
      return std::move(E);
    Cursor += Count;
    if (Denied)
      return errorValue(AccessDenied);
    if (Cursor == End)
      return 0;
  }
  // Linux applies each preceding VMA change before encountering a gap.
  // Do not roll back that visible prefix when returning ENOMEM.
  return errorValue(NoMemory);
}
llvm::Expected<uint64_t> LinuxMemory::unmap(const ProcessServiceEvent &Event) {
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  auto Size = roundSize(Length);
  if (Address % PageSize || !Length || !Size || !validRange(Address, *Size))
    return errorValue(InvalidArgument);
  auto Removed = unmapPages(Address, *Size);
  if (!Removed)
    return Removed.takeError();
  retireGPUObjects(Address, *Size);
  return 0;
}
llvm::Expected<uint64_t> LinuxMemory::setBreak(uint64_t Address) {
  if (Address < MinimumBreak || Address >= UserLimit)
    return ProgramBreak;
  const uint64_t Before = *roundSize(ProgramBreak), After = *roundSize(Address);
  if (Before == After) {
    ProgramBreak = Address;
    return ProgramBreak;
  }
  if (After < Before) {
    auto Removed = unmapPages(After, Before - After);
    if (!Removed)
      return Removed.takeError();
    if (!*Removed)
      return ProgramBreak;
  } else {
    auto Ranges = reservedRanges();
    if (!Ranges)
      return Ranges.takeError();
    // Linux requires a page of separation from the next unrelated mapping.
    if (!validRange(Before, After - Before + PageSize) ||
        !freeRange(*Ranges, Before, After - Before + PageSize))
      return ProgramBreak;
    auto Mapped =
        mapPages(Before, After - Before, Read | Write | UserAccessible);
    if (!Mapped)
      return Mapped.takeError();
    if (!*Mapped)
      return ProgramBreak;
    setMergeable(Before, After - Before, false);
  }
  ProgramBreak = Address;
  return ProgramBreak;
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::handle(ServiceKind Kind, const ProcessServiceEvent &Event,
                    ProcessResult &Result) {
  auto UnsupportedProtection = [&]() -> std::optional<uint64_t> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = llvm::formatv(MemoryOperation, Event.Number,
                                      Event.Arguments[2], Event.Arguments[3])
                            .str();
    return std::optional<uint64_t>();
  };
  auto Reply = [](llvm::Expected<uint64_t> Value)
      -> llvm::Expected<std::optional<uint64_t>> {
    if (!Value)
      return Value.takeError();
    return std::optional<uint64_t>(*Value);
  };
  if (Kind == ServiceKind::Mprotect || Kind == ServiceKind::Munmap ||
      Kind == ServiceKind::Madvise) {
    const auto Size = roundSize(Event.Arguments[1]);
    if (Size && Event.Arguments[0] % PageSize == 0 &&
        validRange(Event.Arguments[0], *Size) &&
        overlapsGPUObject(Event.Arguments[0], *Size) &&
        (Kind == ServiceKind::Madvise ||
         (Kind == ServiceKind::Mprotect &&
          (Event.Arguments[2] & ProtExecute)) ||
         overlapsGPUObject(Event.Arguments[0], *Size, true))) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic =
          "GPU object VMA splitting, execution or advice is unmodeled";
      return std::optional<uint64_t>();
    }
  }
  switch (Kind) {
  case ServiceKind::Prctl:
    return prctl(Event, Result);
  case ServiceKind::Mmap:
    if (!supportedProtection(Event.Arguments[2]) ||
        Event.Arguments[3] != (MapPrivate | MapAnonymous))
      return UnsupportedProtection();
    return Reply(map(Event));
  case ServiceKind::Mprotect:
    if (!supportedProtection(Event.Arguments[2]))
      return UnsupportedProtection();
    return Reply(protect(Event));
  case ServiceKind::Munmap:
    if (auto Size = roundSize(Event.Arguments[1]);
        Size && Event.Arguments[0] % PageSize == 0 &&
        validRange(Event.Arguments[0], *Size) &&
        !canRetireDevices(Event.Arguments[0], *Size)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = "Device last mapping cannot retire pending work";
      return std::optional<uint64_t>();
    }
    return Reply(unmap(Event));
  case ServiceKind::Madvise:
    if (auto Size = roundSize(Event.Arguments[1]);
        Size && validRange(Event.Arguments[0], *Size) &&
        firstDeviceByte(Event.Arguments[0], Event.Arguments[0] + *Size)) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic = DeviceMappingAdvice;
      return std::optional<uint64_t>();
    }
    if (uint32_t(Event.Arguments[2]) == AdviceDontNeed)
      return discard(Event, Result);
    if (uint32_t(Event.Arguments[2]) != AdviceMergeable &&
        uint32_t(Event.Arguments[2]) != AdviceUnmergeable) {
      Result.Stop = ProcessStopReason::UnsupportedService;
      Result.Diagnostic =
          llvm::formatv(MemoryAdvice, int32_t(Event.Arguments[2])).str();
      return std::optional<uint64_t>();
    }
    return Reply(advise(Event));
  case ServiceKind::Mincore:
    return residency(Event, Result);
  case ServiceKind::Brk:
    return Reply(setBreak(Event.Arguments[0]));
  default:
    return failure(MemoryState);
  }
}
} // namespace neverd::emulation::linux_model
