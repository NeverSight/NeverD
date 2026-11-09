//===- MicroMIPSFlowTests.cpp - Mixed-mode graph and source boundaries ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check mixed-mode byte ownership, transfer tags and source return evidence.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/MIPS/MIPSControlTarget.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"

#include <vector>

using namespace neverd;

namespace {

TEST(MicroMIPSDecode, EncodingModesRequireTheAdmittedMIPS32Target) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64,
                      Arch::MIPS64, Arch::MIPS64EL}) {
    Decoder Decode;
    EXPECT_FALSE(Decode.init(Target, InstructionMode::MicroMIPS));
    EXPECT_FALSE(Decode.init(Target, InstructionMode::MIPSClassic));
  }
}

BinaryImage microImage(Arch Target, llvm::ArrayRef<uint16_t> Halfwords) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Mode = InstructionMode::MixedMIPS;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Image.MIPS.emplace();
  Image.MIPS->HasMicroMIPS = true;
  Image.MIPS->CodeModeEntries.emplace(Image.Entry, InstructionMode::MicroMIPS);
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  for (uint16_t Half : Halfwords) {
    Text.Data.push_back(Half >> (archLittleEndian(Target) ? 0 : 8));
    Text.Data.push_back(Half >> (archLittleEndian(Target) ? 8 : 0));
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry, Halfwords.size() * 2));
  return Image;
}

LowFunc buildMicro(const BinaryImage &Image) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  return Builder.build(Image, Decode, Image.Entry, "micro_test");
}

class MicroMIPSFlow : public ::testing::TestWithParam<Arch> {};

TEST_P(MicroMIPSFlow, BranchUsesHalfwordDisplacementAndRealSlotSpan) {
  // b16 target; move v0,a0; unreachable move; target: jrc ra
  auto Image = microImage(GetParam(), {0xcc02, 0x0c44, 0x0c45, 0x45bf});
  const auto Function = buildMicro(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  bool Found = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Address == 0x1000) {
        Found = true;
        EXPECT_EQ(Boundary.Size, 4);
        ASSERT_TRUE(Boundary.DelaySlot);
        EXPECT_EQ(Boundary.DelaySlot->Offset, 2);
        EXPECT_EQ(Boundary.DelaySlot->Size, 2);
        EXPECT_EQ(Boundary.Immediate, 0x1006u);
        EXPECT_EQ(Boundary.Mode, InstructionMode::MicroMIPS);
      }
  EXPECT_TRUE(Found);
}

TEST_P(MicroMIPSFlow, ExactOppositeModeInsideFetchOrSlotRefusesWholeUnit) {
  for (const auto &Code :
       {std::vector<uint16_t>{0x459f, 0x0c44},
        std::vector<uint16_t>{0x03ff, 0x0f3c, 0x0044, 0x1150},
        std::vector<uint16_t>{0x3044, 0x0001, 0x45bf}}) {
    auto Image = microImage(GetParam(), Code);
    const va_t Conflict = Code.front() == 0x03ff ? 0x1004 : 0x1002;
    Image.MIPS->CodeModeEntries[Conflict] = InstructionMode::MIPSClassic;
    EXPECT_FALSE(mips::codeModeRangeMatches(Image, 0x1000, Code.size() * 2,
                                            InstructionMode::MicroMIPS));
    const auto Function = buildMicro(Image);
    EXPECT_FALSE(Function.hasCompleteLiftCoverage());
    EXPECT_FALSE(Function.DecodeFailureAddresses.empty());
  }
}

TEST_P(MicroMIPSFlow, OneDecoderCanSwitchBothWaysWithoutStaleMode) {
  auto Image = microImage(GetParam(), {0x0c44, 0x45bf, 0, 0});
  Image.MIPS->CodeModeEntries[0x1004] = InstructionMode::MIPSClassic;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  const auto *Data = Image.readVA(0x1000, 8);
  ASSERT_NE(Data, nullptr);
  for (va_t At : {0x1000u, 0x1004u, 0x1000u}) {
    ASSERT_TRUE(Decode.selectMode(Image, At));
    DecodedInsn Insn;
    EXPECT_EQ(Decode.decodeOneLight(Data + At - 0x1000, 0x1008 - At, At, Insn),
              At == 0x1004 ? 4 : 2);
    EXPECT_EQ(Decode.currentMode(), At == 0x1004 ? InstructionMode::MIPSClassic
                                                 : InstructionMode::MicroMIPS);
    EXPECT_EQ(Decode.isAlignmentNop(Insn), At == 0x1004);
  }
  EXPECT_FALSE(Decode.selectMode(Image, 0x1004, InstructionMode::MicroMIPS));
}

TEST_P(MicroMIPSFlow, NativeDirectCallMustMatchItsDestinationMode) {
  // micro JALX 0x1010; 32-bit nop; jrc ra; filler; callee.
  auto Image = microImage(GetParam(), {0xf000, 0x0404, 0, 0, 0x45bf, 0x0c00,
                                       0x0c00, 0x0c00, 0x45bf, 0x0c00});
  Image.MIPS->CodeModeEntries[0x1010] = InstructionMode::MicroMIPS;
  const auto Bad = buildMicro(Image);
  EXPECT_FALSE(Bad.DecodeFailureAddresses.empty());
  EXPECT_FALSE(qualifiedMIPSControlTarget(Image, InstructionMode::MicroMIPS,
                                          LowInstructionTargetMode::MIPSClassic,
                                          0x1010));
  Image.MIPS->CodeModeEntries.erase(0x1010);
  EXPECT_FALSE(qualifiedMIPSControlTarget(Image, InstructionMode::MicroMIPS,
                                          LowInstructionTargetMode::MIPSClassic,
                                          0x1010));
  Image.MIPS->CodeModeEntries[0x1010] = InstructionMode::MIPSClassic;
  EXPECT_EQ(qualifiedMIPSControlTarget(Image, InstructionMode::MicroMIPS,
                                       LowInstructionTargetMode::MIPSClassic,
                                       0x1010),
            0x1010u);
  EXPECT_TRUE(buildMicro(Image).DecodeFailureAddresses.empty());
}

TEST_P(MicroMIPSFlow, CallableTagsAndCanonicalDirectDestinationsStayDistinct) {
  auto Image = microImage(GetParam(), {0x45bf, 0x0c00, 0, 0});
  Image.MIPS->CodeModeEntries[0x1004] = InstructionMode::MIPSClassic;
  EXPECT_EQ(mips::serializedCodeTarget(Image, 0x1001), 0x1000u);
  EXPECT_EQ(mips::serializedCodeTarget(Image, 0x1004), 0x1004u);
  EXPECT_FALSE(mips::serializedCodeTarget(Image, 0x1000));
  EXPECT_FALSE(mips::serializedCodeTarget(Image, 0x1005));
  for (uint64_t Raw : {va_t{0x1002}, va_t{0x100000001}, InvalidVA}) {
    auto Invalid =
        canonicalizeLowControlTarget(Raw, InstructionMode::MicroMIPS,
                                     LowInstructionTargetMode::FromTargetBit0);
    EXPECT_FALSE(bool(Invalid));
    llvm::consumeError(Invalid.takeError());
  }
}

TEST_P(MicroMIPSFlow, SourceReturnProvesCapturedEntryLinkIncludingTag) {
  for (bool Before : {false, true}) {
    // move ra,zero before JRC must refuse. In a JR16 slot it cannot change
    // the captured return target and is a valid ordinary source return.
    const auto Image =
        microImage(GetParam(), Before ? std::vector<uint16_t>{0x0fe0, 0x45bf}
                                      : std::vector<uint16_t>{0x459f, 0x0fe0});
    const auto Low = buildMicro(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    LowToMedConverter Convert;
    Convert.setBinaryImage(&Image);
    if (Before)
      EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                   LowToMedConversionError);
    else
      EXPECT_NO_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF));
  }
}

TEST_P(MicroMIPSFlow, MissingInstructionBoundariesCannotBypassReturnProof) {
  const auto Image = microImage(GetParam(), {0x0fe0, 0x45bf});
  auto Low = buildMicro(Image);
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  for (auto &Block : Low.Blocks)
    Block.InstructionBoundaries.clear();
  LowToMedConverter Convert;
  Convert.setBinaryImage(&Image);
  EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
               LowToMedConversionError);
}

TEST_P(MicroMIPSFlow, SyntheticTailReturnCannotCertifyTheCalleesIncomingLink) {
  for (bool ChangedRA : {false, true}) {
    // [move ra,zero | nop16]; j32 0x2000; nop16. A source tail call
    // requires a separate proof of the link supplied to that callee.
    auto Image = microImage(GetParam(), {uint16_t(ChangedRA ? 0x0fe0 : 0x0c00),
                                         0xd400, 0x1000, 0x0c00});
    auto Callee = microImage(GetParam(), {0x45bf});
    Callee.Segments.front().VA = 0x2000;
    Image.Segments.push_back(std::move(Callee.Segments.front()));
    Image.Symbols.push_back(Symbol::makeFunc(0x2000, 2));
    Image.MIPS->CodeModeEntries[0x2000] = InstructionMode::MicroMIPS;
    const auto Low = buildMicro(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    ASSERT_FALSE(Low.Blocks.empty());
    ASSERT_FALSE(Low.Blocks.front().InstructionBoundaries.empty());
    EXPECT_EQ(Low.Blocks.front().InstructionBoundaries.back().Control,
              LowInstructionControl::TailCall);
    LowToMedConverter Convert;
    Convert.setBinaryImage(&Image);
    EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                 LowToMedConversionError);
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSFlow,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
