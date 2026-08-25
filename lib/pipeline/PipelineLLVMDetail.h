//===- PipelineLLVMDetail.h - LLVM emission sharding details -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H

#include "neverd/ir/med/MedIR.h"

#include <vector>

namespace neverd::pipeline_detail {

struct LLVMShardPlan {
  unsigned NumShards = 1;
  std::vector<unsigned> ShardOf;
};

/// Plan deterministic LPT emission shards. C++ EH contributions with the same
/// nonzero native FuncInfo identity are one indivisible weighted unit.
LLVMShardPlan planLLVMEmissionShards(const std::vector<MedFunc> &Funcs,
                                     unsigned NumThreads);

} // namespace neverd::pipeline_detail

#endif // NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H
