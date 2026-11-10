//===- X86PagedMemory.cpp - Restartable x86 RAM operands ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Plan all translations and physical spans before publishing operand bytes.
//===----------------------------------------------------------------------===//

#include "neverd/translate/X86/X86PagedMemory.h"

#include "neverd/translate/TranslationResult.h"

#include "llvm/Support/Errc.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace neverd::translate {
namespace {

llvm::Error invalid(const char *Message) {
  return llvm::createStringError(llvm::errc::invalid_argument, Message);
}

X86MemoryAccessResult backingFault(const GuestMemoryAccessResult &Access) {
  X86MemoryAccessResult Result;
  Result.Status = X86PageWalkStatus::BackingFault;
  Result.BackingFault = Access.Fault;
  return Result;
}

llvm::Expected<X86LegacySegmentResult>
segmentAccess(const X86PagingState &State, const X86SegmentedAccess &A,
              uint64_t Bytes) {
  if (!(State.CR0 & 1) || (State.EFER & 0x400) ||
      ((State.RFlags & 0x20000) && A.CPL != 3) || A.CPL > 3 || !Bytes ||
      Bytes > kMemoryFaultMaxAccessBytes)
    return invalid("segmented operands require legacy protected-mode state");
  X86SegmentAccessKind Kind;
  switch (A.Kind) {
  case MemoryAccessKind::Read:
    Kind = X86SegmentAccessKind::Read;
    break;
  case MemoryAccessKind::Write:
    Kind = X86SegmentAccessKind::Write;
    break;
  case MemoryAccessKind::Execute:
    Kind = X86SegmentAccessKind::Execute;
    break;
  default:
    return invalid("invalid segmented access kind");
  }
  return checkX86LegacySegmentAccess(
      A.Cache, {A.Segment, Kind, A.Offset, uint32_t(Bytes), A.FullLimitChoice,
                bool(State.RFlags & 0x20000)});
}

} // namespace

llvm::Expected<std::unique_ptr<X86PagedMemory>>
X86PagedMemory::create(const GuestState &RAM, const X86PagingFeatures &Features,
                       const X86PagingState &Controls,
                       const GuestMemoryRuntimeLimits &Limits) {
  if (auto Error = validateX86PagingState(Features, Controls))
    return std::move(Error);
  for (const auto &Region : RAM.Memory)
    if (hasPermission(Region.Permissions, MemoryPermission::Execute))
      return invalid("physical RAM cannot carry virtual execute permissions");
  if (RAM.AddressWidth < Features.PhysicalAddressBits)
    return invalid("physical RAM address width is narrower than MAXPHYADDR");
  GuestMemoryRuntimeConfig Config;
  Config.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  Config.Limits = Limits;
  auto Physical = GuestMemoryRuntime::create(RAM, Config);
  if (!Physical)
    return Physical.takeError();
  return std::unique_ptr<X86PagedMemory>(
      new X86PagedMemory(std::move(*Physical), Features, Controls));
}

X86PagedMemory::X86PagedMemory(std::unique_ptr<GuestMemoryRuntime> Physical,
                               const X86PagingFeatures &Features,
                               const X86PagingState &Controls)
    : Physical(std::move(Physical)), Features(Features), Controls(Controls) {}

llvm::Error X86PagedMemory::bindControls(const X86PagingState &State) {
  if (auto Error = validateX86PagingState(Features, State))
    return Error;
  Controls = State;
  return llvm::Error::success();
}

X86PagedMemory::AccessObserver
X86PagedMemory::exchangeAccessObserver(AccessObserver Next) {
  return std::exchange(Observer, Next);
}

void X86PagedMemory::observe(const X86LinearAccess &Access,
                             uint64_t Bytes) const {
  if (Observer.Observe && Access.Kind != MemoryAccessKind::Execute)
    Observer.Observe(Observer.Context, Access, Bytes);
}

llvm::Expected<X86PagedMemory::Plan>
X86PagedMemory::plan(const X86LinearAccess &Access, uint64_t Bytes) {
  Plan P;
  auto Check = checkX86LinearAccess(Controls, Access, Bytes);
  if (!Check)
    return Check.takeError();
  if (*Check) {
    P.Result.Status = X86PageWalkStatus::Exception;
    P.Result.Exception = *Check;
    return P;
  }
  auto Current = Access;
  uint64_t Remaining = Bytes;
  while (Remaining) {
    auto Translation =
        walkX86PageTables(*Physical, Features, Controls, Current);
    if (!Translation)
      return Translation.takeError();
    if (Translation->Status != X86PageWalkStatus::Completed) {
      P.Result = std::move(*Translation);
      P.Fragments.clear();
      return P;
    }
    // Limit fragments to 4 KiB even for large pages. Subsequent operand bytes
    // can alias the physical page tables; all translations precede the store.
    const uint64_t Size = std::min(Remaining, 4096 - (Current.Address & 4095));
    P.Fragments.push_back({Translation->PhysicalAddress, Size});
    Remaining -= Size;
    if (Remaining) {
      Current.Address += Size;
      if (Access.Wrap32)
        Current.Address = uint32_t(Current.Address);
    }
  }
  return P;
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::load(const X86LinearAccess &Access,
                     llvm::MutableArrayRef<uint8_t> Output) {
  if (Access.Kind != MemoryAccessKind::Read &&
      Access.Kind != MemoryAccessKind::Execute)
    return invalid("x86 paged load requires read or instruction-fetch access");
  auto P = plan(Access, Output.size());
  if (!P)
    return P.takeError();
  if (P->Result.Status != X86PageWalkStatus::Completed)
    return P->Result;
  std::vector<uint8_t> Bytes(Output.size());
  size_t Offset = 0;
  for (const auto &Fragment : P->Fragments) {
    const auto R = Physical->loadBytes(
        Fragment.PhysicalAddress,
        llvm::MutableArrayRef(Bytes).slice(Offset, Fragment.Size));
    if (R.Status == GuestMemoryAccessStatus::Fault)
      return backingFault(R);
    Offset += Fragment.Size;
  }
  std::copy(Bytes.begin(), Bytes.end(), Output.begin());
  observe(Access, Output.size());
  return P->Result;
}

llvm::Expected<X86PagedMemory::PreparedStore>
X86PagedMemory::prepareStore(const X86LinearAccess &Access, uint64_t Bytes) {
  if (Access.Kind != MemoryAccessKind::Write)
    return invalid("x86 paged store requires write access");
  auto P = plan(Access, Bytes);
  if (!P)
    return P.takeError();
  PreparedStore Store;
  static_cast<X86MemoryAccessResult &>(Store) = std::move(P->Result);
  if (Store.Status != X86PageWalkStatus::Completed)
    return Store;
  for (const auto &Fragment : P->Fragments) {
    const auto R =
        Physical->validateStoreBytes(Fragment.PhysicalAddress, Fragment.Size);
    if (R.Status == GuestMemoryAccessStatus::Fault) {
      static_cast<X86MemoryAccessResult &>(Store) = backingFault(R);
      return Store;
    }
  }
  Store.Fragments = std::move(P->Fragments);
  Store.Owner = this;
  Store.Bytes = Bytes;
  Store.Access = Access;
  return Store;
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::loadPreparedStore(const PreparedStore &Store,
                                  llvm::MutableArrayRef<uint8_t> Output) {
  assert(Store.Status == X86PageWalkStatus::Completed && Store.Owner == this &&
         Store.Bytes == Output.size() && "read the successful original plan");
  std::vector<uint8_t> Bytes(Output.size());
  size_t Offset = 0;
  for (const auto &Fragment : Store.Fragments) {
    const auto R = Physical->loadBytes(
        Fragment.PhysicalAddress,
        llvm::MutableArrayRef(Bytes).slice(Offset, Fragment.Size));
    if (R.Status == GuestMemoryAccessStatus::Fault)
      return backingFault(R);
    Offset += Fragment.Size;
  }
  std::copy(Bytes.begin(), Bytes.end(), Output.begin());
  auto Access = Store.Access;
  Access.Kind = MemoryAccessKind::Read;
  observe(Access, Output.size());
  return static_cast<const X86MemoryAccessResult &>(Store);
}

void X86PagedMemory::commitStore(const PreparedStore &Store,
                                 llvm::ArrayRef<uint8_t> Input) {
  assert(Store.Status == X86PageWalkStatus::Completed && Store.Owner == this &&
         Store.Bytes == Input.size() && "commit the successful original plan");
  size_t Offset = 0;
  for (const auto &Fragment : Store.Fragments) {
    // No callbacks, concurrent writers, permission changes or executable
    // generation increments can intervene between validation and commit.
    const auto R = Physical->storeBytes(Fragment.PhysicalAddress,
                                        Input.slice(Offset, Fragment.Size));
    assert(R.Status == GuestMemoryAccessStatus::Completed &&
           "validated confined physical RAM store cannot fail");
    (void)R;
    Offset += Fragment.Size;
  }
  observe(Store.Access, Input.size());
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::store(const X86LinearAccess &Access,
                      llvm::ArrayRef<uint8_t> Input) {
  auto Store = prepareStore(Access, Input.size());
  if (!Store)
    return Store.takeError();
  if (Store->Status == X86PageWalkStatus::Completed)
    commitStore(*Store, Input);
  return static_cast<X86MemoryAccessResult &&>(*Store);
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::loadSegment(const X86SegmentedAccess &A,
                            llvm::MutableArrayRef<uint8_t> Output) {
  if (A.Kind != MemoryAccessKind::Read && A.Kind != MemoryAccessKind::Execute)
    return invalid("segmented load requires read or instruction-fetch access");
  auto Check = segmentAccess(Controls, A, Output.size());
  if (!Check)
    return Check.takeError();
  if (Check->FaultVector) {
    X86MemoryAccessResult Result;
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = X86MemoryException{*Check->FaultVector, 0, std::nullopt};
    return Result;
  }
  return load({Check->Linear, A.Kind, A.CPL, false,
               A.Segment == X86SegmentRegister::SS, true},
              Output);
}

llvm::Expected<X86PagedMemory::PreparedStore>
X86PagedMemory::prepareSegmentStore(const X86SegmentedAccess &A,
                                    uint64_t Bytes) {
  if (A.Kind != MemoryAccessKind::Write)
    return invalid("segmented store requires write access");
  auto Check = segmentAccess(Controls, A, Bytes);
  if (!Check)
    return Check.takeError();
  if (Check->FaultVector) {
    PreparedStore Result;
    Result.Status = X86PageWalkStatus::Exception;
    Result.Exception = X86MemoryException{*Check->FaultVector, 0, std::nullopt};
    return Result;
  }
  return prepareStore({Check->Linear, A.Kind, A.CPL, false,
                       A.Segment == X86SegmentRegister::SS, true},
                      Bytes);
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::storeSegment(const X86SegmentedAccess &Access,
                             llvm::ArrayRef<uint8_t> Input) {
  auto Store = prepareSegmentStore(Access, Input.size());
  if (!Store)
    return Store.takeError();
  if (Store->Status == X86PageWalkStatus::Completed)
    commitStore(*Store, Input);
  return static_cast<X86MemoryAccessResult &&>(*Store);
}

llvm::Expected<X86MemoryAccessResult>
X86PagedMemory::probe(const X86LinearAccess &Access, uint64_t Bytes) {
  auto Plan = plan(Access, Bytes);
  if (!Plan)
    return Plan.takeError();
  return std::move(Plan->Result);
}

} // namespace neverd::translate
