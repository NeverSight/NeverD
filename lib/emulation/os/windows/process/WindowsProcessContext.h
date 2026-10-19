//===- WindowsProcessContext.h - Windows user CONTEXT codec ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_WINDOWS_PROCESS_CONTEXT_H
#define NEVERD_EMULATION_WINDOWS_PROCESS_CONTEXT_H
#include "../exception/X64SEH.h"

#include "neverd/emulation/CPU.h"

#include <vector>

namespace neverd::emulation::windows_process {
enum class ContextOrigin { Current, HardwareFault };
llvm::Expected<std::vector<uint8_t>>
captureUserContext(ExecutionBackend &CPU,
                   ContextOrigin Origin = ContextOrigin::Current);
/// RtlCaptureContext records the calling frame without changing CPU state.
/// Validate the destination and return slot before publishing any bytes;
/// fields outside the architecture's native capture contract stay untouched.
llvm::Error captureCallerContext(ExecutionBackend &CPU, uint64_t Destination,
                                 bool UserMode = true);
llvm::Expected<X64SEH::Context>
readUnwindContext(llvm::ArrayRef<uint8_t> Context);
llvm::Error writeUnwindContext(const X64SEH::Context &State,
                               llvm::MutableArrayRef<uint8_t> Context);
/// Validate the complete guest record before restoring any state. CPU state
/// outside the Windows CONTEXT remains owned by the original backend snapshot.
llvm::Error restoreUserContext(ExecutionBackend &CPU,
                               const BackendContext &Snapshot,
                               llvm::ArrayRef<uint8_t> Original,
                               llvm::ArrayRef<uint8_t> Changed,
                               uint64_t StackBase, uint64_t StackTop,
                               ContextOrigin Origin = ContextOrigin::Current);
} // namespace neverd::emulation::windows_process
#endif
