//===- X86RegistrationSEHStateStore.h - Computed state writes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently binds computed source states to current native frame writes.

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHSTATESTORE_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHSTATESTORE_H

#include "neverd/Common.h"

#include "llvm/ADT/STLFunctionalExtras.h"

namespace llvm {
class AllocaInst;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace neverd {
struct MedFunc;

/// A full-word literal or absorbing AND/OR result, without assuming any loaded
/// memory value. The source-operation proof separately preserves the RMW read.
bool isX86RegistrationSEHStateValue(const llvm::Value &Value,
                                    uint32_t Expected);

/// Validate computed parent stores against the current logical frame and an
/// independently constant native value. Other memory and flag effects remain
/// in the ordinary source-operation replay. Lookup selects pre-outline source
/// operations; absent Lookup, consume their checked native operation markers.
bool hasCheckedX86RegistrationSEHComputedStores(
    const MedFunc &Source, const llvm::Function &Parent,
    const llvm::AllocaInst &Frame, uint64_t EntrySP,
    llvm::function_ref<const llvm::Instruction *(va_t, int)> Lookup = {});

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONSEHSTATESTORE_H
