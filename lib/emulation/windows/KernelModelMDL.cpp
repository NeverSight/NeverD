//===- KernelModelMDL.cpp - Bounded virtual MDL ownership -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded system mappings for request buffers and driver nonpaged pool MDLs.
/// Built descriptors publish the physical identities of their existing RAM.
///
//===----------------------------------------------------------------------===//

#include "KernelModel.h"
#include "KernelException.h"
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
  // Public WDM macros read these fields. The PFN area remains unavailable until
  // the descriptor is built against a live physical RAM owner.
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
  if (auto E = initializeMDLPhysicalPages(State))
    return E;
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
  State.DmaWritable = true;
  State.MappingWritable = true;
  return llvm::Error::success();
}

llvm::Error KernelModel::probeAndLockPages(uint64_t MDL, uint32_t Mode,
                                           uint32_t Operation) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner != LockedMdl::Ownership::Driver)
    return mdlError("MmProbeAndLockPages requires an unbuilt driver MDL");
  if (Mode != UserMode ||
      (Operation != IoReadAccess && Operation != IoWriteAccess &&
       Operation != IoModifyAccess))
    return mdlError("MmProbeAndLockPages supports UserMode read/write locks");
  if (CurrentIRQL > APCLevel ||
      !canCatchUserAccess(It->second.Buffer, It->second.ByteCount))
    return mdlError("user page locking requires the requesting process at "
                    "IRQL <= APC_LEVEL");
  auto &State = It->second;
  auto Region = UserAllocations.upper_bound(State.Buffer);
  if (Region == UserAllocations.begin())
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  --Region;
  const uint64_t Offset = State.Buffer - Region->first;
  if (RevokedUserAllocations.contains(Region->first))
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  if (Region->second.ProcessID != CurrentUserProcessID ||
      Offset >= Region->second.Size ||
      State.ByteCount > Region->second.Size - Offset)
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  auto Allowed = Memory.canAccess(State.Buffer, State.ByteCount,
                                   Operation == IoReadAccess ? Read
                                                             : Read | Write);
  if (!Allowed)
    return Allowed.takeError();
  if (!*Allowed)
    return llvm::make_error<KernelGuestException>(
        exceptions::StatusAccessViolation);
  if (auto E = Physical.canPin(Region->first, Offset, State.ByteCount))
    return E;
  if (auto E = initializeMDLPhysicalPages(State))
    return E;
  auto Pin = Physical.pin(Region->first, Offset, State.ByteCount);
  if (!Pin)
    return Pin.takeError();
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, MDLPagesLocked, 2)) {
    llvm::consumeError(Physical.unpin(*Pin));
    return E;
  }
  State.Owner = LockedMdl::Ownership::UserLocked;
  State.Pool = Region->first;
  State.Pin = *Pin;
  State.UserAddress = State.Buffer;
  State.Writable = Operation != IoReadAccess;
  State.DmaWritable = State.Writable;
  return llvm::Error::success();
}

llvm::Error KernelModel::unlockPages(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner != LockedMdl::Ownership::UserLocked)
    return mdlError("MmUnlockPages requires a locked user MDL");
  auto &State = It->second;
  if (auto E = DMA.canReleaseRange(MDL, State.Size))
    return E;
  if (State.Mapped) {
    if (auto E = unmapLockedPages(State.Buffer, MDL))
      return E;
  }
  if (auto E = Physical.unpin(State.Pin))
    return E;
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, 0, 2))
    return E;
  State.Owner = LockedMdl::Ownership::Driver;
  State.Pin = 0;
  State.Pool = 0;
  State.Writable = false;
  State.DmaWritable = false;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeMDL(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("IoFreeMdl received an unknown or already freed MDL");
  if (It->second.Owner == LockedMdl::Ownership::Request)
    return mdlError("IoFreeMdl cannot release a request-owned locked MDL");
  if (It->second.Owner == LockedMdl::Ownership::RequestSystemBuffer)
    return mdlError(
        "IoFreeMdl cannot release a request-owned system-buffer MDL");
  if (It->second.Owner == LockedMdl::Ownership::UserLocked)
    return mdlError("IoFreeMdl requires MmUnlockPages for a user MDL");
  if (auto E = prepareReleaseRange(It->second.Address, It->second.Size))
    return E;
  // A nonpaged pool MDL owns only its descriptor. Its original buffer and
  // mapping survive IoFreeMdl and remain governed by pool allocation lifetime.
  FreedRanges.emplace(It->second.Address, It->second.Size);
  MDLs.erase(It);
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::createRequestMDL(uint64_t IRP, uint32_t Size,
                              llvm::ArrayRef<uint8_t> Initial, bool Writable,
                              uint64_t UserAddress, bool DmaWritable) {
  const auto *Owner = requestForIRP(IRP);
  if (!Owner || Owner->Completed)
    return mdlError("a request MDL requires a live owning IRP");
  if (!Size || Initial.size() > Size)
    return mdlError("a request MDL requires a nonempty bounded buffer");
  const uint64_t Offset = UserAddress & (profile::PageSize - 1);
  const uint64_t Pages =
      (Offset + Size + profile::PageSize - 1) / profile::PageSize;
  const uint64_t AllocationSize = Pages * profile::PageSize;
  const uint64_t Start =
      (NextAllocation + profile::PageSize - 1) & ~(profile::PageSize - 1);
  const uint64_t RecordSize = MDLSize + Pages * profile::PointerSize;
  if (Start > AllocationEnd || AllocationSize > AllocationEnd - Start ||
      RecordSize > AllocationEnd - Start - AllocationSize)
    return mdlError("request MDL exhausted the Windows model arena");
  auto Storage =
      allocatePhysicalBuffer(AllocationSize, profile::PageSize, Offset, Size);
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
  State.OwnerIRP = IRP;
  State.Address = *Record;
  State.Size = MDLSize + Pages * profile::PointerSize;
  State.Buffer = Buffer;
  State.AllocationSize = AllocationSize;
  State.UserAddress = UserAddress;
  State.ByteCount = Size;
  State.Writable = Writable;
  State.DmaWritable = DmaWritable;
  if (auto E = initializeMDLPhysicalPages(State))
    return std::move(E);
  MDLs.emplace(*Record, State);
  if (auto E = Memory.protect(*Storage, AllocationSize, 0))
    return std::move(E);
  return *Record;
}

llvm::Expected<uint64_t> KernelModel::frameworkRequestMDL(uint64_t IRP,
                                                          bool Output) {
  auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return mdlError("framework MDL retrieval requires a live owning IRP");
  const bool IsIOCTL = Request->Kind == DriverRequestKind::DeviceControl;
  const bool IsRead = Request->Kind == DriverRequestKind::Read;
  const bool IsWrite = Request->Kind == DriverRequestKind::Write;
  if ((!IsIOCTL && !IsRead && !IsWrite) || (IsRead && !Output) ||
      (IsWrite && Output))
    return mdlError("framework MDL direction does not match the WDM request");
  const uint32_t Length = Output ? Request->OutputSize : Request->InputSize;
  if (!Length)
    return mdlError("framework MDL requires a nonempty request buffer");
  if (Request->Direct && (!IsIOCTL || Output)) {
    auto It = MDLs.find(Request->Mdl);
    if (It == MDLs.end() || It->second.OwnerIRP != IRP ||
        It->second.Owner != LockedMdl::Ownership::Request)
      return mdlError("framework request lost its direct MDL");
    // Retrieving a descriptor does not create or change its system mapping.
    return Request->Mdl;
  }
  if (!Request->SystemBuffer || Length > Request->BufferSize)
    return mdlError("framework request lost its system buffer");
  if (Request->SystemMdl) {
    auto It = MDLs.find(Request->SystemMdl);
    if (It == MDLs.end() || It->second.OwnerIRP != IRP ||
        It->second.Owner != LockedMdl::Ownership::RequestSystemBuffer)
      return mdlError("framework request lost its system-buffer MDL");
    return Request->SystemMdl;
  }
  // Both public WDF helpers share m_AllocatedMdl. Buffered IOCTL directions
  // therefore reuse the first successful descriptor and its original length.
  // The existing SystemBuffer is the only data allocation and remains the
  // authority for aliasing and lifetime; this owner adds just a descriptor.
  // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/core/km/fxrequestkm.cpp#L301-L338
  // https://github.com/microsoft/Windows-Driver-Frameworks/blob/b6191d9543441329154da32f7ab9bdd97228dd3c/src/framework/shared/core/km/fxrequestkm.cpp#L542-L580
  auto Record =
      createMDLRecord(Request->SystemBuffer, Length, MDLSourceIsNonPagedPool);
  if (!Record)
    return Record.takeError();
  if (!*Record)
    return 0;
  if (auto E = Memory.writeInteger(*Record + MDLMappedSystemVAOffset,
                                   Request->SystemBuffer, profile::PointerSize))
    return std::move(E);
  LockedMdl State;
  State.Owner = LockedMdl::Ownership::RequestSystemBuffer;
  State.OwnerIRP = IRP;
  State.Address = *Record;
  const uint64_t Offset = Request->SystemBuffer & (profile::PageSize - 1);
  State.Size = MDLSize +
               ((Offset + Length + profile::PageSize - 1) / profile::PageSize) *
                   profile::PointerSize;
  State.Buffer = Request->SystemBuffer;
  State.ByteCount = Length;
  State.Writable = true;
  State.DmaWritable = true;
  State.Mapped = true;
  State.MappingWritable = true;
  if (auto E = initializeMDLPhysicalPages(State))
    return std::move(E);
  MDLs.emplace(*Record, State);
  Request->SystemMdl = *Record;
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
  if (State.Owner == LockedMdl::Ownership::RequestSystemBuffer) {
    const auto *Owner = requestForIRP(State.OwnerIRP);
    if (!Owner || Owner->Completed || Owner->SystemMdl != MDL ||
        Owner->SystemBuffer != State.Buffer)
      return mdlError(
          "mapping requires the active request's system-buffer MDL");
    if (!ReuseExisting)
      return mdlError("a request system-buffer MDL cannot create an additional "
                      "system-space mapping");
    return State.Buffer;
  }
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
  if (State.Owner == LockedMdl::Ownership::UserLocked) {
    if (State.Mapped) {
      if (!ReuseExisting)
        return mdlError("an MDL cannot have a second system-space mapping");
      return State.Buffer;
    }
    const uint64_t Offset = State.UserAddress & (profile::PageSize - 1);
    const uint64_t AllocationSize =
        (Offset + State.ByteCount + profile::PageSize - 1) &
        ~(profile::PageSize - 1);
    const uint64_t End = profile::UserAliasBase + profile::UserAliasSize;
    if (NextUserAlias > End || AllocationSize > End - NextUserAlias)
      return 0;
    const uint64_t Alias = NextUserAlias + Offset;
    const bool Writable = State.Writable && !(Priority & MdlMappingNoWrite);
    const unsigned Permissions = Read | (Writable ? Write : 0) |
                                 ((Priority & MdlMappingNoExecute) ? 0
                                                                   : Execute);
    if (auto E = Memory.mapAlias(NextUserAlias, pageBase(State.UserAddress),
                                  AllocationSize, Permissions)) {
      if (E.isA<GuestMemoryLimitError>()) {
        llvm::consumeError(std::move(E));
        return 0;
      }
      return std::move(E);
    }
    if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset, Alias, 8))
      return E;
    if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset,
                                     MDLPagesLocked | MDLMappedToSystemVA, 2))
      return E;
    State.Buffer = Alias;
    State.AllocationSize = AllocationSize;
    State.MappingWritable = Writable;
    State.Mapped = true;
    NextUserAlias += AllocationSize;
    return Alias;
  }
  const auto *Owner = requestForIRP(State.OwnerIRP);
  if (!Owner || Owner->Completed || Owner->Mdl != MDL)
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
      It->second.Owner == LockedMdl::Ownership::RequestSystemBuffer)
    return mdlError("a request system-buffer MDL cannot release its existing "
                    "system-space mapping");
  if (It != MDLs.end() &&
      It->second.Owner == LockedMdl::Ownership::NonPagedPool)
    return mdlError("a nonpaged pool MDL cannot release its existing "
                    "system-space mapping");
  if (It == MDLs.end() ||
      (It->second.Owner != LockedMdl::Ownership::Request &&
       It->second.Owner != LockedMdl::Ownership::UserLocked))
    return mdlError("unmapping requires the active request's locked MDL");
  auto &State = It->second;
  if (State.Owner == LockedMdl::Ownership::Request) {
    const auto *Owner = requestForIRP(State.OwnerIRP);
    if (!Owner || Owner->Completed || Owner->Mdl != MDL)
      return mdlError("unmapping requires the active request's locked MDL");
  }
  if (!State.Mapped || State.Buffer != Address)
    return mdlError("MDL unmapping requires its live system mapping address");
  if (auto E = prepareRevokeVirtualRange(pageBase(State.Buffer),
                                         State.AllocationSize))
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
  if (State.Owner == LockedMdl::Ownership::UserLocked) {
    State.Buffer = State.UserAddress;
    State.AllocationSize = 0;
  }
  return llvm::Error::success();
}

llvm::Expected<std::vector<uint8_t>> KernelModel::readMDLBytes(uint64_t MDL,
                                                               uint32_t Count) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || Count > It->second.ByteCount)
    return mdlError("completion exceeds the locked MDL buffer");
  const auto &State = It->second;
  std::vector<uint8_t> Bytes(Count);
  // The I/O manager consumes the original locked storage after all DMA pins
  // were released. Reading it never grants the guest a system mapping.
  auto Owner = Physical.ownerForRange(State.Buffer, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  if (auto E = Memory.readBacking(State.Buffer, Bytes))
    return std::move(E);
  return Bytes;
}

llvm::Error KernelModel::expireRequestMDL(uint64_t IRP) {
  const auto *Owner = requestForIRP(IRP);
  if (!Owner || Owner->Completed)
    return mdlError("active request lost ownership of its MDL");
  const auto Direct = MDLs.find(Owner->Mdl);
  const auto System = MDLs.find(Owner->SystemMdl);
  if (Owner->Mdl && (Direct == MDLs.end() || Direct->second.OwnerIRP != IRP ||
                     Direct->second.Owner != LockedMdl::Ownership::Request))
    return mdlError("active request lost ownership of its MDL");
  if (Owner->SystemMdl &&
      (System == MDLs.end() || System->second.OwnerIRP != IRP ||
       System->second.Owner != LockedMdl::Ownership::RequestSystemBuffer))
    return mdlError("active request lost ownership of its system-buffer MDL");
  if (Direct != MDLs.end()) {
    const auto &State = Direct->second;
    if (auto E = Physical.canRetire(pageBase(State.Buffer)))
      return E;
    if (auto E =
            Memory.protect(pageBase(State.Buffer), State.AllocationSize, 0))
      return E;
    if (auto E = Physical.retire(pageBase(State.Buffer)))
      return E;
    FreedRanges.emplace(State.Address, State.Size);
    FreedRanges.emplace(pageBase(State.Buffer), State.AllocationSize);
    MDLs.erase(Direct);
  }
  if (System != MDLs.end()) {
    // Completion already owns the SystemBuffer's release. Never revoke its
    // arena page (which may also contain unrelated live allocations).
    FreedRanges.emplace(System->second.Address, System->second.Size);
    MDLs.erase(System);
  }
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
      if (First < MDLMappedSystemVAOffset && MDLFlagsOffset + 2 < Last)
        return mdlError("MDL process fields are not modeled");
      if (Last > MDLSize && State.Owner == LockedMdl::Ownership::Driver)
        return mdlError("unbuilt MDL physical PFN data is unavailable");
    }
    // Driver MDLs describe an existing allocation; neither their byte range
    // nor their lifetime restricts otherwise valid accesses to that pool.
    if (State.Owner != LockedMdl::Ownership::Request &&
        State.Owner != LockedMdl::Ownership::UserLocked)
      continue;
    if (State.Owner == LockedMdl::Ownership::UserLocked && !State.Mapped)
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
