//===- MedLLVMRegistrationCxxStack.h - Private catch stack plans ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MEDLLVMREGISTRATIONCXXSTACK_H
#define NEVERD_MEDLLVMREGISTRATIONCXXSTACK_H

#include "neverd/Common.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <map>

namespace llvm {
class AllocaInst;
class BasicBlock;
class StoreInst;
} // namespace llvm

namespace neverd {
struct MedBlock;

struct RegistrationCxxStackPlan {
  llvm::StoreInst *Seed = nullptr;
  uint32_t Bytes = 0;
};

llvm::Expected<RegistrationCxxStackPlan> prepareRegistrationCxxStack(
    const MedBlock &Source, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Body,
    const std::map<std::pair<int, int>, llvm::AllocaInst *> &Slots,
    llvm::ArrayRef<llvm::StoreInst *> Stores);

void emitRegistrationCxxStack(const RegistrationCxxStackPlan &Plan,
                              va_t FunctionVA, va_t CallbackVA);
} // namespace neverd

#endif // NEVERD_MEDLLVMREGISTRATIONCXXSTACK_H
