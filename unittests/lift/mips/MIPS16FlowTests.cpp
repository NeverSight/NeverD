//===- MIPS16FlowTests.cpp - MIPS16 modes and control ownership -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify halfword branches, exact delay slots, interworking contracts and
/// captured return links independently of an image during source conversion.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/MIPS/MIPSControlTarget.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

TEST(MIPS16Decode, EncodingModeRequiresTheAdmittedMIPS32Revision) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64,
                      Arch::MIPS64, Arch::MIPS64EL}) {
    Decoder Decode;
    EXPECT_FALSE(Decode.init(Target, InstructionMode::MIPS16));
  }
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Bitness::Bits32;
    Image.Mode = InstructionMode::MIPS16;
    Image.MIPS.emplace();
    Image.MIPS->HasMIPS16 = true;
    Decoder Decode;
    for (unsigned Revision : {1u, 3u, 5u, 6u}) {
      Image.MIPS->ISARevision = Revision;
      EXPECT_FALSE(Decode.init(Image));
    }
  }
}

BinaryImage image(Arch Target, llvm::ArrayRef<uint16_t> Halfwords) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Mode = InstructionMode::MixedMIPS;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Image.Entry = 0x1000;
  Image.MIPS.emplace();
  Image.MIPS->HasMIPS16 = true;
  Image.MIPS->CodeModeEntries.emplace(Image.Entry, InstructionMode::MIPS16);
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

LowFunc build(const BinaryImage &Image) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  return Builder.build(Image, Decode, Image.Entry, "mips16_test");
}

class MIPS16Flow : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPS16Flow, CompactBranchesHaveNoSlotAndUsePhysicalInstructionEnd) {
  for (bool Extended : {false, true}) {
    const auto Image = image(
        GetParam(),
        Extended ? std::vector<uint16_t>{0xf000, 0x1001, 0xffff, 0x6a07, 0xe8a0}
                 : std::vector<uint16_t>{0x1001, 0xffff, 0x6a07, 0xe8a0});
    const auto Function = build(Image);
    ASSERT_TRUE(Function.hasCompleteLiftCoverage());
    auto Error = validateLowInstructionBoundaries(
        Function, LowInstructionBoundaryRequirement::Required);
    EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
    ASSERT_FALSE(Function.Blocks.empty());
    ASSERT_FALSE(Function.Blocks.front().InstructionBoundaries.empty());
    const auto &Boundary =
        Function.Blocks.front().InstructionBoundaries.front();
    EXPECT_EQ(Boundary.Size, Extended ? 4 : 2);
    EXPECT_FALSE(Boundary.DelaySlot);
    EXPECT_EQ(Boundary.Immediate, Extended ? 0x1006u : 0x1004u);
    EXPECT_EQ(Boundary.Mode, InstructionMode::MIPS16);
    ASSERT_EQ(Function.Blocks.front().Succs.size(), 1u);
    ASSERT_EQ(Function.Blocks.front().Ops.size(), 1u);
    EXPECT_EQ(Function.Blocks.front().Ops.front().Opcode, NdOp::BRANCH);
  }
}

TEST_P(MIPS16Flow, DelayedReturnsRequireOneNonExtendedNonControlHalfword) {
  const auto Good = build(image(GetParam(), {0xe820, 0x6a07}));
  ASSERT_TRUE(Good.hasCompleteLiftCoverage());
  const auto &Boundary = Good.Blocks.front().InstructionBoundaries.front();
  ASSERT_TRUE(Boundary.DelaySlot);
  EXPECT_EQ(Boundary.Size, 4);
  EXPECT_EQ(Boundary.DelaySlot->Offset, 2);
  EXPECT_EQ(Boundary.DelaySlot->Size, 2);
  EXPECT_EQ(Boundary.TargetMode,
            LowInstructionTargetMode::FromMIPS16TargetBit0);
  auto Error = validateLowInstructionBoundaries(
      Good, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  for (const auto &Words : {std::vector<uint16_t>{0xe820, 0xf010, 0x6a00},
                            {0xe820, 0xe8a0},
                            {0xe820, 0x1000},
                            {0xe820}}) {
    const auto Bad = build(image(GetParam(), Words));
    EXPECT_FALSE(Bad.hasCompleteLiftCoverage());
  }
}

TEST_P(MIPS16Flow, InteriorModeEvidenceCannotSplitAnExtendPairOrSlot) {
  for (const auto &Words :
       {std::vector<uint16_t>{0xf010, 0x6a00, 0xe8a0}, {0xe820, 0x6a07}}) {
    auto Image = image(GetParam(), Words);
    Image.MIPS->CodeModeEntries[0x1002] = InstructionMode::MIPSClassic;
    EXPECT_FALSE(
        mips::codeModeRangeMatches(Image, 0x1000, 4, InstructionMode::MIPS16));
    EXPECT_FALSE(build(Image).hasCompleteLiftCoverage());
  }
}

TEST_P(MIPS16Flow, DirectCallsHaveFourByteHeadsAndOneHalfwordSlots) {
  for (bool Exchange : {false, true})
    for (va_t Address : {va_t(0x10002), va_t(0x0ffffffa), va_t(0x0ffffffc)}) {
      const auto Image = image(
          GetParam(), {uint16_t(Exchange ? 0x1c62 : 0x1862), 0x1234, 0x6a07});
      Decoder Decode;
      ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
      const auto &Bytes = Image.Segments.front().Data;
      DecodedExecutionUnit Unit;
      ASSERT_EQ(
          Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), Address, Unit),
          6);
      std::vector<LowOp> Ops;
      const auto Slot = Decode.liftUnitToLow(Unit, Ops);
      ASSERT_TRUE(Slot);
      EXPECT_EQ(Slot->Offset, 4);
      EXPECT_EQ(Slot->Size, 2);
      const uint32_t Target = (uint32_t(Address + 4) & 0xf0000000) | 0x010c48d0;
      EXPECT_EQ(Decode.directCallTarget(Unit.Head), Target);
      EXPECT_EQ(Decode.controlTargetMode(Unit.Head, InstructionMode::MIPS16),
                Exchange ? LowInstructionTargetMode::MIPSClassic
                         : LowInstructionTargetMode::Preserve);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      for (const auto &Op : Ops) {
        if (Op.Opcode == NdOp::CALL) {
          ASSERT_TRUE(Op.Inputs[0].isConst());
          EXPECT_EQ(Op.Inputs[0].Offset, Target);
          EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 7u);
        } else
          ASSERT_TRUE(Emulator.step(Op));
      }
      EXPECT_EQ(Emulator.getRegister(mipsreg::RA), uint32_t(Address + 6) | 1u);
      for (size_t Length = 0; Length < Bytes.size(); ++Length)
        EXPECT_EQ(Decode.decodeUnitForLift(Bytes.data(), Length, Address, Unit),
                  0);
    }
}

TEST_P(MIPS16Flow, RegisterCallsCaptureTargetsBeforeLinksAndSlots) {
  for (bool Compact : {false, true}) {
    auto Image =
        image(GetParam(), Compact ? std::vector<uint16_t>{0xecc0}
                                  : std::vector<uint16_t>{0xec40, 0x679f});
    Decoder Decode;
    ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
    const auto &Bytes = Image.Segments.front().Data;
    DecodedExecutionUnit Unit;
    ASSERT_EQ(
        Decode.decodeUnitForLift(Bytes.data(), Bytes.size(), 0x10002, Unit),
        int(Bytes.size()));
    std::vector<LowOp> Ops;
    const auto Slot = Decode.liftUnitToLow(Unit, Ops);
    EXPECT_EQ(bool(Slot), !Compact);
    EXPECT_EQ(Decode.controlTargetMode(Unit.Head, InstructionMode::MIPS16),
              LowInstructionTargetMode::FromMIPS16TargetBit0);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, 0x20001);
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::INDIR_CALL)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0x20001u);
      else
        ASSERT_TRUE(Emulator.step(Op));
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::RA), Compact ? 0x10005u : 0x10007u);
    EXPECT_EQ(Emulator.getRegister(mipsreg::A0), Compact ? 0x20001u : 0x10007u);
  }
}

TEST_P(MIPS16Flow, DiscoveryCannotVerifyAnInteriorModeConflict) {
  // Caller names an unsymbolized MIPS16 entry at 0x100c. Its JR slot, or
  // JAL's second halfword, conflicts with local classic-mode evidence.
  for (bool CallHead : {false, true})
    for (bool Conflict : {false, true}) {
      auto Image = image(
          GetParam(), {0x1800, 0x0403, 0x6500, 0xe8a0, 0x6500, 0x6500,
                       uint16_t(CallHead ? 0x1800 : 0xe820),
                       uint16_t(CallHead ? 0x0400 : 0x6500), 0x6500, 0xe8a0});
      Image.Symbols.clear();
      Image.MIPS->CodeRegions.push_back(
          {0x1000, 0x1014, InstructionMode::MIPS16});
      if (Conflict)
        Image.MIPS->CodeModeEntries[0x100e] = InstructionMode::MIPSClassic;
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      FuncDetector Detector;
      Detector.detect(Image, Decode);
      EXPECT_EQ(Image.VerifiedFunctionEntries.count(0x100c),
                Conflict ? 0u : 1u);
    }
}

TEST_P(MIPS16Flow, SourceReturnUsesCapturedEntryRAWithAndWithoutImage) {
  for (bool Before : {false, true}) {
    // MOVE ra,a0 changes the return destination before JRC, but cannot change
    // the destination already captured by JR before executing its slot.
    const auto Image =
        image(GetParam(), Before ? std::vector<uint16_t>{0x65fc, 0xe8a0}
                                 : std::vector<uint16_t>{0xe820, 0x65fc});
    const auto Low = build(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    for (bool WithImage : {false, true}) {
      LowToMedConverter Convert;
      if (WithImage)
        Convert.setBinaryImage(&Image);
      if (Before)
        EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                     LowToMedConversionError);
      else
        EXPECT_NO_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF));
    }
  }
}

TEST_P(MIPS16Flow, CallsRequireAnAuthenticatedPreservedEntryReturn) {
  const std::vector<std::vector<uint16_t>> Programs = {
      {0x671f, 0x1800, 0x0800, 0x6500, 0x65f8, 0xe8a0}, // s0 retains entry RA
      {0x677f, 0x1800, 0x0800, 0x6500, 0x65fb, 0xe8a0}, // v1 is caller-saved
      {0x671f, 0x1800, 0x0800, 0x6801, 0x65f8, 0xe8a0}, // slot overwrites s0
      {0x671f, 0x6801, 0x1800, 0x0800, 0x6500, 0x65f8, 0xe8a0},
      {0x63fd, 0x6204, 0x1800, 0x0800, 0x6500, 0x9004, 0x65f8, 0x6303, 0xe8a0},
  };
  for (unsigned I = 0; I != Programs.size(); ++I) {
    SCOPED_TRACE(I);
    auto Image = image(GetParam(), Programs[I]);
    auto Callee = image(GetParam(), {0xe8a0});
    Callee.Segments.front().VA = 0x2000;
    Image.Segments.push_back(std::move(Callee.Segments.front()));
    Image.Symbols.push_back(Symbol::makeFunc(0x2000, 2));
    Image.MIPS->CodeModeEntries[0x2000] = InstructionMode::MIPS16;
    const auto Low = build(Image);
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    LowToMedConverter Convert;
    Convert.setBinaryImage(&Image);
    if (I == 0) {
      EXPECT_NO_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF));
      LowToMedConverter WithoutImage;
      try {
        WithoutImage.convert(Low, GetParam(), BinaryFormat::ELF);
        FAIL() << "A direct compressed call needs target mode evidence";
      } catch (const LowToMedConversionError &Error) {
        EXPECT_STREQ(Error.what(), "MIPS direct call target has conflicting or "
                                   "unknown instruction mode");
      }
    } else {
      try {
        Convert.convert(Low, GetParam(), BinaryFormat::ELF);
        FAIL() << "A call must not authenticate a different incoming link";
      } catch (const LowToMedConversionError &Error) {
        EXPECT_STREQ(Error.what(), "MIPS source return target is not proven to "
                                   "preserve the incoming link and mode");
      }
    }
  }
}

TEST_P(MIPS16Flow, ClassicRegisterTransfersKeepTheImageCompressedFamily) {
  auto Image = image(GetParam(), {0xe8a0, 0x6500, 0, 0, 0, 0});
  Image.MIPS->CodeModeEntries[0x1004] = InstructionMode::MIPSClassic;
  const uint32_t JR = 0x03e00008;
  for (unsigned I = 0; I != 4; ++I)
    Image.Segments.front().Data[4 + I] =
        JR >> (8 * (archLittleEndian(GetParam()) ? I : 3 - I));
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  for (va_t Address : {0x1000u, 0x1004u, 0x1000u}) {
    ASSERT_TRUE(Decode.selectMode(Image, Address));
    DecodedInsn Insn;
    const auto *Data = Image.readVA(Address, Address == 0x1004 ? 4 : 2);
    ASSERT_NE(Data, nullptr);
    ASSERT_GT(
        Decode.decodeOneForLift(Data, Address == 0x1004 ? 4 : 2, Address, Insn),
        0);
    EXPECT_EQ(Decode.controlTargetMode(Insn, Decode.currentMode()),
              LowInstructionTargetMode::FromMIPS16TargetBit0);
  }
  EXPECT_FALSE(Decode.selectMode(Image, 0x1000, InstructionMode::MicroMIPS));
  EXPECT_EQ(mips::serializedCodeTarget(Image, 0x1001), 0x1000u);
  EXPECT_EQ(mips::serializedCodeTarget(Image, 0x1004), 0x1004u);
  EXPECT_FALSE(mips::serializedCodeTarget(Image, 0x1000));
  EXPECT_FALSE(mips::serializedCodeTarget(Image, 0x1005));
  for (InstructionMode Source :
       {InstructionMode::MIPS16, InstructionMode::MIPSClassic}) {
    auto Target = canonicalizeLowControlTarget(
        0x1001, Source, LowInstructionTargetMode::FromMIPS16TargetBit0);
    ASSERT_TRUE(bool(Target));
    EXPECT_EQ(Target->Address, 0x1000u);
    EXPECT_EQ(Target->Mode, InstructionMode::MIPS16);
  }
  for (uint64_t Raw : {0x1002ULL, 0x100000001ULL, InvalidVA}) {
    auto Target = canonicalizeLowControlTarget(
        Raw, InstructionMode::MIPS16,
        LowInstructionTargetMode::FromMIPS16TargetBit0);
    EXPECT_FALSE(bool(Target));
    llvm::consumeError(Target.takeError());
  }
  for (auto [Source, Contract] :
       {std::pair{InstructionMode::MIPS16,
                  LowInstructionTargetMode::FromTargetBit0},
        {InstructionMode::MicroMIPS,
         LowInstructionTargetMode::FromMIPS16TargetBit0},
        {InstructionMode::MIPS16, LowInstructionTargetMode::MicroMIPS}}) {
    auto Target = canonicalizeLowControlTarget(0x1000, Source, Contract);
    EXPECT_FALSE(bool(Target));
    llvm::consumeError(Target.takeError());
  }
}

TEST_P(MIPS16Flow, ClassicJALXNamesTheMIPS16FamilyWithoutATargetTag) {
  auto Image = image(GetParam(), {0xe8a0, 0x6500, 0, 0, 0, 0});
  Image.MIPS->CodeModeEntries[0x1004] = InstructionMode::MIPSClassic;
  const uint32_t JALX = 0x74000400; // jalx 0x1000
  for (unsigned I = 0; I != 4; ++I)
    Image.Segments.front().Data[4 + I] =
        JALX >> (8 * (archLittleEndian(GetParam()) ? I : 3 - I));
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  ASSERT_TRUE(Decode.selectMode(Image, 0x1004));
  DecodedInsn Insn;
  ASSERT_EQ(Decode.decodeOneForLift(Image.readVA(0x1004, 4), 4, 0x1004, Insn),
            4);
  const auto Contract = Decode.controlTargetMode(Insn, Decode.currentMode());
  EXPECT_EQ(Contract, LowInstructionTargetMode::MIPS16);
  EXPECT_EQ(
      qualifiedMIPSControlTarget(Image, Decode.currentMode(), Contract, 0x1000),
      0x1000u);
  EXPECT_FALSE(qualifiedMIPSControlTarget(Image, Decode.currentMode(), Contract,
                                          0x1001));
  EXPECT_FALSE(qualifiedMIPSControlTarget(Image, Decode.currentMode(),
                                          LowInstructionTargetMode::MicroMIPS,
                                          0x1000));
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Flow,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
