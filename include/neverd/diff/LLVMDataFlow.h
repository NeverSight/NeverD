//===- LLVMDataFlow.h - Optimized machine SSA -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded structural projection of verified LLVM integer SSA. This adapter
/// observes the ordinary machine pipeline; it does not optimize or prove it.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_LLVMDATAFLOW_H
#define NEVERD_DIFF_LLVMDATAFLOW_H

#include "neverd/diff/DataFlow.h"

namespace llvm {
class Function;
}
namespace neverd {
class LLVMSourceMap;
namespace diff {
class LLVMDataFlowUnsupported : public std::runtime_error {
public:
  explicit LLVMDataFlowUnsupported(const std::string &Reason)
      : std::runtime_error("optimized SSA unavailable: " + Reason) {}
};

/// Only direct emission observations surviving replacement/deletion are
/// retained. Unsupported contracts refuse the whole projection; interrupted
/// work never publishes a graph missing definitions or origins.
DataFlowGraph buildLLVMDataFlow(const llvm::Function &Function,
                                const LLVMSourceMap &Sources, uint64_t Entry,
                                llvm::StringRef Revision, llvm::StringRef ABI,
                                bool NativeComplete,
                                const DataFlowOptions &Options = {},
                                const ComparisonControl &Control = {});
/// Observe a native i64 parameter as i32 only when every use is an origin-free,
/// unflagged truncation and its recorded native site is unambiguous. The source
/// signature and elision provenance remain in the detached graph.
DataFlowGraph buildLLVMParameterDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Include parameter views and independently derived finite-execution
/// contracts. Unsupported cyclic proofs retain the exact source attributes.
DataFlowGraph buildLLVMProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for single-entry multi-block counter components.
DataFlowGraph buildLLVMMultiBlockProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for nested single-entry counter components.
DataFlowGraph buildLLVMNestedProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for multiple guarded paths sharing a counter.
DataFlowGraph buildLLVMBranchProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for affine guards and modular counter exit coverage.
DataFlowGraph buildLLVMModularProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for shared parameter-relative modular exit guards.
DataFlowGraph buildLLVMParameterBoundProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
/// Versioned extension for shared input-derived expression modular exit guards.
DataFlowGraph buildLLVMInvariantBoundProgressDataFlow(
    const llvm::Function &Function, const LLVMSourceMap &Sources,
    uint64_t Entry, llvm::StringRef Revision, llvm::StringRef ABI,
    bool NativeComplete, const DataFlowOptions &Options = {},
    const ComparisonControl &Control = {});
} // namespace diff
} // namespace neverd

#endif // NEVERD_DIFF_LLVMDATAFLOW_H
