//===- LLVMCPasses.h - LLVM C emitter analysis passes --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Analysis passes for the LLVM-route C emitter (dead stores, void analysis).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_PASS_LLVMC_LLVMCPASSES_H
#define NEVERD_BACKEND_C_PASS_LLVMC_LLVMCPASSES_H
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <map>
#include <set>
#include <string>

namespace neverd {

struct LLVMCAnalysisState {
  std::set<const llvm::AllocaInst *> DeadFrameAllocas;
  std::set<const llvm::Instruction *> DeadFrameStores;
  std::set<const llvm::Value *> Inlinable;
  std::map<const llvm::Value *, const llvm::Value *> ForwardedLoads;

  std::set<const llvm::Value *> IntrinsicStructVals;
  std::map<const llvm::Value *, std::string> IntrinsicStructNames;
};

void analyzeDeadFrameStores(LLVMCAnalysisState &State, llvm::Function &Fn);
void analyzeStoreForwarding(LLVMCAnalysisState &State, llvm::Function &Fn);
bool analyzeVoidReturn(const LLVMCAnalysisState &State, llvm::Function &Fn);
void analyzeVoidDeadChain(LLVMCAnalysisState &State, llvm::Function &Fn);
void analyzeIntrinsicStructs(LLVMCAnalysisState &State, llvm::Function &Fn);
bool isCallResultLive(const LLVMCAnalysisState &State,
                      const llvm::CallInst *Call);
const llvm::Value *tryCollapseHiLo(const LLVMCAnalysisState &State,
                                   const llvm::Value *RV);

} // namespace neverd

#endif // NEVERD_BACKEND_C_PASS_LLVMC_LLVMCPASSES_H
