//===- LLVMCSourceFunctions.h - Selected definition closure -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Select one native function and the source-only bodies it depends on.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_LLVMC_SOURCEFUNCTIONS_H
#define NEVERD_BACKEND_C_LLVMC_SOURCEFUNCTIONS_H

#include "neverd/backend/llvm/LLVMSourceBody.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"

namespace neverd::llvmc {

class SourceFunctions {
public:
  explicit SourceFunctions(const llvm::Function *Only = nullptr)
      : WholeModule(!Only) {
    if (!Only)
      return;
    Selected.push_back(Only);
    Members.insert(Only);
    for (size_t I = 0; I != Selected.size(); ++I)
      for (const auto &Instruction : llvm::instructions(Selected[I])) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
        const auto *Callee = Call ? Call->getCalledFunction() : nullptr;
        if (const auto *Pad = llvm::dyn_cast<llvm::CatchPadInst>(&Instruction);
            Pad && Pad->arg_size())
          Callee = llvm::dyn_cast<llvm::Function>(
              Pad->getArgOperand(0)->stripPointerCasts());
        if (Callee && !Callee->isDeclaration() &&
            Callee->hasFnAttribute(LLVMSourceBody) &&
            Members.insert(Callee).second)
          Selected.push_back(Callee);
      }
  }

  bool contains(const llvm::Function &Function) const {
    return WholeModule || Members.contains(&Function);
  }
  llvm::ArrayRef<const llvm::Function *> selected() const { return Selected; }

private:
  bool WholeModule;
  llvm::SmallVector<const llvm::Function *, 4> Selected;
  llvm::SmallPtrSet<const llvm::Function *, 4> Members;
};

} // namespace neverd::llvmc

#endif // NEVERD_BACKEND_C_LLVMC_SOURCEFUNCTIONS_H
