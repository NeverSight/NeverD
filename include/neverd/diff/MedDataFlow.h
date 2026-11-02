//===- MedDataFlow.h - Project verified MedIR into diff evidence -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only adapter over the pipeline's SSA, native instruction provenance
/// and CFG. It does not infer instruction semantics or a missing call ABI.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_MEDDATAFLOW_H
#define NEVERD_DIFF_MEDDATAFLOW_H

#include "neverd/diff/DataFlow.h"

namespace neverd {
struct MedFunc;
namespace diff {

/// Throws on malformed SSA, exhausted allowance or cancellation. The caller
/// retains the explicit unavailable reason rather than publishing a partial
/// graph with missing definition edges. Revision/ABI belong to the native
/// snapshot; NativeComplete describes its decode/discovery coverage.
DataFlowGraph buildMedDataFlow(const MedFunc &Function,
                               llvm::StringRef Revision, llvm::StringRef ABI,
                               bool NativeComplete,
                               const DataFlowOptions &Options = {},
                               const ComparisonControl &Control = {});

} // namespace diff
} // namespace neverd

#endif // NEVERD_DIFF_MEDDATAFLOW_H
