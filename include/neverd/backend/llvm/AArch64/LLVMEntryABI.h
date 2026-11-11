//===- LLVMEntryABI.h - Darwin AArch64 entry reconstruction ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reconstruct a declared source entry around a lifted physical-carrier body.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_AARCH64_LLVMENTRYABI_H
#define NEVERD_BACKEND_LLVM_AARCH64_LLVMENTRYABI_H

#include "llvm/Support/Error.h"

#include <cstdint>

namespace llvm {
class Function;
class Value;
}

namespace neverd {
struct MedFunc;

#define LLVM_ENTRY_ABI_CALL(Name, Text) inline constexpr char Name[] = Text;
#include "neverd/backend/llvm/AArch64/LLVMEntryABI.def"

/// Validate the complete fixed Darwin C signature in SourceTypeHint against
/// the body's recovered carriers, then give its public identity to a native
/// ABI entry. Direct carrier calls retain the private body; stored addresses
/// name the new entry. No module publication or image-signing guard is removed.
/// Failure leaves the function and module unchanged.
llvm::Expected<llvm::Function *>
reconstructDarwinAArch64Entry(llvm::Function &Body, const MedFunc &Source);
/// Read a Darwin ABI register with a physical live-in, or return null for
/// other offsets. The caller first resolves ordinary parameters and local defs.
llvm::Value *readDarwinAArch64EntryRegister(llvm::Function &F, uint64_t Offset);
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_AARCH64_LLVMENTRYABI_H
