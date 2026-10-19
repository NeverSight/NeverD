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

#include "KernelException.h"
#include "KernelModel.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/DriverProfile.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
using namespace windows;

llvm::Error mdlError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

bool overlaps(uint64_t A, uint64_t Size, uint64_t B, uint64_t Length) {
  return Size && Length && A < B + Length && B < A + Size;
}

uint64_t pageBase(uint64_t Address) {
  return Address & ~(profile::PageSize - 1);
}

llvm::Expected<uint64_t> mdlRecordSize(uint64_t Address, uint32_t Size,
                                       bool MetadataOnly = false) {
  if (!MetadataOnly && (!Size || Size > UINT64_MAX - Address))
    return mdlError("MDL requires a nonempty, nonoverflowing buffer");
  const uint64_t Offset = Address & (profile::PageSize - 1);
  const uint64_t Pages =
      (Offset + Size + profile::PageSize - 1) / profile::PageSize;
  const uint64_t RecordSize = MDLSize + Pages * profile::PointerSize;
  // Descriptor storage is independent of the described allocation. Keep the
  // bounded public Size field exact; larger descriptor layouts are unsupported.
  if (RecordSize > UINT16_MAX)
    return mdlError("MDL PFN descriptor exceeds its modeled 16-bit size field");
  return RecordSize;
}
} // namespace

uint64_t KernelModel::LockedMdl::systemMappingBase() const {
  return Advance ? Advance->MappingBase : pageBase(Buffer);
}

llvm::Error KernelModel::validateKernelObjectBorrow(uint64_t Address,
                                                    uint64_t Size) const {
  if (!Size)
    return llvm::Error::success();
  if (Size > UINT64_MAX - Address)
    return mdlError("overflowing kernel object storage borrow");
  // Image MDLs expose their whole owned pages; pool mappings retain the
  // logical ByteCount boundary. Neither may bypass kernel-owned fields.
  if (imageOwnerForRange(Address, Size)) {
    const uint64_t Last = pageBase(Address + Size - 1);
    Address = pageBase(Address);
    Size = Last - Address + profile::PageSize;
  }
  if (auto E = validateExecutiveResourceAccess(Address, Size))
    return E;
  for (uint64_t Head : LookasideHeads)
    if (Head && overlaps(Address, Size, Head, ListEntrySize))
      return mdlError("MDL borrow intersects a private lookaside registry");
  for (const auto &[Base, List] : Lookasides)
    if (overlaps(Address, Size, Base, LookasideSize))
      return mdlError("MDL borrow intersects a live lookaside list");
  for (const auto &[Base, Hold] : FastMutexHolds)
    if (overlaps(Address, Size, Base, FastMutexSize))
      return mdlError("MDL borrow intersects a held fast mutex");
  for (const auto &[Base, State] : MDLs)
    if (overlaps(Address, Size, Base, State.Size))
      return mdlError("MDL borrow intersects live descriptor storage");
  return llvm::Error::success();
}

llvm::Error KernelModel::validateUnborrowedKernelStorage(uint64_t Address,
                                                         uint64_t Size) const {
  for (const auto &[MDL, State] : MDLs) {
    if (State.Owner == LockedMdl::Ownership::Driver ||
        State.Owner == LockedMdl::Ownership::ReleasedPages)
      continue;
    uint64_t Backing =
        State.Advance ? State.Advance->BackingAddress : State.BackingAddress;
    uint64_t Length =
        State.Advance ? State.Advance->ByteCount : State.ByteCount;
    if (Length && imageOwnerForRange(Backing, Length)) {
      const uint64_t Last = pageBase(Backing + Length - 1);
      Backing = pageBase(Backing);
      Length = Last - Backing + profile::PageSize;
    }
    if (overlaps(Address, Size, Backing, Length) ||
        (State.Mapped && overlaps(Address, Size, State.systemMappingBase(),
                                  State.AllocationSize)))
      return mdlError(
          "kernel object storage already has an MDL borrow or alias");
  }
  return llvm::Error::success();
}

std::optional<uint64_t> KernelModel::imageOwnerForRange(uint64_t Address,
                                                        uint64_t Size) const {
  auto It = ImageRAM.upper_bound(Address);
  if (It == ImageRAM.begin())
    return std::nullopt;
  --It;
  const uint64_t Offset = Address - It->first;
  if (Offset >= It->second || Size > It->second - Offset)
    return std::nullopt;
  return It->first;
}

llvm::Expected<uint64_t>
KernelModel::createMDLRecord(uint64_t Address, uint32_t Size, uint16_t Flags) {
  auto Extent = mdlRecordSize(Address, Size);
  if (!Extent)
    return Extent.takeError();
  const uint64_t RecordSize = *Extent;
  const uint64_t Start =
      (NextAllocation + PoolAlignment - 1) & ~(PoolAlignment - 1);
  if (Start > AllocationEnd || RecordSize > AllocationEnd - Start)
    return 0;
  auto Record = allocate(RecordSize);
  if (!Record)
    return Record.takeError();
  if (auto E = initializeMDLRecord(*Record, Address, Size, RecordSize, Flags))
    return std::move(E);
  return *Record;
}

llvm::Error KernelModel::initializeMDLRecord(uint64_t Record, uint64_t Address,
                                             uint32_t Size, uint64_t Extent,
                                             uint16_t Flags) {
  struct Field {
    uint64_t Offset, Value;
    unsigned Size;
  };
  // Preserve the original MmCreateMdl store order and untouched caller bytes.
  // PFNs remain unavailable until built against a live physical RAM owner.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ns-wdm-_mdl
  for (const Field &F : std::array<Field, 6>{
           {{MDLNextOffset, 0, 8},
            {MDLFlagsOffset, Flags, 2},
            {MDLByteCountOffset, Size, 4},
            {MDLStartVAOffset, pageBase(Address), 8},
            {MDLSizeOffset, Extent, 2},
            {MDLByteOffsetOffset, Address & (profile::PageSize - 1), 4}}})
    if (auto E = Memory.writeInteger(Record + F.Offset, F.Value, F.Size))
      return E;
  return llvm::Error::success();
}

llvm::Expected<uint64_t>
KernelModel::createMDL(uint64_t Record, uint64_t Address, uint64_t Size) {
  // The original wrapper checks full SIZE_T before touching any storage. It
  // initializes raw metadata without probing even empty or wrapping buffers.
  if (Size > UINT32_MAX)
    return 0;
  auto Extent = mdlRecordSize(Address, uint32_t(Size), true);
  if (!Extent)
    return Extent.takeError();
  LockedMdl State;
  State.Storage = LockedMdl::StorageKind::NonPagedPool;
  if (Record) {
    auto Pool = Allocations.upper_bound(Record);
    if (Pool != Allocations.begin()) {
      --Pool;
      const uint64_t Offset = Record - Pool->first;
      if (Pool->second.NonPaged && Offset < Pool->second.Size &&
          *Extent <= Pool->second.Size - Offset)
        State.StorageOwner = Pool->first;
    }
    if (!State.StorageOwner) {
      auto Stack = currentStackBounds();
      if (!Stack)
        return Stack.takeError();
      if (Record < Stack->first || Record >= Stack->second ||
          *Extent > Stack->second - Record)
        return mdlError("MmCreateMdl storage requires one live nonpaged pool "
                        "allocation or the current execution stack");
      State.Storage = LockedMdl::StorageKind::ExecutionStack;
      State.StorageOwner = Stack->first;
    }
    if (auto E = validateKernelObjectBorrow(Record, *Extent))
      return std::move(E);
    if (auto E = validateGuestAccess(Record, uint32_t(*Extent), true))
      return std::move(E);
    if (auto E = validateUnborrowedKernelStorage(Record, *Extent))
      return std::move(E);
    if (auto E = canRevokeVirtualRange(Record, *Extent))
      return std::move(E);
    auto Writable = Memory.canAccess(Record, *Extent, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return mdlError(
          "MmCreateMdl requires wholly writable descriptor storage");
  } else {
    // The stock wrapper's flag translator requests the same zeroing nonpaged
    // allocation as ExAllocatePool2(POOL_FLAG_NON_PAGED, extent, 'ldmM').
    auto Pool = allocatePool({0x40, *Extent, 0x6c646d4d}, true,
                             PoolSource::KernelResult);
    if (!Pool)
      return Pool.takeError();
    if (!*Pool)
      return 0;
    Record = *Pool;
    State.StorageOwner = Record;
  }
  if (auto E = initializeMDLRecord(Record, Address, uint32_t(Size), *Extent, 0))
    return std::move(E);
  State.Owner = LockedMdl::Ownership::Driver;
  State.Address = Record;
  State.Size = *Extent;
  State.Buffer = Address;
  State.OriginalAddress = Address;
  State.BackingAddress = Address;
  State.ByteCount = uint32_t(Size);
  MDLs.emplace(Record, State);
  return Record;
}

llvm::Error KernelModel::canReleaseMDLStorage(uint64_t Base,
                                              uint64_t Size) const {
  bool Found = false;
  for (const auto &[Address, State] : MDLs) {
    if (State.Storage == LockedMdl::StorageKind::PrivateArena ||
        !overlaps(Base, Size, Address, State.Size))
      continue;
    Found = true;
    if (Address < Base || State.Size > Base + Size - Address)
      return mdlError("cannot partially release MDL descriptor storage");
    if (State.isProbeLocked() || State.OwnsSystemMapping)
      return mdlError("MDL storage release requires unlock and owned mapping "
                      "release first");
    if (auto E = canReleaseMDLDependencies(Address))
      return E;
    if (auto E = canReleaseMdlUserViews(Address))
      return E;
  }
  if (Found)
    for (const auto &[IRP, Request] : Requests)
      if (!Request.Completed) {
        auto Chain = requestMDLChain(IRP);
        if (!Chain)
          return Chain.takeError();
        break; // The chain validator visits every live request.
      }
  return llvm::Error::success();
}

void KernelModel::retireMDLStorage(uint64_t Base, uint64_t Size) {
  // All release ranges passed preflight before any record is forgotten. The
  // containing pool/stack owner marks the bytes freed; no private arena free.
  for (auto It = MDLs.begin(); It != MDLs.end();) {
    const auto &State = It->second;
    if (State.Storage != LockedMdl::StorageKind::PrivateArena &&
        overlaps(Base, Size, State.Address, State.Size))
      It = MDLs.erase(It);
    else
      ++It;
  }
}

llvm::Expected<uint64_t> KernelModel::allocateMDL(llvm::ArrayRef<uint64_t> A) {
  // Allocation initializes metadata without probing the described memory.
  // The public IRP and Next links remain authoritative after guest relinking.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-ioallocatemdl
  const bool Secondary = static_cast<uint8_t>(A[2]) != 0;
  if (static_cast<uint8_t>(A[3]))
    return mdlError("IoAllocateMdl requires ChargeQuota to be FALSE");
  if (Secondary && !A[4])
    return mdlError(
        "IoAllocateMdl secondary buffers require an associated IRP");
  uint64_t Link = 0;
  if (A[4]) {
    auto Chain = requestMDLChain(A[4]);
    if (!Chain)
      return Chain.takeError();
    const auto *Request = requestForIRP(A[4]);
    if (!Secondary && Request->Mdl)
      return mdlError("IoAllocateMdl cannot replace the original direct-I/O "
                      "request MDL");
    Link = Secondary && !Chain->empty() ? Chain->back() + MDLNextOffset
                                        : A[4] + IRPMdlOffset;
    auto Writable = Memory.canAccess(Link, profile::PointerSize, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return mdlError("IoAllocateMdl requires a writable MDL chain link");
  }
  const uint32_t Size = static_cast<uint32_t>(A[1]);
  auto Extent = mdlRecordSize(A[0], Size);
  if (!Extent)
    return Extent.takeError();
  auto Record = createMDLRecord(A[0], Size, 0);
  if (!Record)
    return Record.takeError();
  if (!*Record)
    return 0;
  LockedMdl State;
  State.Owner = LockedMdl::Ownership::Driver;
  State.Address = *Record;
  State.Size = *Extent;
  State.Buffer = A[0];
  State.OriginalAddress = A[0];
  State.BackingAddress = A[0];
  State.ByteCount = Size;
  MDLs.emplace(*Record, State);
  if (Link)
    if (auto E = Memory.writeInteger(Link, *Record, profile::PointerSize)) {
      MDLs.erase(*Record);
      FreedRanges.emplace(*Record, State.Size);
      return E;
    }
  return *Record;
}

llvm::Error KernelModel::buildNonPagedMDL(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner != LockedMdl::Ownership::Driver)
    return mdlError("MmBuildMdlForNonPagedPool requires an unbuilt, live "
                    "driver-allocated MDL");
  auto &State = It->second;
  if (!State.ByteCount || State.ByteCount > UINT64_MAX - State.OriginalAddress)
    return mdlError("MDL page building requires a nonempty, nonoverflowing "
                    "buffer");
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
  if (auto E = validateKernelObjectBorrow(State.Buffer, State.ByteCount))
    return E;
  // The existing pool VA is authoritative; allocating a separate mapping
  // would lose aliasing and permit Windows-forbidden map/unmap operations.
  // https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-mmbuildmdlfornonpagedpool
  if (auto E =
          validatePersistentDumpAccess(State.Buffer, State.ByteCount, false))
    return E;
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
  return llvm::Error::success();
}

llvm::Error KernelModel::canReleaseMDLDependencies(
    uint64_t MDL, llvm::ArrayRef<uint64_t> Retiring, bool ReleasePages) const {
  for (const auto &[Address, State] : MDLs) {
    if (Address == MDL || State.Owner != LockedMdl::Ownership::Partial ||
        std::find(Retiring.begin(), Retiring.end(), Address) != Retiring.end())
      continue;
    if ((ReleasePages && State.LockOwnerMDL == MDL) ||
        State.SystemMappingOwnerMDL == MDL)
      return mdlError("MDL release requires dependent partial MDLs to be "
                      "released or rebuilt first");
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::buildPartialMDL(uint64_t Source, uint64_t Target,
                                         uint64_t Address, uint32_t Length) {
  const auto SourceIt = MDLs.find(Source);
  const auto TargetIt = MDLs.find(Target);
  if (Source == Target || SourceIt == MDLs.end() || TargetIt == MDLs.end() ||
      SourceIt->second.Owner == LockedMdl::Ownership::Driver ||
      SourceIt->second.Owner == LockedMdl::Ownership::ReleasedPages ||
      (TargetIt->second.Owner != LockedMdl::Ownership::Driver &&
       TargetIt->second.Owner != LockedMdl::Ownership::Partial))
    return mdlError("IoBuildPartialMdl requires a built source and an unbuilt "
                    "or reusable partial target");
  const auto &Parent = SourceIt->second;
  auto &Child = TargetIt->second;
  if (Parent.Advance || Child.Advance)
    return mdlError("partial MDLs of an advanced descriptor require "
                    "unmodeled lock and PFN-tail dependencies");
  if (Child.OwnsSystemMapping)
    return mdlError("partial MDL reuse requires MmPrepareMdlForReuse to "
                    "release its system mapping");
  const uint64_t Begin = Parent.OriginalAddress;
  if (Address < Begin || Address - Begin >= Parent.ByteCount)
    return mdlError("partial MDL starts outside the source MDL");
  const uint64_t Offset = Address - Begin;
  const uint64_t Available = Parent.ByteCount - Offset;
  const uint64_t Count = Length ? Length : Available;
  if (!Count || Count > Available)
    return mdlError("partial MDL exceeds the source MDL");
  const uint64_t Pages =
      ((Address & (profile::PageSize - 1)) + Count + profile::PageSize - 1) /
      profile::PageSize;
  if (Child.Size < MDLSize + Pages * profile::PointerSize)
    return mdlError("partial MDL target has insufficient PFN capacity");
  const uint64_t Backing = Parent.BackingAddress + Offset;
  auto Owner = Physical.ownerForRange(Backing, Count);
  if (!Owner)
    return Owner.takeError();
  if (auto E = canReleaseMDLDependencies(Target))
    return E;
  if (auto E = canReleaseMdlUserViews(Target))
    return E;
  if (auto E = canRevokeVirtualRange(Target, Child.Size))
    return E;
  auto Writable = Memory.canAccess(Target, Child.Size, Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return mdlError("partial MDL target requires writable descriptor storage");

  const uint64_t SystemVA = Parent.Mapped ? Parent.Buffer + Offset : 0;
  const uint16_t Flags =
      MDLPartial |
      (Parent.Mapped ? MDLMappedToSystemVA | MDLParentMappedSystemVA : 0);
  LockedMdl Candidate = Child;
  Candidate.Owner = LockedMdl::Ownership::Partial;
  // A partial descriptor borrows a page lock, not the source descriptor's
  // lifetime. Nonpaged storage has no lock owner; descendants inherit the
  // original lock and mapping owners even when an intermediate MDL is freed.
  Candidate.LockOwnerMDL =
      Parent.Owner == LockedMdl::Ownership::Partial
          ? Parent.LockOwnerMDL
          : (Parent.Owner == LockedMdl::Ownership::NonPagedPool &&
                     !Parent.OwnsSystemMapping
                 ? 0
                 : Source);
  Candidate.SystemMappingOwnerMDL =
      !Parent.Mapped || (Parent.Owner == LockedMdl::Ownership::NonPagedPool &&
                         !Parent.OwnsSystemMapping)
          ? 0
          : (Parent.Owner == LockedMdl::Ownership::Partial
                 ? Parent.SystemMappingOwnerMDL
                 : Source);
  Candidate.OwnsSystemMapping = false;
  Candidate.OriginalAddress = Address;
  Candidate.BackingAddress = Backing;
  Candidate.Buffer = SystemVA ? SystemVA : Backing;
  Candidate.ByteCount = Count;
  Candidate.AllocationSize = 0;
  Candidate.UserAddress = Address;
  Candidate.Pool = *Owner;
  Candidate.Pin = Parent.Pin;
  Candidate.Writable = Parent.Writable;
  Candidate.DmaWritable = Parent.DmaWritable;
  Candidate.Mapped = Parent.Mapped;
  if (auto E = initializeMDLPhysicalPages(Candidate))
    return E;
  // Keep Next and the allocation's PFN capacity while publishing the new view.
  for (const auto &[Field, Value, Width] :
       std::array<std::tuple<uint64_t, uint64_t, unsigned>, 5>{
           {{MDLStartVAOffset, pageBase(Address), profile::PointerSize},
            {MDLByteCountOffset, Count, 4},
            {MDLByteOffsetOffset, Address & (profile::PageSize - 1), 4},
            {MDLMappedSystemVAOffset, SystemVA, profile::PointerSize},
            {MDLFlagsOffset, Flags, 2}}})
    if (auto E = Memory.writeInteger(Target + Field, Value, Width))
      return E;
  Child = Candidate;
  return llvm::Error::success();
}

llvm::Error KernelModel::probeAndLockPages(uint64_t MDL, uint32_t Mode,
                                           uint32_t Operation) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || It->second.Owner != LockedMdl::Ownership::Driver)
    return mdlError("MmProbeAndLockPages requires an unbuilt driver MDL");
  if ((Mode != UserMode && Mode != KernelMode) ||
      (Operation != IoReadAccess && Operation != IoWriteAccess &&
       Operation != IoModifyAccess))
    return mdlError(
        "MmProbeAndLockPages requires a supported mode and lock operation");
  auto &State = It->second;
  if (!State.ByteCount || State.ByteCount > UINT64_MAX - State.OriginalAddress)
    return mdlError("MDL page locking requires a nonempty, nonoverflowing "
                    "buffer");
  const auto ImageOwner =
      imageOwnerForRange(State.OriginalAddress, State.ByteCount);
  const bool ImageRange = ImageOwner.has_value();
  // Image aliases expose whole backing pages, including bytes outside the
  // MDL's ByteCount. Retained opaque records exclude every intersecting page.
  if (auto E =
          validateBugCheckPageBorrow(State.OriginalAddress, State.ByteCount))
    return E;
  if (auto E =
          validateKernelObjectBorrow(State.OriginalAddress, State.ByteCount))
    return E;
  const bool UserRange =
      State.OriginalAddress < profile::UserProbeLimit && !ImageRange;
  auto Range = [&]() -> llvm::Expected<UserMemoryRange> {
    if (Mode == UserMode || UserRange) {
      if (CurrentIRQL > APCLevel ||
          !canCatchUserAccess(State.OriginalAddress, State.ByteCount))
        return mdlError("user page locking requires the requesting process at "
                        "IRQL <= APC_LEVEL");
      return resolveUserMemoryRange(State.OriginalAddress, State.ByteCount,
                                    Operation != IoReadAccess);
    }
    uint64_t Start = 0;
    bool Pageable = true;
    if (ImageRange) {
      Start = *ImageOwner;
    } else {
      auto Pool = Allocations.upper_bound(State.OriginalAddress);
      if (Pool == Allocations.begin())
        return mdlError("kernel page locking requires a live pool allocation "
                        "or loader-owned image range");
      --Pool;
      const uint64_t Offset = State.OriginalAddress - Pool->first;
      if (Offset >= Pool->second.Size ||
          State.ByteCount > Pool->second.Size - Offset)
        return mdlError(
            "kernel page locking requires its complete range inside "
            "one live pool allocation or loader-owned image span");
      Start = Pool->first;
      Pageable = !Pool->second.NonPaged;
    }
    if (Pageable && CurrentIRQL > APCLevel)
      return mdlError(
          "pageable kernel page locking requires IRQL <= APC_LEVEL");
    // Driver image pages are private to this loaded image. Their write-lock
    // contract belongs to the MDL alias, independently of the original image
    // VA's read-only or executable protection. Other owners still require the
    // requested permission on their original view.
    const unsigned Permissions =
        Read | (!ImageRange && Operation != IoReadAccess ? Write : 0);
    if (auto E = validatePersistentDumpAccess(State.OriginalAddress,
                                              State.ByteCount, false))
      return E;
    auto Accessible =
        Memory.canAccess(State.OriginalAddress, State.ByteCount, Permissions);
    if (!Accessible)
      return Accessible.takeError();
    if (!*Accessible)
      return llvm::make_error<KernelGuestException>(
          exceptions::StatusAccessViolation);
    if (ImageRange && !Physical.find(Start))
      if (auto E = Physical.registerRegion(Start, Start, ImageRAM.at(Start)))
        return E;
    auto Owner = Physical.ownerForRange(State.OriginalAddress, State.ByteCount);
    if (!Owner)
      return Owner.takeError();
    return UserMemoryRange{*Owner, State.OriginalAddress - Start,
                           State.OriginalAddress};
  }();
  if (!Range)
    return Range.takeError();
  if (auto E = Physical.canPin(Range->Owner, Range->Offset, State.ByteCount))
    return E;
  LockedMdl Candidate = State;
  Candidate.BackingAddress = Range->Backing;
  if (auto E = initializeMDLPhysicalPages(Candidate))
    return E;
  auto Pin = Physical.pin(Range->Owner, Range->Offset, State.ByteCount);
  if (!Pin)
    return Pin.takeError();
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, MDLPagesLocked, 2)) {
    llvm::consumeError(Physical.unpin(*Pin));
    return E;
  }
  State.Owner = Mode == KernelMode && !UserRange
                    ? LockedMdl::Ownership::KernelLocked
                    : LockedMdl::Ownership::UserLocked;
  State.Pool = Range->Owner;
  State.Pin = *Pin;
  State.UserAddress = State.OriginalAddress;
  State.BackingAddress = Range->Backing;
  State.Writable = Operation != IoReadAccess;
  State.DmaWritable = State.Writable;
  return llvm::Error::success();
}

llvm::Expected<uint64_t> KernelModel::advanceMDL(uint64_t MDL, uint32_t Bytes) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("MmAdvanceMdl requires a live modeled MDL");
  auto &State = It->second;
  // The fixed x64 routine rejects reaching the end, including zero/zero,
  // before writing or examining the page flags. ULONG ignores carrier bits.
  auto Readable = Memory.canAccess(MDL + MDLByteCountOffset, 4, Read);
  if (!Readable)
    return Readable.takeError();
  if (!*Readable)
    return mdlError("MDL advance requires a readable byte-count field");
  auto CountField = Memory.readInteger(MDL + MDLByteCountOffset, 4);
  if (!CountField)
    return CountField.takeError();
  if (*CountField != State.ByteCount)
    return mdlError("MDL byte count no longer matches its owned range");
  if (Bytes >= State.ByteCount)
    return StatusInvalidParameter2;
  if (!State.isProbeLocked() || !State.Mapped || !State.OwnsSystemMapping ||
      State.OwnerIRP ||
      imageOwnerForRange(State.BackingAddress, State.ByteCount))
    return mdlError("MDL advance requires a standalone probe-locked pool or "
                    "user buffer with its own live system mapping");
  auto Next = Memory.readInteger(MDL + MDLNextOffset, profile::PointerSize);
  if (!Next)
    return Next.takeError();
  if (*Next)
    return mdlError("MDL advance requires an unchained descriptor");
  for (const auto &[IRP, Request] : Requests) {
    if (Request.Completed)
      continue;
    auto Chain = requestMDLChain(IRP);
    if (!Chain)
      return Chain.takeError();
    if (std::find(Chain->begin(), Chain->end(), MDL) != Chain->end())
      return mdlError("MDL advance cannot change an IRP-owned chain");
  }
  if (auto E = canReleaseMDLDependencies(MDL))
    return E;
  if (auto E = canReleaseMdlUserViews(MDL))
    return E;
  if (auto E = canRevokeVirtualRange(MDL, State.Size))
    return E;
  if (auto E = canRevokeVirtualRange(State.systemMappingBase(),
                                     State.AllocationSize))
    return E;
  auto Writable = Memory.canAccess(MDL, State.Size, Read | Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return mdlError(
        "MDL advance requires readable and writable descriptor storage");

  const auto Origin = State.Advance.value_or(
      LockedMdl::AdvanceRange{State.OriginalAddress, State.BackingAddress,
                              pageBase(State.Buffer), State.ByteCount, 0});
  const uint64_t Offset = State.OriginalAddress & (profile::PageSize - 1);
  const uint64_t Crossed = (Offset + Bytes) / profile::PageSize;
  const uint64_t Pages = ((Origin.OriginalAddress & (profile::PageSize - 1)) +
                          Origin.ByteCount + profile::PageSize - 1) /
                         profile::PageSize;
  constexpr uint64_t Tail = uint64_t(1) << 63;
  std::vector<uint64_t> PFNs;
  if (Crossed) {
    for (uint64_t I = 0; I < Pages; ++I) {
      auto PFN = Memory.readInteger(MDL + MDLSize + I * profile::PointerSize,
                                    profile::PointerSize);
      if (!PFN)
        return PFN.takeError();
      if (bool(*PFN & Tail) != (Origin.DroppedPages && I + 1 == Pages))
        return mdlError("advanced MDL lost its retained PFN-tail identity");
      PFNs.push_back(*PFN & ~Tail);
    }
    std::rotate(PFNs.begin(), PFNs.begin() + Crossed, PFNs.end());
    PFNs.back() |= Tail;
  }
  const uint64_t Address = State.OriginalAddress + Bytes;
  const uint64_t Buffer = State.Buffer + Bytes;
  const uint32_t Count = State.ByteCount - Bytes;
  // Descriptor preflight makes the bounded field/PFN update indivisible at
  // this call boundary. Original fixed-version code rotates the complete PFN
  // capacity and retains its lock until the cleanup consumer restores it.
  for (const auto &[Field, Value, Width] :
       std::array<std::tuple<uint64_t, uint64_t, unsigned>, 4>{
           {{MDLByteCountOffset, Count, 4},
            {MDLByteOffsetOffset, Address & (profile::PageSize - 1), 4},
            {MDLStartVAOffset, pageBase(Address), profile::PointerSize},
            {MDLMappedSystemVAOffset, Buffer, profile::PointerSize}}})
    if (auto E = Memory.writeInteger(MDL + Field, Value, Width))
      return E;
  for (uint64_t I = 0; I < PFNs.size(); ++I)
    if (auto E = Memory.writeInteger(MDL + MDLSize + I * profile::PointerSize,
                                     PFNs[I], profile::PointerSize))
      return E;
  if (Crossed)
    if (auto E = Memory.writeInteger(
            MDL + MDLFlagsOffset,
            MDLPagesLocked | MDLMappedToSystemVA | MDLFreeExtraPtes, 2))
      return E;
  State.Advance = Origin;
  State.Advance->DroppedPages += Crossed;
  State.OriginalAddress = Address;
  State.BackingAddress += Bytes;
  State.UserAddress += Bytes;
  State.Buffer = Buffer;
  State.ByteCount = Count;
  return StatusSuccess;
}

llvm::Error KernelModel::restoreAdvancedMDL(LockedMdl &State) {
  if (!State.Advance || !State.Advance->DroppedPages)
    return llvm::Error::success();
  auto &Origin = *State.Advance;
  const uint64_t Pages = ((Origin.OriginalAddress & (profile::PageSize - 1)) +
                          Origin.ByteCount + profile::PageSize - 1) /
                         profile::PageSize;
  constexpr uint64_t Tail = uint64_t(1) << 63;
  std::vector<uint64_t> PFNs;
  for (uint64_t I = 0; I < Pages; ++I) {
    auto PFN =
        Memory.readInteger(State.Address + MDLSize + I * profile::PointerSize,
                           profile::PointerSize);
    if (!PFN)
      return PFN.takeError();
    if (bool(*PFN & Tail) != (I + 1 == Pages))
      return mdlError("advanced MDL lost its retained PFN-tail identity");
    PFNs.push_back(*PFN & ~Tail);
  }
  std::rotate(PFNs.begin(), PFNs.end() - Origin.DroppedPages, PFNs.end());
  const uint64_t Restored = uint64_t(Origin.DroppedPages) * profile::PageSize;
  for (uint64_t I = 0; I < PFNs.size(); ++I)
    if (auto E = Memory.writeInteger(State.Address + MDLSize +
                                         I * profile::PointerSize,
                                     PFNs[I], profile::PointerSize))
      return E;
  // The original cleanup restores whole pages, preserving the current byte
  // offset. This can differ from the original byte range; it grants no larger
  // pin. Mapping a restored descriptor again requires a new probe/lock cycle.
  for (const auto &[Field, Value, Width] :
       std::array<std::tuple<uint64_t, uint64_t, unsigned>, 4>{
           {{MDLByteCountOffset, State.ByteCount + Restored, 4},
            {MDLStartVAOffset, pageBase(State.OriginalAddress) - Restored,
             profile::PointerSize},
            {MDLMappedSystemVAOffset, State.Buffer - Restored,
             profile::PointerSize},
            {MDLFlagsOffset, MDLPagesLocked | MDLMappedToSystemVA, 2}}})
    if (auto E = Memory.writeInteger(State.Address + Field, Value, Width))
      return E;
  State.OriginalAddress -= Restored;
  State.BackingAddress -= Restored;
  State.UserAddress -= Restored;
  State.Buffer -= Restored;
  State.ByteCount += Restored;
  Origin.DroppedPages = 0;
  return llvm::Error::success();
}

llvm::Error KernelModel::unlockPages(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() || !It->second.isProbeLocked())
    return mdlError(
        "MmUnlockPages requires a locked user MDL or kernel pool MDL");
  auto &State = It->second;
  if (auto E = canReleaseMDLDependencies(MDL))
    return E;
  if (auto E = canReleaseMdlUserViews(MDL))
    return E;
  if (auto E = DMA.canReleaseRange(MDL, State.Size))
    return E;
  if (auto E = Physical.canUnpin(State.Pin))
    return E;
  auto Writable =
      Memory.canAccess(MDL, State.Size, State.Advance ? Read | Write : Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return mdlError(State.Advance
                        ? "advanced unlock requires readable and writable MDL "
                          "descriptor storage"
                        : "unlock requires writable MDL descriptor storage");
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
  State.Advance.reset();
  State.Writable = false;
  State.DmaWritable = false;
  return llvm::Error::success();
}

llvm::Error KernelModel::freeMDL(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("IoFreeMdl received an unknown or already freed MDL");
  if (It->second.MappingReservation)
    return mdlError("MDL release requires MmUnmapReservedMapping first");
  if (It->second.Owner == LockedMdl::Ownership::Request)
    return mdlError("IoFreeMdl cannot release a request-owned locked MDL");
  if (It->second.Owner == LockedMdl::Ownership::RequestSystemBuffer)
    return mdlError(
        "IoFreeMdl cannot release a request-owned system-buffer MDL");
  if (It->second.isProbeLocked())
    return mdlError("IoFreeMdl requires MmUnlockPages for a probed MDL");
  if (It->second.Owner == LockedMdl::Ownership::AllocatedPages ||
      It->second.Owner == LockedMdl::Ownership::ReleasedPages)
    return mdlError("independent physical MDLs require MmFreePagesFromMdl "
                    "followed by ExFreePool");
  if (It->second.Owner == LockedMdl::Ownership::NonPagedPool &&
      It->second.OwnsSystemMapping)
    return mdlError(
        "IoFreeMdl requires unmapping the owned nonpaged pool alias");
  if (auto E = canReleaseMDLDependencies(MDL))
    return E;
  if (auto E = canReleaseMdlUserViews(MDL))
    return E;
  if (It->second.Storage != LockedMdl::StorageKind::PrivateArena) {
    if (It->second.Storage != LockedMdl::StorageKind::NonPagedPool ||
        It->second.StorageOwner != MDL)
      return mdlError("IoFreeMdl requires a pool allocation base; interior and "
                      "stack descriptor storage belongs to its caller");
    // The shared storage preflight refuses a live owned partial mapping until
    // MmPrepareMdlForReuse. It never discards its alias while freeing bytes.
    auto Freed = freePool({MDL}, false);
    if (!Freed)
      return Freed.takeError();
    return llvm::Error::success();
  }
  if (It->second.Owner == LockedMdl::Ownership::Partial &&
      It->second.OwnsSystemMapping) {
    const auto &State = It->second;
    // The descriptor is released, but its alias only loses a virtual mapping.
    // The root MDL's retained bytes and pin survive this partial descriptor.
    if (auto E = canReleaseRange(State.Address, State.Size))
      return E;
    if (auto E =
            canRevokeVirtualRange(pageBase(State.Buffer), State.AllocationSize))
      return E;
    if (auto E = prepareReleaseRange(State.Address, State.Size))
      return E;
    if (auto E = prepareRevokeVirtualRange(pageBase(State.Buffer),
                                           State.AllocationSize))
      return E;
    if (auto E =
            Memory.unmapAlias(pageBase(State.Buffer), State.AllocationSize))
      return E;
    FreedRanges.emplace(State.Address, State.Size);
    MDLs.erase(It);
    return llvm::Error::success();
  }
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
  State.OriginalAddress = UserAddress;
  State.BackingAddress = Buffer;
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
  State.OriginalAddress = Request->SystemBuffer;
  State.BackingAddress = Request->SystemBuffer;
  State.ByteCount = Length;
  State.Writable = true;
  State.DmaWritable = true;
  State.Mapped = true;
  if (auto E = initializeMDLPhysicalPages(State))
    return std::move(E);
  MDLs.emplace(*Record, State);
  Request->SystemMdl = *Record;
  return *Record;
}

llvm::Expected<KernelFramework::LockedUserBuffer>
KernelModel::frameworkProbeAndLockUserBuffer(uint64_t IRP, uint64_t Buffer,
                                             uint64_t Length, bool ForWrite) {
  using Result = KernelFramework::LockedUserBuffer;
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return mdlError("framework user lock requires a live owning IRP");
  if (!Length)
    return Result{framework::RequestInvalidUserBuffer};
  if (Length > UINT32_MAX || Length > KernelMemoryBytes)
    return Result{windows::StatusInvalidParameter};
  if (!canCatchUserAccess(Buffer, Length) ||
      CurrentUserProcessID != Request->ProcessID)
    return Result{framework::RequestAccessViolation};
  const bool OwnedByRequest =
      std::any_of(Request->UserRegions.begin(), Request->UserRegions.end(),
                  [&](const auto &Region) {
                    return Buffer >= Region.Address &&
                           Buffer - Region.Address < Region.Size &&
                           Length <= Region.Size - (Buffer - Region.Address);
                  });
  if (!OwnedByRequest)
    return Result{framework::RequestAccessViolation};
  auto Region = UserAllocations.upper_bound(Buffer);
  if (Region == UserAllocations.begin())
    return Result{framework::RequestAccessViolation};
  --Region;
  const uint64_t Offset = Buffer - Region->first;
  if (Region->second.ProcessID != Request->ProcessID ||
      RevokedUserAllocations.contains(Region->first) ||
      Offset >= Region->second.Size || Length > Region->second.Size - Offset)
    return Result{framework::RequestAccessViolation};
  auto Accessible =
      Memory.canAccess(Buffer, Length, ForWrite ? Read | Write : Read);
  if (!Accessible)
    return Accessible.takeError();
  if (!*Accessible)
    return Result{framework::RequestAccessViolation};
  const std::array<uint64_t, 5> Arguments{Buffer, Length, 0, 0, 0};
  auto MDL = allocateMDL(Arguments);
  if (!MDL)
    return MDL.takeError();
  if (!*MDL)
    return Result{windows::StatusInsufficientResources};
  if (auto E = probeAndLockPages(*MDL, UserMode,
                                 ForWrite ? IoWriteAccess : IoReadAccess))
    return llvm::joinErrors(std::move(E), freeMDL(*MDL));
  MDLs.at(*MDL).OwnerIRP = IRP;
  auto Alias = mapLockedPages(*MDL, NormalPagePriority, false);
  if (!Alias)
    return llvm::joinErrors(Alias.takeError(),
                            releaseFrameworkUserBuffer(*MDL));
  if (!*Alias) {
    if (auto E = releaseFrameworkUserBuffer(*MDL))
      return E;
    return Result{windows::StatusInsufficientResources};
  }
  return Result{0, *MDL, *Alias};
}

llvm::Error KernelModel::releaseFrameworkUserBuffer(uint64_t MDL) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end() ||
      It->second.Owner != LockedMdl::Ownership::UserLocked ||
      !It->second.OwnerIRP)
    return mdlError("framework user memory lost its locked MDL");
  if (auto E = unlockPages(MDL))
    return E;
  return freeMDL(MDL);
}

llvm::Expected<uint64_t>
KernelModel::mapLockedPages(uint64_t MDL, uint32_t Priority, bool ReuseExisting,
                            KernelPhysicalMemory::CacheType RequestedCache) {
  auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("mapping requires a live modeled MDL");
  const uint32_t BasePriority =
      Priority & ~(MdlMappingNoWrite | MdlMappingNoExecute);
  if (BasePriority != LowPagePriority && BasePriority != NormalPagePriority &&
      BasePriority != HighPagePriority)
    return mdlError("unsupported MDL mapping priority or flags");
  auto &State = It->second;
  if (State.Owner == LockedMdl::Ownership::Driver ||
      State.Owner == LockedMdl::Ownership::ReleasedPages)
    return mdlError("mapping requires a built MDL; allocated metadata does "
                    "not lock or map the described buffer");
  if (State.Advance && !State.Mapped)
    return mdlError("remapping a restored advanced MDL requires a new "
                    "probe and lock cycle");
  if (State.Owner == LockedMdl::Ownership::NonPagedPool &&
      !Allocations.count(State.Pool))
    return mdlError("nonpaged pool MDL describes a freed pool allocation");
  auto PhysicalOwner =
      Physical.ownerForRange(State.BackingAddress, State.ByteCount);
  if (!PhysicalOwner)
    return PhysicalOwner.takeError();
  auto Cache = Physical.cacheTypeForMapping(State.BackingAddress,
                                            State.ByteCount, RequestedCache);
  if (!Cache)
    return Cache.takeError();
  if (!State.Mapped || (WindowsNonpagedMDLMappings &&
                        State.Owner == LockedMdl::Ownership::NonPagedPool &&
                        !ReuseExisting && !State.OwnsSystemMapping)) {
    auto Writable = Memory.canAccess(MDL, State.Size, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return mdlError("mapping requires writable MDL descriptor storage");
  }
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
    if (!ReuseExisting && !WindowsNonpagedMDLMappings)
      return mdlError("a nonpaged pool MDL cannot create an additional "
                      "system-space mapping");
    // Existing mappings retain their permissions even when safe-helper flags
    // request no-write/no-execute. No page protections are changed here.
    if (ReuseExisting)
      return State.Buffer;
    if (State.OwnsSystemMapping)
      return mdlError(
          "a nonpaged pool MDL already owns an additional system mapping");
  }
  if (State.Owner == LockedMdl::Ownership::Partial || State.isProbeLocked() ||
      State.Owner == LockedMdl::Ownership::AllocatedPages ||
      State.Owner == LockedMdl::Ownership::NonPagedPool) {
    if (State.Mapped && State.Owner != LockedMdl::Ownership::NonPagedPool) {
      if (!ReuseExisting)
        return mdlError("an MDL cannot have a second system-space mapping");
      return State.Buffer;
    }
    const uint64_t Offset = State.BackingAddress & (profile::PageSize - 1);
    const uint64_t AllocationSize =
        (Offset + State.ByteCount + profile::PageSize - 1) &
        ~(profile::PageSize - 1);
    const uint64_t MappingBase = findSystemMappingAddress(AllocationSize);
    if (!MappingBase)
      return 0;
    const uint64_t Alias = MappingBase + Offset;
    const bool Writable = State.Writable && !(Priority & MdlMappingNoWrite);
    const unsigned Permissions =
        Read | (Writable ? Write : 0) |
        ((Priority & MdlMappingNoExecute) ? 0 : Execute);
    const bool PinPool = State.Owner == LockedMdl::Ownership::NonPagedPool;
    const uint64_t PoolOffset =
        State.BackingAddress - Physical.find(*PhysicalOwner)->Backing;
    if (PinPool)
      if (auto E = Physical.canPin(*PhysicalOwner, PoolOffset, State.ByteCount))
        return E;
    if (auto E = Memory.mapAlias(MappingBase, pageBase(State.BackingAddress),
                                 AllocationSize, Permissions)) {
      if (E.isA<GuestMemoryLimitError>()) {
        llvm::consumeError(std::move(E));
        return 0;
      }
      return E;
    }
    if (auto E = Physical.commitMappingCache(State.BackingAddress,
                                             State.ByteCount, *Cache))
      return E;
    if (PinPool) {
      auto Pin = Physical.pin(*PhysicalOwner, PoolOffset, State.ByteCount);
      if (!Pin)
        return Pin.takeError();
      State.Pin = *Pin;
    }
    const uint16_t Flags =
        MDLMappedToSystemVA |
        (State.Owner == LockedMdl::Ownership::Partial
             ? MDLPartial | MDLPartialHasBeenMapped
             : (PinPool ? MDLSourceIsNonPagedPool : MDLPagesLocked));
    if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset, Alias,
                                     profile::PointerSize))
      return E;
    if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, Flags, 2))
      return E;
    State.Buffer = Alias;
    State.AllocationSize = AllocationSize;
    State.Mapped = true;
    State.OwnsSystemMapping = true;
    State.SystemMappingOwnerMDL = MDL;
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
  const bool Writable = State.Writable && !(Priority & MdlMappingNoWrite);
  const unsigned Permissions = Read | (Writable ? Write : 0) |
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

llvm::Expected<uint64_t>
KernelModel::protectMDLSystemAddress(uint64_t MDL, uint32_t Protection) {
  unsigned Permissions;
  switch (Protection) {
  case PageNoAccess:
    Permissions = 0;
    break;
  case PageReadOnly:
    Permissions = Read;
    break;
  case PageReadWrite:
    Permissions = Read | Write;
    break;
  case PageExecute:
    Permissions = Execute;
    break;
  case PageExecuteRead:
    Permissions = Execute | Read;
    break;
  case PageExecuteReadWrite:
    Permissions = Execute | Read | Write;
    break;
  default:
    return StatusInvalidPageProtection;
  }
  const auto It = MDLs.find(MDL);
  if (It == MDLs.end())
    return mdlError("MDL protection requires a live descriptor");
  const auto &State = It->second;
  if (!State.Mapped)
    return StatusNotMappedView;
  const LockedMdl *Mapping = &State;
  if (State.Owner == LockedMdl::Ownership::Partial &&
      !State.OwnsSystemMapping) {
    const auto Owner = MDLs.find(State.SystemMappingOwnerMDL);
    if (Owner == MDLs.end())
      return mdlError(
          "MDL protection requires an independently mapped system view");
    Mapping = &Owner->second;
  }
  if (!Mapping->OwnsSystemMapping &&
      Mapping->Owner != LockedMdl::Ownership::Request)
    return mdlError(
        "MDL protection cannot change shared pool or system-buffer pages");
  if ((Permissions & Write) && !State.Writable)
    return mdlError(
        "MDL protection cannot exceed the locked write-access contract");
  auto Owner = Physical.ownerForRange(State.BackingAddress, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  const uint64_t Base = pageBase(State.Buffer);
  const uint64_t Size =
      (State.Buffer - Base + State.ByteCount + profile::PageSize - 1) &
      ~(profile::PageSize - 1);
  if (auto E = canRevokeVirtualRange(Base, Size))
    return E;
  if (auto E = Memory.protect(Base, Size, Permissions))
    return E;
  return StatusSuccess;
}

llvm::Error KernelModel::unmapLockedPages(uint64_t Address, uint64_t MDL,
                                          bool Reserved) {
  auto It = MDLs.find(MDL);
  if (It != MDLs.end() && It->second.MappingReservation && !Reserved)
    return mdlError(
        "reserved system mappings require MmUnmapReservedMapping first");
  if (It != MDLs.end() &&
      It->second.Owner == LockedMdl::Ownership::RequestSystemBuffer)
    return mdlError("a request system-buffer MDL cannot release its existing "
                    "system-space mapping");
  if (It != MDLs.end() &&
      It->second.Owner == LockedMdl::Ownership::NonPagedPool &&
      !It->second.OwnsSystemMapping)
    return mdlError("a nonpaged pool MDL cannot release its existing "
                    "system-space mapping");
  if (It == MDLs.end() ||
      (It->second.Owner != LockedMdl::Ownership::Request &&
       !It->second.isProbeLocked() &&
       It->second.Owner != LockedMdl::Ownership::AllocatedPages &&
       It->second.Owner != LockedMdl::Ownership::Partial &&
       It->second.Owner != LockedMdl::Ownership::NonPagedPool))
    return mdlError("unmapping requires the active request's locked MDL");
  auto &State = It->second;
  if (State.Owner == LockedMdl::Ownership::Partial && !State.OwnsSystemMapping)
    return mdlError("partial MDL borrows its parent's system mapping");
  if (State.Owner == LockedMdl::Ownership::Request) {
    const auto *Owner = requestForIRP(State.OwnerIRP);
    if (!Owner || Owner->Completed || Owner->Mdl != MDL)
      return mdlError("unmapping requires the active request's locked MDL");
  }
  if (!State.Mapped || State.Buffer != Address)
    return mdlError("MDL unmapping requires its live system mapping address");
  const bool PoolAlias = State.Owner == LockedMdl::Ownership::NonPagedPool;
  if (PoolAlias)
    if (auto E = Physical.canUnpin(State.Pin))
      return E;
  if (auto E = canReleaseMDLDependencies(MDL, {}, false))
    return E;
  auto Writable =
      Memory.canAccess(MDL, State.Size, State.Advance ? Read | Write : Write);
  if (!Writable)
    return Writable.takeError();
  if (!*Writable)
    return mdlError(State.Advance
                        ? "advanced unmapping requires readable and writable "
                          "MDL descriptor storage"
                        : "unmapping requires writable MDL descriptor storage");
  const uint64_t MappingBase = State.systemMappingBase();
  if (auto E = canRevokeVirtualRange(MappingBase, State.AllocationSize))
    return E;
  if (auto E = Memory.validateBacking(MappingBase, State.AllocationSize))
    return E;
  if (auto E = restoreAdvancedMDL(State))
    return E;
  if (auto E = prepareRevokeVirtualRange(MappingBase, State.AllocationSize))
    return E;
  if (State.OwnsSystemMapping) {
    if (auto E = Memory.unmapAlias(MappingBase, State.AllocationSize))
      return E;
  } else if (auto E = Memory.protect(MappingBase, State.AllocationSize, 0)) {
    return E;
  }
  if (!Reserved && !State.Advance)
    if (auto E = Memory.writeInteger(MDL + MDLMappedSystemVAOffset,
                                     PoolAlias ? State.BackingAddress : 0,
                                     profile::PointerSize))
      return E;
  const uint16_t Flags =
      State.Owner == LockedMdl::Ownership::Partial
          ? MDLPartial
          : (PoolAlias ? MDLSourceIsNonPagedPool : MDLPagesLocked);
  if (auto E = Memory.writeInteger(MDL + MDLFlagsOffset, Flags, 2))
    return E;
  State.Mapped = false;
  State.OwnsSystemMapping = false;
  State.SystemMappingOwnerMDL = 0;
  if (PoolAlias) {
    if (auto E = Physical.unpin(State.Pin))
      return E;
    State.Pin = 0;
    State.Buffer = State.BackingAddress;
    State.Mapped = true;
    State.AllocationSize = 0;
  }
  if (State.isProbeLocked() ||
      State.Owner == LockedMdl::Ownership::AllocatedPages) {
    State.Buffer = State.UserAddress;
    State.AllocationSize = 0;
  }
  if (State.Owner == LockedMdl::Ownership::Partial) {
    State.Buffer = State.BackingAddress;
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
  auto Owner = Physical.ownerForRange(State.BackingAddress, State.ByteCount);
  if (!Owner)
    return Owner.takeError();
  if (auto E = Memory.readBacking(State.BackingAddress, Bytes))
    return std::move(E);
  return Bytes;
}

llvm::Expected<std::vector<uint64_t>>
KernelModel::requestMDLChain(uint64_t IRP) const {
  const auto *Request = requestForIRP(IRP);
  if (!Request || Request->Completed)
    return mdlError("MDL association requires a live owning IRP");
  std::vector<uint64_t> Chain;
  llvm::DenseMap<uint64_t, uint64_t> Associations;
  for (const auto &[OwnerIRP, Owner] : Requests) {
    if (Owner.Completed)
      continue;
    auto Head =
        Memory.readInteger(OwnerIRP + IRPMdlOffset, profile::PointerSize);
    if (!Head)
      return Head.takeError();
    bool FoundDirect = !Owner.Mdl;
    for (uint64_t Address = *Head; Address;) {
      auto It = MDLs.find(Address);
      if (It == MDLs.end())
        return mdlError("MDL chain contains an unknown or freed descriptor");
      auto [Previous, Inserted] = Associations.try_emplace(Address, OwnerIRP);
      if (!Inserted)
        return mdlError(Previous->second == OwnerIRP
                            ? "MDL chain contains a cycle"
                            : "MDL descriptor is shared by multiple IRPs");
      const auto &State = It->second;
      if (State.Advance)
        return mdlError("advanced MDLs cannot transfer ownership to an "
                        "IRP chain");
      if (State.Storage != LockedMdl::StorageKind::PrivateArena)
        return mdlError("caller-storage MDLs cannot transfer ownership to "
                        "an IRP chain");
      if (State.Owner == LockedMdl::Ownership::AllocatedPages ||
          State.Owner == LockedMdl::Ownership::ReleasedPages)
        return mdlError("independent physical-page MDLs cannot transfer "
                        "ownership to an IRP chain");
      if (State.MappingReservation)
        return mdlError(
            "an MDL with a reserved mapping cannot transfer to an IRP chain");
      if (State.Owner == LockedMdl::Ownership::RequestSystemBuffer ||
          (State.OwnerIRP &&
           (State.Owner != LockedMdl::Ownership::Request ||
            State.OwnerIRP != OwnerIRP || Address != Owner.Mdl)))
        return mdlError("MDL chain contains a descriptor owned by another "
                        "request or framework buffer");
      FoundDirect |= Address == Owner.Mdl;
      if (OwnerIRP == IRP)
        Chain.push_back(Address);
      auto Next =
          Memory.readInteger(Address + MDLNextOffset, profile::PointerSize);
      if (!Next)
        return Next.takeError();
      Address = *Next;
    }
    if (!FoundDirect)
      return mdlError("MDL chain lost the original direct-I/O request MDL");
  }
  return Chain;
}

llvm::Error KernelModel::appendRequestMDLReleaseResources(
    uint64_t IRP, std::vector<std::pair<uint64_t, uint64_t>> &Ranges,
    std::vector<uint64_t> &Pins,
    std::vector<std::pair<uint64_t, uint64_t>> &RevokingAliases) const {
  auto Chain = requestMDLChain(IRP);
  if (!Chain)
    return Chain.takeError();
  for (uint64_t Address : *Chain) {
    const auto &State = MDLs.at(Address);
    if (auto E = canReleaseMDLDependencies(Address, *Chain))
      return E;
    if (auto E = canReleaseMdlUserViews(Address))
      return E;
    auto Writable = Memory.canAccess(Address, State.Size, Write);
    if (!Writable)
      return Writable.takeError();
    if (!*Writable)
      return mdlError("completion requires writable MDL descriptor storage");
    Ranges.emplace_back(State.Address, State.Size);
    if (State.Owner == LockedMdl::Ownership::Request) {
      Ranges.emplace_back(pageBase(State.Buffer), State.AllocationSize);
    } else if (State.isProbeLocked() ||
               (State.Owner == LockedMdl::Ownership::NonPagedPool &&
                State.OwnsSystemMapping)) {
      // Unlocking retains the original user allocation but revokes its system
      // alias. Validate every pin and alias before retiring any descriptor.
      if (auto E = Physical.canUnpin(State.Pin))
        return E;
      Pins.push_back(State.Pin);
      if (State.Mapped)
        RevokingAliases.emplace_back(pageBase(State.Buffer),
                                     State.AllocationSize);
    } else if (State.Owner == LockedMdl::Ownership::Partial &&
               State.OwnsSystemMapping) {
      RevokingAliases.emplace_back(pageBase(State.Buffer),
                                   State.AllocationSize);
    }
  }
  const auto *Owner = requestForIRP(IRP);
  if (Owner->SystemMdl) {
    auto It = MDLs.find(Owner->SystemMdl);
    if (It == MDLs.end() || It->second.OwnerIRP != IRP ||
        It->second.Owner != LockedMdl::Ownership::RequestSystemBuffer)
      return mdlError("active request lost ownership of its system-buffer MDL");
    if (auto E = canReleaseMDLDependencies(Owner->SystemMdl, *Chain))
      return E;
    if (auto E = canReleaseMdlUserViews(Owner->SystemMdl))
      return E;
    Ranges.emplace_back(It->second.Address, It->second.Size);
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::expireRequestMDL(uint64_t IRP) {
  auto Chain = requestMDLChain(IRP);
  if (!Chain)
    return Chain.takeError();
  for (uint64_t Address : *Chain) {
    auto It = MDLs.find(Address);
    auto &State = It->second;
    if (State.Owner != LockedMdl::Ownership::Partial)
      continue;
    // Completion preflight checked the whole chain, including descendants of
    // this mapping. Retire every partial before unlocking any root; Next order
    // does not express the dependency order.
    if (State.OwnsSystemMapping)
      if (auto E =
              Memory.unmapAlias(pageBase(State.Buffer), State.AllocationSize))
        return E;
    FreedRanges.emplace(State.Address, State.Size);
    MDLs.erase(It);
  }
  // An independently relocked user view can share a request MDL's backing.
  // Release every such pin before retiring any backing, regardless of Next.
  for (uint64_t Address : *Chain) {
    auto It = MDLs.find(Address);
    if (It != MDLs.end() &&
        It->second.Owner == LockedMdl::Ownership::NonPagedPool &&
        It->second.OwnsSystemMapping)
      if (auto E = unmapLockedPages(It->second.Buffer, Address))
        return E;
    if (It != MDLs.end() && It->second.isProbeLocked())
      if (auto E = unlockPages(Address))
        return E;
  }
  for (uint64_t Address : *Chain) {
    auto It = MDLs.find(Address);
    if (It == MDLs.end())
      continue;
    auto &State = It->second;
    if (State.Owner == LockedMdl::Ownership::Request) {
      if (auto E =
              Memory.protect(pageBase(State.Buffer), State.AllocationSize, 0))
        return E;
      if (auto E = Physical.retire(pageBase(State.Buffer)))
        return E;
      FreedRanges.emplace(pageBase(State.Buffer), State.AllocationSize);
    }
    // Nonpaged descriptors only own their metadata, so their backing pool
    // allocation survives completion just as it survives IoFreeMdl.
    FreedRanges.emplace(State.Address, State.Size);
    MDLs.erase(It);
  }
  const auto *Owner = requestForIRP(IRP);
  if (Owner->SystemMdl) {
    const auto It = MDLs.find(Owner->SystemMdl);
    FreedRanges.emplace(It->second.Address, It->second.Size);
    MDLs.erase(It);
  }
  return llvm::Error::success();
}

llvm::Error KernelModel::validateMDLAccess(uint64_t Address, uint32_t Size,
                                           bool IsWrite) const {
  const uint64_t End = Address + Size;
  for (const auto &[MDL, State] : MDLs) {
    if (Address < MDL + State.Size && MDL < End) {
      const uint64_t First = std::max(Address, MDL) - MDL;
      const uint64_t Last = std::min(End, MDL + State.Size) - MDL;
      if (IsWrite) {
        if (First >= MDLNextOffset &&
            Last <= MDLNextOffset + profile::PointerSize)
          continue;
        return mdlError("modeled MDL fields other than Next are read-only");
      }
      if (First < MDLMappedSystemVAOffset && MDLFlagsOffset + 2 < Last)
        return mdlError("MDL process fields are not modeled");
      if (Last > MDLSize) {
        if (State.Owner == LockedMdl::Ownership::Driver)
          return mdlError("unbuilt MDL physical PFN data is unavailable");
        if (State.Owner == LockedMdl::Ownership::ReleasedPages)
          return mdlError("released MDL physical PFN data is unavailable");
        const uint64_t Original = State.Advance ? State.Advance->OriginalAddress
                                                : State.OriginalAddress;
        const uint64_t Count =
            State.Advance ? State.Advance->ByteCount : State.ByteCount;
        const uint64_t Pages = ((Original & (profile::PageSize - 1)) + Count +
                                profile::PageSize - 1) /
                               profile::PageSize;
        if (Last > MDLSize + Pages * profile::PointerSize)
          return mdlError("MDL physical PFN read exceeds its described pages");
        if (UnpackBaseline)
          UnpackMDLIdentityRead = true;
      }
    }
    // Driver MDLs describe an existing allocation; neither their byte range
    // nor their lifetime restricts otherwise valid accesses to that pool.
    if (State.Owner != LockedMdl::Ownership::Request &&
        !State.isProbeLocked() &&
        State.Owner != LockedMdl::Ownership::AllocatedPages &&
        State.Owner != LockedMdl::Ownership::Partial)
      continue;
    if (((State.isProbeLocked() ||
          State.Owner == LockedMdl::Ownership::AllocatedPages) &&
         !State.Mapped) ||
        (State.Owner == LockedMdl::Ownership::Partial &&
         !State.OwnsSystemMapping))
      continue;
    const uint64_t Base = State.systemMappingBase();
    if (Address >= Base + State.AllocationSize || End <= Base)
      continue;
    if (!State.Mapped)
      return mdlError("guest access to an unmapped MDL system buffer");
    // Image ownership admits known bytes before ByteOffset and after
    // ByteCount. Intersect the physical pages with the actual owned span;
    // an incomplete page must not turn unrelated padding into image data.
    // Object-backed owners retain their bounded logical-buffer contract.
    const auto Image = ImageRAM.find(State.Pool);
    uint64_t Begin = State.Buffer;
    uint64_t Limit = State.Buffer + State.ByteCount;
    if (Image != ImageRAM.end()) {
      const auto Offset = State.OriginalAddress - Image->first;
      const auto PageOffset = State.OriginalAddress & (profile::PageSize - 1);
      Begin -= std::min(Offset, PageOffset);
      Limit = State.Buffer + std::min(Image->second - Offset,
                                      State.AllocationSize - PageOffset);
    }
    if (Address < Begin || End > Limit)
      return mdlError(Image != ImageRAM.end()
                          ? "guest access exceeds the MDL image backing range"
                          : "guest access exceeds the MDL byte range");
    if (IsWrite) {
      auto Writable = Memory.canAccess(Address, Size, Write);
      if (!Writable)
        return Writable.takeError();
      if (!*Writable)
        return mdlError("guest write to an MDL mapping with MdlMappingNoWrite "
                        "or read-only protection");
    }
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation
