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
/// Project only observed physical entry parameters. Caller memory accesses
/// still require their independent occurrence and frame projection proof.
std::optional<llvm::CallingConv::ID>
getX86RegistrationCxxEntryABI(const MedFunc &Source,
                              const llvm::Function &Function);
} // namespace neverd
#endif
