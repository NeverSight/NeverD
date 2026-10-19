//===- DriverProcessInventory.cpp - Guest process membership contract -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Guest process membership contract.
///

#include "neverd/emulation/DriverProcessInventory.h"

#include "../WindowsUnicode.h"
#include "DriverProcessImages.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>
#include <set>

namespace neverd::emulation {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "process_inventory: " + Text);
}
} // namespace
std::vector<uint32_t> initialDriverProcessIDs(
    llvm::ArrayRef<DriverProcessEvent> Events,
    llvm::ArrayRef<DriverRequest> Requests,
    const std::optional<DriverProcessInventory> &Inventory) {
  std::set<uint32_t> Created, Live{4};
  for (const auto &E : Events)
    if (E.Type == DriverProcessEvent::Kind::Create)
      Created.insert(E.ProcessID);
  for (const auto &R : Requests)
    if (R.Kind != DriverRequestKind::Pnp &&
        R.Kind != DriverRequestKind::Power && R.RequestorProcessID &&
        !Created.contains(R.RequestorProcessID))
      Live.insert(R.RequestorProcessID);
  if (Inventory)
    for (const auto &P : *Inventory)
      if (!Created.contains(P.ProcessID))
        Live.insert(P.ProcessID);
  return {Live.begin(), Live.end()};
}
llvm::Error validateDriverProcessInventory(const DriverOptions &O) {
  if (!O.ProcessInventory)
    return llvm::Error::success();
  if (!O.Version || O.Version->Major != 10 || O.Version->Minor != 0 ||
      O.Version->Build != 19041)
    return invalid("requires Windows 10.0.19041");
  if (O.ProcessInventory->empty() ||
      O.ProcessInventory->size() > MaxDriverProcessInventory)
    return invalid("requires 1 to 128 process records");
  std::set<uint32_t> IDs;
  uint64_t MemoryBytes = 0, ImageFileBytes = 0;
  size_t MemoryRegions = 0;
  for (const auto &P : *O.ProcessInventory) {
    if ((P.ProcessID < 4 || (P.ProcessID == 4 && P.ParentID)) ||
        P.ProcessID == P.ParentID || !IDs.insert(P.ProcessID).second)
      return invalid("requires distinct supported PIDs, no self parent, and "
                     "System parent zero");
    if (P.ImageName.empty() || P.ImageName.find('\0') != std::string::npos ||
        (P.ProcessID == 4 && P.ImageName != "System"))
      return invalid("requires a nonempty image name; PID 4 is System");
    auto Name = windows_unicode::bytes(P.ImageName, true);
    if (!Name)
      return Name.takeError();
    if (Name->size() > 1026)
      return invalid("image name exceeds 512 UTF-16 units");
    if (P.BasePriority < 0 || P.BasePriority > 31)
      return invalid("base_priority must be between 0 and 31");
    if (P.BorrowedThreadStart &&
        *P.BorrowedThreadStart > profile::CanonicalUserMax &&
        *P.BorrowedThreadStart < profile::CanonicalKernelMin)
      return invalid("borrowed_thread_start must be canonical");
    if (P.BorrowedThreadSystemStart &&
        *P.BorrowedThreadSystemStart > profile::CanonicalUserMax &&
        *P.BorrowedThreadSystemStart < profile::CanonicalKernelMin)
      return invalid("borrowed_thread_system_start must be canonical");
    if (P.SectionBaseAddress &&
        *P.SectionBaseAddress > profile::CanonicalUserMax &&
        *P.SectionBaseAddress < profile::CanonicalKernelMin)
      return invalid("section_base_address must be canonical");
    if (P.PebAddress && (*P.PebAddress > profile::CanonicalUserMax ||
                         (P.ProcessID == 4 && *P.PebAddress)))
      return invalid("peb_address must be a user pointer; System has no PEB");
    std::vector<std::pair<uint64_t, uint64_t>> Ranges;
    if (P.Memory) {
      if (P.ProcessID == 4 && !P.Memory->empty())
        return invalid("System uses the existing global address space");
      for (const auto &R : *P.Memory) {
        if (!R.Size || R.Size > 1024 * 1024 ||
            R.Base < profile::ProcessMemoryBase ||
            R.Base >= profile::ProcessMemoryEnd ||
            R.Size > profile::ProcessMemoryEnd - R.Base ||
            R.Base % profile::PageSize || R.Size % profile::PageSize ||
            R.Bytes.size() > R.Size)
          return invalid("memory requires page-aligned committed regions in "
                         "the process memory window, at most 1 MiB each");
        switch (R.Access) {
#define NEVERD_DRIVER_USER_PAGE_ACCESS(Name, Spelling)                         \
  case DriverUserPageAccess::Name:                                             \
    break;
#include "neverd/emulation/DriverUserPageAccess.def"
#undef NEVERD_DRIVER_USER_PAGE_ACCESS
        default:
          return invalid("unknown process memory access policy");
        }
        Ranges.emplace_back(R.Base, R.Base + R.Size);
        MemoryBytes += R.Size;
        if (++MemoryRegions > MaxDriverProcessMemoryRegions ||
            MemoryBytes > MaxDriverProcessMemoryBytes)
          return invalid(
              "process memory catalogue exceeds region or byte limits");
      }
    }
    if (P.Images.size() > MaxDriverProcessImages ||
        (P.ProcessID == 4 && !P.Images.empty()))
      return invalid("image snapshots require a bounded non-System process");
    for (const auto &Image : P.Images) {
      auto Plan = planDriverProcessImage(Image);
      if (!Plan)
        return Plan.takeError();
      auto Identity = driverProcessImageFileIdentity(Image, O.FiniteFileSystem);
      if (!Identity)
        return Identity.takeError();
      Ranges.emplace_back(Plan->Base, Plan->Base + Plan->Bytes.size());
      MemoryBytes += Plan->Bytes.size();
      ImageFileBytes += Image.FileBytes.size();
      if (MemoryBytes > MaxDriverProcessMemoryBytes ||
          ImageFileBytes > MaxDriverProcessMemoryBytes)
        return invalid("image snapshots exceed process-memory byte limits");
    }
    std::sort(Ranges.begin(), Ranges.end());
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return invalid("process memory regions overlap");
    for (const auto &E : O.ProcessEvents)
      if (E.Type == DriverProcessEvent::Kind::Create &&
          E.ProcessID == P.ProcessID && P.ParentID && P.ParentID != E.ParentID)
        return invalid("declared parent disagrees with process CREATE");
  }
  auto Required =
      initialDriverProcessIDs(O.ProcessEvents, O.Requests, O.ProcessInventory);
  for (const auto &E : O.ProcessEvents)
    if (E.Type == DriverProcessEvent::Kind::Create)
      Required.push_back(E.ProcessID);
  for (auto PID : Required)
    if (!IDs.contains(PID))
      return invalid("missing metadata for a modeled process");
  return llvm::Error::success();
}
} // namespace neverd::emulation
