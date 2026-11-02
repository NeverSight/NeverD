//===- X86AtomicExchange.h - Memory exchange compatibility ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Existing exchange callers use the shared scalar RMW contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H
#define NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H

#include "neverd/backend/llvm/X86/X86AtomicRmw.h"

#include <optional>

namespace neverd {

using X86AtomicExchangePlan = X86AtomicRmwPlan;

inline std::optional<X86AtomicExchangePlan>
x86AtomicExchangePlan(Arch Architecture, unsigned Bytes,
                      NdMemoryAddressSpace AddressSpace) {
  return x86AtomicRmwPlan(Architecture, NdOp::ATOMIC_XCHG, Bytes, AddressSpace);
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86ATOMICEXCHANGE_H
