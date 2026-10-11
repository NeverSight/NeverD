//===- WindowsProcessState.h - Windows stopped state ownership ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWSPROCESSSTATE_H
#define NEVERD_EMULATION_WINDOWSPROCESSSTATE_H

#include "neverd/emulation/ProcessRuntimeState.h"
#include "neverd/emulation/WindowsProcessOptions.h"

#include <vector>

namespace neverd::emulation {
/// The Windows service owner supplies object identity and lifetime. A consumer
/// must not infer these fields from integer matches or incomplete call logs.
struct WindowsProcessState final : ProcessRuntimeState {
  WindowsProcessState() : ProcessRuntimeState(Kind::WindowsPE64) {}
  struct Allocation {
    uint64_t Address, RequestedSize, Heap;
    bool EnvironmentSnapshot;
    /// Complete committed backing, including allocation padding.
    std::vector<uint8_t> Bytes;
  };
  struct LocalSlot {
    uint32_t Index;
    uint64_t Value, Callback;
  };
  struct CriticalSection {
    uint64_t Address;
    uint32_t Recursion;
  };
  struct VirtualPage {
    uint64_t Address;
    unsigned Permissions;
    std::vector<uint8_t> Bytes;
  };
  struct VirtualAllocation {
    uint64_t Address, Size;
    uint32_t Protection;
    std::vector<VirtualPage> Committed;
  };
  std::optional<WindowsPEBVersion> Version;
  uint64_t ProcessHeap = 0, PointerXor = 0;
  uint32_t LastError = 0;
  std::vector<uint64_t> PrivateHeaps;
  std::vector<Allocation> Allocations;
  std::vector<LocalSlot> ThreadSlots, FiberSlots;
  std::vector<CriticalSection> CriticalSections;
  std::vector<VirtualAllocation> VirtualAllocations;
  /// Objects outside the materialization contract remain explicit refusals.
  bool HasOpenFiles = false, HasSections = false, HasMappedViews = false;
  bool HasThreadSnapshots = false;
  bool HasExceptionState = false, HasActiveFiberCleanup = false;
  bool HasUnownedThreadSlots = false;
  bool hasAdditionalDependencies() const override {
    return !PrivateHeaps.empty() || !CriticalSections.empty() ||
           !VirtualAllocations.empty() || HasOpenFiles || HasSections ||
           HasMappedViews || HasThreadSnapshots || HasExceptionState ||
           HasActiveFiberCleanup;
  }
};
} // namespace neverd::emulation
#endif
