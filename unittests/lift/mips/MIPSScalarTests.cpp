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
    EXPECT_FALSE(execute(Emulator, 0x00001010)); // mfhi v0
    EXPECT_FALSE(execute(Emulator, 0x00001012)); // mflo v0
    ASSERT_TRUE(execute(Emulator, 0x00800011));  // mthi a0
    ASSERT_TRUE(execute(Emulator, 0x00001010));
    EXPECT_FALSE(execute(Emulator, 0x00001012));
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
