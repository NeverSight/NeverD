//===- KernelModelMDL.cpp - Request-owned virtual MDL mappings ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded system mappings for request-owned locked buffers. Physical page
/// identities are not modeled; PFN array access is explicitly rejected.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include <algorithm>
#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error mdlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

uint64_t pageBase(uint64_t Address) {
  return Address & ~(profile::PageSize - 1);
}
} // namespace

llvm::Expected<uint64_t>
KernelModel::createRequestMDL(uint32_t Size, llvm::ArrayRef<uint8_t> Initial,
                              bool Writable, uint64_t UserAddress) {
  if (!Size || Initial.size() > Size)
    return mdlError("a request MDL requires a nonempty bounded buffer");
  const uint64_t Offset = UserAddress & (profile::PageSize - 1);
  const uint64_t Pages =
      (Offset + Size + profile::PageSize - 1) / profile::PageSize;
  const uint64_t AllocationSize = Pages * profile::PageSize;
  auto Storage = allocate(AllocationSize, profile::PageSize);
  if (!Storage)
    return Storage.takeError();
  const uint64_t Buffer = *Storage + Offset;
  if (auto E = Memory.write(Buffer, Initial))
    return std::move(E);
  const uint64_t RecordSize = MDLSize + Pages * profile::PointerSize;
  auto Record = allocate(RecordSize);
  if (!Record)
    return Record.takeError();
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  // Public WDM macros read these fields. The reserved PFN area is opaque, not
  // a fabricated description of physical pages. CPU and API reads reject it.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_mdl
  for (const Field &F :
       std::array<Field, 6>{{{MDLNextOffset, 0, 8},
                             {MDLSizeOffset, RecordSize, 2},
                             {MDLFlagsOffset, MDLPagesLocked, 2},
                             {MDLStartVAOffset, pageBase(UserAddress), 8},
                             {MDLByteCountOffset, Size, 4},
                             {MDLByteOffsetOffset, Offset, 4}}})
    if (auto E = Memory.writeInteger(*Record + F.Offset, F.Value, F.Size))
      return std::move(E);
  LockedMdl State;
  State.Address = *Record;
  State.Size = RecordSize;
  State.Buffer = Buffer;
  State.AllocationSize = AllocationSize;
  State.UserAddress = UserAddress;
  State.ByteCount = Size;
  State.Writable = Writable;
  MDLs.emplace(*Record, State);
  if (auto E = Memory.protect(*Storage, AllocationSize, 0))
    return std::move(E);
  return *Record;
}

llvm::Expected<uint64_t> KernelModel::mapLockedPages(uint64_t MDL,
                                                     uint32_t Priority,
                                                     bool ReuseExisting) {
  if (!Request || Request->Completed || Request->Mdl != MDL || !MDLs.count(MDL))
    return mdlError("mapping requires the active request's locked MDL");
  const uint32_t BasePriority =
      Priority & ~(MdlMappingNoWrite | MdlMappingNoExecute);
  if (BasePriority != LowPagePriority && BasePriority != NormalPagePriority &&
      BasePriority != HighPagePriority)
    return mdlError("unsupported MDL mapping priority or flags");
  auto &State = MDLs.at(MDL);
  if (State.Mapped) {
    if (!ReuseExisting)
      return mdlError("an MDL cannot have a second system-space mapping");
    // MmGetSystemAddressForMdlSafe returns an existing mapping unchanged.
    return State.Buffer;
  }
  State.MappingWritable = State.Writable && !(Priority & MdlMappingNoWrite);
  const unsigned Permissions = Read | (State.MappingWritable ? Write : 0) |
                               ((Priority & MdlMappingNoExecute) ? 0 : Execute);
  if (auto E = Memory.protect(pageBase(State.Buffer), State.AllocationSize,
                              Permissions))
    return std::move(E);
  if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset, State.Buffer,
                                   profile::PointerSize))
    return std::move(E);
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset,
                                   MDLPagesLocked | MDLMappedToSystemVA, 2))
    return std::move(E);
  State.Mapped = true;
  return State.Buffer;
}

llvm::Error KernelModel::unmapLockedPages(uint64_t Address, uint64_t MDL) {
  if (!Request || Request->Completed || Request->Mdl != MDL || !MDLs.count(MDL))
    return mdlError("unmapping requires the active request's locked MDL");
  auto &State = MDLs.at(MDL);
  if (!State.Mapped || State.Buffer != Address)
    return mdlError("MDL unmapping requires its live system mapping address");
  if (auto E = Memory.protect(pageBase(State.Buffer), State.AllocationSize, 0))
    return E;
  if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset, 0,
                                   profile::PointerSize))
    return E;
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, MDLPagesLocked, 2))
    return E;
  State.Mapped = false;
  State.MappingWritable = false;
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>> KernelModel::readMDLBytes(uint64_t MDL,
                                                               uint32_t Count) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || Count > It->second.ByteCount)
    return mdlError("completion exceeds the locked MDL buffer");
  const auto &State = It->second;
  std::vector<uint8_t> Bytes(Count);
  // Unmapping revokes the driver's system VA, not the request's locked data.
  // At this stopped CPU boundary the model may read the same authoritative
  // storage to deliver completion, then reinstate the revoked mapping.
  if (!State.Mapped)
    if (auto E =
            Memory.protect(pageBase(State.Buffer), State.AllocationSize, Read))
      return std::move(E);
  auto ReadError = Memory.read(State.Buffer, Bytes);
  if (!State.Mapped) {
    auto RestoreError =
        Memory.protect(pageBase(State.Buffer), State.AllocationSize, 0);
    if (ReadError || RestoreError)
      return llvm::joinErrors(std::move(ReadError), std::move(RestoreError));
  } else if (ReadError) {
    return std::move(ReadError);
  }
  return Bytes;
}

llvm::Error KernelModel::expireRequestMDL() {
  if (!Request || !Request->Mdl)
    return llvm::Error::success();
  auto It = MDLs.find(Request->Mdl);
  if (It == MDLs.end())
    return mdlError("active request lost ownership of its MDL");
  const auto &State = It->second;
  if (auto E = Memory.protect(pageBase(State.Buffer), State.AllocationSize, 0))
    return E;
  FreedRanges.emplace(State.Address, State.Size);
  FreedRanges.emplace(pageBase(State.Buffer), State.AllocationSize);
  MDLs.erase(It);
  return llvm::Error::success();
}

llvm::Error KernelModel::validateMDLAccess(uint64_t Address, uint32_t Size,
                                           bool IsWrite) const {
  const uint64_t End = Address + Size;
  for (const auto &[MDL, State] : MDLs) {
    if (Address < MDL + State.Size && MDL < End) {
      if (IsWrite)
        return mdlError("request-owned MDL fields are read-only");
      const uint64_t First = std::max(Address, MDL) - MDL;
      const uint64_t Last = std::min(End, MDL + State.Size) - MDL;
      if ((First < MDLMappedSystemVAOffset && MDLFlagsOffset + 2 < Last) ||
          Last > MDLSize)
        return mdlError("MDL process and physical PFN data are not modeled");
    }
    const uint64_t Base = pageBase(State.Buffer);
    if (Address >= Base + State.AllocationSize || End <= Base)
      continue;
    if (!State.Mapped)
      return mdlError("guest access to an unmapped MDL system buffer");
    if (Address < State.Buffer || End > State.Buffer + State.ByteCount)
      return mdlError("guest access exceeds the MDL byte range");
    if (IsWrite && !State.MappingWritable)
      return mdlError("guest write to an MDL mapping with MdlMappingNoWrite");
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
