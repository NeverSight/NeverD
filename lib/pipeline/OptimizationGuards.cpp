//===- OptimizationGuards.cpp - Preserve checked optimization boundaries --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps interprocedural transforms from invalidating optnone contracts.

#include "OptimizationGuards.h"

#include "neverd/backend/llvm/WindowsSEHEmulatedFrame.h"

#include "llvm/ADT/Any.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassInstrumentation.h"

namespace neverd {

void registerOptNoneInterproceduralGuard(
    llvm::PassInstrumentationCallbacks &Callbacks) {
  Callbacks.registerShouldRunOptionalPassCallback(
      [](llvm::StringRef, llvm::Any IR) {
        const llvm::Module *M = nullptr;
        if (const auto *Module = llvm::any_cast<const llvm::Module *>(&IR))
          M = *Module;
        else if (const auto *SCC =
                     llvm::any_cast<const llvm::LazyCallGraph::SCC *>(&IR)) {
          if ((*SCC)->begin() != (*SCC)->end())
            M = (*SCC)->begin()->getFunction().getParent();
        }
        if (!M)
          return true;

        // LLVM's optnone instrumentation only guards function/loop passes.
        // GlobalOpt can still rewrite a protected CFG, and dead-argument
        // elimination can replace checked callback arguments with poison.
        // A call-graph pass on an ordinary caller can also affect a protected
        // callee's ABI, so use the same module-wide boundary for both. Required
        // adaptors still run and optimize ordinary functions independently.
        // An SEH filter adapter (sehFilterAdapterName) is the exception: no
        // code calls it and the runtime takes its address, so no such pass
        // reaches its arguments, and the filter it calls is a lifted function
        // with external linkage, whose signature no pass changes.  Output
        // checks the adapter's exact shape again.  It must not turn these
        // passes off for its module.
        for (const llvm::Function &F : *M)
          if (!F.isDeclaration() && F.hasOptNone() && !isSEHFilterAdapter(F))
            return false;
        return true;
      });
}

} // namespace neverd
