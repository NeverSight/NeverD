//===- CompactUnwindARM32Tests.cpp - ARM32 compact unwind tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "CompactUnwindTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/loader/MachO/MachOExceptions.h"

#include <array>
#include <bit>

namespace {

using namespace neverd;
using namespace neverd::compact_unwind_test;
using namespace neverd::macho_unwind;

/// Independent armv7k compact-unwind ABI values.  Keeping the fixture's input
/// words independent from the decoder constants prevents a shifted production
/// mask from making both the implementation and its test wrong in the same
/// way.
namespace armv7k_abi {
constexpr uint32_t ModeFrame = 0x01000000u;
constexpr uint32_t ModeFrameD = 0x02000000u;
constexpr uint32_t StackAdjust8 = 0x00800000u;
constexpr uint32_t DRegisterPatternMask = 0x00000700u;
constexpr uint32_t ReservedDRegisterBit = 0x00000800u;
constexpr uint32_t ReservedFramePayloadBit = 0x00001000u;
constexpr unsigned StackAdjustShift = 22;
constexpr unsigned DRegisterPatternShift = 8;
constexpr uint32_t FirstPushR4 = 0x00000001u;
constexpr uint32_t FirstPushR6 = 0x00000004u;
constexpr uint32_t SecondPushR8 = 0x00000008u;
constexpr uint32_t SecondPushR10 = 0x00000020u;
constexpr uint32_t SecondPushR12 = 0x00000080u;
} // namespace armv7k_abi

TEST(CompactUnwindARM32, DecodesFrameStackAdjustmentAndGPRLayout) {
  const uint32_t Encoding = armv7k_abi::ModeFrame | armv7k_abi::StackAdjust8 |
                            armv7k_abi::FirstPushR4 | armv7k_abi::FirstPushR6 |
                            armv7k_abi::SecondPushR8 |
                            armv7k_abi::SecondPushR10 |
                            armv7k_abi::SecondPushR12;

  CompactUnwindEntry Entry;
  ASSERT_TRUE(decodeEncoding(Arch::ARM, Encoding, Entry));

  EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
  EXPECT_TRUE(Entry.HasStackAdjustment);
  EXPECT_EQ(Entry.StackAdjustment, 8u);
  EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Complete);
  EXPECT_EQ(slotRegisters(Entry), (std::vector<int>{6, 4, 12, 10, 8}));
  EXPECT_EQ(Entry.SavedGPRMask, gprMask({4, 6, 8, 10, 12}));
  EXPECT_EQ(Entry.SavedFPRMask, 0u);
  EXPECT_FALSE(Entry.HasStackSize);
  EXPECT_EQ(Entry.FrameOffset, 0u);
}

TEST(CompactUnwindARM32, DecodesEveryStackAdjustmentAndSavedGPRBit) {
  constexpr uint32_t AllGPRs = 0x000000ffu;
  for (uint32_t EncodedAdjustment = 0; EncodedAdjustment != 4;
       ++EncodedAdjustment) {
    SCOPED_TRACE(EncodedAdjustment);
    CompactUnwindEntry Entry;
    ASSERT_TRUE(
        decodeEncoding(Arch::ARM,
                       armv7k_abi::ModeFrame | AllGPRs |
                           (EncodedAdjustment << armv7k_abi::StackAdjustShift),
                       Entry));

    EXPECT_TRUE(Entry.HasStackAdjustment);
    EXPECT_EQ(Entry.StackAdjustment, EncodedAdjustment * sizeof(uint32_t));
    EXPECT_EQ(slotRegisters(Entry),
              (std::vector<int>{6, 5, 4, 12, 11, 10, 9, 8}));
    EXPECT_EQ(Entry.SavedGPRMask, gprMask({4, 5, 6, 8, 9, 10, 11, 12}));
  }
}

TEST(CompactUnwindARM32, DecodesFixedFrameDRegisterLayouts) {
  const std::array<std::vector<int>, 4> ExpectedSlots = {
      std::vector<int>{8}, std::vector<int>{10, 8}, std::vector<int>{12, 10, 8},
      std::vector<int>{14, 12, 10, 8}};
  const std::array<uint32_t, 4> ExpectedMasks = {gprMask({8}), gprMask({8, 10}),
                                                 gprMask({8, 10, 12}),
                                                 gprMask({8, 10, 12, 14})};

  for (uint32_t Pattern = 0; Pattern != ExpectedSlots.size(); ++Pattern) {
    SCOPED_TRACE(Pattern);
    CompactUnwindEntry Entry;
    ASSERT_TRUE(decodeEncoding(
        Arch::ARM,
        armv7k_abi::ModeFrameD | (Pattern << armv7k_abi::DRegisterPatternShift),
        Entry));

    EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
    EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Complete);
    EXPECT_EQ(slotRegisters(Entry), ExpectedSlots[Pattern]);
    EXPECT_EQ(Entry.SavedFPRMask, ExpectedMasks[Pattern]);
    EXPECT_EQ(std::popcount(Entry.SavedFPRMask), Pattern + 1);
    EXPECT_EQ(Entry.SavedRegisterSlots.size(), Pattern + 1);
    for (const CompactUnwindRegisterSlot &Slot : Entry.SavedRegisterSlots)
      EXPECT_EQ(Slot.RegisterClass, UnwindRegisterClass::FloatingPoint);
  }
}

TEST(CompactUnwindARM32,
     RetainsProvenDRegistersWithoutInventingAlignedStackSlots) {
  const std::array<std::vector<int>, 4> ExactSlots = {
      std::vector<int>{14, 12}, std::vector<int>{14}, std::vector<int>{},
      std::vector<int>{}};
  const std::array<uint32_t, 4> ProvenMasks = {
      gprMask({8, 9, 10, 12, 14}), gprMask({8, 9, 10, 11, 12, 14}),
      gprMask({8, 9, 10, 11, 12, 13, 14}),
      gprMask({8, 9, 10, 11, 12, 13, 14, 15})};

  for (uint32_t Pattern = 4; Pattern != 8; ++Pattern) {
    SCOPED_TRACE(Pattern);
    CompactUnwindEntry Entry;
    EXPECT_FALSE(
        decodeEncoding(Arch::ARM,
                       armv7k_abi::ModeFrameD | armv7k_abi::StackAdjust8 |
                           armv7k_abi::FirstPushR6 | armv7k_abi::SecondPushR8 |
                           (Pattern << armv7k_abi::DRegisterPatternShift),
                       Entry));

    EXPECT_EQ(Entry.Kind, CompactUnwindKind::FramePointer);
    EXPECT_TRUE(Entry.HasStackAdjustment);
    EXPECT_EQ(Entry.StackAdjustment, 8u);
    EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Partial);
    std::vector<int> ExpectedSlots = {6, 8};
    ExpectedSlots.insert(ExpectedSlots.end(), ExactSlots[Pattern - 4].begin(),
                         ExactSlots[Pattern - 4].end());
    EXPECT_EQ(slotRegisters(Entry), ExpectedSlots);
    EXPECT_EQ(Entry.SavedGPRMask, gprMask({6, 8}));
    EXPECT_EQ(Entry.SavedFPRMask, ProvenMasks[Pattern - 4]);
    EXPECT_EQ(std::popcount(Entry.SavedFPRMask), Pattern + 1);
  }
}

TEST(CompactUnwindARM32, RejectsReservedPayloadAndModeCombinations) {
  EXPECT_EQ(kARMFrameDRegisterCountMask, armv7k_abi::DRegisterPatternMask);

  const std::array<uint32_t, 5> Encodings = {
      armv7k_abi::ModeFrameD | armv7k_abi::ReservedDRegisterBit,
      armv7k_abi::ModeFrame | (1u << armv7k_abi::DRegisterPatternShift),
      armv7k_abi::ModeFrame | armv7k_abi::ReservedFramePayloadBit,
      armv7k_abi::FirstPushR4,
      0x03000000u,
  };

  for (uint32_t Encoding : Encodings) {
    SCOPED_TRACE(::testing::PrintToString(Encoding));
    CompactUnwindEntry Entry;
    EXPECT_FALSE(decodeEncoding(Arch::ARM, Encoding, Entry));
    EXPECT_EQ(Entry.Kind, CompactUnwindKind::Unknown);
    EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Partial);
    EXPECT_FALSE(Entry.HasStackAdjustment);
    EXPECT_EQ(Entry.StackAdjustment, 0u);
    EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
    EXPECT_EQ(Entry.SavedGPRMask, 0u);
    EXPECT_EQ(Entry.SavedFPRMask, 0u);
  }
}

TEST(CompactUnwindARM32, ResetsPartialFactsBeforeReusingAnEntry) {
  CompactUnwindEntry Entry;
  EXPECT_FALSE(decodeEncoding(
      Arch::ARM,
      armv7k_abi::ModeFrameD | armv7k_abi::StackAdjust8 |
          armv7k_abi::FirstPushR6 | (7u << armv7k_abi::DRegisterPatternShift),
      Entry));
  ASSERT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Partial);
  ASSERT_NE(Entry.SavedGPRMask, 0u);
  ASSERT_NE(Entry.SavedFPRMask, 0u);

  constexpr uint32_t DwarfEncoding = 0x04001234u;
  ASSERT_TRUE(decodeEncoding(Arch::ARM, DwarfEncoding, Entry));
  EXPECT_EQ(Entry.Kind, CompactUnwindKind::DwarfFDE);
  EXPECT_EQ(Entry.DwarfFDEOffset, 0x1234u);
  EXPECT_EQ(Entry.SemanticStatus, CompactUnwindSemanticStatus::Complete);
  EXPECT_FALSE(Entry.HasStackAdjustment);
  EXPECT_EQ(Entry.StackAdjustment, 0u);
  EXPECT_TRUE(Entry.SavedRegisterSlots.empty());
  EXPECT_EQ(Entry.SavedGPRMask, 0u);
  EXPECT_EQ(Entry.SavedFPRMask, 0u);
}

TEST(CompactUnwindARM32, PropagatesPartialFrameDLayoutToItsFunction) {
  BinaryImage Img = makeImage(Arch::ARM);
  Img.Bits = Bitness::Bits32;
  attachUnwindInfo(
      Img,
      buildUnwindInfo({{0x100, armv7k_abi::ModeFrameD |
                                   (4u << armv7k_abi::DRegisterPatternShift)}},
                      0x200));

  parseDarwinExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &Function = Img.ExceptionMetadata.Functions.front();
  ASSERT_TRUE(Function.Compact.has_value());
  EXPECT_EQ(Function.Compact->SemanticStatus,
            CompactUnwindSemanticStatus::Partial);
  EXPECT_EQ(Function.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_NE(std::find(Function.Diagnostics.begin(), Function.Diagnostics.end(),
                      "compact unwind register layout depends on runtime stack "
                      "alignment"),
            Function.Diagnostics.end());
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_NE(
      std::find(Img.ExceptionMetadata.Diagnostics.begin(),
                Img.ExceptionMetadata.Diagnostics.end(),
                "__unwind_info entry requires register-layout facts that its "
                "encoding alone cannot prove"),
      Img.ExceptionMetadata.Diagnostics.end());
}

} // namespace
