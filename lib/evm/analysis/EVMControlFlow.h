//===- EVMControlFlow.h - Whole-program control-flow analysis --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the private LowIR control-flow analysis stage.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMCONTROLFLOW_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMCONTROLFLOW_H

#include "llvm/Support/Error.h"

namespace neverd::evm {

struct AnalyzeOptions;
struct EVMLowIR;

/// Propagates bounded abstract operand stacks through pre-partitioned LowIR,
/// resolves feasible jump targets, and populates deterministic CFG metadata.
llvm::Error analyzeControlFlow(EVMLowIR &Low, const AnalyzeOptions &Options);

} // namespace neverd::evm

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMCONTROLFLOW_H
