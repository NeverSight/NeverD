//===- KernelModelMDL.cpp - Bounded virtual MDL ownership -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded system mappings for request buffers and driver nonpaged pool MDLs.
/// Physical page identities are not modeled; PFN access is explicitly rejected.
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
KernelModel::createMDLRecord(uint64_t Address, uint32_t Size, uint16_t Flags) {
  const uint64_t Offset = Address & (profile::PageSize - 1);
  const uint64_t Pages =
      (Offset + Size + profile::PageSize - 1) / profile::PageSize;
  const uint64_t RecordSize = MDLSize + Pages * profile::PointerSize;
  const uint64_t Start =
      (NextAllocation + PoolAlignment - 1) & ~(PoolAlignment - 1);
  if (Start > AllocationEnd || RecordSize > AllocationEnd - Start)
    return 0;
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
                             {MDLFlagsOffset, Flags, 2},
                             {MDLStartVAOffset, pageBase(Address), 8},
                             {MDLByteCountOffset, Size, 4},
                             {MDLByteOffsetOffset, Offset, 4}}})
    if (auto E = Memory.writeInteger(*Record + F.Offset, F.Value, F.Size))
      return std::move(E);
  return *Record;
}

llvm::Expected<uint64_t> KernelModel::allocateMDL(llvm::ArrayRef<uint64_t> A) {
  // Standalone ownership avoids claiming the I/O manager's MDL-chain and
  // completion behavior. IoAllocateMdl initializes metadata without probing
  // the described memory; building the descriptor checks our pool contract.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-ioallocatemdl
  if (static_cast<uint8_t>(A[2]) || static_cast<uint8_t>(A[3]) || A[4])
    return mdlError("IoAllocateMdl supports standalone MDLs without IRP "
                    "association, secondary buffers or quota charging");
  const uint32_t Size = static_cast<uint32_t>(A[1]);
  if (!Size || Size > profile::KernelArenaSize || Size > UINT64_MAX - A[0])
    return mdlError("IoAllocateMdl requires a nonempty, nonoverflowing buffer "
                    "bounded by the model arena size");
  auto Record = createMDLRecord(A[0], Size, 0);
  if (!Record)
    return Record.takeError();
  if (!*Record)
    return 0;
  LockedMdl State;
  State.Owner = LockedMdl::Ownership::Driver;
  State.Address = *Record;
  const uint64_t Offset = A[0] & (profile::PageSize - 1);
  State.Size =
      MDLSize + ((Offset + Size + profile::PageSize - 1) / profile::PageSize) *
                    profile::PointerSize;
  State.Buffer = A[0];
  State.ByteCount = Size;
  MDLs.emplace(*Record, State);
  return *Record;
}

llvm::Error KernelModel::buildNonPagedMDL(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner != LockedMdl::Ownership::Driver)
    return mdlError("MmBuildMdlForNonPagedPool requires an unbuilt, live "
                    "driver-allocated MDL");
  auto &State = It->second;
  auto Pool = Allocations.upper_bound(State.Buffer);
  if (Pool == Allocations.begin())
    return mdlError("MmBuildMdlForNonPagedPool requires a live nonpaged pool "
                    "buffer; stack, image and user buffers are unsupported");
  --Pool;
  const uint64_t Offset = State.Buffer - Pool->first;
  if (!Pool->second.NonPaged || Offset >= Pool->second.Size ||
      State.ByteCount > Pool->second.Size - Offset)
    return mdlError("MmBuildMdlForNonPagedPool requires the complete MDL "
                    "range inside one live nonpaged pool allocation");
  // The existing pool VA is authoritative; allocating a separate mapping
  // would lose aliasing and permit Windows-forbidden map/unmap operations.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-mmbuildmdlfornonpagedpool
  if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset, State.Buffer,
                                   profile::PointerSize))
    return E;
  if (auto E =
          Memory.writeInteger(MDL + MDLFlagsOffset, MDLSourceIsNonPagedPool, 2))
    return E;
  State.Owner = LockedMdl::Ownership::NonPagedPool;
  State.Pool = Pool->first;
  State.Mapped = true;
  State.Writable = true;
  State.MappingWritable = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeMDL(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("IoFreeMdl received an unknown or already freed MDL");
  if (It->second.Owner == LockedMdl::Ownership::Request)
    return mdlError("IoFreeMdl cannot release a request-owned locked MDL");
  // A nonpaged pool MDL owns only its descriptor. Its original buffer and
  // mapping survive IoFreeMdl and remain governed by pool allocation lifetime.
  FreedRanges.emplace(It->second.Address, It->second.Size);
  MDLs.erase(It);
  return llvm::Error::success();
}

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
  auto Record = createMDLRecord(UserAddress, Size, MDLPagesLocked);
  if (!Record)
    return Record.takeError();
  if (!*Record)
    return mdlError("request MDL exhausted the Windows model arena");
  LockedMdl State;
  State.Address = *Record;
  State.Size = MDLSize + Pages * profile::PointerSize;
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
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("mapping requires a live modeled MDL");
  const uint32_t BasePriority =
      Priority & ~(MdlMappingNoWrite | MdlMappingNoExecute);
  if (BasePriority != LowPagePriority && BasePriority != NormalPagePriority &&
      BasePriority != HighPagePriority)
    return mdlError("unsupported MDL mapping priority or flags");
  auto &State = It->second;
  if (State.Owner == LockedMdl::Ownership::Driver)
    return mdlError("mapping requires a built MDL; allocated metadata does "
                    "not lock or map the described buffer");
  if (State.Owner == LockedMdl::Ownership::NonPagedPool) {
    if (!ReuseExisting)
      return mdlError("a nonpaged pool MDL cannot create an additional "
                      "system-space mapping");
    if (!Allocations.count(State.Pool))
      return mdlError("nonpaged pool MDL describes a freed pool allocation");
    // Existing mappings retain their permissions even when safe-helper flags
    // request no-write/no-execute. No page protections are changed here.
    return State.Buffer;
  }
  if (!Request || Request->Completed || Request->Mdl != MDL)
    return mdlError("mapping requires the active request's locked MDL");
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
  auto It = MDLs.find(MDL);
  if (It != MDLs.end() &&
      It->second.Owner == LockedMdl::Ownership::NonPagedPool)
    return mdlError("a nonpaged pool MDL cannot release its existing "
                    "system-space mapping");
  if (!Request || Request->Completed || Request->Mdl != MDL || !MDLs.count(MDL))
    return mdlError("unmapping requires the active request's locked MDL");
  auto &State = MDLs.at(MDL);
  if (!State.Mapped || State.Buffer != Address)
    return mdlError("MDL unmapping requires its live system mapping address");
  if (auto E =
          prepareReleaseRange(pageBase(State.Buffer), State.AllocationSize))
    return E;
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
        return mdlError("modeled MDL fields are read-only; driver MDL chains "
                        "and field mutation are unsupported");
      const uint64_t First = std::max(Address, MDL) - MDL;
      const uint64_t Last = std::min(End, MDL + State.Size) - MDL;
      if ((First < MDLMappedSystemVAOffset && MDLFlagsOffset + 2 < Last) ||
          Last > MDLSize)
        return mdlError("MDL process and physical PFN data are not modeled");
    }
    // Driver MDLs describe an existing allocation; neither their byte range
    // nor their lifetime restricts otherwise valid accesses to that pool.
    if (State.Owner != LockedMdl::Ownership::Request)
      continue;
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
