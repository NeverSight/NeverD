//===- X86AtomicRmw.h - Scalar atomic RMW C projection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the native transaction without promising pointer alignment.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICRMW_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICRMW_H

#include "neverd/backend/llvm/X86/X86AtomicRmw.h"

#include <optional>
#include <string>

namespace llvm {
class CallInst;
}

namespace neverd {

/// Address of the current function's compiler-owned return slot. The local
/// declaration must stay in the caller: an out-of-line helper has its own slot.
std::string renderX86ReturnSlotAddress(const std::string &IntrinsicName);

std::string renderX86AtomicRmw(const X86AtomicRmwPlan &Plan,
                               const std::string &Address,
                               const std::string &Value);
std::optional<X86AtomicRmwPlan>
x86AtomicRmwCallPlan(Arch Architecture, const llvm::CallInst &Call);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86ATOMICRMW_H
