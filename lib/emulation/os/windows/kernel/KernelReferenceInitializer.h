//===- KernelReferenceInitializer.h - Bounded original initialization -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded original initialization.
///

#ifndef NEVERD_EMULATION_KERNELREFERENCEINITIALIZER_H
#define NEVERD_EMULATION_KERNELREFERENCEINITIALIZER_H
#include "neverd/emulation/GuestMemory.h"
#include "neverd/emulation/Registers.h"

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace neverd::emulation {
class KernelReferenceImage;
struct KernelInitializerRange {
  uint64_t Address, Size;
};
struct KernelInitializerBytes {
  uint64_t Address;
  std::vector<uint8_t> Bytes;
};
struct KernelInitializerCall {
  uint64_t Entry;
  std::vector<uint64_t> Arguments;
  bool RequireZeroResult = false;
};
struct KernelInitializerServiceResult {
  uint64_t Value = 0;
  std::vector<KernelInitializerBytes> Writes;
  // Transfer ownership at this original OS boundary without executing a
  // synthetic return. Legal only in the final call without a return-status
  // requirement, and cannot be combined with prepared values or writes.
  bool Handoff = false;
};
struct KernelInitializerServiceInput {
  llvm::ArrayRef<uint64_t> Arguments;
  // Read only defined private bytes. The capability expires when Prepare
  // returns; a provider cannot retain a view of the initializer's CPU memory.
  std::function<llvm::Expected<std::vector<uint8_t>>(uint64_t, uint64_t)> Read;
};
struct KernelInitializerService {
  uint64_t Entry;
  unsigned Arity;
  // Prepare effects in the private execution only. A service must not mutate
  // the caller's real guest or acquire externally owned resources.
  std::function<llvm::Expected<KernelInitializerServiceResult>(
      const KernelInitializerServiceInput &)>
      Prepare;
};
struct KernelInitializerPlan {
  std::vector<KernelInitializerBytes> Inputs;
  // Writable ranges start undefined. Native writes or explicit inputs must
  // establish every byte before it can be read or returned as an output.
  std::vector<KernelInitializerRange> Writable, Outputs;
  std::vector<KernelInitializerCall> Calls;
  std::vector<KernelInitializerService> Services;
  // The initializer owns a zero-initialized Win64 helper stack and return
  // sentinel. Neither is part of the calling driver's execution or stack.
  uint64_t StackBase = 0, StackSize = 0, ReturnPC = 0, GSBase = 0;
  uint64_t MemoryLimit = 128ULL * 1024 * 1024;
  uint64_t InstructionLimit = 100000, TimeoutMicroseconds = 2000000;
  // Optional frozen guest clock during this private initialization operation.
  // Omission keeps timestamp reads unsupported; no host clock is observed.
  std::optional<uint64_t> TimestampCounter;
};
struct KernelInitializerResult {
  std::vector<uint64_t> ReturnValues;
  std::vector<KernelInitializerBytes> Outputs;
  uint64_t Instructions = 0, ServiceCalls = 0;
  uint64_t FinalIRQL = 0;
  struct Boundary {
    uint64_t Entry;
    std::vector<uint64_t> Arguments;
    std::map<X64Register, uint64_t> Registers;
  };
  std::optional<Boundary> Handoff;
};
// Replay original, matched x64 kernel initialization in an independent CPU and
// address space. File-backed code is immutable; mapped kernel data has no
// initial value unless explicitly supplied. Failure publishes no output bytes.
// A successful result proves initialization under the supplied inputs, not an
// active Windows object or permission to expose it to a driver.
llvm::Expected<KernelInitializerResult>
initializeFromKernelReference(const KernelReferenceImage &Reference,
                              const KernelInitializerPlan &Plan);
} // namespace neverd::emulation
#endif
