//===- KernelModelVirtualMemory.cpp - Process virtual memory queries ------===//
// NeverD Decompiler
///
/// \file
/// Process virtual memory queries.
///

#include "../WindowsUnicode.h"
#include "../driver/DriverProcessImages.h"
#include "KernelException.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "llvm/Support/Endian.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t BasicInformationSize = 48;
constexpr uint64_t HighestUserAddress19041 = 0x7ffffffeffff;
constexpr uint32_t MemCommit = 0x1000, MemPrivate = 0x20000,
                   MemMapped = 0x40000, MemImage = 0x1000000;
llvm::Error queryError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "virtual memory query: " + Message);
}
uint32_t protection(unsigned Permissions) {
  return Permissions & Write ? 4 : Permissions & Read ? 2 : 1;
}
uint32_t protection(DriverUserPageAccess Access) {
  switch (Access) {
  case DriverUserPageAccess::ReadOnly:
    return 2;
  case DriverUserPageAccess::ReadWrite:
    return 4;
  case DriverUserPageAccess::NoAccess:
    return 1;
  }
  llvm_unreachable("invalid user page access");
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::queryVirtualMemory(llvm::ArrayRef<uint64_t> A, bool ZwCall) {
  if (!Version || Version->Major != 10 || Version->Minor != 0 ||
      Version->Build != 19041)
    return queryError("requires Windows 10.0.19041");
  const uint32_t Class = uint32_t(A[2]);
  // The matched MmQueryVirtualMemory dispatch accepts class indices 0..11.
  // Valid classes whose providers are unavailable must remain explicit errors.
  if (Class > 11)
    return uint64_t(0xc0000003); // STATUS_INVALID_INFO_CLASS
  if (Class && Class != 2)
    return queryError("unmodeled memory information class: " +
                      llvm::Twine(Class));
  // Native class 0 checks this before probing or inspecting the handle.
  if (!Class && A[4] < BasicInformationSize)
    return StatusInfoLengthMismatch;
  const auto Context = executionProcessContext();
  if (!Context)
    return queryError("requires a modeled thread process context");
  const bool UserCall = !ZwCall && Context->PreviousMode == UserMode;
  if (UserCall) {
    // The native Nt entry probes the supplied extent with 8-byte alignment.
    // Convert only typed guest exceptions to syscall status. Unknown model
    // authority and backend failures are not synthetic access violations.
    auto Probe = [&](uint64_t Address, uint64_t Size,
                     uint32_t Alignment) -> llvm::Expected<uint32_t> {
      auto V = probeUserBuffer(Address, Size, Alignment, true);
      if (V)
        return 0;
      uint32_t Status = 0;
      auto E =
          llvm::handleErrors(V.takeError(), [&](const KernelGuestException &X) {
            Status = X.code();
          });
      if (E)
        return std::move(E);
      return Status;
    };
    auto Status = Probe(A[3], A[4], 8);
    if (!Status)
      return Status.takeError();
    if (*Status)
      return uint64_t(*Status);
    if (A[5]) {
      Status = Probe(A[5], 8, 1);
      if (!Status)
        return Status.takeError();
      if (*Status)
        return uint64_t(*Status);
      if (auto E = objectBuffer(A[5], 8, true))
        return E;
      auto Previous = Memory.readInteger(A[5], 8);
      if (!Previous)
        return Previous.takeError();
      if (auto E = Memory.writeInteger(A[5], *Previous, 8))
        return E;
    }
  }
  if (A[1] > HighestUserAddress19041)
    return StatusInvalidParameter;
  if (A[0] != UINT64_MAX) {
    auto Visible = [&](uint32_t Attributes, uint32_t PID) {
      return !(UserCall && (Attributes & ObjectKernelHandle)) &&
             canAccessHandle(Attributes, PID);
    };
    if (A[0] == CurrentThreadPseudoHandle)
      return StatusObjectTypeMismatch;
    if (auto H = ObjectHandles.find(A[0]); H != ObjectHandles.end())
      return Visible(H->second.Attributes, H->second.ProcessID)
                 ? StatusObjectTypeMismatch
                 : StatusInvalidHandle;
    auto WrongType = [&](const auto &Handles) -> std::optional<uint64_t> {
      auto H = Handles.find(A[0]);
      if (H == Handles.end())
        return std::nullopt;
      return Visible(H->second.Attributes, H->second.ProcessID)
                 ? StatusObjectTypeMismatch
                 : StatusInvalidHandle;
    };
    for (auto Status : {WrongType(FileHandles), WrongType(SectionHandles)})
      if (Status)
        return *Status;
    if (Registry.ownsHandle(A[0])) {
      if (UserCall)
        return queryError("registry handle UserMode visibility is unavailable");
      return StatusObjectTypeMismatch;
    }
    return StatusInvalidHandle;
  }
  if (!isLiveProcess(Context->ProcessID))
    return queryError("current process is not live");
  const uint64_t Page = A[1] & ~(profile::PageSize - 1);
  uint64_t AllocationBase = 0, End = 0;
  uint32_t Protect = 0, AllocationProtect = 0, Type = 0;
  const auto *ProcessView = processMemoryView(Context->ProcessID, Page, 1);
  if (const auto *V = ProcessView) {
    AllocationBase = V->Base;
    End = V->Base + V->Size;
    Protect = protection(V->Permissions);
    Type = MemPrivate;
    if (V->Image) {
      AllocationBase = V->Image->Source->Base;
      Protect = V->Image->Protection;
      AllocationProtect = 0x80;
      Type = MemImage;
      // Coalesce only adjacent image pages with the same provenance and
      // current protection. COW splits a run without changing its allocation.
      while (const auto *Next = processMemoryView(Context->ProcessID, End, 1)) {
        if (!Next->Image || Next->Image->Source != V->Image->Source ||
            Next->Image->Protection != Protect)
          break;
        End += Next->Size;
      }
    }
  }
  for (const auto &[Address, V] : UserAllocations)
    if (!Type && V.ProcessID == Context->ProcessID &&
        !RevokedUserAllocations.contains(Address) && Page >= Address &&
        Page - Address <
            ((V.Size + profile::PageSize - 1) & ~(profile::PageSize - 1))) {
      AllocationBase = Address;
      End = Address +
            ((V.Size + profile::PageSize - 1) & ~(profile::PageSize - 1));
      Protect = protection(V.Access);
      Type = MemPrivate;
    }
  for (const auto &[Key, V] : SectionViews)
    if (!Type && V.ProcessID == Context->ProcessID && Page >= V.Base &&
        Page - V.Base < V.Size) {
      AllocationBase = V.Base;
      End = V.Base + V.Size;
      Protect = protection(V.Permissions);
      if (V.Cache == KernelPhysicalMemory::CacheType::NonCached)
        Protect |= 0x200;
      if (V.Cache == KernelPhysicalMemory::CacheType::WriteCombined)
        Protect |= 0x400;
      Type = MemMapped;
    }
  // No host VAD inference: missing catalogue entries do not prove a native
  // free region's complete extent, nor an image-section allocation.
  if (!Type)
    return queryError("address has no modeled virtual allocation metadata");
  if (Class == 2) {
    if (Type == MemPrivate)
      return uint64_t(
          0xc0000141); // STATUS_INVALID_ADDRESS: no file provenance.
    if (Type != MemImage || !ProcessView || !ProcessView->Image)
      return queryError("mapped allocation has no file-name provider");
    auto Name =
        windows_unicode::bytes(ProcessView->Image->Source->MappedFile, true);
    if (!Name)
      return Name.takeError();
    const uint64_t Needed = 16 + Name->size();
    auto Device =
        windows_unicode::bytes(ProcessView->Image->Source->DeviceName, true);
    if (!Device)
      return Device.takeError();
    if (A[5])
      if (auto E = objectBuffer(A[5], 8, true))
        return E;
    if (A[4] < 16) {
      if (A[5])
        if (auto E = Memory.writeInteger(A[5], Needed, 8))
          return E;
      return StatusInfoLengthMismatch;
    }
    if (A[4] < 16 + Device->size()) {
      if (A[5])
        if (auto E = Memory.writeInteger(A[5], Needed, 8))
          return E;
      return uint64_t(0x80000005); // STATUS_BUFFER_OVERFLOW, output untouched.
    }
    const bool Full = A[4] >= Needed;
    const bool DeviceOnly = A[4] < 18 + Device->size();
    const uint64_t NameCapacity = Full         ? Name->size()
                                  : DeviceOnly ? Device->size()
                                               : (A[4] - 16) & ~uint64_t(1);
    const uint64_t Written = 16 + NameCapacity;
    if (auto E = objectBuffer(A[3], Written, true))
      return E;
    std::vector<uint8_t> Bytes(Written);
    llvm::support::endian::write16le(Bytes.data(), NameCapacity - 2);
    llvm::support::endian::write16le(Bytes.data() + 2, NameCapacity);
    llvm::support::endian::write64le(Bytes.data() + 8, A[3] + 16);
    std::copy_n(Name->begin(), NameCapacity - 2, Bytes.begin() + 16);
    if (Full) {
      if (auto E = Memory.write(A[3], Bytes))
        return E;
    } else {
      // Native partial output preserves the four padding bytes in the caller's
      // UNICODE_STRING. It terminates the copied prefix within the buffer.
      if (auto E = Memory.write(A[3], llvm::ArrayRef(Bytes).take_front(4)))
        return E;
      if (auto E = Memory.write(A[3] + 8, llvm::ArrayRef(Bytes).drop_front(8)))
        return E;
    }
    if (A[5])
      if (auto E = Memory.writeInteger(A[5], DeviceOnly ? 0 : Needed, 8))
        return E;
    APICallDetail = "process=" + std::to_string(Context->ProcessID) +
                    "; mapped_file=" + ProcessView->Image->Source->MappedFile;
    return Full         ? uint64_t(StatusSuccess)
           : DeviceOnly ? uint64_t(0xc0000039)
                        : uint64_t(0x80000005);
  }
  if (auto E = objectBuffer(A[3], BasicInformationSize, true))
    return E;
  if (A[5])
    if (auto E = objectBuffer(A[5], 8, true))
      return E;
  std::array<uint8_t, BasicInformationSize> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), Page);
  llvm::support::endian::write64le(Bytes.data() + 8, AllocationBase);
  llvm::support::endian::write32le(
      Bytes.data() + 16, AllocationProtect ? AllocationProtect : Protect);
  llvm::support::endian::write64le(Bytes.data() + 24, End - Page);
  llvm::support::endian::write32le(Bytes.data() + 32, MemCommit);
  llvm::support::endian::write32le(Bytes.data() + 36, Protect);
  llvm::support::endian::write32le(Bytes.data() + 40, Type);
  if (auto E = Memory.write(A[3], Bytes))
    return E;
  // Native code copies the 48-byte result, then the pointer-sized length.
  // Retain this ordering when the two output ranges alias.
  if (A[5])
    if (auto E = Memory.writeInteger(A[5], BasicInformationSize, 8))
      return E;
  APICallDetail = "process=" + std::to_string(Context->ProcessID) +
                  "; bytes=48; type=" + std::to_string(Type);
  return StatusSuccess;
}
} // namespace neverd::emulation
