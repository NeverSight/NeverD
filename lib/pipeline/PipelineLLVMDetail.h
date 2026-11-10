//===- PipelineLLVMDetail.h - LLVM emission sharding details -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H

#include "neverd/pipeline/Pipeline.h"

#include <string>
#include <utility>
#include <vector>

namespace neverd {

struct Pipeline::LLVMEmissionResult {
  std::unique_ptr<llvm::Module> Module;
  std::shared_ptr<LLVMSourceMap> Sources;
  uint64_t UnhandledValueIntrinsics = 0;
  bool LLVMVerifierFailed = false;
  std::string Error;
  /// The functions the module declares only, by index into the emitted
  /// functions, with why each body refused: their shard was emitted again
  /// without them.
  std::vector<std::pair<size_t, std::string>> LeftOut;
};

namespace pipeline_detail {

struct LLVMShardPlan {
  unsigned NumShards = 1;
  std::vector<unsigned> ShardOf;
};

/// Plan work-budget LPT emission shards independently of worker count. Native
/// C++ groups, SEH parent/callback closure and interior-code dependencies stay
/// in one weighted unit. Image supplies instruction-mode address normalization.
LLVMShardPlan planLLVMEmissionShards(const std::vector<MedFunc> &Funcs,
                                     const BinaryImage *Image = nullptr);

} // namespace pipeline_detail
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINELLVMDETAIL_H
