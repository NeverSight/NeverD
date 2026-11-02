//===- LinuxProcMaps.cpp - Live Linux mapping observations ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Backing labels carry no mapping authority. Every emitted extent and access
/// bit comes from the stopped address space; unmapping retires labels.
#include "LinuxMemory.h"

#include "neverd/emulation/Limits.h"

#include <algorithm>

namespace neverd::emulation::linux_model {
llvm::Error LinuxMemory::initializeProc() {
  if (!Options.LinuxFiles || !Options.LinuxFiles->DynamicProc)
    return llvm::Error::success();
  auto Labels = Options.LinuxFiles->DynamicProc->Mappings;
  for (const auto &M : Labels) {
    if (!validRange(M.Address, M.Size) || M.Address % PageSize ||
        M.Size % PageSize || M.Offset % PageSize)
      return failure(
          "proc backing identity requires a page-aligned guest range");
    auto Mapped = Space.canAccess(M.Address, M.Size, 0);
    if (!Mapped)
      return Mapped.takeError();
    if (!*Mapped)
      return failure("proc backing identity does not cover initial mapped RAM");
  }
  ProcMappings = std::move(Labels);
  return llvm::Error::success();
}

void LinuxMemory::retireProcMappings(uint64_t Address, uint64_t Size) {
  std::vector<LinuxProcMapping> Kept;
  for (auto M : ProcMappings) {
    const auto End = M.Address + M.Size;
    if (End <= Address || M.Address >= Address + Size) {
      Kept.push_back(std::move(M));
      continue;
    }
    if (M.Address < Address) {
      auto Left = M;
      Left.Size = Address - M.Address;
      Kept.push_back(std::move(Left));
    }
    if (End > Address + Size) {
      M.Offset += Address + Size - M.Address;
      M.Address = Address + Size;
      M.Size = End - M.Address;
      Kept.push_back(std::move(M));
    }
  }
  ProcMappings = std::move(Kept);
}

llvm::Expected<std::vector<LinuxMemory::ProcMapping>>
LinuxMemory::procMappings(bool IncludeMMIOBoundaries) const {
  auto Ranges = Space.mappings();
  if (!Ranges)
    return Ranges.takeError();
  std::vector<ProcMapping> Out;
  for (const auto &Range : *Ranges) {
    // This is the OS-owned user address space. PROT_NONE thread guards may
    // clear every access bit, but remain real VMAs and count towards VmSize.
    if (Range.Device && !IncludeMMIOBoundaries)
      return failure("proc maps cannot describe an unmodeled MMIO VMA");
    const auto End = Range.Address + Range.Size;
    std::vector<uint64_t> Cuts{Range.Address, End};
    auto Cut = [&](uint64_t At) {
      if (At > Range.Address && At < End)
        Cuts.push_back(At);
    };
    for (const auto &M : ProcMappings) {
      Cut(M.Address);
      Cut(M.Address + M.Size);
    }
    auto Name = std::upper_bound(
        AnonymousVMANames.begin(), AnonymousVMANames.end(), Range.Address,
        [](uint64_t At, const auto &N) { return At < N.End; });
    for (auto I = Name; I != AnonymousVMANames.end() && I->Begin < End; ++I) {
      Cut(I->Begin);
      Cut(I->End);
    }
    auto Owner = std::upper_bound(
        AnonymousAllocationRanges.begin(), AnonymousAllocationRanges.end(),
        Range.Address,
        [](uint64_t At, const auto &R) { return At < R.second; });
    for (; Owner != AnonymousAllocationRanges.end() && Owner->first < End;
         ++Owner) {
      Cut(Owner->first);
      Cut(Owner->second);
    }
    for (const auto &A : ReadOnlyDeviceRanges) {
      Cut(A.Begin);
      Cut(A.End);
    }
    for (const auto &A : GPUObjectRanges) {
      Cut(A.Begin);
      Cut(A.Begin + A.Size);
    }
    Cut(MinimumBreak);
    Cut(*roundSize(ProgramBreak));
    Cut(GuardBase + PageSize);
    Cut(GuardEnd - PageSize);
    std::sort(Cuts.begin(), Cuts.end());
    Cuts.erase(std::unique(Cuts.begin(), Cuts.end()), Cuts.end());
    for (size_t I = 1; I < Cuts.size(); ++I) {
      ProcMapping M;
      M.Address = Cuts[I - 1];
      M.Size = Cuts[I] - M.Address;
      M.Permissions = Range.Permissions;
      M.IsMMIO = Range.Device;
      while (Name != AnonymousVMANames.end() && Name->End <= M.Address)
        ++Name;
      if (Name != AnonymousVMANames.end() && Name->Begin <= M.Address)
        M.AnonymousName = Name->Name;
      bool Labeled = false;
      for (const auto &Label : ProcMappings) {
        if (M.Address < Label.Address ||
            M.Address - Label.Address >= Label.Size)
          continue;
        const auto Start = M.Address, Size = M.Size;
        static_cast<LinuxProcMapping &>(M) = Label;
        M.Offset += Start - Label.Address;
        M.Address = Start;
        M.Size = Size;
        Labeled = true;
        break;
      }
      for (const auto &A : ReadOnlyDeviceRanges) {
        if (M.Address < A.Begin || M.Address >= A.End)
          continue;
        M.Shared = true;
        M.Offset = A.Mapping->Offset + A.Offset + M.Address - A.Begin;
        if (A.Owner && Options.LinuxFiles)
          for (const auto &[Path, Device] : Options.LinuxFiles->Devices)
            if (&Device == A.Owner->Options)
              M.Path = Path;
        Labeled = true;
        break;
      }
      for (const auto &A : GPUObjectRanges) {
        if (M.Address < A.Begin || M.Address - A.Begin >= A.Size)
          continue;
        M.Shared = true;
        M.Offset = A.Offset + M.Address - A.Begin;
        if (Options.LinuxFiles)
          for (const auto &[Path, Device] : Options.LinuxFiles->Devices)
            if (&Device == A.Owner->Options)
              M.Path = Path;
        Labeled = true;
        break;
      }
      if (!Labeled) {
        if (M.Address >= MinimumBreak && M.Address < *roundSize(ProgramBreak))
          M.Path = "[heap]";
        else if (M.Address >= GuardBase + PageSize &&
                 M.Address < GuardEnd - PageSize)
          M.Path = "[stack]";
      }
      if (Out.size() == ::neverd::limits::emulation::
                            kLinuxProcMapsLinuxMemoryProcMappingsOutSizeLimit)
        return failure("live proc mapping observation budget exhausted");
      Out.push_back(std::move(M));
    }
  }
  return Out;
}
} // namespace neverd::emulation::linux_model
