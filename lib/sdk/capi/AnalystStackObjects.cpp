//===- AnalystStackObjects.cpp - Private source frame preservation --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../backend/c/LLVMC/LLVMCFrameLayout.h"
#include "SessionImpl.h"

#include "neverd/analysis/LLVMPrivateFrameAccesses.h"

using namespace neverd;

void neverd::sdk::preserveAnalystStackObjects(llvm::Module &M) {
  Pipeline::promoteScaffoldingAllocas(M);
  canonicalizePrivateFrameAccesses(M);
  for (auto &F : M) {
    bool HasFrame = false;
    for (auto &B : F)
      for (auto &I : B)
        if (auto *A = llvm::dyn_cast<llvm::AllocaInst>(&I))
          if (auto Base =
                  llvmc::syntheticFrameBaseOffset(*A, M.getDataLayout())) {
            HasFrame = true;
            A->setMetadata(
                llvmc::CapturedFrameBase,
                llvm::MDNode::get(
                    M.getContext(),
                    llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(M.getContext()), *Base))));
          }
    // A user named the physical storage, so scalar replacement must retain
    // it. This optimization barrier belongs only to the private C projection;
    // it adds no machine instruction, memory effect or native ABI authority.
    if (HasFrame) {
      F.removeFnAttr(llvm::Attribute::AlwaysInline);
      F.addFnAttr(llvm::Attribute::NoInline);
      F.addFnAttr(llvm::Attribute::OptimizeNone);
    }
  }
}
