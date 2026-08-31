//===- X86_64_APXNddNfSemanticTests.cpp - APX NDD/NF semantics ---------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr va_t kDataAddress = 0x4000;

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

struct Flags {
  bool CF = false;
  bool PF = false;
  bool AF = false;
  bool ZF = false;
  bool SF = false;
  bool OF = false;
  bool DF = false;
};

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  if (Insn.Raw) {
    try {
      Dec.liftToLow(Insn, Result.Ops);
    } catch (const UnliftedInstruction &) {
      ADD_FAILURE() << "APX NDD/NF instruction was not lifted";
    }
  }
  return Result;
}

template <typename Mutator>
void expectMutatedLiftRejected(const std::vector<uint8_t> &Bytes,
                               Mutator Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(*Insn.Raw);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

BinaryImage imageWithValue(uint64_t Value, unsigned Width) {
  BinaryImage Image = emptyImage();
  Segment Data;
  Data.VA = kDataAddress;
  Data.Size = Width;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(Width);
  for (unsigned I = 0; I < Width; ++I)
    Data.Data[I] = static_cast<uint8_t>(Value >> (I * 8));
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void setGpr(NdOpEmulator &Emulator, x86_reg Reg, uint64_t Value) {
  Emulator.setRegister(mapCapstoneReg(Reg).Offset, Value);
}

uint64_t getGpr(const NdOpEmulator &Emulator, x86_reg Reg) {
  const auto Value = Emulator.getRegister(mapCapstoneReg(Reg).Offset);
  EXPECT_TRUE(Value.has_value());
  return Value.value_or(0);
}

void setFlags(NdOpEmulator &Emulator, const Flags &Value) {
  Emulator.setRegister(x86reg::CF, Value.CF);
  Emulator.setRegister(x86reg::PF, Value.PF);
  Emulator.setRegister(x86reg::AF, Value.AF);
  Emulator.setRegister(x86reg::ZF, Value.ZF);
  Emulator.setRegister(x86reg::SF, Value.SF);
  Emulator.setRegister(x86reg::OF, Value.OF);
  Emulator.setRegister(x86reg::DF, Value.DF);
}

void expectFlags(const NdOpEmulator &Emulator, const Flags &Expected) {
  EXPECT_EQ(Emulator.getRegister(x86reg::CF), Expected.CF);
  EXPECT_EQ(Emulator.getRegister(x86reg::PF), Expected.PF);
  EXPECT_EQ(Emulator.getRegister(x86reg::AF), Expected.AF);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), Expected.ZF);
  EXPECT_EQ(Emulator.getRegister(x86reg::SF), Expected.SF);
  EXPECT_EQ(Emulator.getRegister(x86reg::OF), Expected.OF);
  EXPECT_EQ(Emulator.getRegister(x86reg::DF), Expected.DF);
}

TEST(X86APXNddNfSemantics, AdcNddReadsBothExplicitSourcesAndCarry) {
  const LiftedInstruction Lifted =
      liftX64({0x62, 0x4c, 0x84, 0x10, 0x11, 0xf5});
  ASSERT_EQ(Lifted.Id, X86_INS_ADC);
  ASSERT_FALSE(Lifted.Ops.empty());

  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, {true, false, false, false, true, true, true});
  setGpr(Emulator, X86_REG_R31, UINT64_C(0x1111111111111111));
  setGpr(Emulator, X86_REG_R29, UINT64_MAX);
  setGpr(Emulator, X86_REG_R30, 0);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());

  EXPECT_EQ(getGpr(Emulator, X86_REG_R31), 0U);
  EXPECT_EQ(getGpr(Emulator, X86_REG_R29), UINT64_MAX);
  EXPECT_EQ(getGpr(Emulator, X86_REG_R30), 0U);
  expectFlags(Emulator, {true, true, true, true, false, false, true});
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXNddNfSemantics, AdcNddRejectsDetailThatDisagreesWithRawSources) {
  const std::vector<uint8_t> Bytes = {0x62, 0x4c, 0x84, 0x10, 0x11, 0xf5};
  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.detail->x86.operands[2].reg = X86_REG_R28;
  });
}

TEST(X86APXNddNfSemantics, SbbNddConsumesCarryAndRejectsReservedNf) {
  const std::vector<uint8_t> Bytes = {0x62, 0x4c, 0x84, 0x10, 0x1b, 0xf5};
  const LiftedInstruction Lifted = liftX64(Bytes);
  ASSERT_EQ(Lifted.Id, X86_INS_SBB);
  ASSERT_FALSE(Lifted.Ops.empty());

  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, {true, false, false, true, false, true, true});
  setGpr(Emulator, X86_REG_R31, UINT64_C(0x1111111111111111));
  setGpr(Emulator, X86_REG_R30, 0);
  setGpr(Emulator, X86_REG_R29, 0);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R31), UINT64_MAX);
  expectFlags(Emulator, {true, true, true, false, true, false, true});

  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.bytes[3] |= 0x04;
    Insn.detail->x86.opcode[3] |= 0x04;
  });
}

TEST(X86APXNddNfSemantics, ShrNddNfUsesDedicatedSourceAndPreservesFlags) {
  const LiftedInstruction Lifted =
      liftX64({0x62, 0xdc, 0x04, 0x14, 0xd3, 0xee});
  ASSERT_EQ(Lifted.Id, X86_INS_SHR);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R31, UINT64_C(0xaaaaaaaa55555555));
  setGpr(Emulator, X86_REG_R30, UINT64_C(0x80000000));
  setGpr(Emulator, X86_REG_RCX, 1);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());

  EXPECT_EQ(getGpr(Emulator, X86_REG_R31), UINT64_C(0x40000000));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R30), UINT64_C(0x80000000));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXNddNfSemantics, IncNddNfReadsSourceAndPreservesAllFlags) {
  const LiftedInstruction Lifted =
      liftX64({0x62, 0xec, 0xf5, 0x14, 0xff, 0xc3});
  ASSERT_EQ(Lifted.Id, X86_INS_INC);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Emulator, X86_REG_R19, UINT64_MAX);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R17), 0U);
  EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_MAX);
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXNddNfSemantics, DecAndNegNddNfUseSourceAndPreserveAllFlags) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Id;
    uint64_t Source;
    uint64_t Expected;
  };
  const std::vector<Case> Cases = {
      {"dec", {0x62, 0xec, 0xf5, 0x14, 0xff, 0xcb}, X86_INS_DEC, 0, UINT64_MAX},
      {"neg",
       {0x62, 0xec, 0xf5, 0x14, 0xf7, 0xdb},
       X86_INS_NEG,
       5,
       UINT64_MAX - 4},
  };
  constexpr Flags Input{true, false, true, false, true, true, true};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    const LiftedInstruction Lifted = liftX64(C.Bytes);
    ASSERT_EQ(Lifted.Id, C.Id);
    ASSERT_FALSE(Lifted.Ops.empty());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, C.Source);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), C.Expected);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), C.Source);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXNddNfSemantics, NotNddUsesSourceAndRejectsReservedNf) {
  const std::vector<uint8_t> Bytes = {0x62, 0xec, 0xf5, 0x10, 0xf7, 0xd3};
  const LiftedInstruction Lifted = liftX64(Bytes);
  ASSERT_EQ(Lifted.Id, X86_INS_NOT);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Emulator, X86_REG_R19, UINT64_C(0x0f0f0f0f0f0f0f0f));
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0xf0f0f0f0f0f0f0f0));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_C(0x0f0f0f0f0f0f0f0f));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());

  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.bytes[3] |= 0x04;
    Insn.detail->x86.opcode[3] |= 0x04;
  });
  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.detail->x86.operands[1].reg = X86_REG_R18;
  });
}

TEST(X86APXNddNfSemantics, RemainingSingleShiftsAndRotatesHonorNddAndNf) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Id;
    uint64_t Source;
    uint64_t Expected;
  };
  const std::vector<Case> Cases = {
      {"rol-imm",
       {0x62, 0xdc, 0x04, 0x14, 0xc1, 0xc6, 0x01},
       X86_INS_ROL,
       UINT64_C(0x80000001),
       3},
      {"ror-one",
       {0x62, 0xdc, 0x04, 0x14, 0xd1, 0xce},
       X86_INS_ROR,
       UINT64_C(0x80000001),
       UINT64_C(0xc0000000)},
      {"shl-imm",
       {0x62, 0xdc, 0x04, 0x14, 0xc1, 0xe6, 0x03},
       X86_INS_SHL,
       UINT64_C(0x20000001),
       8},
      {"sar-one",
       {0x62, 0xdc, 0x04, 0x14, 0xd1, 0xfe},
       X86_INS_SAR,
       UINT64_C(0x80000001),
       UINT64_C(0xc0000000)},
  };
  constexpr Flags Input{true, false, true, false, true, true, true};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    const LiftedInstruction Lifted = liftX64(C.Bytes);
    ASSERT_EQ(Lifted.Id, C.Id);
    ASSERT_FALSE(Lifted.Ops.empty());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R31, UINT64_C(0xaaaaaaaa55555555));
    setGpr(Emulator, X86_REG_R30, C.Source);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R31), C.Expected);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R30), C.Source);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }

  expectMutatedLiftRejected(Cases.front().Bytes, [](cs_insn &Insn) {
    Insn.detail->x86.operands[1].reg = X86_REG_R28D;
  });
}

TEST(X86APXNddNfSemantics, CarryRotatesHonorNddCarryInputAndRejectReservedNf) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Id;
    uint64_t Source;
    uint64_t Expected;
  };
  const std::vector<Case> Cases = {
      {"rcl-one",
       {0x62, 0xdc, 0x04, 0x10, 0xd1, 0xd6},
       X86_INS_RCL,
       UINT64_C(0x80000000),
       1},
      {"rcr-imm",
       {0x62, 0xdc, 0x04, 0x10, 0xc1, 0xde, 0x01},
       X86_INS_RCR,
       1,
       UINT64_C(0x80000000)},
  };
  constexpr Flags Input{true, false, true, false, true, false, true};
  constexpr Flags Expected{true, false, true, false, true, true, true};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    const LiftedInstruction Lifted = liftX64(C.Bytes);
    ASSERT_EQ(Lifted.Id, C.Id);
    ASSERT_FALSE(Lifted.Ops.empty());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R31, UINT64_C(0xaaaaaaaa55555555));
    setGpr(Emulator, X86_REG_R30, C.Source);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R31), C.Expected);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R30), C.Source);
    expectFlags(Emulator, Expected);
    EXPECT_FALSE(Emulator.skips().any());

    expectMutatedLiftRejected(C.Bytes, [](cs_insn &Insn) {
      Insn.bytes[3] |= 0x04;
      Insn.detail->x86.opcode[3] |= 0x04;
    });
  }
}

TEST(X86APXNddNfSemantics, DoubleShiftsHonorThirdSourceAndPreserveFlags) {
  constexpr Flags Input{true, false, true, false, true, true, true};
  {
    const LiftedInstruction Lifted =
        liftX64({0x62, 0xec, 0xf4, 0x14, 0xa5, 0xd3});
    ASSERT_EQ(Lifted.Id, X86_INS_SHLD);
    ASSERT_FALSE(Lifted.Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, UINT64_C(0x0123456789abcdef));
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xfedcba9876543210));
    setGpr(Emulator, X86_REG_RCX, 8);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0x23456789abcdeffe));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_C(0x0123456789abcdef));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), UINT64_C(0xfedcba9876543210));
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }

  const std::vector<uint8_t> Shrd = {0x62, 0xec, 0x74, 0x14, 0x2c, 0xd3, 0x08};
  {
    const LiftedInstruction Lifted = liftX64(Shrd);
    ASSERT_EQ(Lifted.Id, X86_INS_SHRD);
    ASSERT_FALSE(Lifted.Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, UINT64_C(0x89abcdef));
    setGpr(Emulator, X86_REG_R18, UINT64_C(0x76543210));
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0x1089abcd));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_C(0x89abcdef));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), UINT64_C(0x76543210));
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }
  expectMutatedLiftRejected(Shrd, [](cs_insn &Insn) {
    Insn.detail->x86.operands[2].reg = X86_REG_R20D;
  });
}

TEST(X86APXNddNfSemantics,
     DoubleShiftNddNfReadsSegmentedMemoryWithoutWritingIt) {
  const std::vector<uint8_t> Bytes = {0x64, 0x62, 0x0c, 0xf4, 0x14,
                                      0xad, 0x54, 0xb5, 0x20};
  const LiftedInstruction Lifted = liftX64(Bytes);
  ASSERT_EQ(Lifted.Id, X86_INS_SHRD);
  ASSERT_FALSE(Lifted.Ops.empty());
  bool SawLoad = false;
  for (const LowOp &Op : Lifted.Ops) {
    SawLoad |= Op.Opcode == NdOp::LOAD;
    EXPECT_NE(Op.Opcode, NdOp::STORE);
  }
  EXPECT_TRUE(SawLoad);

  constexpr Flags Input{true, false, true, false, true, true, true};
  BinaryImage Image = imageWithValue(UINT64_C(0x0123456789abcdef), 8);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Emulator, X86_REG_R26, UINT64_C(0xfe));
  setGpr(Emulator, X86_REG_R29, UINT64_C(0x10));
  setGpr(Emulator, X86_REG_R14, 4);
  setGpr(Emulator, X86_REG_RCX, 8);
  ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                 kDataAddress - 0x40));
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0xfe0123456789abcd));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R26), UINT64_C(0xfe));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());

  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.detail->x86.operands[1].mem.index = X86_REG_R13;
  });
}

TEST(X86APXNddNfSemantics,
     DoubleShiftNddNfZeroCountPreservesDestinationAndFlags) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Id;
  };
  const std::vector<Case> Cases = {
      {"shld-cl-zero", {0x62, 0xec, 0xf4, 0x14, 0xa5, 0xd3}, X86_INS_SHLD},
      {"shrd-imm-zero",
       {0x62, 0xec, 0xf4, 0x14, 0x2c, 0xd3, 0x00},
       X86_INS_SHRD},
  };
  constexpr Flags Input{true, false, true, false, true, true, true};
  constexpr uint64_t Base = UINT64_C(0x0123456789abcdef);
  constexpr uint64_t Source = UINT64_C(0xfedcba9876543210);

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    const LiftedInstruction Lifted = liftX64(C.Bytes);
    ASSERT_EQ(Lifted.Id, C.Id);
    ASSERT_FALSE(Lifted.Ops.empty());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, Base);
    setGpr(Emulator, X86_REG_R18, Source);
    setGpr(Emulator, X86_REG_RCX, 0);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), Base);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), Base);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), Source);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXNddNfSemantics,
     CarryRotateNddZeroCountPreservesDestinationAndFlags) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Id;
    uint64_t Source;
  };
  const std::vector<Case> Cases = {
      {"rcl-imm-zero",
       {0x62, 0xdc, 0x84, 0x10, 0xc1, 0xd6, 0x00},
       X86_INS_RCL,
       UINT64_C(0x0123456789abcdef)},
      {"rcr-imm-zero",
       {0x62, 0xdc, 0x84, 0x10, 0xc1, 0xde, 0x00},
       X86_INS_RCR,
       UINT64_C(0x0123456789abcdee)},
  };
  constexpr Flags Input{true, false, true, false, true, true, true};

  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Name);
    const LiftedInstruction Lifted = liftX64(C.Bytes);
    ASSERT_EQ(Lifted.Id, C.Id);
    ASSERT_FALSE(Lifted.Ops.empty());

    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R31, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R30, C.Source);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R31), C.Source);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R30), C.Source);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXNddNfSemantics, ImulNddNfUsesBothSourcesAndPreservesFlags) {
  const std::vector<uint8_t> Bytes = {0x62, 0xec, 0xf4, 0x14, 0xaf, 0xd3};
  const LiftedInstruction Lifted = liftX64(Bytes);
  ASSERT_EQ(Lifted.Id, X86_INS_IMUL);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Emulator, X86_REG_R18, static_cast<uint64_t>(-3));
  setGpr(Emulator, X86_REG_R19, 7);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_MAX - 20);
  EXPECT_EQ(getGpr(Emulator, X86_REG_R18), static_cast<uint64_t>(-3));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R19), 7U);
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());

  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.detail->x86.operands[2].reg = X86_REG_R20;
  });
}

TEST(X86APXNddNfSemantics, ImulImmediateNfHonorsZeroUpperAndPreservesFlags) {
  constexpr Flags Input{true, false, true, false, true, true, true};

  // IMUL 6B uses ND as ZU rather than as an NDD selector.  With a 16-bit
  // operand, ND=1 therefore clears the complete destination container above
  // the result while VVVVV remains the architectural zero value.
  const std::vector<uint8_t> ZeroUpper = {0x62, 0xec, 0x7d, 0x1c,
                                          0x6b, 0xcb, 0xfd};
  const LiftedInstruction ZeroUpperLifted = liftX64(ZeroUpper);
  ASSERT_EQ(ZeroUpperLifted.Id, X86_INS_IMUL);
  ASSERT_FALSE(ZeroUpperLifted.Ops.empty());

  NdOpEmulator ZeroUpperEmulator(emptyImage());
  ZeroUpperEmulator.setStrictMode(true);
  setFlags(ZeroUpperEmulator, Input);
  setGpr(ZeroUpperEmulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaa1234));
  setGpr(ZeroUpperEmulator, X86_REG_R19, 7);
  ASSERT_EQ(ZeroUpperEmulator.run(ZeroUpperLifted.Ops),
            ZeroUpperLifted.Ops.size());
  EXPECT_EQ(getGpr(ZeroUpperEmulator, X86_REG_R17), UINT64_C(0xffeb));
  EXPECT_EQ(getGpr(ZeroUpperEmulator, X86_REG_R19), 7U);
  expectFlags(ZeroUpperEmulator, Input);
  EXPECT_FALSE(ZeroUpperEmulator.skips().any());

  // The otherwise-identical ND=0 form retains legacy 16-bit merge behavior.
  const LiftedInstruction Merge =
      liftX64({0x62, 0xec, 0x7d, 0x0c, 0x6b, 0xcb, 0xfd});
  ASSERT_EQ(Merge.Id, X86_INS_IMUL);
  ASSERT_FALSE(Merge.Ops.empty());
  NdOpEmulator MergeEmulator(emptyImage());
  MergeEmulator.setStrictMode(true);
  setFlags(MergeEmulator, Input);
  setGpr(MergeEmulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaa1234));
  setGpr(MergeEmulator, X86_REG_R19, 7);
  ASSERT_EQ(MergeEmulator.run(Merge.Ops), Merge.Ops.size());
  EXPECT_EQ(getGpr(MergeEmulator, X86_REG_R17), UINT64_C(0xaaaaaaaaaaaaffeb));
  EXPECT_EQ(getGpr(MergeEmulator, X86_REG_R19), 7U);
  expectFlags(MergeEmulator, Input);
  EXPECT_FALSE(MergeEmulator.skips().any());

  // Opcode 69 carries a sign-extended imm32 for a 64-bit operation.
  const std::vector<uint8_t> Imm32 = {0x62, 0xec, 0xfc, 0x0c, 0x69,
                                      0xcb, 0xfe, 0xff, 0xff, 0xff};
  const LiftedInstruction Imm32Lifted = liftX64(Imm32);
  ASSERT_EQ(Imm32Lifted.Id, X86_INS_IMUL);
  ASSERT_FALSE(Imm32Lifted.Ops.empty());
  NdOpEmulator Imm32Emulator(emptyImage());
  Imm32Emulator.setStrictMode(true);
  setFlags(Imm32Emulator, Input);
  setGpr(Imm32Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Imm32Emulator, X86_REG_R19, 7);
  ASSERT_EQ(Imm32Emulator.run(Imm32Lifted.Ops), Imm32Lifted.Ops.size());
  EXPECT_EQ(getGpr(Imm32Emulator, X86_REG_R17), static_cast<uint64_t>(-14));
  EXPECT_EQ(getGpr(Imm32Emulator, X86_REG_R19), 7U);
  expectFlags(Imm32Emulator, Input);
  EXPECT_FALSE(Imm32Emulator.skips().any());

  expectMutatedLiftRejected(ZeroUpper, [](cs_insn &Insn) {
    Insn.detail->x86.operands[1].reg = X86_REG_R20W;
  });
  expectMutatedLiftRejected(ZeroUpper, [](cs_insn &Insn) {
    Insn.bytes[2] ^= 0x08;
    Insn.detail->x86.opcode[2] ^= 0x08;
  });
  expectMutatedLiftRejected(
      Imm32, [](cs_insn &Insn) { ++Insn.detail->x86.operands[2].imm; });
}

TEST(X86APXNddNfSemantics, ImulOneOperandNfProducesSignedDoubleWidthProduct) {
  constexpr Flags Input{true, false, true, false, true, true, true};

  const std::vector<uint8_t> Wide = {0x62, 0xfc, 0xfc, 0x0c, 0xf7, 0xeb};
  const LiftedInstruction WideLifted = liftX64(Wide);
  ASSERT_EQ(WideLifted.Id, X86_INS_IMUL);
  ASSERT_FALSE(WideLifted.Ops.empty());

  NdOpEmulator WideEmulator(emptyImage());
  WideEmulator.setStrictMode(true);
  setFlags(WideEmulator, Input);
  setGpr(WideEmulator, X86_REG_RAX, static_cast<uint64_t>(-3));
  setGpr(WideEmulator, X86_REG_RDX, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(WideEmulator, X86_REG_R19, 7);
  ASSERT_EQ(WideEmulator.run(WideLifted.Ops), WideLifted.Ops.size());
  EXPECT_EQ(getGpr(WideEmulator, X86_REG_RAX), static_cast<uint64_t>(-21));
  EXPECT_EQ(getGpr(WideEmulator, X86_REG_RDX), UINT64_MAX);
  EXPECT_EQ(getGpr(WideEmulator, X86_REG_R19), 7U);
  expectFlags(WideEmulator, Input);
  EXPECT_FALSE(WideEmulator.skips().any());

  // F6 is an IGNORED-W byte form.  W=1 must remain a valid encoding and the
  // signed byte product is written to AX without touching flags under NF.
  const std::vector<uint8_t> Byte = {0x62, 0xf4, 0xfc, 0x0c, 0xf6, 0xeb};
  const LiftedInstruction ByteLifted = liftX64(Byte);
  ASSERT_EQ(ByteLifted.Id, X86_INS_IMUL);
  ASSERT_FALSE(ByteLifted.Ops.empty());
  NdOpEmulator ByteEmulator(emptyImage());
  ByteEmulator.setStrictMode(true);
  setFlags(ByteEmulator, Input);
  setGpr(ByteEmulator, X86_REG_RAX, UINT64_C(0xaaaaaaaaaaaaaafd));
  setGpr(ByteEmulator, X86_REG_RBX, 7);
  ASSERT_EQ(ByteEmulator.run(ByteLifted.Ops), ByteLifted.Ops.size());
  EXPECT_EQ(getGpr(ByteEmulator, X86_REG_RAX), UINT64_C(0xaaaaaaaaaaaaffeb));
  EXPECT_EQ(getGpr(ByteEmulator, X86_REG_RBX), 7U);
  expectFlags(ByteEmulator, Input);
  EXPECT_FALSE(ByteEmulator.skips().any());

  expectMutatedLiftRejected(Wide, [](cs_insn &Insn) {
    Insn.bytes[3] |= 0x10;
    Insn.detail->x86.opcode[3] |= 0x10;
  });
  expectMutatedLiftRejected(
      Byte, [](cs_insn &Insn) { Insn.detail->regs_write[1] = X86_REG_BH; });
}

TEST(X86APXNddNfSemantics, MulNfPreservesFlagsAndRejectsNdd) {
  const std::vector<uint8_t> Bytes = {0x62, 0xec, 0xfd, 0x0c, 0xf7, 0xe3};
  const LiftedInstruction Lifted = liftX64(Bytes);
  ASSERT_EQ(Lifted.Id, X86_INS_MUL);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_RAX, UINT64_MAX);
  setGpr(Emulator, X86_REG_RDX, UINT64_C(0xaaaaaaaaaaaaaaaa));
  setGpr(Emulator, X86_REG_R19, 2);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_RAX), UINT64_MAX - 1);
  EXPECT_EQ(getGpr(Emulator, X86_REG_RDX), 1U);
  EXPECT_EQ(getGpr(Emulator, X86_REG_R19), 2U);
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());

  expectMutatedLiftRejected(Bytes, [](cs_insn &Insn) {
    Insn.bytes[3] |= 0x10;
    Insn.detail->x86.opcode[3] |= 0x10;
  });
}

TEST(X86APXNddNfSemantics, ByteMulNfAcceptsIgnoredWAndPreservesFlags) {
  // F6 is an IGNORED-W byte form.  W=1 is legal for MUL just as it is for the
  // one-operand IMUL form covered above.
  const LiftedInstruction Lifted =
      liftX64({0x62, 0xec, 0xfc, 0x0c, 0xf6, 0xe3});
  ASSERT_EQ(Lifted.Id, X86_INS_MUL);
  ASSERT_FALSE(Lifted.Ops.empty());

  constexpr Flags Input{true, false, true, false, true, true, true};
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_RAX, UINT64_C(0xaaaaaaaaaaaaaaff));
  setGpr(Emulator, X86_REG_R19, UINT64_C(0xbbbbbbbbbbbbbb02));
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_RAX), UINT64_C(0xaaaaaaaaaaaa01fe));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_C(0xbbbbbbbbbbbbbb02));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());
}

} // namespace
