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
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"
#include "neverd/support/BinaryEncoding.h"

#include <vector>

using namespace neverd;

namespace {

TEST(MicroMIPSDelaySlots, AllFirstHalfwordsAndPhysicalWidths) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    const auto Order = archLittleEndian(Target) ? llvm::endianness::little
                                                : llvm::endianness::big;
    cs_insn Insn{};
    MIPSLifter Base(Target, InstructionMode::MicroMIPS, 2);
    MIPSLifter DSP(Target, InstructionMode::MicroMIPS, 2);
    DSP.setDSPEnabled(true);
    MIPSLifter MSA(Target, InstructionMode::MicroMIPS, 5);
    MSA.setMSAEnabled(true);
    MIPSLifter R6(Target, InstructionMode::MicroMIPS, 6);
    for (unsigned First = 0; First <= 0xffff; ++First) {
      llvm::support::endian::write16(Insn.bytes, First, Order);
      const auto Major = First >> 10;
      for (unsigned Second : {0u, 0x0f3cu, 0x4f3cu}) {
        llvm::support::endian::write16(Insn.bytes + 2, Second, Order);
        for (unsigned Size : {2u, 4u}) {
          Insn.size = Size;
          unsigned Expected = 0;
          if (Size == 2) {
            if (First >= 0x4580 && First < 0x45a0)
              Expected = 1;
            if (First >= 0x45c0 && First < 0x45e0)
              Expected = 4;
            if (First >= 0x45e0 && First < 0x4600)
              Expected = 2;
            if (Major == 0x23 || Major == 0x2b || Major == 0x33)
              Expected = 1;
          } else {
            if (Major == 0x25 || Major == 0x2d || Major == 0x35)
              Expected = 1;
            if (Major == 0x1d)
              Expected = 2;
            if (Major == 0x3c || Major == 0x3d)
              Expected = 4;
            if (Major == 0 && Second != 0)
              Expected = Second == 0x0f3c ? 4 : 2;
          }
          const unsigned Selector = (First >> 5) & 31;
          const bool VBranch =
              Size == 4 && Major == 0x20 &&
              (Selector == 11 || Selector == 15 || Selector >= 24);
          ASSERT_EQ(Base.delaySlotBytes(Insn), Expected)
              << First << " " << Second;
          ASSERT_EQ(DSP.delaySlotBytes(Insn),
                    Size == 4 && First == 0x4360 ? 1u : Expected);
          ASSERT_EQ(MSA.delaySlotBytes(Insn), VBranch ? 1u : Expected);
          ASSERT_EQ(R6.delaySlotBytes(Insn), 0u);
        }
      }
    }
  }
}

TEST(MicroMIPSDecode, EncodingModesRequireAMIPSTarget) {
  for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
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

TEST(MicroMIPS64, FullWidthTargetsRegistersAndScalarMemory) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    // MOVE16 v0,a0; DADDIU v0,v0,-8; SD v0,0(a1); LD v1,0(a1); JRC ra.
    auto Image = microImage(
        Target, {0x0c44, 0x5c42, 0xfff8, 0xd845, 0, 0xdc65, 0, 0x45bf});
    constexpr va_t PC = 0x100001000, VA = 0x220002000;
    Image.Bits = Bitness::Bits64;
    Image.MIPS->ABI = MIPSABI::N64;
    auto &Code = Image.Segments.front();
    const auto Size = Code.Size;
    Code.VA = Image.Entry = PC;
    Image.MIPS->CodeModeEntries.clear();
    Image.MIPS->CodeModeEntries.emplace(PC, InstructionMode::MicroMIPS);
    Image.MIPS->CodeRegions = {{PC, PC + Size, InstructionMode::MicroMIPS}};
    Image.Symbols = {Symbol::makeFunc(PC, Size)};
    Segment Data;
    Data.VA = VA;
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(8);
    Image.Segments.push_back(std::move(Data));
    EXPECT_EQ(mips::serializedCodeTarget(Image, PC | 1), PC);
    EXPECT_FALSE(mips::serializedCodeTarget(Image, PC));
    EXPECT_EQ(Image.instructionModeAt(PC), InstructionMode::MicroMIPS);
    auto Canonical = canonicalizeLowControlTarget(
        PC | 1, InstructionMode::MicroMIPS,
        LowInstructionTargetMode::FromTargetBit0, Image.getPointerSize());
    ASSERT_TRUE(bool(Canonical)) << llvm::toString(Canonical.takeError());
    EXPECT_EQ(Canonical->Address, PC);
    EXPECT_EQ(Canonical->Mode, InstructionMode::MicroMIPS);
    EXPECT_EQ(qualifiedMIPSControlTarget(
                  Image, InstructionMode::MicroMIPS,
                  LowInstructionTargetMode::FromTargetBit0, PC | 1),
              PC);
    auto Func = buildMicro(Image);
    ASSERT_TRUE(Func.hasCompleteLiftCoverage());
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Image));
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    constexpr uint64_t Value = 0xdeadbeef80010009;
    E.setRegister(mipsreg::A0, Value);
    E.setRegister(mipsreg::A1, VA);
    E.setRegister(mipsreg::RA, PC | 1);
    for (unsigned Offset = 0; Offset < Size - 2;) {
      const auto *Bytes = Image.readVA(PC + Offset, Size - Offset);
      ASSERT_TRUE(Bytes);
      DecodedExecutionUnit U;
      const auto N =
          Decode.decodeUnitForLift(Bytes, Size - Offset, PC + Offset, U);
      ASSERT_GT(N, 0);
      std::vector<LowOp> Ops;
      Decode.liftUnitToLow(U, Ops);
      for (const auto &Op : Ops)
        ASSERT_TRUE(E.step(Op)) << Offset;
      Offset += N;
    }
    EXPECT_EQ(E.getRegister(mipsreg::V0), Value - 8);
    EXPECT_EQ(E.getRegister(mipsreg::V1), Value - 8);
  }
}

TEST(MicroMIPS64, DoublewordMergeStoresKeepPhysicalBytesAndSignedDisplacement) {
  // GNU as -mips64r2, including the two signed 12-bit displacement limits.
  constexpr struct {
    uint16_t Half;
    uint32_t Classic;
    unsigned Id;
    int Disp;
  } Cases[] = {{0xc800, 0xb085f800, MIPS_INS_SDL, -2048},
               {0xcfff, 0xb085ffff, MIPS_INS_SDL, -1},
               {0xd006, 0xb4850006, MIPS_INS_SDR, 6},
               {0xd7ff, 0xb48507ff, MIPS_INS_SDR, 2047}};
  for (const auto Target :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    const auto Order = archLittleEndian(Target) ? llvm::endianness::little
                                                : llvm::endianness::big;
    Decoder Micro, Classic;
    ASSERT_TRUE(Micro.init(Target, InstructionMode::MicroMIPS));
    ASSERT_TRUE(Classic.init(Target));
    for (const auto &Case : Cases) {
      uint8_t Bytes[4], Reference[4];
      llvm::support::endian::write16(Bytes, 0x60a4, Order);
      llvm::support::endian::write16(Bytes + 2, Case.Half, Order);
      llvm::support::endian::write32(Reference, Case.Classic, Order);
      DecodedInsn Insn{};
      ASSERT_EQ(Micro.decodeOne(Bytes, 4, 0x1000, Insn), 4);
      ASSERT_EQ(Insn.Id, Case.Id);
      EXPECT_TRUE(std::equal(Bytes, Bytes + 4, Insn.Raw->bytes));
      ASSERT_NE(Insn.Raw->detail, nullptr);
      const auto &Detail = Insn.Raw->detail->mips;
      ASSERT_EQ(Detail.op_count, 2u);
      EXPECT_EQ(Detail.operands[0].reg, MIPS_REG_A1);
      EXPECT_EQ(Detail.operands[1].mem.base, MIPS_REG_A0);
      EXPECT_EQ(Detail.operands[1].mem.disp, Case.Disp);
      std::vector<LowOp> Ops, ReferenceOps;
      ASSERT_EQ(Micro.decodeOneForLift(Bytes, 4, 0x1000, Insn), 4);
      Micro.liftToLow(Insn, Ops);
      ASSERT_EQ(Classic.decodeOneForLift(Reference, 4, 0x1000, Insn), 4);
      Classic.liftToLow(Insn, ReferenceOps);
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(Ops, ReferenceOps);
    }
  }
}

TEST(MicroMIPS64, DoublewordMergeStoresAreRestrictedToPreR6WideGPRs) {
  for (const auto Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const auto Order = archLittleEndian(Target) ? llvm::endianness::little
                                                : llvm::endianness::big;
    for (unsigned Revision : {2u, 6u}) {
      if (Revision == 2 && isMIPS64(Target))
        continue;
      for (auto Mode :
           {InstructionMode::MicroMIPS, InstructionMode::MIPSClassic}) {
        MIPSLifter Lifter(Target, Mode, Revision);
        for (bool Left : {false, true}) {
          cs_insn Insn{};
          Insn.size = 4;
          if (Mode == InstructionMode::MicroMIPS) {
            llvm::support::endian::write16(Insn.bytes, 0x60a4, Order);
            llvm::support::endian::write16(Insn.bytes + 2,
                                           Left ? 0xcfff : 0xd006, Order);
          } else {
            llvm::support::endian::write32(
                Insn.bytes, Left ? 0xb085ffff : 0xb4850006, Order);
          }
          std::vector<LowOp> Ops;
          EXPECT_THROW(Lifter.lift(Insn, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
    }
  }
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
