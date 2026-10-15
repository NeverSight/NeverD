//===- ResolverDefinitionCacheTests.cpp - Definition search replay --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks search identity and exact cold/warm evidence charges.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverDefinitionCache.h"

#include <array>
#include <vector>

using namespace neverd;
using detail::ResolverDefinitionCache;
using detail::ResolverLaneViews;

namespace {

struct Answer {
  std::optional<int> Position;
  size_t Remaining;
  bool operator==(const Answer &) const = default;
};

Answer search(ResolverDefinitionCache &Cache, llvm::ArrayRef<LowOp> Ops,
              int Before, NdVar Value, size_t Budget, int Block = 0,
              Arch Target = Arch::X64) {
  const auto &TRI = getTargetRegInfo(Target);
  ResolverLaneViews Views(TRI);
  const auto Position =
      Cache.previous(Block, Ops, Before, Value, Views.get(Value), TRI, Views,
                     [&](size_t Work) {
                       if (Work > Budget) {
                         Budget = 0;
                         return false;
                       }
                       Budget -= Work;
                       return true;
                     });
  return {Position, Budget};
}

std::vector<LowOp> operations() {
  std::vector<LowOp> Ops(34);
  const auto Write = [&](int I, NdVar V) {
    Ops[I].Opcode = NdOp::COPY;
    Ops[I].Output = V;
    Ops[I].addInput(NdVar::cst(7, V.Size));
  };
  Write(18, NdVar::tmp(7, 8));
  Write(20, NdVar::reg(0, 8));
  Write(24, NdVar::reg(1, 1)); // AH does not replace AL.
  Write(26, NdVar::reg(0, 4)); // EAX also defines the upper RAX bytes.
  Ops[30].Opcode = NdOp::CALL;
  Ops[30].Output = NdVar::reg(8, 8);
  Write(32, NdVar::reg(16, 8));
  return Ops;
}

} // namespace

TEST(ResolverDefinitionCache, ColdAndWarmPayEveryOriginalScanBoundary) {
  const auto Ops = operations();
  struct Case {
    NdVar Value;
    int Before;
    int Expected;
  };
  const Case Cases[] = {
      {NdVar::reg(0, 1), 34, 30}, {NdVar::reg(0, 1), 30, 26},
      {NdVar::reg(0, 1), 26, 24}, {NdVar::reg(1, 1), 26, 24},
      {NdVar::reg(0, 8), 30, 26}, {NdVar::reg(0, 8), 26, 24},
      {NdVar::reg(0, 8), 24, 20}, {NdVar::reg(16, 8), 34, 32},
      {NdVar::tmp(7, 8), 34, 18}, {NdVar::tmp(8, 8), 34, -1},
      {NdVar::reg(0, 8), 0, -1},  {NdVar::reg(0, 8), 15, -1},
  };
  // The AL search also returns AH as a possible writer in the same container.
  // The consuming proof owns the subsequent byte-overlap check.
  for (const auto &C : Cases) {
    SCOPED_TRACE(::testing::Message()
                 << C.Value.Offset << ':' << C.Value.Size << '@' << C.Before);
    ResolverDefinitionCache Warm;
    ASSERT_EQ(search(Warm, Ops, C.Before, C.Value, 100).Position, C.Expected);
    const size_t Work = C.Before - std::max(C.Expected, 0);
    for (size_t Budget = 0; Budget <= size_t(C.Before) + 1; ++Budget) {
      SCOPED_TRACE(Budget);
      ResolverDefinitionCache Cold;
      const Answer Expected{Budget < Work ? std::nullopt
                                          : std::optional<int>(C.Expected),
                            Budget < Work ? 0 : Budget - Work};
      EXPECT_EQ(search(Cold, Ops, C.Before, C.Value, Budget), Expected);
      EXPECT_EQ(search(Warm, Ops, C.Before, C.Value, Budget), Expected);
    }
  }
}

TEST(ResolverDefinitionCache, CallsRemainCandidatesForTheAbiConsumer) {
  for (NdOp Call : {NdOp::CALL, NdOp::INDIR_CALL}) {
    auto Ops = operations();
    Ops[30].Opcode = Call;
    ResolverDefinitionCache Cache;
    // Caller-saved, callee-saved, explicit call output, and an unknown lane
    // all stop here. Only the value proof may decide whether a call preserves
    // a register. Temporaries are not implicit call definitions.
    for (NdVar V : {NdVar::reg(0, 8), NdVar::reg(24, 8), NdVar::reg(8, 8),
                    NdVar::reg(100000, 8)}) {
      EXPECT_EQ(search(Cache, Ops, 32, V, 100).Position, 30);
      EXPECT_EQ(search(Cache, Ops, 32, V, 100).Position, 30);
    }
    EXPECT_EQ(search(Cache, Ops, 32, NdVar::tmp(7, 8), 100).Position, 18);
  }
}

TEST(ResolverDefinitionCache, IncompleteSearchAndArchitectureChangesStayCold) {
  const auto Ops = operations();
  ResolverDefinitionCache Cache;
  const auto HighByte = NdVar::reg(1, 1);
  EXPECT_FALSE(search(Cache, Ops, 24, HighByte, 1).Position);
  EXPECT_EQ(search(Cache, Ops, 24, HighByte, 100).Position, 20);
  // ARM has no x86 high-byte alias at offset one. Sharing a graph does not
  // permit borrowing another architecture's register decomposition.
  EXPECT_EQ(search(Cache, Ops, 24, HighByte, 100, 0, Arch::ARM).Position, -1);
  EXPECT_EQ(search(Cache, Ops, 24, HighByte, 100).Position, 20);
}

TEST(ResolverDefinitionCache,
     EvictionAndIndependentGraphsKeepTheirOwnPositions) {
  std::array<std::vector<LowOp>, 2> Blocks = {std::vector<LowOp>(32),
                                              std::vector<LowOp>(32)};
  for (int I = 0; I != 2; ++I) {
    Blocks[I][I + 1].Opcode = NdOp::COPY;
    Blocks[I][I + 1].Output = NdVar::tmp(0, 8);
  }
  ResolverDefinitionCache Cache;
  for (int Round = 0; Round != 2; ++Round)
    for (int Block = 0; Block != 10000; ++Block)
      ASSERT_EQ(
          search(Cache, Blocks[Block % 2], 32, NdVar::tmp(0, 8), 100, Block)
              .Position,
          Block % 2 + 1);
  // Replacing a graph also replaces its cache, even if the new operations
  // have the same count, block ID and queried lane.
  ResolverDefinitionCache Replacement;
  EXPECT_EQ(search(Replacement, Blocks[1], 32, NdVar::tmp(0, 8), 100).Position,
            2);
  EXPECT_EQ(search(Cache, Blocks[0], 32, NdVar::tmp(0, 8), 100).Position, 1);
}
