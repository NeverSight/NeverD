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
#include "neverd/ir/med/MedSyscallConvention.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
  /// array so named C locals cannot split a store from a read of its bytes
  /// when one of the two goes through the pointer.
  std::set<std::pair<const llvm::AllocaInst *, int64_t>> RawFrameLocations;
  /// The bytes from each raw location that such an access covers, at least
  /// one: no named local may hold any of them.
  std::map<std::pair<const llvm::AllocaInst *, int64_t>, uint64_t>
      RawFrameExtents;
  /// The raw locations a frame record leaves unnamed too: those a read
  /// through the pointer takes. A record lays its members out as the frame
  /// does, so a write through the pointer and the place a join points at
  /// reach its named members where they are; only separate locals lose them.
  std::set<std::pair<const llvm::AllocaInst *, int64_t>>
      RawFrameLocationsInRecord;
  /// Memory inline asm can observe a span beyond a single argument offset.
  /// Keep the complete allocation as one object, including indexed accesses.
  std::set<const llvm::AllocaInst *> RawFrameAllocas;
  /// A frame address passed to a call or stored to memory. Its consumer may
  /// reach bytes beyond any access the function makes itself.
  struct FrameAddressEscape {
    /// The call, store or atomic operation that passes it on.
    const llvm::Instruction *User = nullptr;
    /// The call argument it is, for a call.
    unsigned Operand = 0;
    std::set<std::pair<const llvm::AllocaInst *, int64_t>> Locations;
    /// Frames it may point into at an offset no proof resolves.
    std::set<const llvm::AllocaInst *> UnresolvedFrames;
  };
  std::vector<FrameAddressEscape> FrameEscapes;
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
/// Number of i32 outputs for the exact CPUID/XGETBV contracts emitted by
/// MedLLVM. Non-CPU assembly returns zero; malformed CPU queries throw.
unsigned x86CPUQueryResultWords(const llvm::CallInst &Call);
/// Exact lifted Linux x64 SYSCALL ABI, including its RAX/R11 result pair.
bool isLinuxX64SyscallInlineAsm(const llvm::CallInst &Call);
/// Exact owned EFLAGS image whose unused result has no source effect.
bool isDiscardableX86FlagRead(Arch TheArch, const llvm::CallInst &Call);
/// True when \p Call is the backend's Windows NT x64 SYSCALL inline asm.
bool isWindowsX64SyscallInlineAsm(const llvm::CallInst &Call);
/// The supervisor call convention whose exact lowering \p Call is on
/// \p TheArch, or null.
const SyscallConvention *modeledSyscallInlineAsm(Arch TheArch,
                                                 const llvm::CallInst &Call);
bool isCallResultLive(const LLVMCAnalysisState &State,
                      const llvm::CallInst *Call);
const llvm::Value *tryCollapseHiLo(const LLVMCAnalysisState &State,
                                   const llvm::Value *RV);

} // namespace neverd

#endif // NEVERD_BACKEND_C_PASS_LLVMC_LLVMCPASSES_H
