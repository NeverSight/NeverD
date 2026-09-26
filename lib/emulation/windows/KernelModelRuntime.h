//===- KernelModelRuntime.h - Checked guest runtime services --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bounded string operations and debugger formatting for the Windows model.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EMULATION_KERNELMODELRUNTIME_H
#define NEVERD_EMULATION_KERNELMODELRUNTIME_H

#include "../GuestMemory.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <string>

namespace neverd::emulation {
class KernelModel;
namespace runtime {
#define NEVERD_KERNEL_RUNTIME_LIMIT(Name, Value)                               \
  inline constexpr unsigned Name = Value;
#include "KernelRuntimeLimits.def"
#undef NEVERD_KERNEL_RUNTIME_LIMIT

using ArgumentReader = llvm::function_ref<llvm::Expected<uint64_t>(unsigned)>;
enum class UnicodeOperation { Copy, Compare, Equal };

llvm::Expected<uint64_t> unicodeOperation(const KernelModel &Model,
                                          GuestMemory &Memory,
                                          UnicodeOperation Operation,
                                          llvm::ArrayRef<uint64_t> Arguments);
llvm::Expected<std::string> formatDebugMessage(const KernelModel &Model,
                                               GuestMemory &Memory,
                                               uint64_t Format,
                                               unsigned FirstArgument,
                                               ArgumentReader ReadArgument);
} // namespace runtime
} // namespace neverd::emulation

#endif // NEVERD_EMULATION_KERNELMODELRUNTIME_H
