//===- X86_64_APXCTestTests.cpp - APX CTEST semantics --------------------===//

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

uint8_t encodeP0(unsigned Reg, unsigned RM) {
  return static_cast<uint8_t>(0x04 | ((Reg & 8) ? 0 : 0x80) |
                              ((Reg & 16) ? 0 : 0x10) | ((RM & 8) ? 0 : 0x20) |
                              ((RM & 16) ? 0x08 : 0));
}

uint8_t encodeP1(unsigned DFV, unsigned Width, bool ExtendedIndex = false) {
  return static_cast<uint8_t>(((Width == 8) ? 0x80 : 0) | ((DFV & 15) << 3) |
                              (ExtendedIndex ? 0 : 0x04) |
                              ((Width == 2) ? 1 : 0));
}

std::vector<uint8_t> encodeRegister(unsigned SCC, unsigned DFV, unsigned Width,
                                    unsigned Left = 17, unsigned Right = 26) {
  return {0x62,
          encodeP0(Right, Left),
          encodeP1(DFV, Width),
          static_cast<uint8_t>(SCC & 15),
          static_cast<uint8_t>(Width == 1 ? 0x84 : 0x85),
          static_cast<uint8_t>(0xc0 | ((Right & 7) << 3) | (Left & 7))};
}

std::vector<uint8_t> encodeImmediate(unsigned SCC, unsigned DFV, unsigned Width,
                                     uint32_t Immediate, unsigned Left = 17,
                                     unsigned Group = 0) {
  std::vector<uint8_t> Bytes = {
      0x62,
      encodeP0(0, Left),
      encodeP1(DFV, Width),
      static_cast<uint8_t>(SCC & 15),
      static_cast<uint8_t>(Width == 1 ? 0xf6 : 0xf7),
      static_cast<uint8_t>(0xc0 | ((Group & 7) << 3) | (Left & 7)),
  };
  const unsigned ImmediateSize = Width == 1 ? 1 : Width == 2 ? 2 : 4;
  for (unsigned I = 0; I < ImmediateSize; ++I)
    Bytes.push_back(static_cast<uint8_t>(Immediate >> (I * 8)));
  return Bytes;
}

enum class SegmentOverride { FS, GS };

std::vector<uint8_t> encodeMemory(unsigned SCC, unsigned DFV, unsigned Width,
                                  bool Address32, SegmentOverride Segment) {
  std::vector<uint8_t> Bytes;
  if (Address32)
    Bytes.push_back(0x67);
  Bytes.push_back(Segment == SegmentOverride::FS ? 0x64 : 0x65);

  // CTESTcc [r20 + r13*4 - 16], r17. R20 is an EGPR base and the index
  // exercises the ordinary X3 address-register extension.
  const uint8_t Encoding[] = {
      0x62,
      0xac,
      encodeP1(DFV, Width),
      static_cast<uint8_t>(SCC & 15),
      static_cast<uint8_t>(Width == 1 ? 0x84 : 0x85),
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
    ADD_FAILURE() << "failed to decode complete CTEST instruction";
    return {};
  }

  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "CTEST was not lifted";
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
                      SegmentFlags Permissions = SegmentFlags::Readable) {
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
  const uint64_t Result = (Left & Right) & Mask;
  return {
      false,       evenParity(static_cast<uint8_t>(Result)), Input.AF,
      Result == 0, ((Result >> (Width * 8 - 1)) & 1) != 0,   false,
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

TEST(X86APXCTest, MalformedRawEncodingFailsClosedAtTheLifter) {
  enum class Mutation {
    Map,
    Width,
    ReservedP2,
    Opcode,
    ModRM,
    ImmediateDetail
  };
  constexpr std::array<Mutation, 6> Mutations = {
      Mutation::Map,    Mutation::Width, Mutation::ReservedP2,
      Mutation::Opcode, Mutation::ModRM, Mutation::ImmediateDetail,
  };
  const std::vector<uint8_t> Bytes =
      encodeImmediate(10, 6, 8, UINT32_C(0x80000000), 17, 1);

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
    case Mutation::Width:
      Insn.Raw->bytes[2] ^= 0x80;
      break;
    case Mutation::ReservedP2:
      Insn.Raw->bytes[3] |= 0x10;
      break;
    case Mutation::Opcode:
      Insn.Raw->bytes[4] = 0x86;
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

TEST(X86APXCTest, RawRolesPrefixesAndMemoryDetailMustAgreeExactly) {
  const auto Register = encodeRegister(10, 6, 8);
  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].reg = X86_REG_R18;
  });
  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.detail->x86.operands[1].reg = X86_REG_R25;
  });

  const auto Immediate = encodeImmediate(10, 6, 8, UINT32_C(0x80000000), 17, 1);
  expectMutatedLiftRejected(
      Immediate, [](cs_insn &Raw) { ++Raw.detail->x86.operands[1].imm; });
  expectMutatedLiftRejected(Immediate,
                            [](cs_insn &Raw) { Raw.bytes[Raw.size++] = 0x90; });

  const auto Memory = encodeMemory(10, 6, 8, false, SegmentOverride::FS);
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.base = X86_REG_R21;
  });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.index = X86_REG_R12;
  });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { Raw.detail->x86.operands[0].mem.scale = 2; });
  expectMutatedLiftRejected(
      Memory, [](cs_insn &Raw) { ++Raw.detail->x86.operands[0].mem.disp; });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    Raw.detail->x86.operands[0].mem.segment = X86_REG_GS;
  });
  expectMutatedLiftRejected(Memory,
                            [](cs_insn &Raw) { Raw.detail->x86.sib ^= 1; });
  expectMutatedLiftRejected(Memory, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x65;
    ++Raw.size;
  });

  const auto Address32 = encodeMemory(10, 6, 8, true, SegmentOverride::FS);
  expectMutatedLiftRejected(Address32, [](cs_insn &Raw) {
    for (size_t I = Raw.size; I != 0; --I)
      Raw.bytes[I] = Raw.bytes[I - 1];
    Raw.bytes[0] = 0x67;
    ++Raw.size;
  });

  expectMutatedLiftRejected(Register, [](cs_insn &Raw) {
    Raw.bytes[Raw.detail->x86.encoding.modrm_offset - 3] &=
        static_cast<uint8_t>(~0x04);
  });
}

TEST(X86APXCTest, UBitSelectsR28ForAddr64AndAddr32Memory) {
  const std::array<std::vector<uint8_t>, 2> Forms = {{
      {0x64, 0x62, 0x0c, 0x28, 0x0a, 0x85, 0x54, 0xa5, 0x20},
      {0x67, 0x64, 0x62, 0x0c, 0x28, 0x0a, 0x85, 0x54, 0xa5, 0x20},
  }};
  constexpr uint64_t Target = 0x5800;
  for (unsigned Address32 = 0; Address32 != Forms.size(); ++Address32) {
    SCOPED_TRACE(Address32);
    const LiftedInstruction Test = liftX64(Forms[Address32]);
    ASSERT_EQ(Test.Id, X86_INS_CTESTT);
    ASSERT_FALSE(Test.Ops.empty());

    BinaryImage Image = makeImage(Target, 4, SegmentFlags::Readable);
    Image.Segments[0].Data = {0x78, 0x56, 0x34, 0x12};
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
                                           0x02, 0x85, 0xd1};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Invalid{};
  EXPECT_NE(Dec.decodeOneForLift(RegisterU0.data(), RegisterU0.size(),
                                kInstructionAddress, Invalid),
            static_cast<int>(RegisterU0.size()));
}

TEST(X86APXCTest, EverySourceConditionSelectsTestOrDefaultFlags) {
  constexpr uint64_t LeftValue = UINT64_C(0xdeadbeef8000000f);
  constexpr uint64_t RightValue = UINT64_C(0x0123456780000003);
  const RegInfo Left = mapCapstoneReg(X86_REG_R17D);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26D);

  for (unsigned SCC = 0; SCC < 16; ++SCC) {
    const LiftedInstruction Test = liftX64(encodeRegister(SCC, 13, 4));
    ASSERT_EQ(Test.Id, X86_INS_CTESTO + SCC);
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

TEST(X86APXCTest, FalseConditionCoversEveryDefaultFlagsValue) {
  const RegInfo Left = mapCapstoneReg(X86_REG_R17);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26);
  const Flags Input{true, false, true, false, true, false, true};

  for (unsigned DFV = 0; DFV < 16; ++DFV) {
    SCOPED_TRACE(::testing::Message() << "dfv=" << DFV);
    const LiftedInstruction Test = liftX64(encodeRegister(11, DFV, 8));
    ASSERT_EQ(Test.Id, X86_INS_CTESTF);
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

TEST(X86APXCTest, TrueTestCoversAllWidthsAndOperandForms) {
  struct WidthCase {
    unsigned Width;
    uint64_t Left;
    uint64_t Right;
    uint32_t Immediate;
  };
  constexpr std::array<WidthCase, 4> Cases = {{
      {1, 0xf3, 0x81, 0x81},
      {2, 0x8003, 0x8001, 0x8001},
      {4, 0x80000003, 0x80000001, 0x80000001},
      // The encoded imm32 is negative and must be sign-extended before the
      // qword AND, not zero-extended from its public four-byte operand size.
      {8, UINT64_C(0xffffffff80000003), UINT64_C(0xffffffff80000000),
       UINT32_C(0x80000000)},
  }};
  const RegInfo Left = mapCapstoneReg(X86_REG_R17);
  const RegInfo Right = mapCapstoneReg(X86_REG_R26);
  const Flags Input{true, false, true, true, false, true, true};

  for (const WidthCase &Case : Cases) {
    for (bool ImmediateForm : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "width=" << Case.Width << " immediate=" << ImmediateForm);
      const LiftedInstruction Test =
          ImmediateForm ? liftX64(encodeImmediate(10, 6, Case.Width,
                                                  Case.Immediate, 17, 1))
                        : liftX64(encodeRegister(10, 6, Case.Width));
      ASSERT_EQ(Test.Id, X86_INS_CTESTT);
      ASSERT_FALSE(Test.Ops.empty());

      BinaryImage Image = makeImage();
      NdOpEmulator Emulator(Image);
      initializeStrict(Emulator, Input);
      Emulator.setRegister(Left.Offset, Case.Left);
      Emulator.setRegister(Right.Offset, Case.Right);

      ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
      const uint64_t Second =
          ImmediateForm
              ? static_cast<uint64_t>(static_cast<int64_t>(
                    Case.Width == 1   ? static_cast<int8_t>(Case.Immediate)
                    : Case.Width == 2 ? static_cast<int16_t>(Case.Immediate)
                                      : static_cast<int32_t>(Case.Immediate)))
              : Case.Right;
      expectFlags(Emulator,
                  expectedFlags(10, 6, Case.Width, Case.Left, Second, Input));
      // TEST leaves AF architecturally undefined. NeverD deliberately keeps
      // its existing deterministic logic-op policy, but this assertion is a
      // project contract rather than an architectural oracle.
      EXPECT_EQ(Emulator.getRegister(x86reg::AF), Input.AF);
      EXPECT_EQ(Emulator.getRegister(Left.Offset), Case.Left);
      EXPECT_EQ(Emulator.getRegister(Right.Offset), Case.Right);
      EXPECT_TRUE(Emulator.getLoadRecords().empty());
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}

TEST(X86APXCTest, FalseConditionStillLoadsSegmentedAddress32Memory) {
  constexpr uint64_t SegmentBase = 0x100000;
  constexpr uint64_t Target = SegmentBase + 0x5000;
  constexpr uint64_t BaseValue = UINT64_C(0xabcdef0100005000);
  constexpr uint64_t IndexValue = 4;
  const Flags Input{false, true, true, true, false, true, true};

  for (SegmentOverride Segment : {SegmentOverride::FS, SegmentOverride::GS}) {
    SCOPED_TRACE(Segment == SegmentOverride::FS ? "fs" : "gs");
    const LiftedInstruction Test =
        liftX64(encodeMemory(11, 10, 8, true, Segment));
    ASSERT_EQ(Test.Id, X86_INS_CTESTF);
    ASSERT_FALSE(Test.Ops.empty());
    EXPECT_EQ(
        std::count_if(Test.Ops.begin(), Test.Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
        1);
    EXPECT_TRUE(
        std::none_of(Test.Ops.begin(), Test.Ops.end(),
                     [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; }));

    BinaryImage Image = makeImage(Target, 8, SegmentFlags::Readable);
    Image.Segments[0].Data = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    NdOpEmulator Emulator(Image);
    initializeStrict(Emulator, Input);
    Emulator.setRegister(x86reg::R20, BaseValue);
    Emulator.setRegister(x86reg::R13, IndexValue);
    Emulator.setRegister(x86reg::R17, UINT64_C(0x8877665544332211));
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
        Segment == SegmentOverride::FS ? NdMemoryAddressSpace::X86FS
                                       : NdMemoryAddressSpace::X86GS,
        SegmentBase));

    ASSERT_EQ(Emulator.run(Test.Ops), Test.Ops.size());
    expectFlags(Emulator, expectedFlags(11, 10, 8, 0, 0, Input));
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Target);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 8u);
    EXPECT_EQ(Emulator.getRegister(x86reg::R20), BaseValue);
    EXPECT_EQ(Emulator.getRegister(x86reg::R13), IndexValue);
    EXPECT_EQ(Emulator.getRegister(x86reg::R17), UINT64_C(0x8877665544332211));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86APXCTest, FaultingFalseConditionCommitsNoFlags) {
  const LiftedInstruction Test =
      liftX64(encodeMemory(11, 15, 4, false, SegmentOverride::FS));
  ASSERT_EQ(Test.Id, X86_INS_CTESTF);
  ASSERT_FALSE(Test.Ops.empty());
  const Flags Input{true, false, true, false, true, false, true};

  for (SegmentFlags Permissions :
       {SegmentFlags::None, SegmentFlags::Writable}) {
    SCOPED_TRACE(Permissions == SegmentFlags::None ? "unmapped"
                                                   : "read-denied");
    BinaryImage Image = Permissions == SegmentFlags::None
                            ? makeImage()
                            : makeImage(0x5000, 4, Permissions);
    NdOpEmulator Emulator(Image);
    initializeStrict(Emulator, Input);
    Emulator.setRegister(x86reg::R20, 0x5000);
    Emulator.setRegister(x86reg::R13, 4);
    Emulator.setRegister(x86reg::R17, UINT64_C(0x1122334455667788));
    ASSERT_TRUE(
        Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, 0));

    EXPECT_LT(Emulator.run(Test.Ops), Test.Ops.size());
    expectFlags(Emulator, Input);
    EXPECT_EQ(Emulator.getRegister(x86reg::R20), 0x5000u);
    EXPECT_EQ(Emulator.getRegister(x86reg::R13), 4u);
    EXPECT_EQ(Emulator.getRegister(x86reg::R17), UINT64_C(0x1122334455667788));
    EXPECT_FALSE(Emulator.skips().any());
  }
}

} // namespace
