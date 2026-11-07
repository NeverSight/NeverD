//===- ResolverMemoryDefinitionCacheTests.cpp - Memory scan replay --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "jumptable/ResolverMemoryDefinitionCache.h"

#include <array>
#include <vector>

using namespace neverd;
using detail::ResolverMemoryDefinitionCache;

namespace {

struct Answer {
  std::optional<int> Position;
  size_t Remaining;
  bool operator==(const Answer &) const = default;
};

Answer search(ResolverMemoryDefinitionCache &Cache, llvm::ArrayRef<LowOp> Ops,
              int Before, size_t Budget, int Block = 0) {
  auto Position = Cache.previous(Block, Ops, Before, [&](size_t Work) {
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
  std::vector<LowOp> Ops(40);
  for (auto &Op : Ops) {
    Op.Opcode = NdOp::COPY;
    Op.Output = NdVar::tmp(0, 8);
    Op.addInput(NdVar::scalar(7, 8));
  }
  Ops[4].Opcode = NdOp::STORE;
  Ops[10].Opcode = NdOp::CALL;
  Ops[16].Opcode = NdOp::INTRINSIC;
  Ops[22].Opcode = NdOp::ATOMIC_ADD;
  Ops[28].Opcode = NdOp::INDIR_CALL;
  return Ops;
}

} // namespace

TEST(ResolverMemoryDefinitionCache,
     EveryColdAndWarmScanBoundaryPaysOriginalWork) {
  const auto Ops = operations();
  for (int Before = -1; Before <= 42; ++Before) {
    SCOPED_TRACE(Before);
    const int End = std::clamp(Before, 0, static_cast<int>(Ops.size()));
    int ExpectedPosition = -1;
    for (int Candidate : {4, 10, 16, 22, 28})
      if (Candidate < End)
        ExpectedPosition = Candidate;
    const size_t Work = End - std::max(ExpectedPosition, 0);
    ResolverMemoryDefinitionCache Warm;
    ASSERT_EQ(search(Warm, Ops, Before, 100).Position, ExpectedPosition);
    for (size_t Budget = 0; Budget <= Ops.size() + 1; ++Budget) {
      SCOPED_TRACE(Budget);
      ResolverMemoryDefinitionCache Cold;
      const Answer Expected{
          Budget < Work ? std::nullopt : std::optional<int>(ExpectedPosition),
          Budget < Work ? 0 : Budget - Work};
      EXPECT_EQ(search(Cold, Ops, Before, Budget), Expected);
      EXPECT_EQ(search(Warm, Ops, Before, Budget), Expected);
    }
  }
}

TEST(ResolverMemoryDefinitionCache,
     BarriersAndMalformedWritersReachTheConsumer) {
  // No operand or intrinsic-ID assumption may turn a potential writer into a
  // skipped operation. The value proof still owns all effect/shape decisions.
  for (NdOp Opcode :
       {NdOp::STORE, NdOp::CALL, NdOp::INDIR_CALL, NdOp::ATOMIC_XCHG,
        NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG, NdOp::INTRINSIC}) {
    SCOPED_TRACE(static_cast<unsigned>(Opcode));
    std::vector<LowOp> Ops(40);
    Ops[3].Opcode = Opcode;
    ResolverMemoryDefinitionCache Cache;
    EXPECT_FALSE(search(Cache, Ops, 40, 1).Position);
    EXPECT_EQ(search(Cache, Ops, 40, 100).Position, 3);
    EXPECT_EQ(search(Cache, Ops, 40, 37), (Answer{3, 0}));
    EXPECT_EQ(search(Cache, Ops, 40, 36), (Answer{std::nullopt, 0}));
  }
}

TEST(ResolverMemoryDefinitionCache,
     EvictionAndReplacementKeepTheirOwnPositions) {
  std::array<std::vector<LowOp>, 2> Blocks = {std::vector<LowOp>(40),
                                              std::vector<LowOp>(40)};
  Blocks[0][2].Opcode = NdOp::STORE;
  Blocks[1][5].Opcode = NdOp::CALL;
  ResolverMemoryDefinitionCache Cache;
  for (int Round = 0; Round != 2; ++Round)
    for (int Block = 0; Block != 4096; ++Block)
      ASSERT_EQ(search(Cache, Blocks[Block % 2], 40, 100, Block).Position,
                Block % 2 ? 5 : 2);
  ResolverMemoryDefinitionCache Replacement;
  EXPECT_EQ(search(Replacement, Blocks[1], 40, 100).Position, 5);
  EXPECT_EQ(search(Cache, Blocks[0], 40, 100).Position, 2);
  auto Moved = std::move(Cache);
  EXPECT_EQ(search(Moved, Blocks[0], 40, 100).Position, 2);
}

TEST(ResolverMemoryDefinitionCache, ReadsReachTheConsumerColdAndWarm) {
  for (auto Ordering :
       {NdMemoryOrdering::None, NdMemoryOrdering::Relaxed,
        NdMemoryOrdering::Acquire, NdMemoryOrdering::SequentiallyConsistent}) {
    SCOPED_TRACE(static_cast<unsigned>(Ordering));
    std::vector<LowOp> Ops(40);
    Ops[3].Opcode = NdOp::LOAD;
    Ops[3].MemoryOrdering = Ordering;
    Ops[3].Output = NdVar::tmp(0, 4);
    Ops[3].addInput(NdVar::reg(8, 8));
    ResolverMemoryDefinitionCache Warm;
    EXPECT_EQ(search(Warm, Ops, 40, 100).Position, 3);
    for (size_t Budget :
         {size_t{0}, size_t{1}, size_t{36}, size_t{37}, size_t{40}}) {
      SCOPED_TRACE(Budget);
      ResolverMemoryDefinitionCache Cold;
      const Answer Expected{Budget < 37 ? std::nullopt : std::optional<int>(3),
                            Budget < 37 ? 0 : Budget - 37};
      EXPECT_EQ(search(Cold, Ops, 40, Budget), Expected);
      EXPECT_EQ(search(Warm, Ops, 40, Budget), Expected);
    }
  }
}
