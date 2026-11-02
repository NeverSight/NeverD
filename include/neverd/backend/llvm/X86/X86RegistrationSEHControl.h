//===- X86RegistrationSEHControl.h - Source finally returns ------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHCONTROL_H
#define NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHCONTROL_H

#include "neverd/Common.h"

#include <map>
#include <optional>

namespace neverd {
struct MedFunc;

/// Bind ordinary finally returns to their source callback and CALL successor.
/// Each entry maps a returning source block to its callback entry. The native
/// outliner implements these exits with its ordinary C return ABI; source
/// replay must also require a return at the corresponding generated block.
/// Parent-frame jumps need a separate continuation projection.
std::optional<std::map<int, va_t>>
getX86RegistrationSEHReturnBlocks(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86REGISTRATIONSEHCONTROL_H
