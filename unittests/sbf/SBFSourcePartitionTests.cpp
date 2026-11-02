//===- SBFSourcePartitionTests.cpp - Lossless source layout ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Covers continuation/unreachable slots, canonical function ownership and
/// shared tails without turning a rendering partition into a semantic owner.
///
//===----------------------------------------------------------------------===//

#include "SBFSolanaTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/emit/SBFSourcePartition.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <vector>

namespace neverd::sbf {
namespace {

TEST(SBFSourcePartition, RetainsContinuationsAndUnreachableSlotsExactlyOnce) {
  auto Text = test::loadImm64(0, 0x123456789abcdef0);
  Text.push_back(test::encode(Opcode::EXIT));
  Text.push_back(test::encode(Opcode::EXIT));
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Partition = partitionSource(*Program, 2);
  ASSERT_TRUE(static_cast<bool>(Partition))
      << llvm::toString(Partition.takeError());
  EXPECT_EQ(Partition->RegionBySlot.size(), Text.size());
  std::vector<unsigned> Seen(Text.size());
  for (size_t ID = 0; ID < Partition->Regions.size(); ++ID) {
    const auto &Region = Partition->Regions[ID];
    EXPECT_LE(Region.Slots.size(), 2u);
    for (size_t Slot : Region.Slots) {
      ASSERT_LT(Slot, Seen.size());
      ++Seen[Slot];
      EXPECT_EQ(Partition->RegionBySlot[Slot], ID);
    }
  }
  EXPECT_TRUE(
      std::all_of(Seen.begin(), Seen.end(), [](unsigned N) { return N == 1; }));
  EXPECT_EQ(Partition->Regions[Partition->RegionBySlot[3]].Owner,
            SourceRegionOwner::Unowned);
}

TEST(SBFSourcePartition, SharedTailsNeverAcquireAnArbitraryFunctionOwner) {
  auto Image = test::makeImage(
      {test::encode(Opcode::JA, 0, 0, 3), test::encode(Opcode::EXIT),
       test::encode(Opcode::JA, 0, 0, 1), test::encode(Opcode::EXIT),
       test::encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
       test::encode(Opcode::EXIT)},
      {});
  Image.addSymbol("other", kBytecodeStart + 2 * kInstructionSize,
                  2 * kInstructionSize, true);
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Partition = partitionSource(*Program, 2);
  ASSERT_TRUE(static_cast<bool>(Partition))
      << llvm::toString(Partition.takeError());
  for (size_t Slot : {4u, 5u}) {
    const auto &Region = Partition->Regions[Partition->RegionBySlot[Slot]];
    EXPECT_EQ(Region.Owner, SourceRegionOwner::Shared);
    EXPECT_FALSE(Region.FunctionIndex);
  }
  const auto &Entry = Partition->Regions[Partition->RegionBySlot[0]];
  const auto &Other = Partition->Regions[Partition->RegionBySlot[2]];
  EXPECT_EQ(Entry.Owner, SourceRegionOwner::Function);
  EXPECT_EQ(Other.Owner, SourceRegionOwner::Function);
  EXPECT_NE(Entry.FunctionIndex, Other.FunctionIndex);
}

TEST(SBFSourcePartition, RejectsZeroWorkAndOverlappingBlockRanges) {
  auto Program = analyze(test::makeImage({test::encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Zero = partitionSource(*Program, 0);
  ASSERT_FALSE(static_cast<bool>(Zero));
  llvm::consumeError(Zero.takeError());
  Program->Low.Blocks.push_back(Program->Low.Blocks.front());
  auto Overlap = partitionSource(*Program);
  ASSERT_FALSE(static_cast<bool>(Overlap));
  llvm::consumeError(Overlap.takeError());
}

} // namespace
} // namespace neverd::sbf
