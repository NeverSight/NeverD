//===- LinuxWriteBuffers.h - Imported Linux write buffers ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Captured streams and memory files share complete LP64 iovec import,
/// with borrowed model bytes bypassing guest-memory reads only.
#ifndef NEVERD_EMULATION_OS_LINUX_KERNEL_LINUXWRITEBUFFERS_H
#define NEVERD_EMULATION_OS_LINUX_KERNEL_LINUXWRITEBUFFERS_H

#include "LinuxKernel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <variant>

namespace neverd::emulation::linux_model {
struct WriteBuffer {
  std::variant<uint64_t, llvm::StringRef> Source;
  uint64_t Size;
};
/// Import all LP64 vector metadata before any provider or payload effect.
/// Returns the capped total or a negative guest errno. Buffers is initially
/// empty and is only usable after a nonnegative result.
llvm::Expected<int64_t>
importWriteBuffers(ExecutionBackend &CPU, uint64_t Address, uint32_t Count,
                   const MemoryLayout &Layout,
                   const std::optional<LinuxKernelOptions> &Kernel,
                   llvm::SmallVectorImpl<WriteBuffer> &Buffers);
/// Trusted model-owned bytes have no guest descriptor/payload memory to read.
/// Retain borrowed views with the same cumulative Linux write size cap.
void materializeWriteBuffers(llvm::ArrayRef<llvm::StringRef> Bytes,
                             const MemoryLayout &Layout,
                             llvm::SmallVectorImpl<WriteBuffer> &Buffers);
} // namespace neverd::emulation::linux_model
#endif // NEVERD_EMULATION_OS_LINUX_KERNEL_LINUXWRITEBUFFERS_H
