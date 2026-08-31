//===- X86_64_APXCCmpTests.cpp - APX CCMP semantics ---------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

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

enum class SegmentOverride { FS, GS };

uint8_t encodeP0(unsigned Reg, unsigned RM) {
  return static_cast<uint8_t>(0x04 | ((Reg & 8) ? 0 : 0x80) |
                              ((Reg & 16) ? 0 : 0x10) | ((RM & 8) ? 0 : 0x20) |
                              ((RM & 16) ? 0x08 : 0));
}

uint8_t encodeP1(unsigned DFV, unsigned Width) {
  return static_cast<uint8_t>(((Width == 8) ? 0x80 : 0) | ((DFV & 15) << 3) |
                              0x04 | ((Width == 2) ? 1 : 0));
}

std::vector<uint8_t> encodeRegister(unsigned SCC, unsigned DFV, unsigned Width,
                                    bool RMFirst, unsigned Left = 17,
                                    unsigned Right = 26) {
  const unsigned Reg = RMFirst ? Right : Left;
  const unsigned RM = RMFirst ? Left : Right;
  return {
      0x62,
      encodeP0(Reg, RM),
      encodeP1(DFV, Width),
      static_cast<uint8_t>(SCC & 15),
      static_cast<uint8_t>(RMFirst ? (Width == 1 ? 0x38 : 0x39)
                                   : (Width == 1 ? 0x3a : 0x3b)),
      static_cast<uint8_t>(0xc0 | ((Reg & 7) << 3) | (RM & 7)),
  };
}

std::vector<uint8_t> encodeImmediate(unsigned SCC, unsigned DFV, unsigned Width,
                                     uint32_t Immediate,
                                     bool CompactImm8 = false,
                                     unsigned Left = 17) {
  const uint8_t Opcode = Width == 1 ? 0x80 : CompactImm8 ? 0x83 : 0x81;
  std::vector<uint8_t> Bytes = {
      0x62,
      encodeP0(7, Left),
      encodeP1(DFV, Width),
      static_cast<uint8_t>(SCC & 15),
      Opcode,
      static_cast<uint8_t>(0xf8 | (Left & 7)),
  };
  const unsigned ImmediateSize = CompactImm8 || Width == 1 ? 1
                                 : Width == 2              ? 2
                                                           : 4;
  for (unsigned I = 0; I < ImmediateSize; ++I)
    Bytes.push_back(static_cast<uint8_t>(Immediate >> (I * 8)));
  return Bytes;
}

std::vector<uint8_t> encodeMemory(unsigned SCC, unsigned DFV, unsigned Width,
                                  bool RMFirst, bool Address32,
                                  SegmentOverride Segment) {
  std::vector<uint8_t> Bytes;
  if (Address32)
    Bytes.push_back(0x67);
  Bytes.push_back(Segment == SegmentOverride::FS ? 0x64 : 0x65);

  // [r20 + r13*4 - 16] exercises an EGPR base and an extended index. The
  // register operand is r17, on either side according to opcode 39 or 3B.
  const uint8_t Encoding[] = {
      0x62,
      encodeP0(17, 20),
      encodeP1(DFV, Width),
      static_cast<uint8_t>(SCC & 15),
      static_cast<uint8_t>(RMFirst ? (Width == 1 ? 0x38 : 0x39)
                                   : (Width == 1 ? 0x3a : 0x3b)),
      0x4c,
      0xac,
      0xf0,
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
    ADD_FAILURE() << "failed to decode complete CCMP instruction";
    return {};
  }

  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "CCMP was not lifted";
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

BinaryImage makeImage(uint64_t Address = 0, unsigned Size = 0,
                      SegmentFlags Permissions = SegmentFlags::Readable,
                      uint64_t Value = 0) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  if (Size != 0) {
    Segment Data;
    Data.VA = Address;
    Data.Size = Size;
    Data.Flags = Permissions;
    Data.Data.resize(Size);
    for (unsigned I = 0; I < Size; ++I)
      Data.Data[I] = static_cast<uint8_t>(Value >> (I * 8));
    Image.Segments.push_back(std::move(Data));
  }
  return Image;
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

Flags flagsFromBits(unsigned Bits) {
  return {
      (Bits & 0x01) != 0, (Bits & 0x02) != 0, (Bits & 0x04) != 0,
      (Bits & 0x08) != 0, (Bits & 0x10) != 0, (Bits & 0x20) != 0,
      (Bits & 0x40) != 0,
  };
}

bool sourceCondition(unsigned SCC, const Flags &F) {
  switch (SCC) {
  case 0:
    return F.OF;
  case 1:
    return !F.OF;
  case 2:
    return F.CF;
  case 3:
    return !F.CF;
  case 4:
    return F.ZF;
  case 5:
    return !F.ZF;
  case 6:
    return F.CF || F.ZF;
  case 7:
    return !F.CF && !F.ZF;
  case 8:
    return F.SF;
  case 9:
    return !F.SF;
  case 10:
    return true;
  case 11:
    return false;
  case 12:
    return F.SF != F.OF;
  case 13:
    return F.SF == F.OF;
  case 14:
    return F.ZF || F.SF != F.OF;
  case 15:
    return !F.ZF && F.SF == F.OF;
  default:
    return false;
  }
}

bool evenParity(uint8_t Value) {
  unsigned Bits = 0;
  for (unsigned I = 0; I < 8; ++I)
    Bits += (Value >> I) & 1;
  return (Bits & 1) == 0;
}

Flags expectedFlags(unsigned SCC, unsigned DFV, unsigned Width, uint64_t Left,
                    uint64_t Right, const Flags &Input) {
  if (!sourceCondition(SCC, Input)) {
    return {
        (DFV & 1) != 0, (DFV & 1) != 0, false,    (DFV & 2) != 0,
        (DFV & 4) != 0, (DFV & 8) != 0, Input.DF,
    };
  }

  const uint64_t Mask =
      Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - 1;
  const uint64_t A = Left & Mask;
  const uint64_t B = Right & Mask;
  const uint64_t Result = (A - B) & Mask;
  const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
  return {
      A < B,
      evenParity(static_cast<uint8_t>(Result)),
      ((A ^ B ^ Result) & 0x10) != 0,
      Result == 0,
      (Result & Sign) != 0,
      (((A ^ B) & (A ^ Result)) & Sign) != 0,
      Input.DF,
  };
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

void initializeStrict(NdOpEmulator &Emulator, const Flags &Input) {
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  setFlags(Emulator, Input);
}

uint64_t signExtendImmediate(uint32_t Immediate, unsigned ImmediateSize) {
  const int64_t Signed = ImmediateSize == 1   ? static_cast<int8_t>(Immediate)
                         : ImmediateSize == 2 ? static_cast<int16_t>(Immediate)
                                              : static_cast<int32_t>(Immediate);
  return static_cast<uint64_t>(Signed);
}

TEST(X86APXCCmp, MalformedRawEncodingFailsBeforeProducingState) {
  enum class Mutation {
    Map,
    RequiredP1,
    Width,
    ReservedP2,
    Opcode,
    ModRM,
    ImmediateDetail,
  };
  constexpr std::array<Mutation, 7> Mutations = {
      Mutation::Map,
      Mutation::RequiredP1,
      Mutation::Width,
      Mutation::ReservedP2,
      Mutation::Opcode,
      Mutation::ModRM,
      Mutation::ImmediateDetail,
  };
  const std::vector<uint8_t> Bytes =
      encodeImmediate(10, 6, 8, UINT32_C(0x80000000));

  for (Mutation Kind : Mutations) {
    SCOPED_TRACE(static_cast<unsigned>(Kind));
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                   kInstructionAddress, Insn),
              static_cast<int>(Bytes.size()));
    ASSERT_NE(Insn.Raw, nullptr);
    ASSERT_NE(Insn.Raw->detail, nullptr);

    switch (Kind) {
    case Mutation::Map:
      Insn.Raw->bytes[1] =
          static_cast<uint8_t>((Insn.Raw->bytes[1] & 0xf8) | 0x03);
      break;
    case Mutation::RequiredP1:
      Insn.Raw->bytes[2] &= static_cast<uint8_t>(~0x04);
      break;
    case Mutation::Width:
      Insn.Raw->bytes[2] ^= 0x80;
      break;
    case Mutation::ReservedP2:
      Insn.Raw->bytes[3] |= 0x10;
      break;
    case Mutation::Opcode:
      Insn.Raw->bytes[4] = 0x82;
      break;
    case Mutation::ModRM:
      Insn.Raw->bytes[5] ^= 1;
      break;
    case Mutation::ImmediateDetail:
      Insn.Raw->detail->x86.encoding.imm_size = 1;
      break;
    }

    std::vector<LowOp> Ops;
    EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(X86APXCCmp, RawRolesPrefixesAndMemoryDetailMustAgreeExactly) {
  const auto Register = encodeRegister(10, 6, 8, true);
  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].reg = X86_REG_R18;
  });
  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.detail->x86.operands[1].reg = X86_REG_R25;
  });

  const auto Immediate = encodeImmediate(10, 6, 8, UINT32_C(0x80000000));
  expectMutatedLiftRejected(
      Immediate, [](cs_insn &Raw) { ++Raw.detail->x86.operands[1].imm; });

  const auto Memory = encodeMemory(10, 6, 8, true, false, SegmentOverride::FS);
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.base = X86_REG_R21;
  });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.index = X86_REG_R12;
  });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.segment = X86_REG_GS;
  });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { Raw.detail->x86.prefix[1] = 0x65; });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x64;
    ++Raw.size;
  });

  const auto Address32 =
      encodeMemory(10, 6, 8, true, true, SegmentOverride::GS);
  expectMutatedLiftRejected(Address32, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x67;
    ++Raw.size;
  });
}

TEST(X86APXCCmp, UBitSelectsR28ForAddr64AndAddr32Memory) {
  const std::array<std::vector<uint8_t>, 2> Forms = {{
      {0x64, 0x62, 0x0c, 0x28, 0x0a, 0x39, 0x54, 0xa5, 0x20},
      {0x67, 0x64, 0x62, 0x0c, 0x28, 0x0a, 0x39, 0x54, 0xa5, 0x20},
  }};
  constexpr uint64_t Target = 0x5900;
  for (unsigned Address32 = 0; Address32 != Forms.size(); ++Address32) {
    SCOPED_TRACE(Address32);
    const LiftedInstruction Test = liftX64(Forms[Address32]);
    ASSERT_EQ(Test.Id, X86_INS_CCMPT);
    ASSERT_FALSE(Test.Ops.empty());

    BinaryImage Image =
        makeImage(Target, 4, SegmentFlags::Readable, UINT64_C(0x12345678));
    NdOpEmulator Emulator(Image);
    initializeStrict(Emulator, {});
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
        NdMemoryAddressSpace::X86FS, Target - 0x30));
    Emulator.setRegister(x86reg::R29,
                         Address32 ? UINT64_C(0xaaaaaaaa00000008) : 8);
    Emulator.setRegister(x86reg::R28,
                         Address32 ? UINT64_C(0xbbbbbbbb00000002) : 2);
    Emulator.setRegister(x86reg::R26, UINT64_C(0x0000000011111111));
    ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Target);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);

    expectMutatedLiftRejected(Forms[Address32], [Address32](cs_insn &Raw) {
      Raw.detail->x86.operands[0].mem.index =
          Address32 ? X86_REG_R14D : X86_REG_R14;
    });
    expectMutatedLiftRejected(Forms[Address32], [](cs_insn &Raw) {
      Raw.bytes[Raw.detail->x86.encoding.modrm_offset - 3] |= 0x04;
    });
  }

  const std::vector<uint8_t> RegisterU0 = {0x62, 0x6c, 0x28,
                                           0x02, 0x39, 0xd1};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Invalid{};
  EXPECT_NE(Dec.decodeOneForLift(RegisterU0.data(), RegisterU0.size(),
                                kInstructionAddress, Invalid),
            static_cast<int>(RegisterU0.size()));
}

TEST(X86APXCCmp, EverySourceConditionSelectsCompareOrDefaultFlags) {
  constexpr uint64_t LeftValue = UINT64_C(0x0123456780000000);
  constexpr uint64_t RightValue = UINT64_C(0xfedcba9800000001);
  const RegInfo Left = mapCapstoneReg(X86_REG_R17D);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26D);

  for (unsigned SCC = 0; SCC < 16; ++SCC) {
    const LiftedInstruction Test = liftX64(encodeRegister(SCC, 13, 4, true));
    ASSERT_EQ(Test.Id, X86_INS_CCMPO + SCC);
    ASSERT_FALSE(Test.Ops.empty());

    bool SawFalse = false;
    bool SawTrue = false;
    for (unsigned Bits = 0; Bits < 128; ++Bits) {
      SCOPED_TRACE(::testing::Message() << "scc=" << SCC << " flags=" << Bits);
      const Flags Input = flagsFromBits(Bits);
      SawFalse |= !sourceCondition(SCC, Input);
      SawTrue |= sourceCondition(SCC, Input);

      BinaryImage Image = makeImage();
      NdOpEmulator Emulator(Image);
      initializeStrict(Emulator, Input);
      Emulator.setRegister(Left.Offset, LeftValue);
      Emulator.setRegister(Right.Offset, RightValue);

      ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
      expectFlags(Emulator,
                  expectedFlags(SCC, 13, 4, LeftValue, RightValue, Input));
      EXPECT_EQ(Emulator.getRegister(Left.Offset), LeftValue);
      EXPECT_EQ(Emulator.getRegister(Right.Offset), RightValue);
      EXPECT_TRUE(Emulator.getLoadRecords().empty());
      EXPECT_FALSE(Emulator.skips().any());
    }

    if (SCC == 10) {
      EXPECT_FALSE(SawFalse);
      EXPECT_TRUE(SawTrue);
    } else if (SCC == 11) {
      EXPECT_TRUE(SawFalse);
      EXPECT_FALSE(SawTrue);
    } else {
      EXPECT_TRUE(SawFalse);
      EXPECT_TRUE(SawTrue);
    }
  }
}

TEST(X86APXCCmp, FalseConditionCoversEveryDefaultFlagsValue) {
  const RegInfo Left = mapCapstoneReg(X86_REG_R17);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26);
  const Flags Input{true, false, true, false, true, false, true};

  for (unsigned DFV = 0; DFV < 16; ++DFV) {
    SCOPED_TRACE(::testing::Message() << "dfv=" << DFV);
    const LiftedInstruction Test = liftX64(encodeRegister(11, DFV, 8, true));
    ASSERT_EQ(Test.Id, X86_INS_CCMPF);
    ASSERT_FALSE(Test.Ops.empty());

    BinaryImage Image = makeImage();
    NdOpEmulator Emulator(Image);
    initializeStrict(Emulator, Input);
    Emulator.setRegister(Left.Offset, UINT64_C(0xfedcba9876543210));
    Emulator.setRegister(Right.Offset, UINT64_C(0x1122334455667788));

    ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
    expectFlags(Emulator, expectedFlags(11, DFV, 8, 0, 0, Input));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXCCmp, TrueCompareCoversWidthsOrdersAndImmediateSignExtension) {
  struct CompareCase {
    unsigned Width;
    uint64_t Left;
    uint64_t Right;
    uint32_t FullImmediate;
    uint8_t CompactImmediate;
  };
  constexpr std::array<CompareCase, 2> Cases = {{
      {4, UINT64_C(0x112233447fffffff), UINT64_C(0x55667788ffffffff),
       UINT32_C(0xffffffff), 0x80},
      {8, UINT64_C(0x8000000000000000), UINT64_C(0x0000000000000001),
       UINT32_C(0x80000000), 0x80},
  }};
  const RegInfo Left = mapCapstoneReg(X86_REG_R17);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26);
  const Flags Input{true, false, false, true, false, false, true};

  for (const CompareCase &Case : Cases) {
    for (bool RMFirst : {true, false}) {
      SCOPED_TRACE(::testing::Message()
                   << "width=" << Case.Width << " rm-first=" << RMFirst);
      const LiftedInstruction Test =
          liftX64(encodeRegister(10, 3, Case.Width, RMFirst));
      ASSERT_EQ(Test.Id, X86_INS_CCMPT);
      ASSERT_FALSE(Test.Ops.empty());

      BinaryImage Image = makeImage();
      NdOpEmulator Emulator(Image);
      initializeStrict(Emulator, Input);
      Emulator.setRegister(Left.Offset, Case.Left);
      Emulator.setRegister(Right.Offset, Case.Right);

      ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
      expectFlags(Emulator, expectedFlags(10, 3, Case.Width, Case.Left,
                                          Case.Right, Input));
      EXPECT_EQ(Emulator.getRegister(Left.Offset), Case.Left);
      EXPECT_EQ(Emulator.getRegister(Right.Offset), Case.Right);
      EXPECT_FALSE(Emulator.skips().any());
    }

    for (bool Compact : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "width=" << Case.Width << " compact=" << Compact);
      const uint32_t Immediate =
          Compact ? Case.CompactImmediate : Case.FullImmediate;
      const LiftedInstruction Test =
          liftX64(encodeImmediate(10, 12, Case.Width, Immediate, Compact));
      ASSERT_EQ(Test.Id, X86_INS_CCMPT);
      ASSERT_FALSE(Test.Ops.empty());

      BinaryImage Image = makeImage();
      NdOpEmulator Emulator(Image);
      initializeStrict(Emulator, Input);
      Emulator.setRegister(Left.Offset, Case.Left);

      ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
      const uint64_t RightValue =
          signExtendImmediate(Immediate, Compact           ? 1
                                         : Case.Width == 8 ? 4
                                                           : Case.Width);
      expectFlags(Emulator, expectedFlags(10, 12, Case.Width, Case.Left,
                                          RightValue, Input));
      EXPECT_EQ(Emulator.getRegister(Left.Offset), Case.Left);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }

  // Opcode 3A is the byte-sized reverse-order partner of 3B.
  const LiftedInstruction ReverseByte =
      liftX64(encodeRegister(10, 5, 1, false));
  ASSERT_EQ(ReverseByte.Id, X86_INS_CCMPT);
  BinaryImage ByteImage = makeImage();
  NdOpEmulator ByteEmulator(ByteImage);
  initializeStrict(ByteEmulator, Input);
  ByteEmulator.setRegister(Left.Offset, 0x80);
  ByteEmulator.setRegister(Right.Offset, 1);
  ASSERT_EQ(ByteEmulator.run(ReverseByte.Ops), ReverseByte.Ops.size());
  expectFlags(ByteEmulator, expectedFlags(10, 5, 1, 0x80, 1, Input));
}

TEST(X86APXCCmp, TrueMemoryPreservesOperandOrderWidthAndAddressSpace) {
  constexpr uint64_t BaseValue = UINT64_C(0xabcdef0100005000);
  constexpr uint64_t IndexValue = 4;
  constexpr uint64_t RegisterValue = UINT64_C(0x800000007fffffff);
  const Flags Input{false, true, false, false, true, false, true};
  const RegInfo Register = mapCapstoneReg(X86_REG_R17);

  for (unsigned Width : {4u, 8u}) {
    for (bool RMFirst : {true, false}) {
      for (SegmentOverride Segment :
           {SegmentOverride::FS, SegmentOverride::GS}) {
        SCOPED_TRACE(::testing::Message()
                     << "width=" << Width << " rm-first=" << RMFirst
                     << " segment=" << static_cast<unsigned>(Segment));
        const uint64_t SegmentBase =
            Segment == SegmentOverride::FS ? 0x100000 : 0x200000;
        const uint64_t Target = SegmentBase + 0x5000;
        const uint64_t MemoryValue =
            Width == 4 ? UINT64_C(0x80000001) : UINT64_C(0x8000000000000001);
        const LiftedInstruction Test =
            liftX64(encodeMemory(10, 9, Width, RMFirst, true, Segment));
        ASSERT_EQ(Test.Id, X86_INS_CCMPT);
        ASSERT_EQ(std::count_if(
                      Test.Ops.begin(), Test.Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
                  1);

        BinaryImage Image =
            makeImage(Target, Width, SegmentFlags::Readable, MemoryValue);
        NdOpEmulator Emulator(Image);
        initializeStrict(Emulator, Input);
        Emulator.setRegister(x86reg::R20, BaseValue);
        Emulator.setRegister(x86reg::R13, IndexValue);
        Emulator.setRegister(Register.Offset, RegisterValue);
        ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
            Segment == SegmentOverride::FS ? NdMemoryAddressSpace::X86FS
                                           : NdMemoryAddressSpace::X86GS,
            SegmentBase));

        ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
        const uint64_t Left = RMFirst ? MemoryValue : RegisterValue;
        const uint64_t Right = RMFirst ? RegisterValue : MemoryValue;
        expectFlags(Emulator, expectedFlags(10, 9, Width, Left, Right, Input));
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Target);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Width);
        EXPECT_EQ(Emulator.getRegister(Register.Offset), RegisterValue);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXCCmp, FalseConditionStillLoadsBeforeSelectingDefaultFlags) {
  constexpr uint64_t SegmentBase = 0x100000;
  constexpr uint64_t Target = SegmentBase + 0x5000;
  const Flags Input{false, true, true, true, false, true, true};

  for (bool RMFirst : {true, false}) {
    SCOPED_TRACE(RMFirst ? "memory-left" : "memory-right");
    const LiftedInstruction Test =
        liftX64(encodeMemory(11, 10, 8, RMFirst, true, SegmentOverride::FS));
    ASSERT_EQ(Test.Id, X86_INS_CCMPF);
    ASSERT_EQ(
        std::count_if(Test.Ops.begin(), Test.Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
        1);
    EXPECT_TRUE(
        std::none_of(Test.Ops.begin(), Test.Ops.end(),
                     [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; }));

    BinaryImage Image = makeImage(Target, 8, SegmentFlags::Readable,
                                  UINT64_C(0x8877665544332211));
    NdOpEmulator Emulator(Image);
    initializeStrict(Emulator, Input);
    Emulator.setRegister(x86reg::R20, UINT64_C(0xabcdef0100005000));
    Emulator.setRegister(x86reg::R13, 4);
    Emulator.setRegister(x86reg::R17, UINT64_C(0x1122334455667788));
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   SegmentBase));

    ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
    expectFlags(Emulator, expectedFlags(11, 10, 8, 0, 0, Input));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Target);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXCCmp, FaultingFalseConditionCommitsNoFlags) {
  const Flags Input{true, false, true, false, true, false, true};

  for (bool RMFirst : {true, false}) {
    for (SegmentFlags Permissions :
         {SegmentFlags::None, SegmentFlags::Writable}) {
      SCOPED_TRACE(::testing::Message()
                   << "rm-first=" << RMFirst
                   << " permissions=" << static_cast<unsigned>(Permissions));
      const LiftedInstruction Test =
          liftX64(encodeMemory(11, 15, 4, RMFirst, false, SegmentOverride::GS));
      ASSERT_EQ(Test.Id, X86_INS_CCMPF);
      ASSERT_FALSE(Test.Ops.empty());

      BinaryImage Image = Permissions == SegmentFlags::None
                              ? makeImage()
                              : makeImage(0x5000, 4, Permissions);
      NdOpEmulator Emulator(Image);
      initializeStrict(Emulator, Input);
      Emulator.setRegister(x86reg::R20, 0x5000);
      Emulator.setRegister(x86reg::R13, 4);
      Emulator.setRegister(x86reg::R17, UINT64_C(0x1122334455667788));
      ASSERT_TRUE(
          Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS, 0));

      EXPECT_LT(Emulator.run(Test.Ops), Test.Ops.size());
      expectFlags(Emulator, Input);
      EXPECT_EQ(Emulator.getRegister(x86reg::R20), 0x5000u);
      EXPECT_EQ(Emulator.getRegister(x86reg::R13), 4u);
      EXPECT_EQ(Emulator.getRegister(x86reg::R17),
                UINT64_C(0x1122334455667788));
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

} // namespace
