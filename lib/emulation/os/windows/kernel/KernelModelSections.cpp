//===- KernelModelSections.cpp - Guest physical section handles and views -===//
// NeverD Decompiler
///
/// \file
/// Guest physical section handles and views.
///

#include "../WindowsUnicode.h"
#include "KernelModel.h"
#include "KernelSectionNames.h"
#include "WindowsKernelLayout.h"

#include "neverd/emulation/AddressSpace.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>
namespace neverd::emulation {
namespace {
using namespace windows;
constexpr uint64_t HandleLimit = 0x520ffffc;
constexpr uint32_t NoMemory = 0xc0000017, Conflict = 0xc0000018,
                   InvalidViewSize = 0xc000001f, AccessDenied = 0xc0000022,
                   InvalidProtection = 0xc0000045, NotMapped = 0xc0000019;
constexpr size_t MaxHandles = 256, MaxViews = 256;
constexpr uint64_t Granularity = 0x10000, PageSize = profile::PageSize;
using section::PhysicalName;
llvm::Error sectionError(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "section: " + Text);
}
bool overlap(uint64_t A, uint64_t Size, uint64_t B, uint64_t Count) {
  return Size && Count && A < B + Count && B < A + Size;
}
} // namespace
llvm::Error KernelModel::initializeSections(const DriverOptions &Options) {
  if (auto E = validateDriverSectionNamespace(Options.SectionNamespace))
    return E;
  SectionNamespace = Options.SectionNamespace;
  if (!SectionNamespace)
    return llvm::Error::success();
  for (const auto &Name : SectionNamespace->Directories) {
    const bool Known = std::any_of(
        ReferencedObjects.begin(), ReferencedObjects.end(), [&](const auto &O) {
          return llvm::StringRef(O.second.Name).equals_insensitive(Name);
        });
    if (Known)
      continue;
    auto Object = allocate(profile::ProcessTokenSize);
    if (!Object)
      return Object.takeError();
    ReferencedObjects.emplace(
        *Object, ObjectReferences{ObjectKind::Directory, 0, 0, Name});
  }
  for (const auto &[Name, Target] : SectionNamespace->Aliases) {
    const auto Key = llvm::StringRef(Name).upper();
    if (findSymbolicLink(Key))
      return sectionError("directory alias collides with an existing link");
    auto Encoded = windows_unicode::bytes(Target, false);
    if (!Encoded)
      return Encoded.takeError();
    auto Object = allocate(profile::ProcessTokenSize);
    if (!Object)
      return Object.takeError();
    ReferencedObjects.emplace(
        *Object, ObjectReferences{ObjectKind::SymbolicLink, 0, 0, Name});
    SymbolicLinks.emplace(*Object,
                          SymbolicLinkObject{Target, std::move(*Encoded)});
  }
  for (const auto &S : SectionNamespace->Sections) {
    const uint64_t Size = (S.Size + PageSize - 1) & ~(PageSize - 1);
    auto Backing = allocatePhysicalBuffer(Size, PageSize, 0, Size);
    if (!Backing)
      return Backing.takeError();
    if (!S.Bytes.empty())
      if (auto E = Memory.write(*Backing, S.Bytes))
        return E;
    NamedSections.emplace(llvm::StringRef(S.Name).lower(),
                          NamedSection{S.Name, *Backing, Size, S.Protection});
  }
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::openSection(llvm::ArrayRef<uint64_t> A) {
  if (auto E = objectBuffer(A[2], ObjectAttributesSize, false))
    return E;
  auto Size = Memory.readInteger(A[2], 4),
       Root = Memory.readInteger(A[2] + ObjectRootOffset, 8),
       Name = Memory.readInteger(A[2] + ObjectNameOffset, 8),
       Flags = Memory.readInteger(A[2] + ObjectFlagsOffset, 4),
       Security = Memory.readInteger(A[2] + ObjectSecurityOffset, 8),
       QoS = Memory.readInteger(A[2] + ObjectQualityOffset, 8);
  if (!Size)
    return Size.takeError();
  if (!Root)
    return Root.takeError();
  if (!Name)
    return Name.takeError();
  if (!Flags)
    return Flags.takeError();
  if (!Security)
    return Security.takeError();
  if (!QoS)
    return QoS.takeError();
  if (auto E = objectBuffer(A[0], 8, true))
    return E;
  auto Complete = [&](uint32_t Status,
                      uint64_t H = 0) -> llvm::Expected<uint64_t> {
    // NtOpenSection copies its zero-initialized local handle on failure too.
    if (auto E = Memory.writeInteger(A[0], H, 8))
      return E;
    return Status;
  };
  if (*Size != ObjectAttributesSize)
    return Complete(StatusInvalidParameter);
  if (*Root || *Security || *QoS || (*Flags & ~uint64_t(0x240)))
    return sectionError("directory handles, security descriptors and these "
                        "attributes are unmodeled");
  const uint32_t PID = handleProcessID();
  if (PID != SystemProcessID && !(*Flags & ObjectKernelHandle))
    return sectionError("opening outside System requires OBJ_KERNEL_HANDLE");
  auto Text = readObjectName(*Name);
  if (!Text)
    return Text.takeError();
  auto Expanded = resolveObjectPath(*Text, bool(*Flags & 0x40));
  if (!Expanded)
    return Expanded.takeError();
  APICallDetail = *Text;
  if (*Expanded != *Text)
    APICallDetail += " -> " + *Expanded;
  std::string SectionName;
  if (!llvm::StringRef(*Expanded).equals_insensitive(PhysicalName)) {
    auto D = resolveDeviceName(*Expanded, bool(*Flags & 0x40));
    if (!D)
      return D.takeError();
    if (*D)
      return Complete(StatusObjectTypeMismatch);
    auto It = NamedSections.find(llvm::StringRef(*Expanded).lower());
    if (It != NamedSections.end()) {
      if (!(*Flags & 0x40) && *Expanded != It->second.Name)
        return Complete(
            llvm::StringRef(*Expanded).rsplit('\\').first ==
                    llvm::StringRef(It->second.Name).rsplit('\\').first
                ? StatusObjectNameNotFound
                : uint32_t(0xc000003a));
      SectionName = It->first;
    } else {
      if (SectionNamespace) {
        for (const auto &D : SectionNamespace->Directories) {
          const auto Path = llvm::StringRef(*Expanded);
          if (Path.equals_insensitive(D)) {
            if ((*Flags & 0x40) || Path == D)
              return Complete(StatusObjectTypeMismatch);
            return Complete(Path.rsplit('\\').first ==
                                    llvm::StringRef(D).rsplit('\\').first
                                ? StatusObjectNameNotFound
                                : uint32_t(0xc000003a));
          }
          if (Path.starts_with_insensitive(D + "\\")) {
            const auto Parent = Path.rsplit('\\').first;
            bool KnownParent = false;
            for (const auto &P : SectionNamespace->Directories)
              KnownParent |=
                  (*Flags & 0x40) ? Parent.equals_insensitive(P) : Parent == P;
            for (const auto &[Key, S] : NamedSections)
              if ((*Flags & 0x40) ? Parent.equals_insensitive(S.Name)
                                  : Parent == S.Name)
                return Complete(StatusObjectTypeMismatch);
            return Complete(KnownParent ? StatusObjectNameNotFound
                                        : uint32_t(0xc000003a));
          }
        }
      }
      return sectionError("named section inventory is unknown beyond the "
                          "PhysicalMemory provider: " +
                          *Expanded);
    }
  }
  if (SectionName.empty() && !(*Flags & 0x40) && *Expanded != PhysicalName)
    return Complete(StatusObjectNameNotFound);
  const uint32_t Access = section::accessMask(uint32_t(A[1]));
  if (Access & 0x01000000)
    return sectionError(
        "ACCESS_SYSTEM_SECURITY requires an explicit privilege policy");
  if (Access & ~uint32_t(0x1f001f))
    return Complete(AccessDenied);
  if (SectionHandles.size() >= MaxHandles || NextSectionHandle > HandleLimit)
    return Complete(StatusInsufficientResources);
  const uint64_t H = NextSectionHandle;
  auto Status = Complete(0, H);
  if (!Status)
    return Status.takeError();
  SectionHandles.emplace(H, SectionHandle{Access,
                                          uint32_t(*Flags & ObjectKernelHandle),
                                          PID, std::move(SectionName)});
  NextSectionHandle += 4;
  return *Status;
}
llvm::Error KernelModel::snapshotSections() {
  if (!SectionNamespace) {
    Result.SectionNamespace.reset();
    return llvm::Error::success();
  }
  auto Snapshot = *SectionNamespace;
  for (auto &S : Snapshot.Sections) {
    const auto It = NamedSections.find(llvm::StringRef(S.Name).lower());
    if (It == NamedSections.end())
      return sectionError("snapshot lost its provider-owned section");
    S.Size = It->second.Size;
    S.Bytes.resize(S.Size);
    if (auto E = Memory.snapshotBacking(It->second.Backing, S.Bytes))
      return E;
  }
  Result.SectionNamespace = std::move(Snapshot);
  return llvm::Error::success();
}
llvm::Error KernelModel::releaseSectionPins(SectionView &V) {
  llvm::Error Errors = llvm::Error::success();
  for (const auto &[Owner, Pin] : V.Pins)
    Errors = llvm::joinErrors(std::move(Errors), Physical.unpin(Pin));
  V.Pins.clear();
  return Errors;
}
llvm::Error KernelModel::pinSectionView(SectionView &V) {
  std::set<uint64_t> Owners;
  for (const auto &P : V.Pages)
    Owners.insert(P.Owners.begin(), P.Owners.end());
  if (Owners.size() > Physical.availablePins())
    return llvm::make_error<PhysicalMemoryLimitError>(
        "section view pin capacity exhausted");
  for (uint64_t O : Owners) {
    const auto *R = Physical.find(O);
    if (!R)
      return sectionError("physical view lost a backing allocation");
    if (auto E = Physical.canPin(O, 0, R->Size))
      return E;
  }
  for (uint64_t O : Owners) {
    auto Pin = Physical.pin(O, 0, Physical.find(O)->Size);
    if (!Pin)
      return llvm::joinErrors(Pin.takeError(), releaseSectionPins(V));
    V.Pins.emplace(O, *Pin);
  }
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::mapSection(llvm::ArrayRef<uint64_t> A) {
  auto H = SectionHandles.find(A[0]);
  if (H == SectionHandles.end()) {
    if (FileHandles.contains(A[0]) || ObjectHandles.contains(A[0]) ||
        Registry.ownsHandle(A[0]))
      return StatusObjectTypeMismatch;
    return StatusInvalidHandle;
  }
  if (!canAccessHandle(H->second.Attributes, H->second.ProcessID))
    return StatusInvalidHandle;
  if (H->second.Image)
    return mapImageSection(A);
  if (A[1] != UINT64_MAX) {
    if (FileHandles.contains(A[1]) || ObjectHandles.contains(A[1]) ||
        SectionHandles.contains(A[1]))
      return StatusObjectTypeMismatch;
    return StatusInvalidHandle;
  }
  const uint32_t PID = handleProcessID();
  if (ExitedUserProcesses.contains(PID))
    return sectionError("cannot map into an exited process");
  if (PID !=
      (UserRequestContext ? CurrentUserProcessID : uint32_t(SystemProcessID)))
    return sectionError(
        "process context does not own the active address space");
  if (A[3] >= 21)
    return StatusInvalidParameter;
  if (A[3])
    return sectionError("nonzero ZeroBits placement requires an explicit "
                        "address constraint policy");
  const uint32_t Inherit = uint32_t(A[7]), Allocation = uint32_t(A[8]),
                 Protect = uint32_t(A[9]);
  const auto *Named =
      H->second.Name.empty() ? nullptr : &NamedSections.at(H->second.Name);
  if (Named && (A[4] || (Protect & 0x700)))
    return sectionError("named SEC_COMMIT views require zero CommitSize and "
                        "ordinary cached protection");
  if (Inherit != 1 && Inherit != 2)
    return StatusInvalidParameter;
  if (Allocation & ~uint32_t(0x100000))
    return sectionError("reserve, large pages and placeholder allocation "
                        "policies are unmodeled");
  if (Protect & ~uint32_t(0x6ff))
    return InvalidProtection;
  if ((Protect & 0x600) == 0x600)
    return InvalidProtection;
  const uint32_t BaseProtect = Protect & 0xff;
  unsigned Permissions = 0;
  if (BaseProtect == 2)
    Permissions = Read;
  else if (BaseProtect == 4)
    Permissions = Read | Write;
  else if (BaseProtect == 1 || BaseProtect == 8 || BaseProtect == 0x10 ||
           BaseProtect == 0x20 || BaseProtect == 0x40 || BaseProtect == 0x80)
    return sectionError("this physical RAM view profile supports PAGE_READONLY "
                        "and PAGE_READWRITE");
  else
    return InvalidProtection;
  const uint32_t Needed = (Permissions & Write) ? 2 : 4;
  if ((H->second.Access & Needed) != Needed)
    return AccessDenied;
  if (Named && (Permissions & Write) && Named->Protection == 2)
    return uint32_t(0xc000004e); // STATUS_SECTION_PROTECTION
  if (auto E = objectBuffer(A[2], 8, true))
    return E;
  if (auto E = objectBuffer(A[6], 8, true))
    return E;
  if (A[5])
    if (auto E = objectBuffer(A[5], 8, true))
      return E;
  auto Base = Memory.readInteger(A[2], 8), Size = Memory.readInteger(A[6], 8);
  if (!Base)
    return Base.takeError();
  if (!Size)
    return Size.takeError();
  uint64_t Offset = 0;
  if (A[5]) {
    auto O = Memory.readInteger(A[5], 8);
    if (!O)
      return O.takeError();
    Offset = *O;
  }
  if (Offset > INT64_MAX)
    return StatusInvalidParameter;
  if (Named && Offset % Granularity)
    return uint32_t(0xc0000220); // STATUS_MAPPED_ALIGNMENT
  if (!Named && Offset % PageSize)
    return sectionError(
        "unaligned physical SectionOffset rounding is not modeled");
  const uint64_t Limit =
      Named ? Named->Size
            : physical::PhysicalBase + Physical.physicalRangeSize();
  if (Offset >= Limit)
    return InvalidViewSize;
  if (!*Size)
    *Size = Limit - Offset;
  if (*Size > UINT64_MAX - (PageSize - 1))
    return InvalidViewSize;
  const uint64_t Bytes = (*Size + PageSize - 1) & ~(PageSize - 1);
  if (Bytes > Limit - Offset)
    return InvalidViewSize;
  if (SectionViews.size() >= MaxViews)
    return StatusInsufficientResources;
  const uint64_t First = profile::SectionViewBase,
                 Last = First + profile::SectionViewSize;
  if (Bytes > profile::SectionViewSize)
    return NoMemory;
  const uint64_t Delta = Offset & (Granularity - 1);
  auto Available = [&](uint64_t Candidate) {
    if (Candidate < First || Candidate >= Last || Bytes > Last - Candidate)
      return false;
    for (const auto &[Key, V] : SectionViews)
      if (V.ProcessID == PID && overlap(Candidate, Bytes, V.Base, V.Size))
        return false;
    return true;
  };
  uint64_t Address = 0;
  if (*Base) {
    const uint64_t Candidate = (*Base & ~(Granularity - 1)) + Delta;
    if (!Available(Candidate))
      return Conflict;
    Address = Candidate;
  } else if (Allocation & 0x100000) {
    for (uint64_t Block = (Last - PageSize) & ~(Granularity - 1);;
         Block -= Granularity) {
      if (Available(Block + Delta)) {
        Address = Block + Delta;
        break;
      }
      if (Block == First)
        break;
    }
  } else {
    for (uint64_t Block = First; Block < Last; Block += Granularity)
      if (Available(Block + Delta)) {
        Address = Block + Delta;
        break;
      }
  }
  if (!Address)
    return NoMemory;
  auto Pages = [&]()
      -> llvm::Expected<std::vector<KernelPhysicalMemory::PhysicalViewPage>> {
    if (!Named)
      return Physical.physicalViewPages(Offset, Bytes);
    std::vector<KernelPhysicalMemory::PhysicalViewPage> P;
    for (uint64_t I = 0; I < Bytes; I += PageSize) {
      auto PA = Physical.physicalAddress(Named->Backing + Offset + I);
      if (!PA)
        return PA.takeError();
      auto Page = Physical.physicalViewPages(*PA, PageSize);
      if (!Page)
        return Page.takeError();
      P.push_back(std::move(Page->front()));
    }
    return P;
  }();
  if (!Pages)
    return Pages.takeError();
  const auto Cache =
      (Protect & 0x200)   ? KernelPhysicalMemory::CacheType::NonCached
      : (Protect & 0x400) ? KernelPhysicalMemory::CacheType::WriteCombined
                          : KernelPhysicalMemory::CacheType::Cached;
  for (const auto &P : *Pages)
    if (P.Cache && *P.Cache != Cache)
      return sectionError(
          "physical view cache conflicts with an existing guest frame");
  SectionView V{Address,     Bytes,        Offset, PID,
                Permissions, Inherit == 1, Cache,  std::move(*Pages),
                {}};
  if (auto E = pinSectionView(V)) {
    if (E.isA<PhysicalMemoryLimitError>()) {
      llvm::consumeError(std::move(E));
      return StatusInsufficientResources;
    }
    return E;
  }
  std::vector<GuestAliasMapping> Adding;
  for (size_t I = 0; I < V.Pages.size(); ++I)
    Adding.push_back(
        {V.Base + I * PageSize, V.Pages[I].Backing, PageSize, V.Permissions});
  if (auto E = Memory.replaceAliases({}, Adding)) {
    const bool Shortage = E.isA<GuestMemoryLimitError>();
    auto Rollback = releaseSectionPins(V);
    if (Rollback)
      return llvm::joinErrors(std::move(E), std::move(Rollback));
    if (Shortage) {
      llvm::consumeError(std::move(E));
      return NoMemory;
    }
    return E;
  }
  for (const auto &P : V.Pages)
    if (!P.Cache)
      if (auto E = Physical.commitMappingCache(P.Backing, PageSize, Cache))
        return E;
  SectionViews.emplace(SectionViewKey{PID, V.Base}, std::move(V));
  // Matched NtMapViewOfSection publishes ViewSize, BaseAddress, SectionOffset.
  if (auto E = Memory.writeInteger(A[6], Bytes, 8))
    return E;
  if (auto E = Memory.writeInteger(A[2], Address, 8))
    return E;
  if (A[5])
    if (auto E = Memory.writeInteger(A[5], Offset, 8))
      return E;
  return StatusSuccess;
}
llvm::Error KernelModel::retireSectionView(SectionViewKey Key,
                                           bool AlreadyUnmapped) {
  auto It = SectionViews.find(Key);
  if (It == SectionViews.end())
    return sectionError("view retirement lost its process and VAD");
  auto &V = It->second;
  if (auto E = canRevokeVirtualRange(V.Base, V.Size))
    return E;
  for (const auto &[O, Pin] : V.Pins)
    if (auto E = Physical.canUnpin(Pin))
      return E;
  const uint32_t Active =
      UserRequestContext ? CurrentUserProcessID : uint32_t(SystemProcessID);
  if (V.ProcessID == Active && !AlreadyUnmapped) {
    std::vector<GuestAliasRange> Remove;
    for (size_t I = 0; I < V.Pages.size(); ++I)
      Remove.push_back({V.Base + I * PageSize, PageSize});
    if (auto E = Memory.replaceAliases(Remove, {}))
      return E;
  }
  if (auto E = prepareRevokeVirtualRange(V.Base, V.Size))
    return E;
  if (auto E = releaseSectionPins(V))
    return E;
  SectionViews.erase(It);
  return llvm::Error::success();
}
llvm::Expected<uint64_t> KernelModel::unmapSection(llvm::ArrayRef<uint64_t> A) {
  if (A[0] != UINT64_MAX)
    return StatusInvalidHandle;
  const uint32_t PID = handleProcessID();
  for (const auto &[Key, V] : ImageMappings)
    if (Key.first == PID && A[1] >= Key.second && A[1] - Key.second < V.Size) {
      if (auto E = retireImageMapping(PID, Key.second))
        return E;
      return StatusSuccess;
    }
  for (const auto &[Key, V] : SectionViews)
    if (V.ProcessID == PID && A[1] >= V.Base && A[1] - V.Base < V.Size) {
      if (auto E = retireSectionView(Key))
        return E;
      return StatusSuccess;
    }
  return NotMapped;
}
llvm::Error KernelModel::retireProcessSections(uint32_t PID,
                                               bool AlreadyUnmapped) {
  std::vector<SectionViewKey> Views;
  for (const auto &[Key, V] : SectionViews)
    if (V.ProcessID == PID)
      Views.push_back(Key);
  for (const auto &Key : Views)
    if (auto E = retireSectionView(Key, AlreadyUnmapped))
      return E;
  for (auto It = SectionHandles.begin(); It != SectionHandles.end();)
    if (It->second.ProcessID == PID &&
        !(It->second.Attributes & ObjectKernelHandle))
      It = SectionHandles.erase(It);
    else
      ++It;
  return llvm::Error::success();
}
llvm::Error KernelModel::inheritSectionViews(uint32_t Parent, uint32_t Child) {
  std::vector<SectionView> Copies;
  uint64_t Pins = 0;
  for (const auto &[Key, V] : SectionViews)
    if (V.ProcessID == Parent && V.Inherit) {
      Copies.push_back(V);
      Pins += V.Pins.size();
    }
  for (const auto &[Key, V] : SectionViews)
    if (V.ProcessID == Child)
      return sectionError("child process already has section views");
  if (Copies.size() > MaxViews - SectionViews.size() ||
      Pins > Physical.availablePins())
    return sectionError("inherited section view capacity exhausted");
  std::vector<SectionViewKey> Published;
  for (auto &V : Copies) {
    V.ProcessID = Child;
    V.Pins.clear();
    if (auto E = pinSectionView(V)) {
      for (const auto &K : Published)
        E = llvm::joinErrors(std::move(E), retireSectionView(K));
      return E;
    }
    const SectionViewKey K{Child, V.Base};
    Published.push_back(K);
    SectionViews.emplace(K, std::move(V));
  }
  return llvm::Error::success();
}
llvm::Expected<bool>
KernelModel::validateSectionViewAccess(uint64_t Address, uint32_t Size,
                                       bool IsWrite,
                                       bool IncludeDispatcher) const {
  if (!Size || Address >= profile::SectionViewBase + profile::SectionViewSize ||
      Address + Size <= profile::SectionViewBase)
    return false;
  const uint32_t PID =
      UserRequestContext ? CurrentUserProcessID : uint32_t(SystemProcessID);
  for (const auto &[Key, V] : SectionViews) {
    if (V.ProcessID != PID || !overlap(Address, Size, V.Base, V.Size))
      continue;
    const auto Context = executionProcessContext();
    if (!Context || Context->ProcessID != PID)
      return sectionError(
          "section view access requires its known process context");
    if (Address < V.Base || Size > V.Size - (Address - V.Base))
      return sectionError("access crosses a physical section view boundary");
    if (IsWrite && !(V.Permissions & Write))
      return true; // CPU page protection owns the fault.
    uint64_t Cursor = Address;
    uint32_t Remaining = Size;
    while (Remaining) {
      const uint64_t Offset = Cursor - V.Base, InPage = Offset % PageSize;
      const auto &P = V.Pages.at(Offset / PageSize);
      const uint32_t Count = std::min<uint64_t>(Remaining, PageSize - InPage);
      // Memory validation runs inside the CPU's read-only observer lease.
      // Inspect allocation identity through AddressSpace metadata; the backend
      // facade's pinBacking intentionally requires a fully stopped CPU.
      auto Space = Memory.addressSpace();
      if (!Space)
        return sectionError("view validation lost its address space");
      auto Current = Space->pinBacking(P.Backing, PageSize);
      if (!Current)
        return Current.takeError();
      if (!Current->describesSameBytes(P.Storage))
        return sectionError("physical source VA changed allocation identity");
      if (!P.Owners.empty() && P.Backing < profile::UserProbeLimit) {
        auto Owner = Physical.ownerForRange(P.Backing + InPage, Count);
        if (!Owner)
          return Owner.takeError();
      } else if (IsWrite && P.Owners.empty()) {
        auto Writable = Memory.canAccess(P.Backing + InPage, Count, Write);
        if (!Writable)
          return Writable.takeError();
        if (!*Writable)
          return sectionError(
              "write to a read-only reference or environment frame");
      }
      if (auto E = validateGuestAccessImpl(P.Backing + InPage, Count, IsWrite,
                                           IncludeDispatcher, true))
        return E;
      Cursor += Count;
      Remaining -= Count;
    }
    return true;
  }
  return false;
}
llvm::Expected<uint64_t>
KernelModel::querySectionObject(llvm::ArrayRef<uint64_t> A) {
  auto H = SectionHandles.find(A[0]);
  if (H == SectionHandles.end() ||
      !canAccessHandle(H->second.Attributes, H->second.ProcessID))
    return StatusInvalidHandle;
  if (uint32_t(A[1]) != 1)
    return sectionError(
        "this object query class is unmodeled for section objects");
  if (H->second.Image)
    return sectionError("unnamed image section object-name query is unmodeled");
  auto Text = windows_unicode::bytes(
      H->second.Name.empty()
          ? llvm::StringRef(PhysicalName)
          : llvm::StringRef(NamedSections.at(H->second.Name).Name),
      true);
  if (!Text)
    return Text.takeError();
  const uint32_t Need = 16 + uint32_t(Text->size()), Size = uint32_t(A[3]);
  if (A[4]) {
    if (auto E = objectBuffer(A[4], 4, true))
      return E;
    if (overlap(A[4], 4, A[2], Need))
      return sectionError(
          "overlapping query and return-length outputs are unmodeled");
  }
  if (Size >= Need)
    if (auto E = objectBuffer(A[2], Need, true))
      return E;
  if (A[4])
    if (auto E = Memory.writeInteger(A[4], Need, 4))
      return E;
  if (Size < Need)
    return StatusInfoLengthMismatch;
  std::vector<uint8_t> Bytes(Need);
  llvm::support::endian::write16le(Bytes.data(), uint16_t(Text->size() - 2));
  llvm::support::endian::write16le(Bytes.data() + 2, uint16_t(Text->size()));
  llvm::support::endian::write64le(Bytes.data() + 8, A[2] + 16);
  std::copy(Text->begin(), Text->end(), Bytes.begin() + 16);
  if (auto E = Memory.write(A[2], Bytes))
    return E;
  return StatusSuccess;
}
} // namespace neverd::emulation
