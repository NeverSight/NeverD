//===- X86RegistrationEntry.h - PE32 C++ parent entry ABI ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONENTRY_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONENTRY_H

#include "llvm/IR/CallingConv.h"

#include <optional>

namespace llvm {
class Function;
}
namespace neverd {
struct MedFunc;
/// Select the convention for an unassigned physical entry signature. Caller
/// memory accesses still require their independent frame projection proof.
std::optional<llvm::CallingConv::ID>
getX86RegistrationCxxEntryABI(const MedFunc &Source,
                              const llvm::Function &Function);
/// Check the assigned convention and every parameter attribute, including the
/// inreg attributes required by LLVM's x86 fastcall lowering.
bool hasX86RegistrationCxxEntryABI(const MedFunc &Source,
                                   const llvm::Function &Function);
} // namespace neverd
#endif
