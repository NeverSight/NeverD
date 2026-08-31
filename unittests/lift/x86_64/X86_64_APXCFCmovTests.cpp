//===- X86_64_APXCFCmovTests.cpp - APX CFCMOV semantics -------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

constexpr std::array<unsigned, 16> kCFCmovIds = {
    X86_INS_CFCMOVO, X86_INS_CFCMOVNO, X86_INS_CFCMOVB,  X86_INS_CFCMOVAE,
    X86_INS_CFCMOVE, X86_INS_CFCMOVNE, X86_INS_CFCMOVBE, X86_INS_CFCMOVA,
    X86_INS_CFCMOVS, X86_INS_CFCMOVNS, X86_INS_CFCMOVP,  X86_INS_CFCMOVNP,
    X86_INS_CFCMOVL, X86_INS_CFCMOVGE, X86_INS_CFCMOVLE, X86_INS_CFCMOVG,
};

std::vector<uint8_t> encodeRegister(unsigned Condition, bool W, unsigned PP,
                                    unsigned Destination, unsigned Source) {
  return {
      0x62,
      static_cast<uint8_t>(0x44 | ((Destination & 8) ? 0 : 0x80) |
                           ((Destination & 16) ? 0 : 0x10) |
                           ((Source & 8) ? 0 : 0x20) |
                           ((Source & 16) ? 0x08 : 0)),
      static_cast<uint8_t>((W ? 0x80 : 0) | 0x7c | (PP & 1)),
      0x08,
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      static_cast<uint8_t>(0xc0 | ((Destination & 7) << 3) | (Source & 7)),
  };
}

std::vector<uint8_t> encodeRegisterVariant(unsigned Condition, bool ND, bool NF,
                                           bool W, unsigned PP, unsigned NDD,
                                           unsigned Reg, unsigned RM) {
  if (!ND)
    NDD = 0;
  return {
      0x62,
      static_cast<uint8_t>(0x44 | ((Reg & 8) ? 0 : 0x80) |
                           ((Reg & 16) ? 0 : 0x10) | ((RM & 8) ? 0 : 0x20) |
                           ((RM & 16) ? 0x08 : 0)),
      static_cast<uint8_t>((W ? 0x80 : 0) | (((~NDD) & 15) << 3) | 0x04 |
                           (PP & 1)),
      static_cast<uint8_t>((ND ? 0x10 : 0) | ((NDD & 16) ? 0 : 0x08) |
                           (NF ? 0x04 : 0)),
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      static_cast<uint8_t>(0xc0 | ((Reg & 7) << 3) | (RM & 7)),
  };
}

std::vector<uint8_t> encodeMemorySource(unsigned Condition, bool W = false,
                                        unsigned PP = 0) {
  // CFCMOVcc r17d, [r20 + r29*4 - 16] (ND=0, NF=0).
  return {0x62,
          0xac,
          static_cast<uint8_t>((W ? 0x80 : 0) | 0x78 | (PP & 1)),
          0x08,
          static_cast<uint8_t>(0x40 | (Condition & 15)),
          0x4c,
          0xac,
          0xf0};
}

std::vector<uint8_t> encodeNddMemorySource(unsigned Condition) {
  // CFCMOVcc r30d, r17d, [r20 + r29*4 - 16] (ND=1, NF=1).
  return {0x62, 0xac, 0x08, 0x14, static_cast<uint8_t>(0x40 | (Condition & 15)),
          0x4c, 0xac, 0xf0};
}

std::vector<uint8_t> encodeMemoryDestination(unsigned Condition, bool W = false,
                                             unsigned PP = 0) {
  // CFCMOVcc [r20 + r29*4 - 16], r17d (ND=0, NF=1).
  return {0x62,
          0xac,
          static_cast<uint8_t>((W ? 0x80 : 0) | 0x78 | (PP & 1)),
          0x0c,
          static_cast<uint8_t>(0x40 | (Condition & 15)),
          0x4c,
          0xac,
          0xf0};
}

enum class SegmentOverride { FS, GS };

std::vector<uint8_t> encodeSegmentedMemorySource(unsigned Condition,
                                                 bool Address32,
                                                 SegmentOverride Segment) {
  std::vector<uint8_t> Bytes;
  if (Address32)
    Bytes.push_back(0x67);
  Bytes.push_back(Segment == SegmentOverride::FS ? 0x64 : 0x65);
  const uint8_t Encoding[] = {
      0x62, 0xac, 0x78, 0x08, static_cast<uint8_t>(0x40 | (Condition & 15)),
      0x4c, 0xac, 0xf0,
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

TEST(X86APXCFCmov, FalseConditionSuppressesUnmappedMemorySource) {
  const LiftedInstruction Move = liftX64(encodeMemorySource(4));
  ASSERT_EQ(Move.Id, X86_INS_CFCMOVE);
  ASSERT_FALSE(Move.Ops.empty());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);

  const RegInfo Destination = mapCapstoneReg(X86_REG_R17D);
  ASSERT_EQ(Destination.Size, 4u);
  Emulator.setRegister(Destination.Offset, UINT64_C(0xaabbccddeeff0011));
  Emulator.setRegister(x86reg::R20, UINT64_C(0xffff800000005000));
  Emulator.setRegister(x86reg::R29, 4);
  Emulator.setRegister(x86reg::CF, 1);
  Emulator.setRegister(x86reg::PF, 1);
  Emulator.setRegister(x86reg::ZF, 0); // CFCMOVE condition is false.
  Emulator.setRegister(x86reg::SF, 1);
  Emulator.setRegister(x86reg::OF, 0);

  EXPECT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(Emulator.getRegister(Destination.Offset), 0u);
  EXPECT_TRUE(Emulator.getLoadRecords().empty());
  EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::PF), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXCFCmov, RegisterFormsCoverAllConditionsAndZeroUpperWidths) {
  constexpr uint64_t Initial = UINT64_C(0xaabbccddeeff0011);
  constexpr uint64_t SourceValue = UINT64_C(0x8877665544332211);
  constexpr unsigned DestinationNumber = 17;
  constexpr unsigned SourceNumber = 29;
  const RegInfo Destination = mapCapstoneReg(X86_REG_R17);
  const RegInfo Source = mapCapstoneReg(X86_REG_R29);
  ASSERT_EQ(Destination.Size, 8u);
  ASSERT_EQ(Source.Size, 8u);

  struct WidthCase {
    bool W;
    unsigned PP;
    unsigned Bytes;
  };
  constexpr std::array<WidthCase, 3> Widths = {
      WidthCase{false, 1, 2}, WidthCase{false, 0, 4}, WidthCase{true, 0, 8}};

  for (unsigned Condition = 0; Condition < 16; ++Condition) {
    const std::array<unsigned, 2> FlagCases = falseTrueFlags(Condition);
    for (const WidthCase &Width : Widths) {
      const LiftedInstruction Move = liftX64(encodeRegister(
          Condition, Width.W, Width.PP, DestinationNumber, SourceNumber));
      SCOPED_TRACE(::testing::Message()
                   << "condition=" << Condition << " bytes=" << Width.Bytes);
      ASSERT_EQ(Move.Id, kCFCmovIds[Condition]);
      ASSERT_FALSE(Move.Ops.empty());

      for (unsigned Result = 0; Result < 2; ++Result) {
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setLoadCollect(true);
        Emulator.setRegister(Destination.Offset, Initial);
        Emulator.setRegister(Source.Offset, SourceValue);
        setFlags(Emulator, FlagCases[Result]);

        ASSERT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
        const uint64_t WidthMask = Width.Bytes == 8
                                       ? UINT64_MAX
                                       : (UINT64_C(1) << (Width.Bytes * 8)) - 1;
        const uint64_t Expected = Result ? SourceValue & WidthMask : 0;
        EXPECT_EQ(Emulator.getRegister(Destination.Offset), Expected);
        EXPECT_EQ(Emulator.getRegister(Source.Offset), SourceValue);
        EXPECT_EQ(Emulator.getRegister(x86reg::CF),
                  (FlagCases[Result] >> 0) & 1);
        EXPECT_EQ(Emulator.getRegister(x86reg::PF),
                  (FlagCases[Result] >> 1) & 1);
        EXPECT_EQ(Emulator.getRegister(x86reg::ZF),
                  (FlagCases[Result] >> 2) & 1);
        EXPECT_EQ(Emulator.getRegister(x86reg::SF),
                  (FlagCases[Result] >> 3) & 1);
        EXPECT_EQ(Emulator.getRegister(x86reg::OF),
                  (FlagCases[Result] >> 4) & 1);
        EXPECT_TRUE(Emulator.getLoadRecords().empty());
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXCFCmov, RegisterOperandDirectionsMatchNDAndNF) {
  constexpr uint64_t RegValue = UINT64_C(0x1111222233334444);
  constexpr uint64_t RMValue = UINT64_C(0xaaaabbbbccccdddd);
  const RegInfo R17 = mapCapstoneReg(X86_REG_R17);
  const RegInfo R29 = mapCapstoneReg(X86_REG_R29);
  const RegInfo R30 = mapCapstoneReg(X86_REG_R30);

  const LiftedInstruction Reversed =
      liftX64(encodeRegisterVariant(5, false, true, true, 0, 0, 17, 29));
  ASSERT_EQ(Reversed.Id, X86_INS_CFCMOVNE);
  ASSERT_FALSE(Reversed.Ops.empty());
  for (const bool Condition : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(R17.Offset, RegValue);
    Emulator.setRegister(R29.Offset, RMValue);
    Emulator.setRegister(x86reg::ZF, Condition ? 0 : 1);
    ASSERT_EQ(Emulator.run(Reversed.Ops), Reversed.Ops.size());
    EXPECT_EQ(Emulator.getRegister(R29.Offset), Condition ? RegValue : 0);
    EXPECT_EQ(Emulator.getRegister(R17.Offset), RegValue);
    EXPECT_FALSE(Emulator.skips().any());
  }

  const LiftedInstruction Ndd =
      liftX64(encodeRegisterVariant(5, true, true, true, 0, 30, 17, 29));
  ASSERT_EQ(Ndd.Id, X86_INS_CFCMOVNE);
  ASSERT_FALSE(Ndd.Ops.empty());
  for (const bool Condition : {false, true}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(R17.Offset, RegValue);
    Emulator.setRegister(R29.Offset, RMValue);
    Emulator.setRegister(R30.Offset, UINT64_C(0xfeedfacecafebeef));
    Emulator.setRegister(x86reg::ZF, Condition ? 0 : 1);
    ASSERT_EQ(Emulator.run(Ndd.Ops), Ndd.Ops.size());
    EXPECT_EQ(Emulator.getRegister(R30.Offset), Condition ? RMValue : RegValue);
    EXPECT_EQ(Emulator.getRegister(R17.Offset), RegValue);
    EXPECT_EQ(Emulator.getRegister(R29.Offset), RMValue);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXCFCmov, NddMemorySourceIsReadOnlyWhenConditionIsTrue) {
  constexpr uint64_t Target = 0x6000;
  constexpr uint64_t Initial = UINT64_C(0xdeadbeefcafef00d);
  constexpr uint64_t Fallback = UINT64_C(0xaabbccdd11223344);
  constexpr uint64_t Loaded = UINT64_C(0x88776655);
  const LiftedInstruction Move = liftX64(encodeNddMemorySource(4));
  ASSERT_EQ(Move.Id, X86_INS_CFCMOVE);
  ASSERT_FALSE(Move.Ops.empty());

  const RegInfo Destination = mapCapstoneReg(X86_REG_R30);
  const RegInfo FallbackRegister = mapCapstoneReg(X86_REG_R17);
  ASSERT_EQ(Destination.Size, 8u);
  ASSERT_EQ(FallbackRegister.Size, 8u);

  auto Initialize = [&](NdOpEmulator &Emulator, bool Condition) {
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(Destination.Offset, Initial);
    Emulator.setRegister(FallbackRegister.Offset, Fallback);
    Emulator.setRegister(x86reg::R20, Target);
    Emulator.setRegister(x86reg::R29, 4);
    Emulator.setRegister(x86reg::ZF, Condition ? 1 : 0);
  };

  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  NdOpEmulator FalsePath(Empty);
  Initialize(FalsePath, false);
  ASSERT_EQ(FalsePath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(FalsePath.getRegister(Destination.Offset),
            Fallback & UINT64_C(0xffffffff));
  EXPECT_TRUE(FalsePath.getLoadRecords().empty());
  EXPECT_EQ(FalsePath.getRegister(x86reg::ZF), 0u);
  EXPECT_FALSE(FalsePath.skips().any());

  BinaryImage Mapped;
  Mapped.Arch = Arch::X64;
  Mapped.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = Target;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {0x55, 0x66, 0x77, 0x88};
  Mapped.Segments.push_back(std::move(Data));
  NdOpEmulator TruePath(Mapped);
  Initialize(TruePath, true);
  ASSERT_EQ(TruePath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(TruePath.getRegister(Destination.Offset), Loaded);
  ASSERT_EQ(TruePath.getLoadRecords().size(), 1u);
  EXPECT_EQ(TruePath.getLoadRecords()[0].Addr, Target);
  EXPECT_EQ(TruePath.getLoadRecords()[0].Size, 4u);
  EXPECT_EQ(TruePath.getRegister(x86reg::ZF), 1u);
  EXPECT_FALSE(TruePath.skips().any());

  NdOpEmulator FaultPath(Empty);
  Initialize(FaultPath, true);
  EXPECT_LT(FaultPath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(FaultPath.getRegister(Destination.Offset), Initial);
  EXPECT_TRUE(FaultPath.getLoadRecords().empty());
  EXPECT_EQ(FaultPath.getRegister(x86reg::ZF), 1u);
  EXPECT_FALSE(FaultPath.skips().any());
}

TEST(X86APXCFCmov, MemorySourcePreservesAddressSizeAndSegmentBase) {
  struct AddressCase {
    const char *Name;
    bool Address32;
    SegmentOverride Segment;
    uint64_t Base;
    uint64_t Index;
    uint64_t SegmentBase;
    uint64_t Target;
  };
  constexpr std::array<AddressCase, 2> Cases = {{
      {"fs64", false, SegmentOverride::FS, 0x20, 4, 0x8000, 0x8020},
      {"gs32", true, SegmentOverride::GS, UINT64_C(0x12345678fffffff0), 8,
       0x9000, 0x9000},
  }};
  const RegInfo Destination = mapCapstoneReg(X86_REG_R17);

  for (const AddressCase &Case : Cases) {
    const LiftedInstruction Move =
        liftX64(encodeSegmentedMemorySource(4, Case.Address32, Case.Segment));
    SCOPED_TRACE(Case.Name);
    ASSERT_EQ(Move.Id, X86_INS_CFCMOVE);
    ASSERT_FALSE(Move.Ops.empty());

    auto Initialize = [&](NdOpEmulator &Emulator, bool Condition) {
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(Destination.Offset, UINT64_C(0xaabbccddeeff0011));
      Emulator.setRegister(x86reg::R20, Case.Base);
      Emulator.setRegister(x86reg::R29, Case.Index);
      Emulator.setRegister(x86reg::ZF, Condition ? 1 : 0);
      ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
          Case.Segment == SegmentOverride::FS ? NdMemoryAddressSpace::X86FS
                                              : NdMemoryAddressSpace::X86GS,
          Case.SegmentBase));
    };

    BinaryImage Empty;
    Empty.Arch = Arch::X64;
    Empty.Bits = Bitness::Bits64;
    NdOpEmulator FalsePath(Empty);
    Initialize(FalsePath, false);
    ASSERT_EQ(FalsePath.run(Move.Ops), Move.Ops.size());
    EXPECT_EQ(FalsePath.getRegister(Destination.Offset), 0u);
    EXPECT_TRUE(FalsePath.getLoadRecords().empty());
    EXPECT_EQ(FalsePath.getRegister(x86reg::R20), Case.Base);
    EXPECT_EQ(FalsePath.getRegister(x86reg::R29), Case.Index);
    EXPECT_FALSE(FalsePath.skips().any());

    BinaryImage Mapped;
    Mapped.Arch = Arch::X64;
    Mapped.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = Case.Target;
    Data.Size = 4;
    Data.Flags = SegmentFlags::Readable;
    Data.Data = {0x78, 0x56, 0x34, 0x12};
    Mapped.Segments.push_back(std::move(Data));
    NdOpEmulator TruePath(Mapped);
    Initialize(TruePath, true);
    ASSERT_EQ(TruePath.run(Move.Ops), Move.Ops.size());
    EXPECT_EQ(TruePath.getRegister(Destination.Offset), UINT64_C(0x12345678));
    ASSERT_EQ(TruePath.getLoadRecords().size(), 1u);
    EXPECT_EQ(TruePath.getLoadRecords()[0].Addr, Case.Target);
    EXPECT_EQ(TruePath.getLoadRecords()[0].Size, 4u);
    EXPECT_EQ(TruePath.getRegister(x86reg::R20), Case.Base);
    EXPECT_EQ(TruePath.getRegister(x86reg::R29), Case.Index);
    EXPECT_FALSE(TruePath.skips().any());
  }
}

TEST(X86APXCFCmov, MemoryDestinationIsWrittenOnlyWhenConditionIsTrue) {
  constexpr uint64_t Target = 0x7000;
  constexpr uint64_t SourceValue = UINT64_C(0xaabbccdd11223344);
  constexpr uint64_t InitialMemory = UINT64_C(0x88776655);
  const LiftedInstruction Move = liftX64(encodeMemoryDestination(5));
  ASSERT_EQ(Move.Id, X86_INS_CFCMOVNE);
  ASSERT_FALSE(Move.Ops.empty());

  const RegInfo Source = mapCapstoneReg(X86_REG_R17);
  ASSERT_EQ(Source.Size, 8u);
  auto Initialize = [&](NdOpEmulator &Emulator, bool Condition) {
    Emulator.setStrictMode(true);
    Emulator.setRegister(Source.Offset, SourceValue);
    Emulator.setRegister(x86reg::R20, Target);
    Emulator.setRegister(x86reg::R29, 4);
    // CFCMOVNE is true exactly when ZF is clear.
    Emulator.setRegister(x86reg::ZF, Condition ? 0 : 1);
  };
  auto Probe = [=](NdOpEmulator &Emulator) {
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(UINT64_C(0x77000000), 4);
    Load.addInput(NdVar::cst(Target, 8));
    EXPECT_TRUE(Emulator.step(Load));
    return Emulator.getRegister(UINT64_C(0x77000000)).value_or(0);
  };

  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  NdOpEmulator FalsePath(Empty);
  Initialize(FalsePath, false);
  ASSERT_EQ(FalsePath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(FalsePath.getRegister(Source.Offset), SourceValue);
  EXPECT_EQ(FalsePath.getRegister(x86reg::ZF), 1u);
  EXPECT_FALSE(FalsePath.skips().any());

  auto MemoryImage = [&](SegmentFlags Flags) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = Target;
    Data.Size = 4;
    Data.Flags = Flags;
    Data.Data = {0x55, 0x66, 0x77, 0x88};
    Image.Segments.push_back(std::move(Data));
    return Image;
  };

  BinaryImage WritableImage =
      MemoryImage(SegmentFlags::Readable | SegmentFlags::Writable);
  NdOpEmulator TruePath(WritableImage);
  Initialize(TruePath, true);
  ASSERT_EQ(TruePath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(Probe(TruePath), SourceValue & UINT64_C(0xffffffff));
  EXPECT_EQ(TruePath.getRegister(Source.Offset), SourceValue);
  EXPECT_EQ(TruePath.getRegister(x86reg::ZF), 0u);
  EXPECT_FALSE(TruePath.skips().any());

  BinaryImage ReadOnlyImage = MemoryImage(SegmentFlags::Readable);
  NdOpEmulator FaultPath(ReadOnlyImage);
  Initialize(FaultPath, true);
  EXPECT_LT(FaultPath.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(Probe(FaultPath), InitialMemory);
  EXPECT_EQ(FaultPath.getRegister(Source.Offset), SourceValue);
  EXPECT_EQ(FaultPath.getRegister(x86reg::ZF), 0u);
  EXPECT_FALSE(FaultPath.skips().any());
}

TEST(X86APXCFCmov, MemoryLoadAndStoreCoverAllOperandWidths) {
  constexpr uint64_t Target = 0x8000;
  constexpr uint64_t SourceValue = UINT64_C(0xa8a7a6a5a4a3a2a1);
  constexpr uint64_t MemoryValue = UINT64_C(0x8877665544332211);
  const RegInfo R17 = mapCapstoneReg(X86_REG_R17);
  struct WidthCase {
    bool W;
    unsigned PP;
    unsigned Bytes;
  };
  constexpr std::array<WidthCase, 3> Widths = {
      WidthCase{false, 1, 2}, WidthCase{false, 0, 4}, WidthCase{true, 0, 8}};

  auto MakeImage = [&] {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Segment Data;
    Data.VA = Target;
    Data.Size = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    Image.Segments.push_back(std::move(Data));
    return Image;
  };
  auto Initialize = [&](NdOpEmulator &Emulator) {
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::R20, Target);
    Emulator.setRegister(x86reg::R29, 4);
    Emulator.setRegister(x86reg::ZF, 0); // CFCMOVNE is true.
  };

  for (const WidthCase &Width : Widths) {
    SCOPED_TRACE(::testing::Message() << "bytes=" << Width.Bytes);
    const uint64_t Mask =
        Width.Bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (Width.Bytes * 8)) - 1;

    const LiftedInstruction Load =
        liftX64(encodeMemorySource(5, Width.W, Width.PP));
    ASSERT_EQ(Load.Id, X86_INS_CFCMOVNE);
    BinaryImage LoadImage = MakeImage();
    NdOpEmulator LoadEmulator(LoadImage);
    Initialize(LoadEmulator);
    LoadEmulator.setRegister(R17.Offset, UINT64_MAX);
    ASSERT_EQ(LoadEmulator.run(Load.Ops), Load.Ops.size());
    EXPECT_EQ(LoadEmulator.getRegister(R17.Offset), MemoryValue & Mask);
    EXPECT_FALSE(LoadEmulator.skips().any());

    const LiftedInstruction Store =
        liftX64(encodeMemoryDestination(5, Width.W, Width.PP));
    ASSERT_EQ(Store.Id, X86_INS_CFCMOVNE);
    BinaryImage StoreImage = MakeImage();
    NdOpEmulator StoreEmulator(StoreImage);
    Initialize(StoreEmulator);
    StoreEmulator.setRegister(R17.Offset, SourceValue);
    ASSERT_EQ(StoreEmulator.run(Store.Ops), Store.Ops.size());
    LowOp Probe;
    Probe.Opcode = NdOp::LOAD;
    Probe.Output = NdVar::tmp(UINT64_C(0x78000000) + Width.Bytes, Width.Bytes);
    Probe.addInput(NdVar::cst(Target, 8));
    ASSERT_TRUE(StoreEmulator.step(Probe));
    EXPECT_EQ(StoreEmulator.getRegister(Probe.Output.Offset),
              SourceValue & Mask);
    EXPECT_EQ(StoreEmulator.getRegister(R17.Offset), SourceValue);
    EXPECT_FALSE(StoreEmulator.skips().any());
  }
}

} // namespace
