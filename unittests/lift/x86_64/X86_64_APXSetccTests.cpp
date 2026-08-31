//===- X86_64_APXSetccTests.cpp - APX SETcc semantics ------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

constexpr std::array<unsigned, 16> kSetIds = {
    X86_INS_SETO,  X86_INS_SETNO, X86_INS_SETB,  X86_INS_SETAE,
    X86_INS_SETE,  X86_INS_SETNE, X86_INS_SETBE, X86_INS_SETA,
    X86_INS_SETS,  X86_INS_SETNS, X86_INS_SETP,  X86_INS_SETNP,
    X86_INS_SETL,  X86_INS_SETGE, X86_INS_SETLE, X86_INS_SETG,
};

constexpr std::array<unsigned, 16> kSetzuIds = {
    X86_INS_SETZUO,  X86_INS_SETZUNO, X86_INS_SETZUB,
    X86_INS_SETZUAE, X86_INS_SETZUE,  X86_INS_SETZUNE,
    X86_INS_SETZUBE, X86_INS_SETZUA,  X86_INS_SETZUS,
    X86_INS_SETZUNS, X86_INS_SETZUP,  X86_INS_SETZUNP,
    X86_INS_SETZUL,  X86_INS_SETZUGE, X86_INS_SETZULE,
    X86_INS_SETZUG,
};

constexpr std::array<x86_reg, 32> kGpr8 = {
    X86_REG_AL,   X86_REG_CL,   X86_REG_DL,   X86_REG_BL,
    X86_REG_SPL,  X86_REG_BPL,  X86_REG_SIL,  X86_REG_DIL,
    X86_REG_R8B,  X86_REG_R9B,  X86_REG_R10B, X86_REG_R11B,
    X86_REG_R12B, X86_REG_R13B, X86_REG_R14B, X86_REG_R15B,
    X86_REG_R16B, X86_REG_R17B, X86_REG_R18B, X86_REG_R19B,
    X86_REG_R20B, X86_REG_R21B, X86_REG_R22B, X86_REG_R23B,
    X86_REG_R24B, X86_REG_R25B, X86_REG_R26B, X86_REG_R27B,
    X86_REG_R28B, X86_REG_R29B, X86_REG_R30B, X86_REG_R31B,
};

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

std::vector<uint8_t> encodeRegister(unsigned Condition, bool ZeroUpper,
                                    bool W, unsigned Register) {
  return {
      0x62,
      static_cast<uint8_t>(0xd4 | ((Register & 8) != 0 ? 0 : 0x20) |
                           ((Register & 16) != 0 ? 0x08 : 0)),
      static_cast<uint8_t>((W ? 0x80 : 0) | 0x7f),
      static_cast<uint8_t>((ZeroUpper ? 0x10 : 0) | 0x08),
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      static_cast<uint8_t>(0xc0 | (Register & 7)),
  };
}

enum class SegmentOverride { None, FS, GS };

std::vector<uint8_t> encodeMemory(unsigned Condition, bool ZeroUpper,
                                  bool Address32, SegmentOverride Segment) {
  std::vector<uint8_t> Bytes;
  if (Segment == SegmentOverride::FS)
    Bytes.push_back(0x64);
  else if (Segment == SegmentOverride::GS)
    Bytes.push_back(0x65);
  if (Address32)
    Bytes.push_back(0x67);
  const uint8_t Encoding[] = {
      0x62, 0xbc, 0x7b,
      static_cast<uint8_t>((ZeroUpper ? 0x10 : 0) | 0x08),
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      0x44, 0xac, 0xf0,
  };
  Bytes.insert(Bytes.end(), std::begin(Encoding), std::end(Encoding));
  return Bytes;
}

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  const int Decoded = Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                           kInstructionAddress, Insn);
  if (Decoded != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete instruction";
    return {};
  }
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "instruction was not lifted";
  }
  return Result;
}

BinaryImage makeMemoryImage(uint64_t Address, size_t Size,
                            SegmentFlags Flags = SegmentFlags::Readable |
                                                 SegmentFlags::Writable) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = Address;
  Data.Size = Size;
  Data.Flags = Flags;
  Data.Data.resize(Size);
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void writeImageBytes(BinaryImage &Image, uint64_t Address,
                     const std::vector<uint8_t> &Bytes) {
  ASSERT_EQ(Image.Segments.size(), 1u);
  Segment &Data = Image.Segments.front();
  ASSERT_GE(Address, Data.VA);
  const uint64_t Offset = Address - Data.VA;
  ASSERT_LE(Offset + Bytes.size(), Data.Data.size());
  std::memcpy(Data.Data.data() + Offset, Bytes.data(), Bytes.size());
}

uint8_t probeByte(NdOpEmulator &Emulator, uint64_t Address, uint64_t Temp) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(Temp, 1);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(Address, 8));
  EXPECT_TRUE(Emulator.step(Load));
  return static_cast<uint8_t>(Emulator.getRegister(Temp).value_or(0xff));
}

void setFlags(NdOpEmulator &Emulator, unsigned Bits) {
  Emulator.setRegister(x86reg::CF, (Bits >> 0) & 1);
  Emulator.setRegister(x86reg::PF, (Bits >> 1) & 1);
  Emulator.setRegister(x86reg::ZF, (Bits >> 2) & 1);
  Emulator.setRegister(x86reg::SF, (Bits >> 3) & 1);
  Emulator.setRegister(x86reg::OF, (Bits >> 4) & 1);
}

bool conditionResult(unsigned Condition, unsigned Bits) {
  const bool CF = ((Bits >> 0) & 1) != 0;
  const bool PF = ((Bits >> 1) & 1) != 0;
  const bool ZF = ((Bits >> 2) & 1) != 0;
  const bool SF = ((Bits >> 3) & 1) != 0;
  const bool OF = ((Bits >> 4) & 1) != 0;
  switch (Condition) {
  case 0:
    return OF;
  case 1:
    return !OF;
  case 2:
    return CF;
  case 3:
    return !CF;
  case 4:
    return ZF;
  case 5:
    return !ZF;
  case 6:
    return CF || ZF;
  case 7:
    return !CF && !ZF;
  case 8:
    return SF;
  case 9:
    return !SF;
  case 10:
    return PF;
  case 11:
    return !PF;
  case 12:
    return SF != OF;
  case 13:
    return SF == OF;
  case 14:
    return ZF || SF != OF;
  case 15:
    return !ZF && SF == OF;
  default:
    return false;
  }
}

std::array<unsigned, 2> falseTrueFlags(unsigned Condition) {
  std::optional<unsigned> False;
  std::optional<unsigned> True;
  for (unsigned Bits = 0; Bits < 32; ++Bits) {
    if (conditionResult(Condition, Bits))
      True = Bits;
    else
      False = Bits;
    if (False && True)
      break;
  }
  EXPECT_TRUE(False.has_value());
  EXPECT_TRUE(True.has_value());
  return {False.value_or(0), True.value_or(0)};
}

TEST(X86APXSetcc, AllConditionsProduceExactFalseAndTrueResults) {
  constexpr uint64_t Initial = UINT64_C(0xa1b2c3d4e5f60789);
  const RegInfo R29 = mapCapstoneReg(X86_REG_R29B);
  ASSERT_EQ(R29.Size, 1u);

  for (unsigned Condition = 0; Condition < 16; ++Condition) {
    const std::array<unsigned, 2> FlagCases = falseTrueFlags(Condition);
    for (const bool ZeroUpper : {false, true}) {
      const LiftedInstruction Set =
          liftX64(encodeRegister(Condition, ZeroUpper, Condition & 1, 29));
      SCOPED_TRACE(::testing::Message()
                   << "condition=" << Condition
                   << " zero_upper=" << ZeroUpper);
      EXPECT_EQ(Set.Id,
                ZeroUpper ? kSetzuIds[Condition] : kSetIds[Condition]);
      ASSERT_FALSE(Set.Ops.empty());
      for (unsigned Result = 0; Result < 2; ++Result) {
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(R29.Offset, Initial);
        setFlags(Emulator, FlagCases[Result]);
        ASSERT_EQ(Emulator.run(Set.Ops), Set.Ops.size());
        const uint64_t Expected =
            ZeroUpper ? Result : ((Initial & ~UINT64_C(0xff)) | Result);
        EXPECT_EQ(Emulator.getRegister(R29.Offset), Expected);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXSetcc, RegisterDestinationsCoverTheCompleteApxBank) {
  constexpr uint64_t Initial = UINT64_C(0xfedcba98765432a5);
  for (unsigned Register = 0; Register < kGpr8.size(); ++Register) {
    const RegInfo Destination = mapCapstoneReg(kGpr8[Register]);
    ASSERT_EQ(Destination.Size, 1u);
    for (const bool ZeroUpper : {false, true}) {
      for (unsigned Result = 0; Result < 2; ++Result) {
        SCOPED_TRACE(::testing::Message()
                     << "register=" << Register
                     << " zero_upper=" << ZeroUpper << " result=" << Result);
        const LiftedInstruction Set =
            liftX64(encodeRegister(5, ZeroUpper, Register & 1, Register));
        EXPECT_EQ(Set.Id, ZeroUpper ? X86_INS_SETZUNE : X86_INS_SETNE);
        ASSERT_FALSE(Set.Ops.empty());
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(Destination.Offset, Initial);
        // SETNE is true exactly when ZF is clear.
        Emulator.setRegister(x86reg::ZF, Result == 0 ? 1 : 0);
        ASSERT_EQ(Emulator.run(Set.Ops), Set.Ops.size());
        const uint64_t Expected =
            ZeroUpper ? Result : ((Initial & ~UINT64_C(0xff)) | Result);
        EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

struct MemoryCase {
  const char *Name;
  bool Address32;
  SegmentOverride Segment;
  uint64_t Base;
  uint64_t Index;
  uint64_t SegmentBase;
  uint64_t Target;
};

TEST(X86APXSetcc, MemoryFormsAlwaysStoreOneByteWithExactAddressing) {
  constexpr std::array<MemoryCase, 4> Cases = {{
      {"egpr", false, SegmentOverride::None, 0x5000, 4, 0, 0x5000},
      {"addr32", true, SegmentOverride::None, UINT64_C(0xfffffff0), 0x1408,
       0, 0x5000},
      {"fs", false, SegmentOverride::FS, 0x20, 4, 0x6000, 0x6020},
      {"gs", false, SegmentOverride::GS, 0x30, 4, 0x7000, 0x7030},
  }};
  uint64_t ProbeTemp = UINT64_C(0x73000000);

  for (const MemoryCase &Case : Cases) {
    for (const bool ZeroUpper : {false, true}) {
      for (unsigned Result = 0; Result < 2; ++Result) {
        SCOPED_TRACE(::testing::Message()
                     << Case.Name << " zero_upper=" << ZeroUpper
                     << " result=" << Result);
        const LiftedInstruction Set = liftX64(
            encodeMemory(5, ZeroUpper, Case.Address32, Case.Segment));
        EXPECT_EQ(Set.Id, ZeroUpper ? X86_INS_SETZUNE : X86_INS_SETNE);
        ASSERT_FALSE(Set.Ops.empty());
        BinaryImage Image = makeMemoryImage(Case.Target - 1, 3);
        writeImageBytes(Image, Case.Target - 1, {0xa5, 0x5a, 0xc3});
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(x86reg::R20, Case.Base);
        Emulator.setRegister(x86reg::R29, Case.Index);
        Emulator.setRegister(x86reg::ZF, Result == 0 ? 1 : 0);
        if (Case.Segment == SegmentOverride::FS)
          ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
              NdMemoryAddressSpace::X86FS, Case.SegmentBase));
        else if (Case.Segment == SegmentOverride::GS)
          ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
              NdMemoryAddressSpace::X86GS, Case.SegmentBase));
        ASSERT_EQ(Emulator.run(Set.Ops), Set.Ops.size());
        EXPECT_EQ(probeByte(Emulator, Case.Target - 1, ProbeTemp++), 0xa5);
        EXPECT_EQ(probeByte(Emulator, Case.Target, ProbeTemp++), Result);
        EXPECT_EQ(probeByte(Emulator, Case.Target + 1, ProbeTemp++), 0xc3);
        EXPECT_EQ(Emulator.getRegister(x86reg::R20), Case.Base);
        EXPECT_EQ(Emulator.getRegister(x86reg::R29), Case.Index);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXSetcc, MemoryFaultsCommitNoByteOrArchitecturalRegister) {
  constexpr uint64_t Target = 0x5000;
  constexpr uint64_t Sentinel = UINT64_C(0x8877665544332211);
  uint64_t ProbeTemp = UINT64_C(0x74000000);

  for (const bool ZeroUpper : {false, true}) {
    SCOPED_TRACE(ZeroUpper ? "ND=1" : "ND=0");
    const LiftedInstruction Set =
        liftX64(encodeMemory(5, ZeroUpper, false, SegmentOverride::None));
    ASSERT_FALSE(Set.Ops.empty());

    BinaryImage DeniedImage =
        makeMemoryImage(Target, 1, SegmentFlags::Readable);
    writeImageBytes(DeniedImage, Target, {0x5a});
    NdOpEmulator Denied(DeniedImage);
    Denied.setStrictMode(true);
    Denied.setRegister(x86reg::R20, Target);
    Denied.setRegister(x86reg::R29, 4);
    Denied.setRegister(x86reg::ZF, 0);
    Denied.setRegister(x86reg::RAX, Sentinel);
    EXPECT_LT(Denied.run(Set.Ops), Set.Ops.size());
    EXPECT_EQ(probeByte(Denied, Target, ProbeTemp++), 0x5a);
    EXPECT_EQ(Denied.getRegister(x86reg::RAX), Sentinel);
    EXPECT_EQ(Denied.getRegister(x86reg::R20), Target);
    EXPECT_EQ(Denied.getRegister(x86reg::R29), 4u);
    EXPECT_EQ(Denied.getRegister(x86reg::ZF), 0u);
    EXPECT_FALSE(Denied.skips().any());

    BinaryImage UnmappedImage;
    UnmappedImage.Arch = Arch::X64;
    UnmappedImage.Bits = Bitness::Bits64;
    NdOpEmulator Unmapped(UnmappedImage);
    Unmapped.setStrictMode(true);
    Unmapped.setRegister(x86reg::R20, Target);
    Unmapped.setRegister(x86reg::R29, 4);
    Unmapped.setRegister(x86reg::ZF, 0);
    Unmapped.setRegister(x86reg::RAX, Sentinel);
    EXPECT_LT(Unmapped.run(Set.Ops), Set.Ops.size());
    EXPECT_EQ(Unmapped.getRegister(x86reg::RAX), Sentinel);
    EXPECT_EQ(Unmapped.getRegister(x86reg::R20), Target);
    EXPECT_EQ(Unmapped.getRegister(x86reg::R29), 4u);
    EXPECT_EQ(Unmapped.getRegister(x86reg::ZF), 0u);
    EXPECT_FALSE(Unmapped.skips().any());
  }
}

} // namespace
