//===- ResolverGraphCacheTests.cpp - Frozen proof graph reuse -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"

#include <functional>
#include <memory>
#include <vector>

using namespace neverd;

namespace neverd::detail {

// Exercise the production graph/value query, including its resource account.
// The access shim changes graph facts directly so equal-size mutations cannot
// accidentally pass a test that only adds/removes instructions.
struct ResolverGraphCacheTestAccess {
  struct Answer {
    std::vector<bool> Values;
    std::vector<bool> QueryComplete;
    bool Complete = false;
    size_t Remaining = 0;
    bool operator==(const Answer &) const = default;
  };

  static void seed(CFGBuilder &B, const BinaryImage &Image,
                   unsigned Length = 3) {
    B.CurrentImg = &Image;
    B.JumpTableProofContextComplete = true;
    B.PersistentCFGRoots = {0x100};
    for (unsigned I = 0; I <= Length; ++I) {
      const va_t Address = 0x100 + I;
      CFGBuilder::InsnRecord R{};
      R.Addr = Address;
      R.Size = 1;
      LowOp Op;
      Op.Addr = Address;
      if (I == Length) {
        R.IsRet = true;
        Op.Opcode = NdOp::RETURN;
        Op.addInput(NdVar::tmp((I - 1) * 8, 8));
      } else {
        Op.Opcode = NdOp::COPY;
        Op.Output = NdVar::tmp(I * 8, 8);
        Op.addInput(I ? NdVar::tmp((I - 1) * 8, 8) : NdVar::cst(7, 8));
      }
      R.Ops.push_back(Op);
      B.Insns.emplace(Address, std::move(R));
      B.BlockStarts.insert(Address);
    }
  }

  static Answer query(CFGBuilder &B, size_t Budget = 1000000,
                      unsigned Length = 3, uint32_t Depth = 0,
                      const std::vector<va_t> *Targets = nullptr) {
    CFGBuilder::JumpTableValueQuery Q;
    Q.Candidate = NdVar::tmp((Length - 1) * 8, 8);
    Q.UseAddr = 0x100 + Length;
    Q.UseSeq = 0;
    Q.Alternatives.push_back({NdVar::cst(7, 8), Q.UseAddr, 0, false});
    Answer A;
    A.Remaining = Budget;
    A.Values = B.tableValuesMatchAtUses({Q}, &A.Complete, &A.QueryComplete,
                                        0x100, Targets, &A.Remaining, 0,
                                        nullptr, nullptr, Depth);
    return A;
  }

  static std::shared_ptr<const void> token(const CFGBuilder &B) {
    return B.CachedResolverGraph;
  }

  static void clear(CFGBuilder &B) { B.CachedResolverGraph.reset(); }

  static void changeValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(8, 8);
  }

  static void addBackedge(CFGBuilder &B) {
    auto &R = B.Insns.at(0x103);
    R.IsRet = false;
    R.IsBranch = true;
    R.BranchTarget = 0x100;
    R.Ops.front().Opcode = NdOp::BRANCH;
    R.Ops.front().Inputs[0] = NdVar::cst(0x100, 8);
  }

  static void changeRoot(CFGBuilder &B) {
    B.ActiveJumpTableProofRoots = std::set<va_t>{0x101};
  }

  static void changeConditionalRoot(CFGBuilder &B) {
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void addOwner(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets = {0x101};
    auto &Info = B.ResolvedTableInfo[0x100];
    Info.setBaseAddr(0x102);
    Info.EntrySize = 1;
    Info.MaxEntries = 1;
    B.DiscoveredCodeRefSources[0x102] = {0x100};
  }

  static void changeOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).BaseAddr = 0x103;
  }

  static void changeOwnerTargets(CFGBuilder &B) {
    B.Insns.at(0x100).JumpTableTargets.clear();
  }

  static void explicitOwnerRange(CFGBuilder &B) {
    B.ResolvedTableInfo.at(0x100).StorageRanges = {{0x102, 1, 1, 1}};
  }

  static void rollbackValue(CFGBuilder &B) {
    B.Insns.at(0x100).Ops.front().Inputs[0] = NdVar::cst(7, 8);
  }
};

} // namespace neverd::detail

namespace {
using Access = detail::ResolverGraphCacheTestAccess;

BinaryImage image() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  return Image;
}

void expectColdAnswer(CFGBuilder &B, const Access::Answer &Warm) {
  Access::clear(B);
  EXPECT_EQ(Access::query(B), Warm);
}

TEST(ResolverGraphCache, ReusesFrozenGraphAndPaysEveryColdBudgetBoundary) {
  const auto Image = image();
  CFGBuilder Warm;
  Access::seed(Warm, Image);
  const auto First = Access::query(Warm);
  ASSERT_EQ(First.Values, (std::vector<bool>{true}));
  ASSERT_TRUE(First.Complete);
  const auto Token = Access::token(Warm);
  ASSERT_TRUE(Token);
  EXPECT_EQ(Access::query(Warm), First);
  EXPECT_EQ(Access::token(Warm), Token);

  const size_t Work = 1000000 - First.Remaining;
  ASSERT_LT(Work, 50000u);
  for (size_t Budget = 0; Budget <= Work + 1; ++Budget) {
    SCOPED_TRACE(Budget);
    CFGBuilder Cold;
    Access::seed(Cold, Image);
    EXPECT_EQ(Access::query(Warm, Budget), Access::query(Cold, Budget));
  }
}

TEST(ResolverGraphCache, EqualCountsDoNotHideBackedgesRootsOrChangedOps) {
  const auto Image = image();
  for (auto Mutate : {Access::changeValue, Access::addBackedge,
                      Access::changeRoot, Access::changeConditionalRoot}) {
    CFGBuilder B;
    Access::seed(B, Image);
    ASSERT_TRUE(Access::query(B).Complete);
    const auto Before = Access::token(B);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, StorageOwnershipAndCallbackChargesRemainCurrent) {
  const auto Image = image();
  for (auto Mutate : {Access::changeOwnerRange, Access::changeOwnerTargets,
                      Access::explicitOwnerRange}) {
    CFGBuilder B;
    Access::seed(B, Image);
    Access::addOwner(B);
    const auto First = Access::query(B);
    ASSERT_TRUE(First.Complete);
    const auto Before = Access::token(B);
    ASSERT_TRUE(Before);
    Mutate(B);
    const auto After = Access::query(B);
    EXPECT_NE(Access::token(B), Before);
    expectColdAnswer(B, After);
  }
}

TEST(ResolverGraphCache, OverridePayloadAndRolledBackPayloadAreOwned) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image);
  std::vector<va_t> Targets{0x101};
  const auto First = Access::query(B, 1000000, 3, 0, &Targets);
  const auto FirstToken = Access::token(B);
  Targets.front() = 0x102;
  const auto Changed = Access::query(B, 1000000, 3, 0, &Targets);
  EXPECT_NE(Access::token(B), FirstToken);
  Access::clear(B);
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), Changed);
  Targets.front() = 0x101;
  EXPECT_EQ(Access::query(B, 1000000, 3, 0, &Targets), First);

  Access::clear(B);
  const auto BeforeMutation = Access::query(B);
  const auto Frozen = Access::token(B);
  Access::changeValue(B);
  EXPECT_NE(Access::query(B).Values, BeforeMutation.Values);
  EXPECT_NE(Access::token(B), Frozen);
  Access::rollbackValue(B);
  EXPECT_EQ(Access::query(B), BeforeMutation);
}

TEST(ResolverGraphCache, ResourceFailureAndAnalysisIncompleteAreDistinct) {
  const auto Image = image();
  CFGBuilder B;
  Access::seed(B, Image, 16);
  const auto Exhausted = Access::query(B, 1, 16);
  EXPECT_FALSE(Exhausted.Complete);
  EXPECT_EQ(Exhausted.Remaining, 0u);
  EXPECT_FALSE(Access::token(B));

  const auto Incomplete = Access::query(B, 1000000, 16, 1);
  EXPECT_FALSE(Incomplete.Complete);
  EXPECT_GT(Incomplete.Remaining, 0u);
  const auto Graph = Access::token(B);
  ASSERT_TRUE(Graph);
  EXPECT_EQ(Access::query(B, 1000000, 16, 1), Incomplete);
  const auto Complete = Access::query(B, 1000000, 16);
  EXPECT_TRUE(Complete.Complete);
  EXPECT_EQ(Complete.Values, (std::vector<bool>{true}));
  EXPECT_EQ(Access::token(B), Graph);
}

} // namespace
