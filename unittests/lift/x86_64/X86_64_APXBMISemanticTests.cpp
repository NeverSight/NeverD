//===- X86_64_APXBMISemanticTests.cpp - APX BMI/ADX semantics -----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr va_t kDataAddress = 0x7000;

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
  const int Decoded = Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                           kInstructionAddress, Insn);
  if (Decoded != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "APX BMI/ADX instruction was not decoded exactly";
    return {};
  }
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "APX BMI/ADX instruction was not lifted";
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

BinaryImage imageWithValue(uint64_t Value, unsigned Width) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
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

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
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

uint64_t widthMask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - UINT64_C(1);
}

uint64_t bextr(uint64_t Source, uint64_t Control, unsigned Width) {
  const unsigned Bits = Width * 8;
  const unsigned Start = Control & 0xff;
  const unsigned Length = (Control >> 8) & 0xff;
  if (Start >= Bits || Length == 0)
    return 0;
  const unsigned Effective = std::min(Length, Bits - Start);
  const uint64_t Mask =
      Effective == 64 ? UINT64_MAX : (UINT64_C(1) << Effective) - UINT64_C(1);
  return (Source >> Start) & Mask;
}

uint64_t pdep(uint64_t Source, uint64_t Mask, unsigned Width) {
  const unsigned Bits = Width * 8;
  uint64_t Result = 0;
  unsigned SourceBit = 0;
  for (unsigned Bit = 0; Bit < Bits; ++Bit) {
    if (((Mask >> Bit) & 1) == 0)
      continue;
    Result |= ((Source >> SourceBit) & 1) << Bit;
    ++SourceBit;
  }
  return Result & widthMask(Width);
}

uint64_t pext(uint64_t Source, uint64_t Mask, unsigned Width) {
  const unsigned Bits = Width * 8;
  uint64_t Result = 0;
  unsigned DestinationBit = 0;
  for (unsigned Bit = 0; Bit < Bits; ++Bit) {
    if (((Mask >> Bit) & 1) == 0)
      continue;
    Result |= ((Source >> Bit) & 1) << DestinationBit;
    ++DestinationBit;
  }
  return Result & widthMask(Width);
}

TEST(X86APXBMISemantics, NfFamiliesComputeResultsAndSuppressEveryFlagWrite) {
  struct Form {
    unsigned Id;
    std::vector<uint8_t> Bytes;
    x86_reg Destination;
  };
  const std::array<Form, 6> Forms = {{
      {X86_INS_ANDN, {0x62, 0xea, 0xf4, 0x04, 0xf2, 0xd3}, X86_REG_R18},
      {X86_INS_BEXTR, {0x62, 0xea, 0xf4, 0x04, 0xf7, 0xd3}, X86_REG_R18},
      {X86_INS_BLSR, {0x62, 0xea, 0xf4, 0x04, 0xf3, 0xcb}, X86_REG_R17},
      {X86_INS_BLSMSK, {0x62, 0xea, 0xf4, 0x04, 0xf3, 0xd3}, X86_REG_R17},
      {X86_INS_BLSI, {0x62, 0xea, 0xf4, 0x04, 0xf3, 0xdb}, X86_REG_R17},
      {X86_INS_BZHI, {0x62, 0xea, 0xf4, 0x04, 0xf5, 0xd3}, X86_REG_R18},
  }};
  constexpr Flags Input{true, false, true, false, true, true, true};
  constexpr uint64_t R17 = UINT64_C(0x0000000000000c08);
  constexpr uint64_t R19 = UINT64_C(0xfedcba9876543210);

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Id);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_EQ(Lifted.Id, F.Id);
    ASSERT_FALSE(Lifted.Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, R17);
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, R19);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());

    uint64_t Expected = 0;
    switch (F.Id) {
    case X86_INS_ANDN:
      Expected = ~R17 & R19;
      break;
    case X86_INS_BEXTR:
      Expected = bextr(R19, R17, 8);
      break;
    case X86_INS_BLSR:
      Expected = R19 & (R19 - 1);
      break;
    case X86_INS_BLSMSK:
      Expected = R19 ^ (R19 - 1);
      break;
    case X86_INS_BLSI:
      Expected = R19 & (UINT64_C(0) - R19);
      break;
    case X86_INS_BZHI: {
      const unsigned Index = R17 & 0xff;
      Expected = Index >= 64 ? R19 : R19 & ((UINT64_C(1) << Index) - 1);
      break;
    }
    default:
      FAIL() << "unexpected instruction";
    }
    EXPECT_EQ(getGpr(Emulator, F.Destination), Expected);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXBMISemantics, OrdinaryFormsUpdateOnlyArchitecturallyOwnedFlags) {
  struct Form {
    unsigned Id;
    std::vector<uint8_t> Bytes;
    x86_reg Destination;
  };
  const std::array<Form, 6> Forms = {{
      {X86_INS_ANDN, {0x62, 0xea, 0xf4, 0x00, 0xf2, 0xd3}, X86_REG_R18},
      {X86_INS_BEXTR, {0x62, 0xea, 0xf4, 0x00, 0xf7, 0xd3}, X86_REG_R18},
      {X86_INS_BLSR, {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xcb}, X86_REG_R17},
      {X86_INS_BLSMSK, {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xd3}, X86_REG_R17},
      {X86_INS_BLSI, {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xdb}, X86_REG_R17},
      {X86_INS_BZHI, {0x62, 0xea, 0xf4, 0x00, 0xf5, 0xd3}, X86_REG_R18},
  }};
  constexpr Flags Input{true, false, true, false, false, true, true};
  constexpr uint64_t R17 = UINT64_C(0x0000000000000c04);
  constexpr uint64_t R19 = UINT64_C(0x80000000000000f0);

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Id);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_EQ(Lifted.Id, F.Id);
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, R17);
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, R19);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());

    uint64_t Result = 0;
    Flags Expected = Input;
    switch (F.Id) {
    case X86_INS_ANDN:
      Result = ~R17 & R19;
      Expected.CF = false;
      Expected.OF = false;
      Expected.ZF = Result == 0;
      Expected.SF = (Result >> 63) != 0;
      break;
    case X86_INS_BEXTR:
      Result = bextr(R19, R17, 8);
      Expected.CF = false;
      Expected.OF = false;
      Expected.ZF = Result == 0;
      break;
    case X86_INS_BLSR:
      Result = R19 & (R19 - 1);
      Expected.CF = R19 == 0;
      Expected.OF = false;
      Expected.ZF = Result == 0;
      Expected.SF = (Result >> 63) != 0;
      break;
    case X86_INS_BLSMSK:
      Result = R19 ^ (R19 - 1);
      Expected.CF = R19 == 0;
      Expected.OF = false;
      Expected.ZF = false;
      Expected.SF = (Result >> 63) != 0;
      break;
    case X86_INS_BLSI:
      Result = R19 & (UINT64_C(0) - R19);
      Expected.CF = R19 != 0;
      Expected.OF = false;
      Expected.ZF = Result == 0;
      Expected.SF = (Result >> 63) != 0;
      break;
    case X86_INS_BZHI: {
      const unsigned Index = R17 & 0xff;
      Result = Index >= 64 ? R19 : R19 & ((UINT64_C(1) << Index) - 1);
      Expected.CF = Index >= 64;
      Expected.OF = false;
      Expected.ZF = Result == 0;
      Expected.SF = (Result >> 63) != 0;
      break;
    }
    default:
      FAIL() << "unexpected instruction";
    }
    EXPECT_EQ(getGpr(Emulator, F.Destination), Result);
    expectFlags(Emulator, Expected);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXBMISemantics, FlaglessFamiliesRunExactlyInStrictMode) {
  constexpr Flags Input{true, false, true, true, false, true, true};
  constexpr uint64_t Source = UINT64_C(0x89abcdef01234567);
  constexpr uint64_t Mask = UINT64_C(0xf0f00f0faaaa5555);

  struct Form {
    unsigned Id;
    std::vector<uint8_t> Bytes;
  };
  const std::array<Form, 5> Forms = {{
      {X86_INS_PDEP, {0x62, 0xea, 0xf7, 0x00, 0xf5, 0xd3}},
      {X86_INS_PEXT, {0x62, 0xea, 0xf6, 0x00, 0xf5, 0xd3}},
      {X86_INS_SARX, {0x62, 0xea, 0xf6, 0x00, 0xf7, 0xd3}},
      {X86_INS_SHLX, {0x62, 0xea, 0xf5, 0x00, 0xf7, 0xd3}},
      {X86_INS_SHRX, {0x62, 0xea, 0xf7, 0x00, 0xf7, 0xd3}},
  }};
  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Id);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_EQ(Lifted.Id, F.Id);
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17,
           F.Id == X86_INS_PDEP || F.Id == X86_INS_PEXT ? Source : 12);
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, Mask);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    uint64_t Expected = 0;
    if (F.Id == X86_INS_PDEP)
      Expected = pdep(Source, Mask, 8);
    else if (F.Id == X86_INS_PEXT)
      Expected = pext(Source, Mask, 8);
    else if (F.Id == X86_INS_SARX)
      Expected = static_cast<uint64_t>(static_cast<int64_t>(Mask) >> 12);
    else if (F.Id == X86_INS_SHLX)
      Expected = Mask << 12;
    else
      Expected = Mask >> 12;
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), Expected);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }

  const LiftedInstruction Mulx = liftX64({0x62, 0xea, 0xf7, 0x00, 0xf6, 0xd3});
  ASSERT_EQ(Mulx.Id, X86_INS_MULX);
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  constexpr uint64_t Left = UINT64_C(0xfedcba9876543211);
  constexpr uint64_t Right = UINT64_C(0x8123456789abcdef);
  setGpr(Emulator, X86_REG_RDX, Left);
  setGpr(Emulator, X86_REG_R19, Right);
  ASSERT_EQ(Emulator.run(Mulx.Ops), Mulx.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0xa35a1df76f0d5adf));
  EXPECT_EQ(getGpr(Emulator, X86_REG_R18), UINT64_C(0x8090574ce8a1f04a));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXBMISemantics, ExtendedSibIndexMemoryFormsRunExactly) {
  struct Form {
    unsigned Id;
    std::vector<uint8_t> Bytes;
    x86_reg Destination;
  };
  const std::array<Form, 9> Forms = {{
      {X86_INS_ANDN,
       {0x62, 0x8a, 0xf0, 0x04, 0xf2, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_BZHI,
       {0x62, 0x8a, 0xf0, 0x04, 0xf5, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_BLSI,
       {0x62, 0x8a, 0xf0, 0x04, 0xf3, 0x5c, 0xa5, 0x20},
       X86_REG_R17},
      {X86_INS_BEXTR,
       {0x62, 0x8a, 0xf0, 0x04, 0xf7, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_SARX,
       {0x62, 0x8a, 0xf2, 0x00, 0xf7, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_SHLX,
       {0x62, 0x8a, 0xf1, 0x00, 0xf7, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_SHRX,
       {0x62, 0x8a, 0xf3, 0x00, 0xf7, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_MULX,
       {0x62, 0x8a, 0xf3, 0x00, 0xf6, 0x54, 0xa5, 0x20},
       X86_REG_R18},
      {X86_INS_RORX,
       {0x62, 0xab, 0xfb, 0x08, 0xf0, 0x54, 0xa5, 0x20, 0x07},
       X86_REG_R18},
  }};
  constexpr uint64_t Memory = UINT64_C(0x8123456789abcdef);
  constexpr uint64_t Control = UINT64_C(0x1008);
  constexpr uint64_t Multiplicand = UINT64_C(0xfedcba9876543211);
  constexpr Flags Input{true, false, true, true, false, true, true};

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Id);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_EQ(Lifted.Id, F.Id);
    ASSERT_FALSE(Lifted.Ops.empty());
    BinaryImage Image = imageWithValue(Memory, 8);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, Control);
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R28, 3);
    setGpr(Emulator, X86_REG_R29, kDataAddress - 3 * 4 - 0x20);
    setGpr(Emulator, X86_REG_R21, kDataAddress - 3 * 4 - 0x20);
    setGpr(Emulator, X86_REG_RDX, Multiplicand);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());

    uint64_t Expected = 0;
    switch (F.Id) {
    case X86_INS_ANDN:
      Expected = ~Control & Memory;
      break;
    case X86_INS_BZHI:
      Expected = Memory & UINT64_C(0xff);
      break;
    case X86_INS_BLSI:
      Expected = Memory & (UINT64_C(0) - Memory);
      break;
    case X86_INS_BEXTR:
      Expected = bextr(Memory, Control, 8);
      break;
    case X86_INS_SARX:
      Expected = static_cast<uint64_t>(static_cast<int64_t>(Memory) >> 8);
      break;
    case X86_INS_SHLX:
      Expected = Memory << 8;
      break;
    case X86_INS_SHRX:
      Expected = Memory >> 8;
      break;
    case X86_INS_MULX:
      EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0xa35a1df76f0d5adf));
      Expected = UINT64_C(0x8090574ce8a1f04a);
      break;
    case X86_INS_RORX:
      Expected = std::rotr(Memory, 7);
      break;
    default:
      FAIL() << "unexpected instruction";
    }
    EXPECT_EQ(getGpr(Emulator, F.Destination), Expected);
    expectFlags(Emulator, Input);
    EXPECT_FALSE(Emulator.skips().any());
  }

  const LiftedInstruction Address32 =
      liftX64({0x67, 0x62, 0x8a, 0x70, 0x04, 0xf2, 0x54, 0xa5, 0x20});
  ASSERT_EQ(Address32.Id, X86_INS_ANDN);
  BinaryImage Image = imageWithValue(Memory, 8);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17, Control);
  setGpr(Emulator, X86_REG_R28D, 3);
  setGpr(Emulator, X86_REG_R29D, kDataAddress - 3 * 4 - 0x20);
  ASSERT_EQ(Emulator.run(Address32.Ops), Address32.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R18D),
            static_cast<uint32_t>(~Control & Memory));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());

  const LiftedInstruction Address32Rorx =
      liftX64({0x67, 0x62, 0xab, 0x7b, 0x08, 0xf0, 0x54, 0xa5, 0x20, 0x07});
  ASSERT_EQ(Address32Rorx.Id, X86_INS_RORX);
  NdOpEmulator RorxEmulator(Image);
  RorxEmulator.setStrictMode(true);
  setFlags(RorxEmulator, Input);
  setGpr(RorxEmulator, X86_REG_R28D, 3);
  setGpr(RorxEmulator, X86_REG_R21D, kDataAddress - 3 * 4 - 0x20);
  ASSERT_EQ(RorxEmulator.run(Address32Rorx.Ops), Address32Rorx.Ops.size());
  EXPECT_EQ(getGpr(RorxEmulator, X86_REG_R18),
            std::rotr(static_cast<uint32_t>(Memory), 7));
  expectFlags(RorxEmulator, Input);
  EXPECT_FALSE(RorxEmulator.skips().any());
}

TEST(X86APXBMISemantics, ApxCountNfPreservesFlagsAndPopcntClearsOwnedFlags) {
  struct Family {
    unsigned Id;
    uint8_t Opcode;
  };
  struct WidthForm {
    unsigned Width;
    uint8_t P1;
  };
  const std::array<Family, 3> Families = {{
      {X86_INS_LZCNT, 0xf5},
      {X86_INS_TZCNT, 0xf4},
      {X86_INS_POPCNT, 0x88},
  }};
  const std::array<WidthForm, 3> Widths = {{
      {2, 0x7d},
      {4, 0x7c},
      {8, 0xfd},
  }};
  constexpr uint64_t Source = UINT64_C(0x8000000000008010);
  constexpr uint64_t InitialDestination = UINT64_C(0xaaaabbbbccccdddd);
  constexpr Flags Initial{true, true, true, false, true, true, true};
  const auto CountResult = [](unsigned Id, uint64_t Value, unsigned Width) {
    const unsigned Bits = Width * 8;
    Value &= widthMask(Width);
    if (Id == X86_INS_LZCNT)
      return static_cast<uint64_t>(
          Value == 0 ? Bits : std::countl_zero(Value) - (64 - Bits));
    if (Id == X86_INS_TZCNT)
      return static_cast<uint64_t>(Value == 0 ? Bits : std::countr_zero(Value));
    return static_cast<uint64_t>(std::popcount(Value));
  };
  const auto MergedDestination = [](uint64_t Original, uint64_t Value,
                                    unsigned Width) {
    if (Width == 2)
      return (Original & ~UINT64_C(0xffff)) | (Value & UINT64_C(0xffff));
    if (Width == 4)
      return Value & UINT64_C(0xffffffff);
    return Value;
  };

  for (const Family &F : Families) {
    for (const WidthForm &W : Widths) {
      SCOPED_TRACE(F.Id);
      SCOPED_TRACE(W.Width);
      const LiftedInstruction Lifted =
          liftX64({0x62, 0xec, W.P1, 0x0c, F.Opcode, 0xd3});
      ASSERT_EQ(Lifted.Id, F.Id);
      NdOpEmulator Emulator(emptyImage());
      Emulator.setStrictMode(true);
      setFlags(Emulator, Initial);
      setGpr(Emulator, X86_REG_R18, InitialDestination);
      setGpr(Emulator, X86_REG_R19, Source);
      ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
      const uint64_t Result = CountResult(F.Id, Source, W.Width);
      EXPECT_EQ(getGpr(Emulator, X86_REG_R18),
                MergedDestination(InitialDestination, Result, W.Width));
      expectFlags(Emulator, Initial);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  const LiftedInstruction MemoryForm =
      liftX64({0x62, 0x8c, 0xf8, 0x0c, 0xf5, 0x54, 0xa5, 0x20});
  ASSERT_EQ(MemoryForm.Id, X86_INS_LZCNT);
  BinaryImage Image = imageWithValue(Source, 8);
  NdOpEmulator MemoryEmulator(Image);
  MemoryEmulator.setStrictMode(true);
  setFlags(MemoryEmulator, Initial);
  setGpr(MemoryEmulator, X86_REG_R28, 3);
  setGpr(MemoryEmulator, X86_REG_R29, kDataAddress - 3 * 4 - 0x20);
  ASSERT_EQ(MemoryEmulator.run(MemoryForm.Ops), MemoryForm.Ops.size());
  EXPECT_EQ(getGpr(MemoryEmulator, X86_REG_R18), std::countl_zero(Source));
  expectFlags(MemoryEmulator, Initial);
  EXPECT_FALSE(MemoryEmulator.skips().any());

  for (const WidthForm &W : Widths) {
    SCOPED_TRACE(W.Width);
    const LiftedInstruction Popcnt =
        liftX64({0x62, 0xec, W.P1, 0x08, 0x88, 0xd3});
    ASSERT_EQ(Popcnt.Id, X86_INS_POPCNT);
    NdOpEmulator PopcntEmulator(emptyImage());
    PopcntEmulator.setStrictMode(true);
    setFlags(PopcntEmulator, Initial);
    setGpr(PopcntEmulator, X86_REG_R18, InitialDestination);
    setGpr(PopcntEmulator, X86_REG_R19, Source);
    ASSERT_EQ(PopcntEmulator.run(Popcnt.Ops), Popcnt.Ops.size());
    const uint64_t Result = CountResult(X86_INS_POPCNT, Source, W.Width);
    EXPECT_EQ(getGpr(PopcntEmulator, X86_REG_R18),
              MergedDestination(InitialDestination, Result, W.Width));
    Flags Expected = Initial;
    Expected.CF = false;
    Expected.PF = false;
    Expected.AF = false;
    Expected.ZF = Result == 0;
    Expected.SF = false;
    Expected.OF = false;
    expectFlags(PopcntEmulator, Expected);
    EXPECT_FALSE(PopcntEmulator.skips().any());
  }

  for (unsigned FamilyIndex = 0; FamilyIndex != 2; ++FamilyIndex) {
    const Family &F = Families[FamilyIndex];
    for (const WidthForm &W : Widths) {
      SCOPED_TRACE(F.Id);
      SCOPED_TRACE(W.Width);
      const LiftedInstruction Aliased =
          liftX64({0x62, 0xec, W.P1, 0x08, F.Opcode, 0xd2});
      ASSERT_EQ(Aliased.Id, F.Id);
      NdOpEmulator Emulator(emptyImage());
      Emulator.setStrictMode(true);
      const Flags Input{false, true, true, true, true, true, true};
      setFlags(Emulator, Input);
      const uint64_t Original = W.Width == 2   ? UINT64_C(0xaaaabbbbcccc0000)
                                : W.Width == 4 ? UINT64_C(0xaaaabbbb00000000)
                                               : UINT64_C(0);
      setGpr(Emulator, X86_REG_R18, Original);
      ASSERT_EQ(Emulator.run(Aliased.Ops), Aliased.Ops.size());
      EXPECT_EQ(getGpr(Emulator, X86_REG_R18),
                MergedDestination(Original, W.Width * 8, W.Width));
      Flags Expected = Input;
      Expected.CF = true;
      Expected.ZF = false;
      Expected.SF = false;
      Expected.OF = false;
      expectFlags(Emulator, Expected);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

TEST(X86APXBMISemantics, PdepPextIntrinsicContractFailsClosed) {
  const LiftedInstruction Lifted =
      liftX64({0x62, 0xea, 0xf7, 0x00, 0xf5, 0xd3});
  ASSERT_EQ(Lifted.Id, X86_INS_PDEP);
  ASSERT_EQ(Lifted.Ops.size(), 1u);
  ASSERT_EQ(Lifted.Ops.front().Opcode, NdOp::INTRINSIC);

  const auto ExpectRejected = [](const LowOp &Malformed) {
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0x0123456789abcdef));
    setGpr(Emulator, X86_REG_R18, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R19, UINT64_C(0xf0f00f0faaaa5555));
    const std::vector<LowOp> Ops = {Malformed};
    EXPECT_EQ(Emulator.run(Ops), 0u);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), UINT64_C(0xaaaaaaaaaaaaaaaa));
    EXPECT_FALSE(Emulator.skips().any());
  };

  LowOp Malformed = Lifted.Ops.front();
  Malformed.NumInputs = 0;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[0] = NdVar::reg(x86reg::RAX, 2);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[0] = NdVar::ram(kDataAddress, 2);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.NumInputs = 2;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Output.Size = 2;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[0].Size = 1;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Output = NdVar::cst(0, 8);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Output = NdVar::ram(kDataAddress, 8);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[1].Size = 2;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[1] = NdVar::ram(kDataAddress, 8);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[2] = NdVar::ram(kDataAddress, 8);
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.Inputs[2].Size = 2;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.MemoryOrdering = NdMemoryOrdering::Relaxed;
  ExpectRejected(Malformed);
  Malformed = Lifted.Ops.front();
  Malformed.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  ExpectRejected(Malformed);
}

TEST(X86APXBMISemantics, AdxNddUsesBothSourcesAndOnlyItsCarryChainFlag) {
  struct Form {
    unsigned Id;
    std::vector<uint8_t> Bytes;
  };
  const std::array<Form, 2> Forms = {{
      {X86_INS_ADCX, {0x62, 0xec, 0xf5, 0x10, 0x66, 0xd3}},
      {X86_INS_ADOX, {0x62, 0xec, 0xf6, 0x10, 0x66, 0xd3}},
  }};
  constexpr uint64_t Left = UINT64_C(0xfffffffffffffff0);
  constexpr uint64_t Right = UINT64_C(0x20);
  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Id);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_EQ(Lifted.Id, F.Id);
    const Flags Input{F.Id == X86_INS_ADCX, true, false, true, true,
                      F.Id == X86_INS_ADOX, true};
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0xaaaaaaaaaaaaaaaa));
    setGpr(Emulator, X86_REG_R18, Left);
    setGpr(Emulator, X86_REG_R19, Right);
    ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), Left + Right + 1);
    Flags Expected = Input;
    if (F.Id == X86_INS_ADCX)
      Expected.CF = true;
    else
      Expected.OF = true;
    expectFlags(Emulator, Expected);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), Left);
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), Right);
    EXPECT_FALSE(Emulator.skips().any());
  }

  const LiftedInstruction TwoOperand =
      liftX64({0x62, 0xec, 0xfd, 0x08, 0x66, 0xd3});
  ASSERT_EQ(TwoOperand.Id, X86_INS_ADCX);
  NdOpEmulator Emulator(emptyImage());
  Emulator.setStrictMode(true);
  const Flags Input{false, true, true, false, true, true, false};
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R18, UINT64_MAX);
  setGpr(Emulator, X86_REG_R19, 1);
  ASSERT_EQ(Emulator.run(TwoOperand.Ops), TwoOperand.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R18), 0u);
  Flags Expected = Input;
  Expected.CF = true;
  expectFlags(Emulator, Expected);
}

TEST(X86APXBMISemantics, SegmentedAddress32MemoryUsesRawRolesAndZeroesDword) {
  // bzhi {nf} r26d, dword ptr gs:[r29d + r14d*4 + 0x20], r17d
  const LiftedInstruction Lifted =
      liftX64({0x65, 0x67, 0x62, 0x0a, 0x74, 0x04, 0xf5, 0x54, 0xb5, 0x20});
  ASSERT_EQ(Lifted.Id, X86_INS_BZHI);
  const auto Load =
      std::find_if(Lifted.Ops.begin(), Lifted.Ops.end(),
                   [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; });
  ASSERT_NE(Load, Lifted.Ops.end());
  EXPECT_EQ(Load->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);
  EXPECT_EQ(Load->Output.Size, 4u);

  BinaryImage Image = imageWithValue(UINT64_C(0xdeadbeef), 4);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                                 kDataAddress - 0x30));
  const Flags Input{true, false, true, true, false, true, true};
  setFlags(Emulator, Input);
  setGpr(Emulator, X86_REG_R17D, 12);
  setGpr(Emulator, X86_REG_R29, UINT64_C(0x100000008));
  setGpr(Emulator, X86_REG_R14, 2);
  setGpr(Emulator, X86_REG_R26, UINT64_MAX);
  ASSERT_EQ(Emulator.run(Lifted.Ops), Lifted.Ops.size());
  EXPECT_EQ(getGpr(Emulator, X86_REG_R26), UINT64_C(0xeef));
  expectFlags(Emulator, Input);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXBMISemantics, EveryMemoryFamilyFaultsBeforeArchitecturalState) {
  struct Form {
    std::string_view Name;
    std::vector<uint8_t> Bytes;
  };
  const std::array<Form, 18> Forms = {{
      {"andn", {0x64, 0x67, 0x62, 0x0a, 0x74, 0x00, 0xf2, 0x54, 0xb5, 0x20}},
      {"bextr", {0x65, 0x67, 0x62, 0x0a, 0x74, 0x00, 0xf7, 0x54, 0xb5, 0x20}},
      {"blsr", {0x64, 0x67, 0x62, 0x8a, 0x74, 0x00, 0xf3, 0x4c, 0xb5, 0x20}},
      {"blsmsk", {0x65, 0x67, 0x62, 0x8a, 0x74, 0x00, 0xf3, 0x54, 0xb5, 0x20}},
      {"blsi", {0x64, 0x67, 0x62, 0x8a, 0x74, 0x00, 0xf3, 0x5c, 0xb5, 0x20}},
      {"bzhi", {0x65, 0x67, 0x62, 0x0a, 0x74, 0x00, 0xf5, 0x54, 0xb5, 0x20}},
      {"lzcnt", {0x62, 0x8c, 0xf8, 0x0c, 0xf5, 0x54, 0xa5, 0x20}},
      {"tzcnt", {0x62, 0x8c, 0xf8, 0x0c, 0xf4, 0x54, 0xa5, 0x20}},
      {"popcnt", {0x62, 0x8c, 0xf8, 0x0c, 0x88, 0x54, 0xa5, 0x20}},
      {"mulx", {0x64, 0x67, 0x62, 0x8a, 0x77, 0x00, 0xf6, 0x54, 0xb5, 0x20}},
      {"pdep", {0x65, 0x67, 0x62, 0x0a, 0x77, 0x00, 0xf5, 0x54, 0xb5, 0x20}},
      {"pext", {0x64, 0x67, 0x62, 0x0a, 0x76, 0x00, 0xf5, 0x54, 0xb5, 0x20}},
      {"sarx", {0x65, 0x67, 0x62, 0x0a, 0x76, 0x00, 0xf7, 0x54, 0xb5, 0x20}},
      {"shlx", {0x64, 0x67, 0x62, 0x0a, 0x75, 0x00, 0xf7, 0x54, 0xb5, 0x20}},
      {"shrx", {0x65, 0x67, 0x62, 0x0a, 0x77, 0x00, 0xf7, 0x54, 0xb5, 0x20}},
      {"rorx", {0x62, 0xab, 0xfb, 0x08, 0xf0, 0x54, 0xa5, 0x20, 0x07}},
      {"adcx", {0x64, 0x67, 0x62, 0x8c, 0x75, 0x10, 0x66, 0x54, 0xb5, 0x20}},
      {"adox", {0x65, 0x67, 0x62, 0x8c, 0x76, 0x10, 0x66, 0x54, 0xb5, 0x20}},
  }};
  constexpr Flags Input{true, false, true, true, false, true, true};
  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Name);
    const LiftedInstruction Lifted = liftX64(F.Bytes);
    ASSERT_FALSE(Lifted.Ops.empty());
    NdOpEmulator Emulator(emptyImage());
    Emulator.setStrictMode(true);
    setFlags(Emulator, Input);
    setGpr(Emulator, X86_REG_R17, UINT64_C(0x1111111111111111));
    setGpr(Emulator, X86_REG_R18, UINT64_C(0x2222222222222222));
    setGpr(Emulator, X86_REG_R19, UINT64_C(0x3333333333333333));
    setGpr(Emulator, X86_REG_R26, UINT64_C(0x6666666666666666));
    setGpr(Emulator, X86_REG_RDX, UINT64_C(0xdddddddddddddddd));
    setGpr(Emulator, X86_REG_R29, UINT64_C(0xdead0000));
    setGpr(Emulator, X86_REG_R21, UINT64_C(0xdead0000));
    setGpr(Emulator, X86_REG_R14, 0);
    EXPECT_LT(Emulator.run(Lifted.Ops), Lifted.Ops.size());
    EXPECT_EQ(getGpr(Emulator, X86_REG_R17), UINT64_C(0x1111111111111111));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R18), UINT64_C(0x2222222222222222));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R19), UINT64_C(0x3333333333333333));
    EXPECT_EQ(getGpr(Emulator, X86_REG_R26), UINT64_C(0x6666666666666666));
    EXPECT_EQ(getGpr(Emulator, X86_REG_RDX), UINT64_C(0xdddddddddddddddd));
    expectFlags(Emulator, Input);
  }
}

TEST(X86APXBMISemantics, OutOfRangeBitFieldsDoNotUseMaskedHostShifts) {
  const LiftedInstruction Bzhi = liftX64({0x62, 0xea, 0xf4, 0x00, 0xf5, 0xd3});
  NdOpEmulator BzhiEmulator(emptyImage());
  BzhiEmulator.setStrictMode(true);
  setGpr(BzhiEmulator, X86_REG_R17, 64);
  setGpr(BzhiEmulator, X86_REG_R19, UINT64_C(0x89abcdef01234567));
  ASSERT_EQ(BzhiEmulator.run(Bzhi.Ops), Bzhi.Ops.size());
  EXPECT_EQ(getGpr(BzhiEmulator, X86_REG_R18), UINT64_C(0x89abcdef01234567));
  EXPECT_EQ(BzhiEmulator.getRegister(x86reg::CF), 1u);

  const LiftedInstruction Bextr = liftX64({0x62, 0xea, 0xf4, 0x00, 0xf7, 0xd3});
  NdOpEmulator Whole(emptyImage());
  Whole.setStrictMode(true);
  setGpr(Whole, X86_REG_R17, UINT64_C(64) << 8);
  setGpr(Whole, X86_REG_R19, UINT64_C(0x89abcdef01234567));
  ASSERT_EQ(Whole.run(Bextr.Ops), Bextr.Ops.size());
  EXPECT_EQ(getGpr(Whole, X86_REG_R18), UINT64_C(0x89abcdef01234567));

  NdOpEmulator PastEnd(emptyImage());
  PastEnd.setStrictMode(true);
  setGpr(PastEnd, X86_REG_R17, UINT64_C(8) << 8 | 64);
  setGpr(PastEnd, X86_REG_R19, UINT64_C(0x89abcdef01234567));
  ASSERT_EQ(PastEnd.run(Bextr.Ops), Bextr.Ops.size());
  EXPECT_EQ(getGpr(PastEnd, X86_REG_R18), 0u);
}

TEST(X86APXBMISemantics, MalformedRawShapesFailBeforeLowStateEmission) {
  struct Form {
    std::string_view Name;
    std::vector<uint8_t> Bytes;
    bool SupportsNf;
  };
  const std::array<Form, 18> Forms = {{
      {"andn", {0x62, 0xea, 0xf4, 0x00, 0xf2, 0xd3}, true},
      {"bextr", {0x62, 0xea, 0xf4, 0x00, 0xf7, 0xd3}, true},
      {"blsr", {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xcb}, true},
      {"blsmsk", {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xd3}, true},
      {"blsi", {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xdb}, true},
      {"bzhi", {0x62, 0xea, 0xf4, 0x00, 0xf5, 0xd3}, true},
      {"lzcnt", {0x62, 0xec, 0xfd, 0x08, 0xf5, 0xd3}, true},
      {"tzcnt", {0x62, 0xec, 0xfd, 0x08, 0xf4, 0xd3}, true},
      {"popcnt", {0x62, 0xec, 0xfd, 0x08, 0x88, 0xd3}, true},
      {"mulx", {0x62, 0xea, 0xf7, 0x00, 0xf6, 0xd3}, false},
      {"pdep", {0x62, 0xea, 0xf7, 0x00, 0xf5, 0xd3}, false},
      {"pext", {0x62, 0xea, 0xf6, 0x00, 0xf5, 0xd3}, false},
      {"sarx", {0x62, 0xea, 0xf6, 0x00, 0xf7, 0xd3}, false},
      {"shlx", {0x62, 0xea, 0xf5, 0x00, 0xf7, 0xd3}, false},
      {"shrx", {0x62, 0xea, 0xf7, 0x00, 0xf7, 0xd3}, false},
      {"rorx", {0x62, 0xeb, 0xff, 0x08, 0xf0, 0xd3, 0x07}, false},
      {"adcx", {0x62, 0xec, 0xf5, 0x10, 0x66, 0xd3}, false},
      {"adox", {0x62, 0xec, 0xf6, 0x10, 0x66, 0xd3}, false},
  }};
  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(F.Bytes.data(), F.Bytes.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(F.Bytes.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    Insn.Raw->bytes[3] |= F.SupportsNf ? 0x10 : 0x04;
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(X86APXBMISemantics, RawRolesAndMemoryDetailMustAgreeExactly) {
  const std::vector<uint8_t> Register = {0x62, 0xea, 0xf4, 0x00, 0xf5, 0xd3};
  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.detail->x86.operands[2].reg = X86_REG_R20;
  });
  expectMutatedLiftRejected(
      Register, [](cs_insn &Raw) { Raw.detail->x86.encoding.imm_offset = 1; });

  const std::vector<uint8_t> Pdep = {0x62, 0xea, 0xf7, 0x00, 0xf5, 0xd3};
  expectMutatedLiftRejected(Pdep,
                            [](cs_insn &Raw) { Raw.detail->x86.eflags = 1; });
  expectMutatedLiftRejected(Pdep, [](cs_insn &Raw) {
    Raw.detail->regs_write_count = 1;
    Raw.detail->regs_write[0] = X86_REG_EFLAGS;
  });
  expectMutatedLiftRejected(Pdep, [](cs_insn &Raw) { Raw.bytes[0] = 0x90; });

  const std::vector<uint8_t> Count = {0x62, 0xec, 0xfd, 0x0c, 0xf5, 0xd3};
  expectMutatedLiftRejected(Count,
                            [](cs_insn &Raw) { Raw.detail->x86.eflags = 1; });
  expectMutatedLiftRejected(Count, [](cs_insn &Raw) {
    Raw.detail->regs_write_count = 1;
    Raw.detail->regs_write[0] = X86_REG_EFLAGS;
  });
  expectMutatedLiftRejected(Count, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].reg = X86_REG_R20;
  });
  expectMutatedLiftRejected(Count, [](cs_insn &Raw) { Raw.bytes[2] ^= 0x08; });
  expectMutatedLiftRejected(Count, [](cs_insn &Raw) { Raw.bytes[3] ^= 0x08; });

  const std::vector<uint8_t> Rorx = {0x62, 0xeb, 0xff, 0x08, 0xf0, 0xd3, 0x07};
  expectMutatedLiftRejected(
      Rorx, [](cs_insn &Raw) { Raw.detail->x86.operands[2].imm = 8; });
  expectMutatedLiftRejected(
      Rorx, [](cs_insn &Raw) { Raw.detail->x86.encoding.imm_offset = 5; });
  expectMutatedLiftRejected(Rorx,
                            [](cs_insn &Raw) { Raw.bytes[Raw.size - 1] = 8; });

  const std::vector<uint8_t> Memory = {0x65, 0x67, 0x62, 0x0a, 0x74,
                                       0x04, 0xf5, 0x54, 0xb5, 0x20};
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[2].mem.base = X86_REG_R28D;
  });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { Raw.detail->x86.sib_base = X86_REG_R28D; });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { Raw.detail->x86.encoding.disp_offset = 0; });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { Raw.detail->x86.prefix[1] = 0x64; });
}

TEST(X86APXBMISemantics, EveryApxBmiDetailContractFailsClosed) {
  struct Form {
    std::string_view Name;
    std::vector<uint8_t> Bytes;
    x86_reg ImplicitRead = X86_REG_INVALID;
    x86_reg ImplicitWrite = X86_REG_INVALID;
  };
  const std::vector<Form> Forms = {
      {"andn",
       {0x62, 0xea, 0xf4, 0x00, 0xf2, 0xd3},
       X86_REG_INVALID,
       X86_REG_EFLAGS},
      {"andn-nf", {0x62, 0xea, 0xf4, 0x04, 0xf2, 0xd3}},
      {"bzhi",
       {0x62, 0xea, 0xf4, 0x00, 0xf5, 0xd3},
       X86_REG_INVALID,
       X86_REG_EFLAGS},
      {"bzhi-nf", {0x62, 0xea, 0xf4, 0x04, 0xf5, 0xd3}},
      {"blsi",
       {0x62, 0xea, 0xf4, 0x00, 0xf3, 0xdb},
       X86_REG_INVALID,
       X86_REG_EFLAGS},
      {"blsi-nf", {0x62, 0xea, 0xf4, 0x04, 0xf3, 0xdb}},
      {"bextr",
       {0x62, 0xea, 0xf4, 0x00, 0xf7, 0xd3},
       X86_REG_INVALID,
       X86_REG_EFLAGS},
      {"bextr-nf", {0x62, 0xea, 0xf4, 0x04, 0xf7, 0xd3}},
      {"sarx", {0x62, 0xea, 0xf6, 0x00, 0xf7, 0xd3}},
      {"shlx", {0x62, 0xea, 0xf5, 0x00, 0xf7, 0xd3}},
      {"shrx", {0x62, 0xea, 0xf7, 0x00, 0xf7, 0xd3}},
      {"mulx32", {0x62, 0xea, 0x77, 0x00, 0xf6, 0xd3}, X86_REG_EDX},
      {"mulx64", {0x62, 0xea, 0xf7, 0x00, 0xf6, 0xd3}, X86_REG_RDX},
      {"adcx",
       {0x62, 0xec, 0xf5, 0x10, 0x66, 0xd3},
       X86_REG_EFLAGS,
       X86_REG_EFLAGS},
      {"adox",
       {0x62, 0xec, 0xf6, 0x10, 0x66, 0xd3},
       X86_REG_EFLAGS,
       X86_REG_EFLAGS},
  };

  for (const Form &F : Forms) {
    SCOPED_TRACE(F.Name);
    expectMutatedLiftRejected(
        F.Bytes, [](cs_insn &Raw) { Raw.detail->x86.eflags ^= UINT64_C(1); });
    expectMutatedLiftRejected(
        F.Bytes, [Expected = F.ImplicitRead](cs_insn &Raw) {
          Raw.detail->regs_read_count = 1;
          Raw.detail->regs_read[0] =
              Expected == X86_REG_EFLAGS ? X86_REG_RAX : X86_REG_EFLAGS;
        });
    expectMutatedLiftRejected(
        F.Bytes, [Expected = F.ImplicitWrite](cs_insn &Raw) {
          Raw.detail->regs_write_count = 1;
          Raw.detail->regs_write[0] =
              Expected == X86_REG_EFLAGS ? X86_REG_RAX : X86_REG_EFLAGS;
        });
  }
}

TEST(X86APXBMISemantics, DuplicateSegmentOrAddressPrefixFailsBeforeLowState) {
  const std::vector<uint8_t> Valid = {0x65, 0x67, 0x62, 0x0a, 0x74,
                                      0x04, 0xf5, 0x54, 0xb5, 0x20};
  for (const size_t Insertion : {size_t{1}, size_t{2}}) {
    SCOPED_TRACE(Insertion);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Valid.data(), Valid.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(Valid.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_LT(Insn.Raw->size, sizeof(Insn.Raw->bytes));
    for (size_t I = Insn.Raw->size; I > Insertion; --I)
      Insn.Raw->bytes[I] = Insn.Raw->bytes[I - 1];
    Insn.Raw->bytes[Insertion] = Insertion == 1 ? 0x65 : 0x67;
    ++Insn.Raw->size;
    ++Insn.Size;
    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

} // namespace
