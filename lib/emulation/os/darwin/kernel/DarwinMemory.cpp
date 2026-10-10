//===- DarwinMemory.cpp - Darwin virtual memory --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinMemory.h"

#include "DarwinFiles.h"

#include <algorithm>

namespace neverd::emulation::darwin_model {
using namespace value;
namespace {
ServiceResult error(uint64_t Code) { return {Code, true}; }
std::optional<uint64_t> rounded(uint64_t Size, uint64_t Page) {
  if (Size > UINT64_MAX - Page + 1)
    return std::nullopt;
  return (Size + Page - 1) & ~(Page - 1);
}
bool overlaps(uint64_t A, uint64_t Size, uint64_t B, uint64_t Count) {
  return A < B ? B - A < Size : A - B < Count;
}
unsigned normalizedProtection(uint64_t Prot) {
  // XNU's BSD mmap/mprotect entry points imply READ for WRITE or EXECUTE.
  return unsigned(Prot | ((Prot & (ProtWrite | ProtExecute)) ? ProtRead : 0));
}
unsigned guestPermissions(unsigned Prot) {
  return UserAccessible | ((Prot & ProtRead) ? Read : 0u) |
         ((Prot & ProtWrite) ? Write : 0u) |
         ((Prot & ProtExecute) ? Execute : 0u);
}
} // namespace

DarwinMemory::DarwinMemory(AddressSpace &Space, const MemoryLayout &Layout,
                           const ProcessOptions &Options)
    : Space(Space), PageSize(Layout.PageSize), Minimum(Layout.MinimumAddress),
      Limit(Options.MemoryLimit),
      GuardBase(StackTop - Options.StackSize - PageSize),
      Maximum(Layout.Maximum) {
  Maximum.push_back({StackTop - Options.StackSize, Options.StackSize, 3});
  Maximum.push_back({ReturnGate, PageSize, 5});
}

llvm::Expected<std::optional<ServiceResult>>
DarwinMemory::map(const ProcessServiceEvent &E, DarwinFiles &Files,
                  ProcessResult &Result) {
  const auto [Hint, Length, Prot, Flags, FD, Offset] = E.Arguments;
  auto Error = [](uint64_t Code) {
    return std::optional<ServiceResult>(error(Code));
  };
  auto Unsupported = [&](const char *Diagnostic) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Diagnostic;
    return std::optional<ServiceResult>();
  };
  const bool Anonymous = Flags & MapAnonymous;
  // Raw legacy mmap allows unaligned file offsets; UNIX03 rejects them.
  // Check file-end rounding before looking up the descriptor, as XNU does.
  if (((Flags & MapUnix03) && (!Length || Offset % PageSize)) ||
      Length > UINT64_MAX - Offset || !rounded(Offset + Length, PageSize))
    return Error(InvalidArgument);
  if (Offset % PageSize)
    return Unsupported(diagnostic::MemoryFileOffset);
  auto Size = rounded(Length, PageSize);
  if (!Size || *Size > UserLimit)
    return Error(InvalidArgument);
  llvm::ArrayRef<uint8_t> Bytes;
  std::shared_ptr<const unsigned> FileLease;
  if (!Anonymous) {
    auto Source = Files.mappingSource(uint32_t(FD));
    if (auto *Code = std::get_if<uint32_t>(&Source))
      return Error(*Code);
    if (auto *Diagnostic = std::get_if<const char *>(&Source))
      return Unsupported(*Diagnostic);
    const auto &Mapping = std::get<DarwinFiles::Mapping>(Source);
    if (!Mapping.Readable && (normalizedProtection(Prot) & ProtRead))
      return Error(PermissionDenied);
    const auto File = Mapping.Bytes;
    FileLease = Mapping.Lease;
    // A complete page beyond EOF would fault through the vnode pager. Until
    // that fault model exists, refuse before allocating instead of zeroing it.
    if (Length && (Offset >= File.size() ||
                   *Size > *rounded(File.size(), PageSize) - Offset))
      return Unsupported(diagnostic::MemoryFileExtent);
    if (Length)
      Bytes =
          File.slice(Offset, std::min<uint64_t>(*Size, File.size() - Offset));
  }
  // Legacy zero length still validates the file descriptor and its kind.
  if (!Length)
    return std::optional<ServiceResult>({0, false});
  if (*Size > Limit || Space.mappedBytes() > Limit - *Size ||
      Space.physicalMemory()->allocatedBytes() > Limit - *Size)
    return Error(NoMemory);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  Ranges->push_back({GuardBase, StackTop + PageSize - GuardBase, 0, false});
  Ranges->push_back({ReturnGate, PageSize, 0, false});
  std::sort(Ranges->begin(), Ranges->end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  auto Find = [&](uint64_t Start) -> std::optional<uint64_t> {
    uint64_t Candidate = std::max(Minimum, Start);
    for (const auto &R : *Ranges) {
      if (Candidate >= UserLimit || *Size > UserLimit - Candidate)
        return std::nullopt;
      if (R.Address > Candidate && *Size <= R.Address - Candidate)
        break;
      Candidate = std::max(Candidate, R.Address + R.Size);
    }
    return Candidate < UserLimit && *Size <= UserLimit - Candidate
               ? std::optional<uint64_t>(Candidate)
               : std::nullopt;
  };
  // Non-fixed Darwin hints round UP and start the search at that address.
  // An exhausted high hint retries the ordinary deterministic placement.
  auto RoundedHint = rounded(Hint, PageSize);
  auto Found = Find(Hint ? RoundedHint.value_or(UserLimit) : MmapBase);
  if (!Found && Hint)
    Found = Find(MmapBase);
  if (!Found)
    return Error(NoMemory);
  const uint64_t Address = *Found;
  std::vector<std::shared_ptr<MemoryRegion>> Pages;
  for (uint64_t Offset = 0; Offset < *Size; Offset += PageSize) {
    auto Page = Space.physicalMemory()->allocate(PageSize);
    if (!Page) {
      auto AllocationError = Page.takeError();
      if (!AllocationError.isA<GuestMemoryLimitError>())
        return std::move(AllocationError);
      llvm::consumeError(std::move(AllocationError));
      return Error(NoMemory);
    }
    Pages.push_back(std::move(*Page));
  }
  uint64_t Mapped = 0;
  for (auto &Page : Pages) {
    if (auto Error =
            Space.mapRegion(Address + Mapped, Page, 0, PageSize,
                            guestPermissions(normalizedProtection(Prot)))) {
      if (Mapped)
        Error =
            llvm::joinErrors(std::move(Error), Space.unmap(Address, Mapped));
      return std::move(Error);
    }
    Mapped += PageSize;
  }
  // Initialize physical backing while the CPU is stopped, including PROT_NONE
  // and read-only mappings. The catalogue is never shared as writable RAM.
  if (!Bytes.empty())
    if (auto CopyError = Space.writeBacking(Address, Bytes))
      return llvm::joinErrors(std::move(CopyError),
                              Space.unmap(Address, *Size));
  Maximum.push_back({Address, *Size, 7});
  // Private maxprot gains WRITE and EXECUTE before XNU implies READ, even
  // for a write-only descriptor initially mapped with PROT_NONE.
  if (FileLease)
    FileMappings.push_back({Address, *Size, std::move(FileLease)});
  return std::optional<ServiceResult>({Address, false});
}

llvm::Expected<ServiceResult>
DarwinMemory::protect(const ProcessServiceEvent &E) {
  const auto Address = E.Arguments[0], Length = E.Arguments[1];
  if (Address % PageSize)
    return error(InvalidArgument);
  auto Size = rounded(Length, PageSize);
  if (!Size || Address >= UserLimit || *Size > UserLimit - Address)
    return error(NoMemory);
  if (!*Size)
    return ServiceResult{0};
  const unsigned Prot = normalizedProtection(E.Arguments[2]);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  uint64_t Cursor = Address;
  for (const auto &R : *Ranges) {
    if (R.Address > Cursor)
      break;
    if (Cursor >= R.Address + R.Size)
      continue;
    if (R.Device)
      return failure(diagnostic::MemoryDevice);
    Cursor = std::min(Address + *Size, R.Address + R.Size);
    if (Cursor == Address + *Size)
      break;
  }
  if (Cursor != Address + *Size)
    return error(NoMemory);
  // Darwin validates the entire range and maximum rights before any change.
  // Linux's visible-prefix mprotect behavior must not be reused here.
  for (uint64_t P = Address; P < Address + *Size; P += PageSize) {
    const auto Max =
        std::find_if(Maximum.begin(), Maximum.end(), [&](const auto &R) {
          return P >= R.Address && P - R.Address < R.Size;
        });
    if (Max == Maximum.end())
      return failure(diagnostic::MemoryMaximum);
    if (Prot & ~Max->Protection)
      return error(PermissionDenied);
  }
  if (auto Error = Space.protect(Address, *Size, guestPermissions(Prot)))
    return std::move(Error);
  return ServiceResult{0};
}

void DarwinMemory::forgetMaximum(uint64_t Address, uint64_t Size) {
  std::vector<MaximumProtection> Kept;
  for (const auto &R : Maximum) {
    if (!overlaps(Address, Size, R.Address, R.Size)) {
      Kept.push_back(R);
      continue;
    }
    if (R.Address < Address)
      Kept.push_back({R.Address, Address - R.Address, R.Protection});
    if (R.Address + R.Size > Address + Size)
      Kept.push_back(
          {Address + Size, R.Address + R.Size - Address - Size, R.Protection});
  }
  Maximum = std::move(Kept);
}

llvm::Expected<ServiceResult>
DarwinMemory::unmap(const ProcessServiceEvent &E) {
  const auto Address = E.Arguments[0], Length = E.Arguments[1];
  const auto Size = rounded(Length, PageSize);
  if (Address % PageSize || !Length || !Size || Address >= UserLimit ||
      *Size > UserLimit - Address)
    return error(InvalidArgument);
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  for (const auto &R : *Ranges) {
    const uint64_t Begin = std::max(Address, R.Address);
    const uint64_t End = std::min(Address + *Size, R.Address + R.Size);
    if (Begin >= End)
      continue;
    if (auto Error = Space.unmap(Begin, End - Begin))
      return std::move(Error);
    forgetMaximum(Begin, End - Begin);
    forgetFileMappings(Begin, End - Begin);
  }
  return ServiceResult{0};
}

void DarwinMemory::forgetFileMappings(uint64_t Address, uint64_t Size) {
  std::vector<FileMapping> Kept;
  for (const auto &R : FileMappings) {
    if (!overlaps(Address, Size, R.Address, R.Size)) {
      Kept.push_back(R);
      continue;
    }
    if (R.Address < Address)
      Kept.push_back({R.Address, Address - R.Address, R.Lease});
    if (R.Address + R.Size > Address + Size)
      Kept.push_back(
          {Address + Size, R.Address + R.Size - Address - Size, R.Lease});
  }
  FileMappings = std::move(Kept);
}

llvm::Expected<std::optional<ServiceResult>>
DarwinMemory::handle(ServiceKind Kind, const ProcessServiceEvent &E,
                     DarwinFiles &Files, ProcessResult &Result) {
  const bool IsMap = Kind == ServiceKind::Mmap;
  const bool HasProtection = IsMap || Kind == ServiceKind::Mprotect;
  const auto Flags = E.Arguments[3] & ~uint64_t(MapUnix03);
  // A known symbolic vnode rejects mapping before the pager/access checks.
  // Admit only the existing aligned, non-executable argument prefix so that
  // SHARED can reach that same refusal without enabling shared file mappings.
  const bool SymbolicShared =
      IsMap && Flags == MapShared && !(E.Arguments[5] % PageSize) &&
      Files.symbolicLinkDescriptor(uint32_t(E.Arguments[4]));
  if ((HasProtection && (E.Arguments[2] & ~uint64_t(7))) ||
      (IsMap && (Flags != MapPrivate && Flags != (MapPrivate | MapAnonymous) &&
                 !SymbolicShared)) ||
      (IsMap && (Flags & MapAnonymous) &&
       (uint32_t(E.Arguments[4]) != UINT32_MAX || E.Arguments[5])) ||
      (HasProtection && (E.Arguments[2] & ProtExecute)) ||
      (!IsMap && E.Arguments[0] < UserLimit &&
       E.Arguments[1] <= UserLimit - E.Arguments[0] &&
       overlaps(E.Arguments[0], E.Arguments[1], ReturnGate, PageSize))) {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = diagnostic::MemoryMode;
    return std::optional<ServiceResult>();
  }
  if (IsMap)
    return map(E, Files, Result);
  llvm::Expected<ServiceResult> Value =
      Kind == ServiceKind::Mprotect ? protect(E) : unmap(E);
  if (!Value)
    return Value.takeError();
  return std::optional<ServiceResult>(*Value);
}
} // namespace neverd::emulation::darwin_model
