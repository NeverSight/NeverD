//===- MIPSScalarTests.cpp - Merge loads and MUL effects -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check exact byte footprints, unchanged destinations on faults, and explicit
/// HI/LO invalidation independently of generated source.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/MIPS/MIPSIntrinsicShapes.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <array>
#include <memory>
#include <vector>

using namespace neverd;

namespace {

class MIPSScalar : public testing::TestWithParam<Arch> {
protected:
  std::array<uint8_t, 4> bytes(uint32_t Word) const {
    std::array<uint8_t, 4> Bytes;
    for (unsigned I = 0; I != 4; ++I)
      Bytes[I] =
          uint8_t(Word >> (8 * (archLittleEndian(GetParam()) ? I : 3 - I)));
    return Bytes;
  }

  std::vector<LowOp> lift(uint32_t Word) const {
    Decoder Decode;
    EXPECT_TRUE(Decode.init(GetParam()));
    const auto Bytes = bytes(Word);
    DecodedInsn Instruction;
    EXPECT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                      Instruction),
              4);
    std::vector<LowOp> Ops;
    Decode.liftToLow(Instruction, Ops);
    return Ops;
  }

  bool execute(NdOpEmulator &Emulator, uint32_t Word) const {
    for (const auto &Op : lift(Word))
      if (!Emulator.step(Op))
        return false;
    return true;
  }

  BinaryImage image(uint64_t First, llvm::ArrayRef<uint8_t> Data) const {
    BinaryImage Image;
    Image.Arch = GetParam();
    Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
    Segment Segment;
    Segment.VA = First;
    Segment.Data.assign(Data.begin(), Data.end());
    Segment.Size = Segment.FileSz = Data.size();
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Segment));
    return Image;
  }
};

TEST_P(MIPSScalar, MergeReadsOnlySelectedBytesAndPreservesOtherLanes) {
  const bool Little = archLittleEndian(GetParam());
  const bool Wide = isMIPS64(GetParam());
  const std::array<uint8_t, 8> Data = {0x83, 0x21, 0x46, 0x65,
                                       0x9a, 0xbc, 0xde, 0xf1};
  for (unsigned Size : {4u, 8u}) {
    if (Size == 8 && !Wide)
      continue;
    for (bool Left : {true, false})
      for (unsigned Offset = 0; Offset != Size; ++Offset)
        for (uint64_t Old :
             {UINT64_C(0x12345678), UINT64_C(0xffffffffaabbccdd)}) {
          SCOPED_TRACE(testing::Message()
                       << Size << ':' << Left << ':' << Offset << ':' << Old);
          // Address/register lanes from the ISA's left/right byte diagrams.
          const unsigned AddressLane = Little ? Size - 1 - Offset : Offset;
          const unsigned First = Left ? Offset : (Little ? Offset : 0);
          const unsigned Start = Left && Little ? 0 : First;
          const unsigned Count = Left ? Size - AddressLane : AddressLane + 1;
          const unsigned RegStart = Left ? Size - Count : 0;
          uint64_t Expected = Wide ? Old : uint32_t(Old);
          for (unsigned I = 0; I != Count; ++I) {
            const unsigned Shift = 8 * (RegStart + I);
            const auto Byte = Data[Start + (Little ? I : Count - 1 - I)];
            Expected = (Expected & ~(UINT64_C(255) << Shift)) |
                       (uint64_t(Byte) << Shift);
          }
          if (Size == 4)
            Expected =
                Wide ? mips::signExtendWord(Expected) : uint32_t(Expected);
          // No bytes outside this subrange exist in the image. An aligned
          // whole-word read plus masking would fail this check.
          auto Image =
              image(0x2000 + Start, llvm::ArrayRef(Data).slice(Start, Count));
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setLoadCollect(true);
          Emulator.setRegister(mipsreg::A0, 0x2010);
          Emulator.setRegister(mipsreg::V0, Wide ? Old : uint32_t(Old));
          const unsigned Opcode =
              Size == 8 ? (Left ? 0x1a : 0x1b) : (Left ? 0x22 : 0x26);
          const uint32_t Word = (Opcode << 26) | (4 << 21) | (2 << 16) |
                                uint16_t(int(Offset) - 16);
          ASSERT_TRUE(execute(Emulator, Word));
          EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected);
          ASSERT_EQ(Emulator.getLoadRecords().size(), Count);
          for (unsigned I = 0; I != Count; ++I) {
            EXPECT_EQ(Emulator.getLoadRecords()[I].Addr, 0x2000 + Start + I);
            EXPECT_EQ(Emulator.getLoadRecords()[I].Size, 1);
          }
          EXPECT_FALSE(Emulator.skips().any());
          if (Count > 1) {
            auto FaultImage = image(
                0x2000 + Start, llvm::ArrayRef(Data).slice(Start, Count - 1));
            NdOpEmulator Fault(FaultImage);
            Fault.setStrictMode(true);
            Fault.setRegister(mipsreg::A0, 0x2010);
            Fault.setRegister(mipsreg::V0, Wide ? Old : uint32_t(Old));
            EXPECT_FALSE(execute(Fault, Word));
            EXPECT_EQ(Fault.getRegister(mipsreg::V0),
                      Wide ? Old : uint32_t(Old));
          }
        }
  }
}

TEST_P(MIPSScalar, ZeroDestinationStillReadsAndBaseMayEqualDestination) {
  const std::array<uint8_t, 4> Data = {0x80, 0x91, 0xa2, 0xb3};
  auto Image = image(0x2000, Data);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  Emulator.setRegister(mipsreg::A0, 0x2000);
  ASSERT_TRUE(execute(Emulator, 0x88800001)); // lwl zero,1(a0)
  EXPECT_FALSE(Emulator.getLoadRecords().empty());
  EXPECT_FALSE(Emulator.getRegister(mipsreg::Zero));
  Emulator.setRegister(mipsreg::A0, 0x3000);
  EXPECT_FALSE(execute(Emulator, 0x88800001));
  Emulator.setRegister(mipsreg::A0, 0x2000);
  ASSERT_TRUE(execute(Emulator, 0x88840001)); // lwl a0,1(a0)
  EXPECT_NE(Emulator.getRegister(mipsreg::A0), 0x2000u);
}

TEST_P(MIPSScalar, MulInvalidatesBothAccumulatorsUntilTheyAreOverwritten) {
  auto Image = image(0x2000, {});
  for (unsigned Destination : {0u, 2u}) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, UINT64_C(0xffffffff80000001));
    Emulator.setRegister(mipsreg::A1, 3);
    Emulator.setRegister(mipsreg::HI, 111);
    Emulator.setRegister(mipsreg::LO, 222);
    ASSERT_TRUE(execute(Emulator, 0x70850002 | (Destination << 11)));
    if (Destination)
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
                isMIPS64(GetParam()) ? UINT64_C(0xffffffff80000003)
                                     : UINT64_C(0x80000003));
    EXPECT_FALSE(Emulator.getRegister(mipsreg::HI));
    EXPECT_FALSE(Emulator.getRegister(mipsreg::LO));
    EXPECT_TRUE(execute(Emulator, 0x00001010)); // mfhi v0 transports unknown
    EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
    EXPECT_FALSE(Emulator.getRegisterBytes(mipsreg::V0));
    EXPECT_TRUE(execute(Emulator, 0x00001012)); // mflo v0 transports unknown
    EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
    EXPECT_FALSE(execute(Emulator, 0x00441021)); // addu v0,v0,a0 observes it
    ASSERT_TRUE(execute(Emulator, 0x00800011));  // mthi a0
    ASSERT_TRUE(execute(Emulator, 0x00001010));
    EXPECT_TRUE(execute(Emulator, 0x00001012));
    EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
    ASSERT_TRUE(execute(Emulator, 0x00850018)); // mult a0,a1
    ASSERT_TRUE(execute(Emulator, 0x00001012));
    EXPECT_FALSE(Emulator.skips().any());
  }
  if (isMIPS64(GetParam())) {
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, UINT64_C(0x0000000080000001));
    Emulator.setRegister(mipsreg::A1, 3);
    Emulator.setRegister(mipsreg::V0, 123);
    EXPECT_FALSE(execute(Emulator, 0x70851002));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 123u);
  }
}

TEST_P(MIPSScalar, CompletePairsDiscardArbitraryIncomingBitsInEitherOrder) {
  const bool Wide = isMIPS64(GetParam());
  const bool Little = archLittleEndian(GetParam());
  const std::array<uint8_t, 16> Data = {0x80, 0x91, 0xa2, 0xb3, 0x44, 0x55,
                                        0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                        0xcc, 0xdd, 0xee, 0xff};
  for (unsigned Size : {4u, 8u}) {
    if (Size == 8 && !Wide)
      continue;
    for (unsigned Offset = 0; Offset != Size; ++Offset)
      for (bool LeftFirst : {false, true})
        for (uint64_t Old : {UINT64_C(0x12345678abcdef98),
                             UINT64_C(0x0000000080000000), UINT64_MAX}) {
          SCOPED_TRACE(testing::Message() << Size << ':' << Offset << ':'
                                          << LeftFirst << ':' << Old);
          auto Image = image(0x2000, Data);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setLoadCollect(true);
          Emulator.setRegister(mipsreg::A0, 0x2000 + Offset);
          Emulator.setRegister(mipsreg::V0, Wide ? Old : uint32_t(Old));
          const uint32_t Left = ((Size == 4 ? 0x22u : 0x1au) << 26) |
                                (4 << 21) | (2 << 16) | (Little ? Size - 1 : 0);
          const uint32_t Right = ((Size == 4 ? 0x26u : 0x1bu) << 26) |
                                 (4 << 21) | (2 << 16) |
                                 (Little ? 0 : Size - 1);
          ASSERT_TRUE(execute(Emulator, LeftFirst ? Left : Right));
          ASSERT_TRUE(execute(Emulator, LeftFirst ? Right : Left));
          uint64_t Expected = 0;
          for (unsigned I = 0; I != Size; ++I)
            Expected |= uint64_t(Data[Offset + I])
                        << (8 * (Little ? I : Size - 1 - I));
          if (Size == 4 && Wide)
            Expected = mips::signExtendWord(Expected);
          EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected);
          EXPECT_GE(Emulator.getLoadRecords().size(), Size);
          EXPECT_FALSE(Emulator.skips().any());
        }
  }
}

TEST_P(MIPSScalar, PartialLwrExposesOnlyBitsSharedByBothPermittedResults) {
  if (!isMIPS64(GetParam()))
    return;
  const std::array<uint8_t, 4> Data = {0x11, 0x22, 0x33, 0x44};
  auto Image = image(0x2000, Data);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::A0, 0x2000);
  // The upper word can be 0x00000080 or zero. Bit 39 is the sole difference.
  Emulator.setRegister(mipsreg::V0, UINT64_C(0x0000008012345678));
  ASSERT_TRUE(execute(Emulator, 0x98820001)); // lwr v0,1(a0)
  EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
  EXPECT_FALSE(Emulator.getRegisterBytes(mipsreg::V0));
  auto Slice = [&](NdVar Source, unsigned Offset, unsigned Bytes) {
    LowOp Op;
    Op.Opcode = NdOp::SUBBYTES;
    Op.Output = NdVar::tmp(0x9000, Bytes);
    Op.addInput(Source);
    Op.addInput(NdVar::cst(Offset, 8));
    return Emulator.step(Op);
  };
  ASSERT_TRUE(Slice(NdVar::reg(mipsreg::V0, 8), 0, 4));
  EXPECT_EQ(Emulator.getRegister(0x9000), archLittleEndian(GetParam())
                                              ? UINT64_C(0x12443322)
                                              : UINT64_C(0x12341122));
  ASSERT_TRUE(Slice(NdVar::reg(mipsreg::V0, 8), 5, 3));
  EXPECT_EQ(Emulator.getRegister(0x9000), 0u);
  ASSERT_TRUE(Slice(NdVar::reg(mipsreg::V0, 8), 4, 1));
  EXPECT_FALSE(Emulator.getRegister(0x9000));
  LowOp Extend;
  Extend.Opcode = NdOp::INT_SEXT;
  Extend.Output = NdVar::tmp(0x9010, 8);
  Extend.addInput(NdVar::tmp(0x9000, 1));
  ASSERT_TRUE(Emulator.step(Extend));
  ASSERT_TRUE(Slice(NdVar::tmp(0x9010, 8), 4, 4));
  EXPECT_FALSE(Emulator.getRegister(0x9000));
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Inputs[0] = NdVar::reg(mipsreg::V0, 4);
  ASSERT_TRUE(Emulator.step(Extend));
  ASSERT_TRUE(Slice(NdVar::tmp(0x9010, 8), 4, 4));
  EXPECT_EQ(Emulator.getRegister(0x9000), 0u);

  // A word store observes only the defined low word; a doubleword store
  // would expose the implementation-dependent bit and must refuse.
  EXPECT_FALSE(execute(Emulator, 0xfc820000)); // sd v0,0(a0)
  ASSERT_TRUE(execute(Emulator, 0xac820000));  // sw v0,0(a0)
  ASSERT_TRUE(execute(Emulator, 0x8c830000));  // lw v1,0(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), archLittleEndian(GetParam())
                                                   ? UINT64_C(0x12443322)
                                                   : UINT64_C(0x12341122));

  Emulator.setRegister(mipsreg::A0, 0x3000);
  EXPECT_FALSE(
      execute(Emulator, 0x88820001)); // fault leaves uncertain rt intact
  EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
  ASSERT_TRUE(Slice(NdVar::reg(mipsreg::V0, 8), 0, 4));
  EXPECT_EQ(Emulator.getRegister(0x9000), archLittleEndian(GetParam())
                                              ? UINT64_C(0x12443322)
                                              : UINT64_C(0x12341122));
}

TEST_P(MIPSScalar, CompletePairsRefineAFullyUnknownDestination) {
  const bool Wide = isMIPS64(GetParam());
  const bool Little = archLittleEndian(GetParam());
  const std::array<uint8_t, 16> Data = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5,
                                        0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb,
                                        0xfc, 0xfd, 0xfe, 0xff};
  for (unsigned Size : {4u, 8u}) {
    if (Size == 8 && !Wide)
      continue;
    for (bool LeftFirst : {false, true}) {
      auto Image = image(0x2000, Data);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(mipsreg::A0, 0x2001);
      LowOp Unknown;
      Unknown.Opcode = NdOp::INTRINSIC;
      Unknown.Output = NdVar::reg(mipsreg::V0, Wide ? 8 : 4);
      Unknown.addInput(NdVar::cst(uint64_t(Intrinsic::MIPSUnpredictable),
                                  sizeof(Intrinsic)));
      ASSERT_TRUE(Emulator.step(Unknown));
      const uint32_t Left = ((Size == 4 ? 0x22u : 0x1au) << 26) | (4 << 21) |
                            (2 << 16) | (Little ? Size - 1 : 0);
      const uint32_t Right = ((Size == 4 ? 0x26u : 0x1bu) << 26) | (4 << 21) |
                             (2 << 16) | (Little ? 0 : Size - 1);
      ASSERT_TRUE(execute(Emulator, LeftFirst ? Left : Right));
      EXPECT_FALSE(Emulator.getRegister(mipsreg::V0));
      ASSERT_TRUE(execute(Emulator, LeftFirst ? Right : Left));
      uint64_t Expected = 0;
      for (unsigned I = 0; I != Size; ++I)
        Expected |= uint64_t(Data[I + 1]) << (8 * (Little ? I : Size - 1 - I));
      if (Size == 4 && Wide)
        Expected = mips::signExtendWord(Expected);
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

TEST_P(MIPSScalar, UnknownValuesRespectWritesCallsResetAndMissingOperands) {
  auto Image = image(0x2000, {});
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  const unsigned Width = isMIPS64(GetParam()) ? 8 : 4;
  LowOp Unknown;
  Unknown.Opcode = NdOp::INTRINSIC;
  Unknown.Output = NdVar::reg(mipsreg::HI, Width);
  Unknown.addInput(
      NdVar::cst(uint64_t(Intrinsic::MIPSUnpredictable), sizeof(Intrinsic)));
  ASSERT_TRUE(Emulator.step(Unknown));
  LowOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = NdVar::reg(mipsreg::S0, Width);
  Copy.addInput(NdVar::reg(mipsreg::HI, Width));
  ASSERT_TRUE(Emulator.step(Copy));
  EXPECT_FALSE(Emulator.getRegister(mipsreg::S0));

  Copy.Output = NdVar::tmp(0x9000, Width);
  ASSERT_TRUE(Emulator.step(Copy));
  Copy.Output.Size = 1;
  Copy.Inputs[0] = NdVar::cst(0x56, 1);
  ASSERT_TRUE(Emulator.step(Copy));
  EXPECT_EQ(Emulator.getRegister(0x9000), 0x56u); // fresh narrow temporary
  Copy.Output = NdVar::reg(mipsreg::S0, 1);
  ASSERT_TRUE(Emulator.step(Copy));
  EXPECT_FALSE(Emulator.getRegister(mipsreg::S0)); // partial physical overwrite
  LowOp Slice;
  Slice.Opcode = NdOp::SUBBYTES;
  Slice.Output = NdVar::tmp(0x9000, 1);
  Slice.addInput(NdVar::reg(mipsreg::S0, Width));
  Slice.addInput(NdVar::cst(0, Width));
  ASSERT_TRUE(Emulator.step(Slice));
  EXPECT_EQ(Emulator.getRegister(0x9000), 0x56u);

  Copy.Output = NdVar::tmp(0x9000, Width);
  Copy.Inputs[0] = NdVar::reg(mipsreg::T9, Width); // never seeded
  ASSERT_TRUE(Emulator.step(Copy));
  EXPECT_FALSE(Emulator.getRegister(0x9000));
  LowOp Join;
  Join.Opcode = NdOp::CONCAT;
  Join.Output = NdVar::tmp(0x9000, 8);
  Join.addInput(NdVar::reg(mipsreg::HI, 4));
  Join.addInput(NdVar::reg(mipsreg::T9, 4));
  ASSERT_TRUE(Emulator.step(Join));
  EXPECT_FALSE(Emulator.getRegister(0x9000));

  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(NdVar::cst(0x2000, Width));
  Store.addInput(NdVar::reg(mipsreg::S0, Width));
  EXPECT_FALSE(Emulator.step(Store));
  Emulator.setCallPreservedRegisters({mipsreg::S0});
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.addInput(NdVar::cst(0x4000, Width));
  ASSERT_TRUE(Emulator.step(Call));
  EXPECT_FALSE(Emulator.getRegister(mipsreg::S0));
  EXPECT_FALSE(Emulator.getRegister(mipsreg::HI));
  ASSERT_TRUE(Emulator.step(Slice));
  EXPECT_EQ(Emulator.getRegister(0x9000), 0x56u);
  Emulator.setRegister(mipsreg::S0, 0x1234);
  EXPECT_EQ(Emulator.getRegister(mipsreg::S0), 0x1234u);
  ASSERT_TRUE(Emulator.step(Unknown));
  Emulator.setRegisterBytes(mipsreg::HI, std::array<uint8_t, 1>{0x78});
  EXPECT_EQ(Emulator.getRegister(mipsreg::HI), 0x78u);
  ASSERT_TRUE(Emulator.step(Unknown));
  Emulator.reset();
  EXPECT_FALSE(Emulator.getRegister(mipsreg::HI));
  Emulator.setRegister(mipsreg::HI, 0x90);
  EXPECT_EQ(Emulator.getRegister(mipsreg::HI), 0x90u);
}

TEST_P(MIPSScalar, UncertainTransportRejectsMalformedViewsBeforeWriting) {
  auto Image = image(0x2000, {});
  NdOpEmulator Emulator(Image);
  const unsigned Width = isMIPS64(GetParam()) ? 8 : 4;
  LowOp Unknown;
  Unknown.Opcode = NdOp::INTRINSIC;
  Unknown.Output = NdVar::reg(mipsreg::HI, Width);
  Unknown.addInput(
      NdVar::cst(uint64_t(Intrinsic::MIPSUnpredictable), sizeof(Intrinsic)));
  ASSERT_TRUE(Emulator.step(Unknown));
  for (unsigned Case = 0; Case != 7; ++Case) {
    SCOPED_TRACE(Case);
    Emulator.setRegister(mipsreg::V0, 123);
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = NdVar::reg(mipsreg::V0, Width);
    Copy.addInput(NdVar::reg(mipsreg::HI, Width));
    switch (Case) {
    case 0:
      Copy.Output.Size = 0;
      break;
    case 1:
      Copy.Output.Size = 16; // vector uncertainty has no scalar contract
      break;
    case 2:
      Copy.Inputs[0].Size = 0;
      break;
    case 3:
      Copy.addInput(NdVar::cst(0, Width));
      break;
    case 4:
      Copy.Opcode = NdOp::SUBBYTES;
      Copy.addInput(NdVar::cst(Width, Width));
      break;
    case 5:
      Copy.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 6:
      Copy.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    }
    EXPECT_FALSE(Emulator.step(Copy));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 123u);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST_P(MIPSScalar, EveryMergeOffsetMatchesPinnedUnicorn) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  const bool Wide = isMIPS64(GetParam());
  const bool Little = archLittleEndian(GetParam());
  const std::array<uint8_t, 8> Data = {0x83, 0x21, 0x46, 0x65,
                                       0x9a, 0xbc, 0xde, 0xf1};
  for (unsigned Size : {4u, 8u}) {
    if (Size == 8 && !Wide)
      continue;
    for (bool Left : {true, false})
      for (unsigned Offset = 0; Offset != Size; ++Offset) {
        SCOPED_TRACE(testing::Message()
                     << Size << ':' << Left << ':' << Offset);
        uc_engine *Raw = nullptr;
        const auto Mode = static_cast<uc_mode>(
            (Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
            (Little ? UC_MODE_LITTLE_ENDIAN : UC_MODE_BIG_ENDIAN));
        ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
        std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw,
                                                                  uc_close);
        ASSERT_EQ(
            uc_ctl_set_cpu_model(Raw, Wide ? int(UC_CPU_MIPS64_MIPS64R2_GENERIC)
                                           : int(UC_CPU_MIPS32_24KC)),
            UC_ERR_OK);
        ASSERT_EQ(uc_mem_map(Raw, 0x1000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
        const unsigned Opcode =
            Size == 8 ? (Left ? 0x1a : 0x1b) : (Left ? 0x22 : 0x26);
        const uint32_t Word = (Opcode << 26) | (4 << 21) | (2 << 16) | Offset;
        const auto Code = bytes(Word);
        ASSERT_EQ(uc_mem_write(Raw, 0x1000, Code.data(), Code.size()),
                  UC_ERR_OK);
        ASSERT_EQ(uc_mem_write(Raw, 0x2000, Data.data(), Data.size()),
                  UC_ERR_OK);
        uint64_t Base = 0x2000, Old = UINT64_C(0xffffffffaabbccdd), Actual = 0;
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_A0, &Base), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_V0, &Old), UC_ERR_OK);
        ASSERT_EQ(uc_emu_start(Raw, 0x1000, 0x1004, 0, 1), UC_ERR_OK);
        ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_V0, &Actual), UC_ERR_OK);
        auto Image = image(0x2000, Data);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(mipsreg::A0, Base);
        Emulator.setRegister(mipsreg::V0, Wide ? Old : uint32_t(Old));
        ASSERT_TRUE(execute(Emulator, Word));
        EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
                  Wide ? Actual : uint32_t(Actual));
      }
  }
#endif
}

TEST_P(MIPSScalar, MalformedScalarContractsFailBeforeReadingOrWriting) {
  const unsigned Width = isMIPS64(GetParam()) ? 8 : 4;
  const std::array<uint8_t, 4> Data = {0x80, 0x91, 0xa2, 0xb3};
  auto Image = image(0x2000, Data);
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::reg(mipsreg::V0, 4);
    Op.addInput(NdVar::cst(uint64_t(Intrinsic::MIPSLwl), sizeof(Intrinsic)));
    Op.addInput(NdVar::cst(0x2000, Width));
    Op.addInput(NdVar::cst(0, 4));
    switch (Mutation) {
    case 0:
      Op.NumInputs = 2;
      break;
    case 1:
      Op.Inputs[0].Size = 8;
      break;
    case 2:
      Op.Inputs[1].Size = 2;
      break;
    case 3:
      Op.Inputs[2].Size = 8;
      break;
    case 4:
      Op.Output.Size = 8;
      break;
    case 5:
      Op.Output = NdVar::cst(0, 4);
      break;
    case 6:
      Op.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 7:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    }
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(mipsreg::V0, 123);
    EXPECT_FALSE(Emulator.step(Op));
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 123u);
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_FALSE(Emulator.skips().any());
  }
}

INSTANTIATE_TEST_SUITE_P(EndianAndWidth, MIPSScalar,
                         testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                         Arch::MIPS64, Arch::MIPS64EL));

} // namespace
