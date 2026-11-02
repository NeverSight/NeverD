//===- LinuxMemory.h - Linux anonymous memory services ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_OS_LINUX_LINUXMEMORY_H
#define NEVERD_EMULATION_OS_LINUX_LINUXMEMORY_H

#include "LinuxDeviceState.h"
#include "LinuxKernel.h"

#include "neverd/emulation/AddressSpace.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace neverd::emulation::linux_model {
/// Own Linux placement and program-break policy for one stopped process.
/// AddressSpace remains the only mapping/permission authority. These services
/// require exclusive OS ownership of mutations across the whole operation.
class LinuxMemory {
public:
  LinuxMemory(AddressSpace &Space, const MemoryLayout &Layout,
              uint64_t InitialBreak, const ProcessOptions &Options);
  llvm::Expected<std::optional<uint64_t>>
  handle(ServiceKind Kind, const ProcessServiceEvent &Event,
         ProcessResult &Result);
  struct ProcMapping : LinuxProcMapping {
    unsigned Permissions = 0;
    uint64_t AnonymousName = 0;
    bool IsMMIO = false;
  };
  llvm::Error initializeProc();
  llvm::Expected<std::vector<ProcMapping>>
  procMappings(bool IncludeMMIOBoundaries = false) const;
  llvm::Expected<std::string> procAnonymousName(uint64_t Address) const;
  /// KSM eligibility only; the deterministic profile has no background scanner.
  bool isMergeable(uint64_t Address) const;
  llvm::Expected<std::optional<uint64_t>>
  mapDevice(const ProcessServiceEvent &Event, const LinuxDeviceOptions *Device,
            ProcessResult &Result,
            std::shared_ptr<LinuxDeviceState> Owner = {});
  bool contextIdle(const LinuxDeviceState &Device, uint32_t ID) const;
  llvm::Expected<MemoryView> allocateGPUObject(uint64_t Size);
  llvm::Error
  synchronizeDevices(uint64_t Elapsed,
                     const std::optional<LinuxFileOptions> &Options);
  llvm::ArrayRef<uint8_t> deviceBytes(const LinuxDeviceMapping &Mapping) const;
  llvm::Error writeDeviceBytes(const LinuxDeviceMapping &Mapping,
                               uint64_t Offset, llvm::ArrayRef<uint8_t> Bytes);
  std::optional<uint64_t> binderAddress(const LinuxDeviceState &Device,
                                        uint64_t Offset, uint64_t Size) const;

private:
  static unsigned permissions(uint64_t Protection);
  AddressSpace &Space;
  const ProcessOptions &Options;
  std::vector<LinuxProcMapping> ProcMappings;
  void retireProcMappings(uint64_t Address, uint64_t Size);
  // Sorted disjoint metadata intervals. Pointer identity controls merging;
  // no name bytes or RAM owners survive the calling service here.
  struct AnonymousVMAName {
    uint64_t Begin, End, Name;
  };
  std::vector<AnonymousVMAName> AnonymousVMANames;
  // Provenance only, independent of names and page permissions. No RAM owners
  // are retained; shared unmap retires these sorted, disjoint intervals.
  std::vector<std::pair<uint64_t, uint64_t>> AnonymousAllocationRanges;
  size_t NextResidencyObservation = 0;
  void setAllocationOwnership(uint64_t Address, uint64_t Size, bool Owned);
  std::vector<AnonymousVMAName> keptAnonymousVMANames(uint64_t Address,
                                                      uint64_t Size) const;
  void retireAnonymousVMANames(uint64_t Address, uint64_t Size);
  llvm::Expected<std::optional<uint64_t>>
  prctl(const ProcessServiceEvent &Event, ProcessResult &Result);
  const uint64_t PageSize, UserLimit, Limit, MinimumBreak, GuardBase, GuardEnd;
  uint64_t ProgramBreak;
  // Sorted, disjoint eligible intervals [begin, end). These own neither RAM
  // nor mappings; unmapping and new anonymous allocations retire the policy.
  std::vector<std::pair<uint64_t, uint64_t>> MergeableRanges;
  // Maximum-protection policy only, not a second page table. AddressSpace
  // owns mappings/permissions; its references keep the shared RAM alive.
  struct DeviceAlias {
    uint64_t Begin, End, Offset;
    const LinuxDeviceMapping *Mapping;
    std::shared_ptr<LinuxDeviceState> Owner;
  };
  std::vector<DeviceAlias> ReadOnlyDeviceRanges;
  struct GPUAlias {
    uint64_t Begin, Size, GPUAddress, Offset;
    std::shared_ptr<LinuxDeviceState> Owner;
  };
  std::vector<GPUAlias> GPUObjectRanges;
  bool overlapsGPUObject(uint64_t Address, uint64_t Size,
                         bool PartialOnly = false) const;
  void retireGPUObjects(uint64_t Address, uint64_t Size);
  llvm::Expected<std::optional<uint64_t>>
  mapGPUObject(const ProcessServiceEvent &Event,
               std::shared_ptr<LinuxDeviceState> Owner, ProcessResult &Result);
  struct DeviceState {
    std::vector<uint8_t> Bytes;
    size_t NextUpdate = 0;
  };
  std::map<const LinuxDeviceMapping *, DeviceState> DeviceStates;
  std::optional<uint64_t> LastDeviceTime;
  std::map<const LinuxDeviceMapping *, std::vector<std::weak_ptr<MemoryRegion>>>
      DevicePages;

  void retireDeviceRanges(uint64_t Address, uint64_t Size);
  bool canRetireDevices(uint64_t Address, uint64_t Size) const;
  std::optional<uint64_t> firstDeviceByte(uint64_t Address, uint64_t End) const;
  llvm::Expected<bool> mapDevicePages(uint64_t Address, uint64_t Size,
                                      unsigned Permissions,
                                      const LinuxDeviceMapping &Mapping,
                                      uint64_t Offset,
                                      std::shared_ptr<LinuxDeviceState> Owner);
  void setMergeable(uint64_t Address, uint64_t Size, bool Mergeable);
  std::optional<uint64_t> roundSize(uint64_t Size) const;
  bool validRange(uint64_t Address, uint64_t Size) const;
  llvm::Expected<std::vector<AddressMapping>> reservedRanges() const;
  llvm::Expected<std::optional<uint64_t>> placement(uint64_t Hint,
                                                    uint64_t Size) const;
  std::optional<uint64_t> findGap(llvm::ArrayRef<AddressMapping> Ranges,
                                  uint64_t Start, uint64_t Size) const;
  llvm::Expected<bool> mapPages(uint64_t Address, uint64_t Size,
                                unsigned Permissions);
  llvm::Expected<bool> unmapPages(uint64_t Address, uint64_t Size);
  llvm::Expected<uint64_t> map(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> protect(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> unmap(const ProcessServiceEvent &Event);
  llvm::Expected<uint64_t> advise(const ProcessServiceEvent &Event);
  llvm::Expected<std::optional<uint64_t>>
  discard(const ProcessServiceEvent &Event, ProcessResult &Result);
  llvm::Expected<std::optional<uint64_t>>
  residency(const ProcessServiceEvent &Event, ProcessResult &Result);
  llvm::Expected<uint64_t> setBreak(uint64_t Address);
};
} // namespace neverd::emulation::linux_model
#endif
