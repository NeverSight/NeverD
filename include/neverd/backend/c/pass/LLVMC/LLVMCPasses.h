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

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd {

struct LLVMCAnalysisState {
  std::set<const llvm::AllocaInst *> DeadFrameAllocas;
  std::set<const llvm::Instruction *> DeadFrameStores;
  /// Complete, unique SSA address proofs shared with the C frame-slot printer.
  /// A PHI read must use the same backing as its incoming stores. Mutable
  /// scalar-home loads need a separate reaching-store proof.
  std::map<const llvm::Value *, std::pair<const llvm::AllocaInst *, int64_t>>
      FramePointerLocations;
  /// Frame offsets reached through an ambiguous carrier stay in the backing
  /// array so named C locals cannot split stores from later pointer reads.
  std::set<std::pair<const llvm::AllocaInst *, int64_t>> RawFrameLocations;
  /// Memory inline asm can observe a span beyond a single argument offset.
  /// Keep the complete allocation as one object, including indexed accesses.
  std::set<const llvm::AllocaInst *> RawFrameAllocas;
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
/// Exact lifted Linux x64 SYSCALL ABI, including its RAX/R11 result pair.
bool isLinuxX64SyscallInlineAsm(const llvm::CallInst &Call);
bool isCallResultLive(const LLVMCAnalysisState &State,
                      const llvm::CallInst *Call);
const llvm::Value *tryCollapseHiLo(const LLVMCAnalysisState &State,
                                   const llvm::Value *RV);

} // namespace neverd

#endif // NEVERD_BACKEND_C_PASS_LLVMC_LLVMCPASSES_H
