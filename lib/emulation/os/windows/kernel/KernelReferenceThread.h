//===- KernelReferenceThread.h - Original thread construction boundary ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Original thread construction boundary.
///

#ifndef NEVERD_EMULATION_KERNELREFERENCETHREAD_H
#define NEVERD_EMULATION_KERNELREFERENCETHREAD_H
#include "neverd/emulation/GuestMemory.h"

#include <array>
namespace neverd::emulation {
class KernelReferenceImage;
struct KernelReferenceThreadInput {
  static constexpr uint64_t AuxiliaryBytes = 0x41000;
  static constexpr uint64_t ProcessThreadListHeadOffset = 0x30;
  uint64_t Object, Process, ThreadID, StartAddress;
  uint64_t StackBase, StackSize, AuxiliaryBase;
};
struct KernelReferenceThreadResult {
  std::vector<uint8_t> Body;
  uint64_t Instructions, HelperInstructions;
  std::array<uint8_t, 16> ProcessThreadListHead;
};
bool hasKernelThreadConstruction(const KernelReferenceImage &Reference);
// Construct one initial System thread using original code and private owned
// resources. The CPU stops at the validated hardware-context handoff. Only the
// fully defined target body is returned; no caller return or guest publication
// is implied. The caller owns guest lifetime and completion of CPU dispatch.
llvm::Expected<KernelReferenceThreadResult>
constructKernelReferenceThread(const KernelReferenceImage &Reference,
                               const KernelReferenceThreadInput &Input);
} // namespace neverd::emulation
#endif
