//===- PipelineLLVMShardingTests.cpp -------------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PipelineLLVMDetail.h"
#include "gtest/gtest.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace {

MedFunc weightedFunction(va_t Entry, size_t Weight,
                         std::optional<va_t> NativeFuncInfoVA = std::nullopt) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "sub_" + std::to_string(Entry);

  MedBlock Block;
  Block.Id = 0;
  Block.Ops.resize(Weight - 1);
  Func.Blocks.push_back(std::move(Block));

  if (NativeFuncInfoVA) {
    Func.ExceptionMetadata.emplace();
    Func.ExceptionMetadata->Cxx.emplace();
    Func.ExceptionMetadata->Cxx->NativeFuncInfoVA = *NativeFuncInfoVA;
  }
  return Func;
}

TEST(PipelineLLVMSharding, KeepsSharedNativeCxxFunctionInfoInOneWeightedUnit) {
  constexpr va_t Group = 0x140003040;
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x140001000, 100, Group));
  Funcs.push_back(weightedFunction(0x140001100, 80));
  Funcs.push_back(weightedFunction(0x140001200, 90, Group));
  Funcs.push_back(weightedFunction(0x140001300, 70));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs, 2);

  ASSERT_EQ(Plan.NumShards, 2u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[2]);
  EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[3]);
  EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
}

TEST(PipelineLLVMSharding, DoesNotGroupZeroNativeFunctionInfoIdentities) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x140001000, 100, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001100, 90, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001200, 80, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001300, 70, va_t{0}));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs, 2);

  ASSERT_EQ(Plan.NumShards, 2u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
  EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[3]);
  EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[2]);
}

TEST(PipelineLLVMSharding, DoesNotCreateEmptyShardsForOneIndivisibleGroup) {
  constexpr va_t Group = 0x140003040;
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x140001000, 100, Group));
  Funcs.push_back(weightedFunction(0x140001100, 90, Group));
  Funcs.push_back(weightedFunction(0x140001200, 80, Group));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs, 64);

  ASSERT_EQ(Plan.NumShards, 1u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_EQ(Plan.ShardOf[0], 0u);
  EXPECT_EQ(Plan.ShardOf[1], 0u);
  EXPECT_EQ(Plan.ShardOf[2], 0u);
}

} // namespace
} // namespace neverd
