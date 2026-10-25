//===- LinuxVMAName.cpp - Borrowed anonymous VMA names -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Explicit legacy Android PR_SET_VMA contract; no host prctl or proc access.
/// Names are guest pointers, read only when proc generates a maps record.
///
//===----------------------------------------------------------------------===//

#include "LinuxMemory.h"

#include "neverd/emulation/Limits.h"

#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::emulation::linux_model {
std::vector<LinuxMemory::AnonymousVMAName>
LinuxMemory::keptAnonymousVMANames(uint64_t Address, uint64_t Size) const {
  const auto End = Address + Size;
  std::vector<AnonymousVMAName> Kept;
  for (auto N : AnonymousVMANames) {
    if (N.End <= Address || N.Begin >= End) {
      Kept.push_back(N);
      continue;
    }
    if (N.Begin < Address)
      Kept.push_back({N.Begin, Address, N.Name});
    if (N.End > End)
      Kept.push_back({End, N.End, N.Name});
  }
  return Kept;
}

void LinuxMemory::retireAnonymousVMANames(uint64_t Address, uint64_t Size) {
  AnonymousVMANames = keptAnonymousVMANames(Address, Size);
}

llvm::Expected<std::optional<uint64_t>>
LinuxMemory::prctl(const ProcessServiceEvent &Event, ProcessResult &Result) {
  auto Unsupported = [&](llvm::StringRef Reason) -> std::optional<uint64_t> {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Reason.str();
    return std::nullopt;
  };
  // The outer option is an int; all remaining LP64 arguments retain 64 bits.
  const auto Option = uint32_t(Event.Arguments[0]);
  // Negative options are outside this Linux command namespace. Reject them
  // before consulting optional providers or interpreting any pointer argument.
  if (Option > INT32_MAX)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  if (Option != PRSetVMA)
    return Unsupported("prctl operation is unmodeled");
  if (!Options.LinuxKernel || Options.LinuxKernel->AnonymousVMANames !=
                                  LinuxAnonymousVMANames::AndroidUserPointer)
    return Unsupported("PR_SET_VMA requires linux_kernel.anonymous_vma_names "
                       "= android-user-pointer");
  const auto Address = Event.Arguments[2];
  const auto Size = roundSize(Event.Arguments[3]);
  if (Address % PageSize || !Size || *Size > UINT64_MAX - Address)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  // The legacy kernel accepts a zero-length range before dispatching subop.
  if (!*Size)
    return std::optional<uint64_t>(0);
  if (Event.Arguments[1] != PRSetVMAAnonName)
    return std::optional<uint64_t>(uint64_t(0) - InvalidArgument);
  const auto End = Address + *Size;
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  // Only anonymous RAM is admitted. Backing identities and device splitting
  // require contracts beyond this profile; reject those before any effects.
  for (const auto &M : ProcMappings)
    if (M.Address < End && Address < M.Address + M.Size)
      return Unsupported("PR_SET_VMA on a declared backing VMA is unmodeled");
  if (firstDeviceByte(Address, End) || overlapsGPUObject(Address, *Size))
    return Unsupported("PR_SET_VMA on a device VMA is unmodeled");
  for (const auto &M : *Ranges)
    if (M.Device && M.Address < End && Address < M.Address + M.Size)
      return Unsupported("PR_SET_VMA on an MMIO VMA is unmodeled");

  auto Updated = keptAnonymousVMANames(Address, *Size);
  uint64_t Cursor = Address;
  bool Hole = false;
  for (const auto &M : *Ranges) {
    const auto Begin = std::max(Address, M.Address);
    const auto Limit = std::min(End, M.Address + M.Size);
    if (Begin >= Limit)
      continue;
    Hole |= Begin > Cursor;
    Cursor = Limit;
    if (Event.Arguments[4])
      Updated.push_back({Begin, Limit, Event.Arguments[4]});
  }
  Hole |= Cursor < End;
  std::sort(Updated.begin(), Updated.end(),
            [](const auto &A, const auto &B) { return A.Begin < B.Begin; });
  std::vector<AnonymousVMAName> Merged;
  for (const auto &N : Updated) {
    if (!Merged.empty() && Merged.back().End == N.Begin &&
        Merged.back().Name == N.Name)
      Merged.back().End = N.End;
    else
      Merged.push_back(N);
  }
  if (Merged.size() > ::neverd::limits::emulation::
                          kLinuxProcMapsLinuxMemoryProcMappingsOutSizeLimit)
    return Unsupported("anonymous VMA name metadata budget exhausted");
  AnonymousVMANames = std::move(Merged);
  // Holes do not stop the legacy walk: mapped intervals on both sides change.
  return std::optional<uint64_t>(Hole ? uint64_t(0) - NoMemory : 0);
}

llvm::Expected<std::string>
LinuxMemory::procAnonymousName(uint64_t Address) const {
  std::string Name = "[anon:";
  uint64_t Remaining = FileNameLimit; // Legacy NAME_MAX, not modern Linux 80.
  while (Remaining) {
    const auto Count = std::min(Remaining, PageSize - Address % PageSize);
    auto Readable = Space.canAccess(Address, Count, Read | UserAccessible);
    if (!Readable)
      return Readable.takeError();
    if (!*Readable) {
      Name += "<fault>";
      break;
    }
    std::vector<uint8_t> Bytes(Count);
    if (auto E = Space.read(Address, Bytes))
      return std::move(E);
    auto Null = std::find(Bytes.begin(), Bytes.end(), 0);
    Name.append(Bytes.begin(), Null);
    if (Null != Bytes.end())
      break;
    Remaining -= Count;
    if (Remaining && Count > UINT64_MAX - Address) {
      Name += "<fault>";
      break;
    }
    Address += Count;
  }
  Name += ']';
  return Name;
}
} // namespace neverd::emulation::linux_model
