//===- LinuxResidency.cpp - Ordered anonymous residency observations ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Match explicit query observations against live anonymous allocation
/// provenance. AddressSpace alone owns mappings and access permissions.
//===----------------------------------------------------------------------===//

#include "LinuxMemory.h"

#include "llvm/ADT/ArrayRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::emulation::linux_model {
llvm::Expected<std::optional<uint64_t>>
LinuxMemory::residency(const ProcessServiceEvent &Event,
                       ProcessResult &Result) {
  using Reply = std::optional<uint64_t>;
  const uint64_t Address = Event.Arguments[0], Length = Event.Arguments[1];
  const uint64_t Vector = Event.Arguments[2];
  if (Address % PageSize)
    return Reply(uint64_t(0) - InvalidArgument);
  if (Address > UserLimit || Length > UserLimit - Address)
    return Reply(uint64_t(0) - NoMemory);
  const uint64_t Pages = Length / PageSize + (Length % PageSize != 0);
  // access_ok checks the numerical range, not the vector's mapping. A hole
  // at the first queried page is reported before copy_to_user can run.
  if (Vector > UserLimit || Pages > UserLimit - Vector)
    return Reply(uint64_t(0) - BadAddress);
  if (!Pages)
    return Reply(0);
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  auto First =
      std::find_if(Mappings->begin(), Mappings->end(), [&](const auto &M) {
        return Address >= M.Address && Address - M.Address < M.Size;
      });
  if (First == Mappings->end())
    return Reply(uint64_t(0) - NoMemory);
  auto Unsupported = [&](const char *Text) -> Reply {
    Result.Stop = ProcessStopReason::UnsupportedService;
    Result.Diagnostic = Text;
    return std::nullopt;
  };
  if (PageSize != ResidencyPageSize || !Options.LinuxKernel ||
      !Options.LinuxKernel->ResidencyObservations)
    return Unsupported(MemoryResidency);
  const auto &Observations = *Options.LinuxKernel->ResidencyObservations;
  if (NextResidencyObservation == Observations.size())
    return Unsupported(ResidencyExhausted);
  const auto &Observation = Observations[NextResidencyObservation];
  if (Observation.Address != Address || Observation.Pages.size() != Pages)
    return Unsupported(ResidencyMismatch);
  // Share live VMA boundaries with proc, including protection and name splits.
  // MMIO boundaries may be represented here, but never imply known residency.
  auto VMAs = procMappings(/*IncludeMMIOBoundaries=*/true);
  if (!VMAs)
    return VMAs.takeError();
  auto VMA = VMAs->begin();
  auto Owner = AnonymousAllocationRanges.begin();
  auto OutputMapping = Mappings->begin();
  uint64_t Done = 0;
  bool Consumed = false;
  while (Done < Pages) {
    const uint64_t At = Address + Done * PageSize;
    while (VMA != VMAs->end() && VMA->Address + VMA->Size <= At)
      ++VMA;
    if (VMA == VMAs->end() || VMA->Address > At)
      return Reply(uint64_t(0) - NoMemory);
    const uint64_t Count = std::min(
        {Pages - Done, PageSize, (VMA->Address + VMA->Size - At) / PageSize});
    while (Owner != AnonymousAllocationRanges.end() && Owner->second <= At)
      ++Owner;
    if (!Count || VMA->IsMMIO || Owner == AnonymousAllocationRanges.end() ||
        Owner->first > At || Count * PageSize > Owner->second - At)
      return Unsupported(ResidencyBacking);
    if (!Consumed) {
      // Query-time observation precedes output access, including EFAULT.
      ++NextResidencyObservation;
      Consumed = true;
    }
    const uint64_t Output = Vector + Done, End = Output + Count;
    for (uint64_t Byte = Output; Byte < End;) {
      while (OutputMapping != Mappings->end() &&
             OutputMapping->Address + OutputMapping->Size <= Byte)
        ++OutputMapping;
      if (OutputMapping != Mappings->end() && OutputMapping->Address <= Byte &&
          OutputMapping->Device)
        return Unsupported(ResidencyDeviceOutput);
      if (OutputMapping == Mappings->end() || OutputMapping->Address > Byte ||
          (OutputMapping->Permissions & (Write | UserAccessible)) !=
              (Write | UserAccessible)) {
        if (Byte == Output)
          return Reply(uint64_t(0) - BadAddress);
        // Partial copy progress is architecture-specific. Preserve preceding
        // complete chunks without inventing bytes from this incomplete chunk.
        return Unsupported(ResidencyPartialCopy);
      }
      Byte = std::min(End, OutputMapping->Address + OutputMapping->Size);
    }
    if (auto E = Space.write(
            Output,
            llvm::ArrayRef<uint8_t>(Observation.Pages).slice(Done, Count)))
      return std::move(E);
    Done += Count;
  }
  return Reply(0);
}
} // namespace neverd::emulation::linux_model
