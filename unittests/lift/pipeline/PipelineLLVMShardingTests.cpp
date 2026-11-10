//===- PipelineLLVMShardingTests.cpp -------------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PipelineLLVMDetail.h"
#include "gtest/gtest.h"

#include "neverd/support/Parallel.h"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd {
namespace {

struct ScopedThreadCount {
  unsigned Previous = workerThreadOverride().load();
  explicit ScopedThreadCount(unsigned Count) { setWorkerThreadCount(Count); }
  ~ScopedThreadCount() { setWorkerThreadCount(Previous); }
};

MedFunc weightedFunction(va_t Entry, size_t Weight,
                         std::optional<va_t> NativeFuncInfoVA = std::nullopt) {
  MedFunc Func;
  Func.Entry = Entry;
  Func.Name = "sub_" + std::to_string(Entry);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  Block.EndAddr = Entry + 0x10;
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
  Funcs.push_back(weightedFunction(0x140001000, 750, Group));
  Funcs.push_back(weightedFunction(0x140001100, 600));
  Funcs.push_back(weightedFunction(0x140001200, 675, Group));
  Funcs.push_back(weightedFunction(0x140001300, 525));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs);

  ASSERT_EQ(Plan.NumShards, 2u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[2]);
  EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[3]);
  EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
}

TEST(PipelineLLVMSharding, DoesNotGroupZeroNativeFunctionInfoIdentities) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x140001000, 750, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001100, 675, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001200, 600, va_t{0}));
  Funcs.push_back(weightedFunction(0x140001300, 525, va_t{0}));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs);

  ASSERT_EQ(Plan.NumShards, 2u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
  EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[3]);
  EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[2]);
}

TEST(PipelineLLVMSharding, DoesNotCreateEmptyShardsForOneIndivisibleGroup) {
  constexpr va_t Group = 0x140003040;
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x140001000, 750, Group));
  Funcs.push_back(weightedFunction(0x140001100, 675, Group));
  Funcs.push_back(weightedFunction(0x140001200, 600, Group));

  const pipeline_detail::LLVMShardPlan Plan =
      pipeline_detail::planLLVMEmissionShards(Funcs);

  ASSERT_EQ(Plan.NumShards, 1u);
  ASSERT_EQ(Plan.ShardOf.size(), Funcs.size());
  EXPECT_EQ(Plan.ShardOf[0], 0u);
  EXPECT_EQ(Plan.ShardOf[1], 0u);
  EXPECT_EQ(Plan.ShardOf[2], 0u);
}

TEST(PipelineLLVMSharding, KeepsSEHCallbacksWithEveryReferencingParent) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(weightedFunction(0x1000, 750));
  Funcs.push_back(weightedFunction(0x2000, 725));
  Funcs.push_back(weightedFunction(0x3000, 700));
  Funcs.push_back(weightedFunction(0x4000, 675));
  Funcs.push_back(weightedFunction(0x5000, 650));
  for (size_t Parent : {size_t{0}, size_t{2}}) {
    auto &EH = Funcs[Parent].ExceptionMetadata.emplace();
    auto &SEH = EH.SEH.emplace();
    SEHScopeRecord Scope;
    Scope.Kind = Parent == 0 ? SEHScopeKind::Filter : SEHScopeKind::Finally;
    Scope.FilterOrFinallyVA = 0x2000;
    SEH.Scopes.push_back(Scope);
  }
  for (unsigned Threads : {1u, 2u, 4u}) {
    SCOPED_TRACE(Threads);
    ScopedThreadCount Workers(Threads);
    const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
    ASSERT_GT(Plan.NumShards, 1u);
    EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[1]);
    EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[2]);
    EXPECT_NE(Plan.ShardOf[1], Plan.ShardOf[3]);
  }
}

TEST(PipelineLLVMSharding, WorkerCountDoesNotChangeOptimizationNeighbors) {
  ScopedThreadCount OneWorker(1);
  std::vector<MedFunc> Funcs;
  for (size_t I = 0; I != 12; ++I)
    Funcs.push_back(weightedFunction(0x1000 + I * 0x100, 225 + I));
  const auto Serial = pipeline_detail::planLLVMEmissionShards(Funcs);
  ASSERT_GT(Serial.NumShards, 1u);
  for (unsigned Threads : {2u, 4u, 64u}) {
    SCOPED_TRACE(Threads);
    ScopedThreadCount Workers(Threads);
    const auto Parallel = pipeline_detail::planLLVMEmissionShards(Funcs);
    EXPECT_EQ(Serial.NumShards, Parallel.NumShards);
    EXPECT_EQ(Serial.ShardOf, Parallel.ShardOf);
  }
}

TEST(PipelineLLVMSharding,
     InteriorCodeDependenciesDoNotFollowScalarCoincidences) {
  for (auto Provenance :
       {ConstantAddressProvenance::CodeAddress,
        ConstantAddressProvenance::Address, ConstantAddressProvenance::Scalar,
        ConstantAddressProvenance::DataAddress}) {
    SCOPED_TRACE(unsigned(Provenance));
    std::vector<MedFunc> Funcs;
    for (size_t I = 0; I != 5; ++I)
      Funcs.push_back(weightedFunction(0x1000 + I * 0x1000, 750));
    Funcs[0].Blocks[0].Ops[0].addInput(
        MedVar::makeConst(0x2004, 8, Provenance));
    const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
    ASSERT_GT(Plan.NumShards, 1u);
    if (Provenance == ConstantAddressProvenance::CodeAddress ||
        Provenance == ConstantAddressProvenance::Address)
      EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[1]);
    else
      EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
  }
}

TEST(PipelineLLVMSharding, EntryDeclarationsAndHalfOpenEndsDoNotNeedBodies) {
  for (va_t Target : {va_t{0x2000}, va_t{0x2010}}) {
    SCOPED_TRACE(Target);
    std::vector<MedFunc> Funcs;
    for (size_t I = 0; I != 5; ++I)
      Funcs.push_back(weightedFunction(0x1000 + I * 0x1000, 750));
    Funcs[0].Blocks[0].Ops[0].addInput(
        MedVar::makeConst(Target, 8, ConstantAddressProvenance::CodeAddress));
    const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
    EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
  }
}

TEST(PipelineLLVMSharding, LegacyAndZeroLabelsRetainExactAddressRoles) {
  for (va_t Target : {va_t{0}, va_t{0x2004}}) {
    for (auto Provenance : {ConstantAddressProvenance::CodeAddress,
                            ConstantAddressProvenance::Scalar}) {
      SCOPED_TRACE(::testing::Message()
                   << Target << ':' << unsigned(Provenance));
      std::vector<MedFunc> Funcs;
      for (size_t I = 0; I != 5; ++I)
        Funcs.push_back(weightedFunction(0x1000 + I * 0x1000, 750));
      Funcs[1].Blocks[0].StartAddr = 0;
      Funcs[1].Blocks[0].EndAddr = 0;
      Funcs[1].Blocks[0].Ops[0].Addr = Target;
      Funcs[0].Blocks[0].Ops[0].addInput(
          MedVar::makeConst(Target, 8, Provenance));
      const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
      if (Provenance == ConstantAddressProvenance::CodeAddress)
        EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[1]);
      else
        EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
    }
  }
}

TEST(PipelineLLVMSharding, UnwindFragmentsDoNotClaimTheGapBetweenThem) {
  std::vector<MedFunc> Funcs;
  for (size_t I = 0; I != 5; ++I)
    Funcs.push_back(weightedFunction(0x1000 + I * 0x1000, 750));
  Funcs[1].OriginalSize = 0x2010;
  auto &EH = Funcs[1].ExceptionMetadata.emplace();
  EH.CodeRange = {0x2000, 0x2010};
  EH.FragmentRanges.push_back({0x4000, 0x4010});
  Funcs[0].Blocks[0].Ops[0].addInput(
      MedVar::makeConst(0x3004, 8, ConstantAddressProvenance::CodeAddress));
  const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
  EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[2]);
  EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[3]);
  EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
}

TEST(PipelineLLVMSharding, FullWidthAddressKeysRetainIndependentGroups) {
  for (va_t Identity : {std::numeric_limits<va_t>::max(),
                        std::numeric_limits<va_t>::max() - 1}) {
    SCOPED_TRACE(Identity);
    std::vector<MedFunc> Funcs;
    Funcs.push_back(weightedFunction(0x1000, 2125, Identity));
    Funcs.push_back(weightedFunction(Identity, 2125));
    Funcs.push_back(weightedFunction(0x3000, 2125, Identity));
    Funcs.push_back(weightedFunction(0x4000, 2125));
    auto &SEH = Funcs[3].ExceptionMetadata.emplace().SEH.emplace();
    SEHScopeRecord Scope;
    Scope.Kind = SEHScopeKind::Filter;
    Scope.FilterOrFinallyVA = Identity;
    SEH.Scopes.push_back(Scope);
    const auto Plan = pipeline_detail::planLLVMEmissionShards(Funcs);
    ASSERT_EQ(Plan.NumShards, 2u);
    EXPECT_EQ(Plan.ShardOf[0], Plan.ShardOf[2]);
    EXPECT_EQ(Plan.ShardOf[1], Plan.ShardOf[3]);
    EXPECT_NE(Plan.ShardOf[0], Plan.ShardOf[1]);
  }
}

} // namespace
} // namespace neverd
