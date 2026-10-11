//===- WindowsProcessState.cpp - Snapshot authoritative runtime objects
//----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "WindowsProcessExceptions.h"

#include "llvm/Support/Endian.h"

namespace neverd::emulation::windows_process {
llvm::Expected<std::shared_ptr<const ProcessRuntimeState>>
Services::runtimeState(bool IncludeBacking) const {
  auto State = std::make_shared<WindowsProcessState>();
  if (Options.Windows)
    State->Version = Options.Windows->PEBVersion;
  State->ProcessHeap = value::HeapHandle;
  State->PointerXor = value::PointerCookie;
  uint8_t LastError[4];
  if (auto E =
          Memory.snapshotBacking(value::TEB + value::TebLastError, LastError))
    return std::move(E);
  State->LastError = llvm::support::endian::read32le(LastError);
  State->PrivateHeaps.assign(CreatedHeaps.begin(), CreatedHeaps.end());
  uint64_t Total = 0;
  for (const auto &[Address, Allocation] : Allocations) {
    if (Allocation.MappedSize > Options.MemoryLimit - Total)
      return failure("runtime-state backing exceeds the process memory limit");
    Total += Allocation.MappedSize;
    WindowsProcessState::Allocation Out{
        Address, Allocation.Size, Allocation.Heap,
        Allocation.EnvironmentSnapshot,
        std::vector<uint8_t>(IncludeBacking ? Allocation.MappedSize : 0)};
    if (IncludeBacking)
      if (auto E = Memory.snapshotBacking(Address, Out.Bytes))
        return std::move(E);
    State->Allocations.push_back(std::move(Out));
  }
  uint8_t ThreadValues[value::DynamicTLSCount * value::PointerSize];
  if (auto E =
          Memory.snapshotBacking(value::TEB + value::TebTLSSlots, ThreadValues))
    return std::move(E);
  for (uint32_t I = 0; I < value::DynamicTLSCount; ++I) {
    const uint64_t V =
        llvm::support::endian::read64le(ThreadValues + I * value::PointerSize);
    if (TLSSlots[I])
      State->ThreadSlots.push_back({I, V, 0});
    else if (V)
      State->HasUnownedThreadSlots = true;
  }
  for (const auto &[Index, Data] : FLSData) {
    State->FiberSlots.push_back({Index, Data.Value, Data.Callback});
    State->HasActiveFiberCleanup |= Data.Cleaning;
  }
  State->HasActiveFiberCleanup |= FLSExit.has_value();
  for (const auto &[Address, Depth] : CriticalSections) {
    std::array<uint8_t, value::CriticalSectionSize> Actual{}, Expected{};
    if (auto E = Memory.snapshotBacking(Address, Actual))
      return std::move(E);
    llvm::support::endian::write32le(Expected.data() +
                                         value::CriticalSectionLock,
                                     Depth ? UINT32_MAX - 1 : UINT32_MAX);
    llvm::support::endian::write32le(
        Expected.data() + value::CriticalSectionRecursion, Depth);
    llvm::support::endian::write64le(Expected.data() +
                                         value::CriticalSectionOwner,
                                     Depth ? value::ThreadID : 0);
    if (Actual != Expected)
      return failure(
          "captured critical-section bytes disagree with their owner");
    State->CriticalSections.push_back({Address, Depth});
  }
  State->HasOpenFiles = !Files.empty();
  State->HasSections = !Sections.empty();
  State->HasMappedViews = !Views.empty();
  State->HasThreadSnapshots = !ThreadSnapshots.empty();
  if (auto E = Virtual.snapshotReservations(*State, Options.MemoryLimit - Total,
                                            IncludeBacking))
    return std::move(E);
  State->HasExceptionState = Exceptions.hasRuntimeState();
  return std::shared_ptr<const ProcessRuntimeState>(std::move(State));
}

llvm::Error VirtualMemory::snapshotReservations(WindowsProcessState &State,
                                                uint64_t RemainingBytes,
                                                bool IncludeBacking) const {
  auto Mappings = Space.mappings();
  if (!Mappings)
    return Mappings.takeError();
  for (const auto &[Address, Range] : Ranges) {
    if (Range.Kind != Owner::Virtual)
      continue;
    WindowsProcessState::VirtualAllocation Out{
        Address, Range.Size, Range.Protection, {}};
    if (!IncludeBacking) {
      State.VirtualAllocations.push_back(std::move(Out));
      continue;
    }
    for (const auto &M : *Mappings) {
      const uint64_t First = std::max(Address, M.Address);
      const uint64_t Last = std::min(Address + Range.Size, M.Address + M.Size);
      if (First >= Last)
        continue;
      if (M.Device || Last - First > RemainingBytes)
        return failure("virtual runtime backing exceeds its RAM contract");
      RemainingBytes -= Last - First;
      WindowsProcessState::VirtualPage Page{First, M.Permissions,
                                            std::vector<uint8_t>(Last - First)};
      if (auto E = Space.snapshotBacking(First, Page.Bytes))
        return E;
      Out.Committed.push_back(std::move(Page));
    }
    State.VirtualAllocations.push_back(std::move(Out));
  }
  return llvm::Error::success();
}
} // namespace neverd::emulation::windows_process
