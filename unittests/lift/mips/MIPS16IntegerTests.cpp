//===- MIPS16IntegerTests.cpp - MIPS16e32 scalar semantics ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Literal encodings, independent results, register fields and pinned target
/// execution qualify the first MIPS16 scalar families in both byte orders.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <cstdint>
#include <memory>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> bytes(uint16_t First, uint16_t Second, unsigned Size,
                           Arch Target) {
  std::vector<uint8_t> Result;
  for (uint16_t Half : {First, Second}) {
    Result.push_back(Half >> (archLittleEndian(Target) ? 0 : 8));
    Result.push_back(Half >> (archLittleEndian(Target) ? 8 : 0));
  }
  Result.resize(Size);
  return Result;
}

struct Scalar {
  const char *Name;
  uint16_t First, Second;
  unsigned Size, Destination;
  uint32_t Result;
};
constexpr Scalar Scalars[] = {
#define MIPS16_SCALAR(Name, First, Second, Size, Destination, Result)          \
  {#Name, First, Second, Size, Destination, Result},
#include "MIPS16ScalarFixtures.def"
};

class MIPS16Integer : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPS16Integer, LiteralFieldsAndTruncationsHaveIndependentExpectations) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  for (const auto &Case : Scalars) {
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    DecodedInsn Insn;
    ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x10002, Insn),
              int(Case.Size));
    std::vector<LowOp> Ops;
    Decode.liftToLow(Insn, Ops);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(2), 0x89abcdef);
    Emulator.setRegister(mipsreg::r(4), 0x89abcdef);
    Emulator.setRegister(mipsreg::r(5), 0x12345678);
    Emulator.setRegister(mipsreg::SP, 0x700000);
    for (const auto &Op : Ops) {
      ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(Op.Addr, 0x10002u);
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::r(Case.Destination)), Case.Result);
    EXPECT_FALSE(Emulator.skips().any());
    for (unsigned Length = 0; Length < Case.Size; ++Length)
      EXPECT_EQ(Decode.decodeOneForLift(Code.data(), Length, 0x10002, Insn), 0);
    EXPECT_EQ(Decode.decodeOneLight(Code.data(), Code.size(), 0x10003, Insn),
              0);
  }
}

TEST_P(MIPS16Integer, FullDestinationMoveCoversBothCompactBanksAndZero) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  for (unsigned Source = 0; Source != 8; ++Source)
    for (unsigned Destination = 0; Destination != 32; ++Destination) {
      SCOPED_TRACE(Source);
      SCOPED_TRACE(Destination);
      const unsigned Register = Source < 2 ? Source + 16 : Source;
      const uint16_t Half = 0x6500 | ((Destination & 7) << 5) |
                            ((Destination >> 3) << 3) | Source;
      const auto Code = bytes(Half, 0, 2, GetParam());
      DecodedInsn Insn;
      ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Insn),
                2);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Insn, Ops);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      // A write to zero has no input dependency, even with an unknown source.
      if (Destination)
        Emulator.setRegister(mipsreg::r(Register), 0xa5000000 + Register);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emulator.step(Op));
      if (Destination)
        EXPECT_EQ(Emulator.getRegister(mipsreg::r(Destination)),
                  0xa5000000 + Register);
      EXPECT_FALSE(Emulator.skips().any());
    }
}

TEST_P(MIPS16Integer,
       ShortComparisonsZeroExtendButExtendedComparisonsSignExtend) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  for (bool Signed : {false, true})
    for (bool Extended : {false, true}) {
      const uint16_t Short = Signed ? 0x54ff : 0x5cff;
      const auto Code =
          Extended ? bytes(0xf7ff, Signed ? 0x541f : 0x5c1f, 4, GetParam())
                   : bytes(Short, 0, 2, GetParam());
      DecodedInsn Insn;
      ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Insn),
                int(Code.size()));
      std::vector<LowOp> Ops;
      Decode.liftToLow(Insn, Ops);
      for (uint32_t Input :
           {0u, 128u, 255u, 256u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(mipsreg::A0, Input);
        for (const auto &Op : Ops)
          ASSERT_TRUE(Emulator.step(Op));
        const bool Expected = Signed ? int32_t(Input) < (Extended ? -1 : 255)
                                     : Input < (Extended ? 0xffffffffu : 255u);
        EXPECT_EQ(Emulator.getRegister(mipsreg::r(24)), unsigned(Expected));
      }
    }
}

TEST_P(MIPS16Integer, DecoderAcceptanceDoesNotAdmitUnqualifiedSemantics) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  // PC-relative LW and the other MOVE form need separate decoder/PC contracts.
  for (uint16_t Half : {0xb200, 0x6744}) {
    const auto Code = bytes(Half, 0, 2, GetParam());
    DecodedInsn Insn;
    ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, Insn),
              2);
    std::vector<LowOp> Ops;
    EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST_P(MIPS16Integer, ScalarsMatchPinned24KfExecution) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    static_cast<uc_mode>(UC_MODE_MIPS32 |
                                         (archLittleEndian(GetParam())
                                              ? UC_MODE_LITTLE_ENDIAN
                                              : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_24KF), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  unsigned Index = 0;
  for (const auto &Case : Scalars) {
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    const uint64_t Address = 0x10002 + Index++ * 8;
    ASSERT_EQ(uc_mem_write(Raw, Address, Code.data(), Code.size()), UC_ERR_OK);
    uint32_t A = 0x89abcdef, B = 0x12345678, Stack = 0x700000, Result = 0;
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_2, &A), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &A), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &B), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_SP, &Stack), UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(Raw, Address | 1, 0x11000, 0, 1), UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Case.Destination, &Result),
              UC_ERR_OK);
    EXPECT_EQ(Result, Case.Result);
  }
#endif
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Integer,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
