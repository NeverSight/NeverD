//===- MIPSBitfieldTests.cpp - Independent r2 bitfield semantics ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Enumerate logical field positions and lengths independently of the lifter's
/// encoding table. A bit-at-a-time oracle and pinned Unicorn check defined
/// results; malformed encodings and unpredictable inputs must be refused.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <ios>
#include <memory>
#include <random>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

uint64_t wordValue(uint64_t Value) {
  return uint64_t(int64_t(int32_t(uint32_t(Value))));
}

std::array<uint8_t, 4> encode(uint32_t Word, Arch Target) {
  std::array<uint8_t, 4> Bytes;
  for (unsigned I = 0; I != 4; ++I)
    Bytes[I] = Word >> (8 * (archLittleEndian(Target) ? I : 3 - I));
  return Bytes;
}

// Assemble from semantic pos/size, rather than decoding through production
// selectors/biases. The assembler's aliases choose a 64-bit encoding family.
uint32_t fieldWord(bool Insert, unsigned Bits, unsigned Pos, unsigned Size,
                   unsigned RS = 4, unsigned RT = 2) {
  unsigned Function = Insert ? 4 : 0;
  unsigned First = Pos;
  unsigned Last = Insert ? Pos + Size - 1 : Size - 1;
  if (Bits == 64) {
    if (Pos >= 32) {
      Function = Insert ? 6 : 2;
      First -= 32;
      if (Insert)
        Last -= 32;
    } else if ((Insert ? Pos + Size : Size) > 32) {
      Function = Insert ? 5 : 1;
      Last -= 32;
    } else {
      Function = Insert ? 7 : 3;
    }
  }
  return (31u << 26) | (RS << 21) | (RT << 16) | (Last << 11) | (First << 6) |
         Function;
}

uint64_t fieldResult(bool Insert, unsigned Bits, unsigned Pos, unsigned Size,
                     uint64_t Source, uint64_t Old) {
  uint64_t Result = 0;
  for (unsigned Bit = 0; Bit != Bits; ++Bit) {
    bool Set;
    if (Insert)
      Set = Bit >= Pos && Bit - Pos < Size ? (Source >> (Bit - Pos)) & 1
                                           : (Old >> Bit) & 1;
    else
      Set = Bit < Size && ((Source >> (Bit + Pos)) & 1);
    if (Set)
      Result |= uint64_t(1) << Bit;
  }
  return Bits == 32 ? wordValue(Result) : Result;
}

class MIPSBitfield : public ::testing::TestWithParam<Arch> {
protected:
  BinaryImage Image;
  Decoder Decode;
  void SetUp() override {
    Image.Arch = GetParam();
    Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
    ASSERT_TRUE(Decode.init(GetParam()));
  }
  std::vector<LowOp> lift(uint32_t Word) {
    const auto Bytes = encode(Word, GetParam());
    DecodedInsn Instruction{};
    const auto Size = Decode.decodeOneForLift(Bytes.data(), Bytes.size(),
                                              0x10000, Instruction);
    EXPECT_EQ(Size, 4) << std::hex << Word;
    std::vector<LowOp> Ops;
    if (Size == 4)
      Decode.liftToLow(Instruction, Ops);
    return Ops;
  }
  uint64_t targetValue(uint64_t Value) const {
    return isMIPS64(GetParam()) ? Value : uint32_t(Value);
  }
};

TEST_P(MIPSBitfield, EveryFieldPositionAndWidthHasIndependentExpectedBits) {
  std::mt19937_64 Random(97);
  const std::array<uint64_t, 6> Values = {
      0,        UINT64_MAX, 0x8000000080000000ULL, 0x0123456789abcdefULL,
      Random(), Random()};
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  for (unsigned Bits : {32u, 64u}) {
    if (Bits == 64 && !isMIPS64(GetParam()))
      continue;
    for (bool Insert : {false, true})
      for (unsigned Pos = 0; Pos != Bits; ++Pos)
        for (unsigned Size = 1; Size <= Bits - Pos; ++Size) {
          SCOPED_TRACE(::testing::Message()
                       << Bits << ':' << Insert << ':' << Pos << ':' << Size);
          const auto Ops = lift(fieldWord(Insert, Bits, Pos, Size));
          ASSERT_FALSE(Ops.empty());
          for (unsigned I = 0; I != Values.size(); ++I) {
            const auto Source = Bits == 32 ? wordValue(Values[I]) : Values[I];
            const auto Old = Bits == 32
                                 ? wordValue(Values[(I + 1) % Values.size()])
                                 : Values[(I + 1) % Values.size()];
            Emulator.setRegister(mipsreg::r(4), targetValue(Source));
            Emulator.setRegister(mipsreg::V0, targetValue(Old));
            for (const auto &Op : Ops)
              ASSERT_TRUE(Emulator.step(Op));
            EXPECT_EQ(
                Emulator.getRegister(mipsreg::V0),
                targetValue(fieldResult(Insert, Bits, Pos, Size, Source, Old)));
          }
        }
  }
  EXPECT_FALSE(Emulator.skips().any());
}

TEST_P(MIPSBitfield, AliasedAndZeroRegistersRetainTheirArchitecturalRoles) {
  for (unsigned Bits : {32u, 64u}) {
    if (Bits == 64 && !isMIPS64(GetParam()))
      continue;
    for (bool Insert : {false, true})
      for (const auto Registers :
           {std::pair{2u, 2u}, std::pair{0u, 2u}, std::pair{4u, 0u}}) {
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        const uint64_t Old = targetValue(wordValue(0x89abcdef));
        Emulator.setRegister(mipsreg::V0, Old);
        Emulator.setRegister(mipsreg::r(4), 0x12345678);
        const uint64_t Source = !Registers.first       ? 0
                                : Registers.first == 2 ? Old
                                                       : 0x12345678;
        const auto Ops = lift(fieldWord(Insert, Bits, 1, Bits - 2,
                                        Registers.first, Registers.second));
        for (const auto &Op : Ops) {
          EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == mipsreg::Zero);
          ASSERT_TRUE(Emulator.step(Op));
        }
        EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
                  Registers.second
                      ? targetValue(
                            fieldResult(Insert, Bits, 1, Bits - 2, Source, Old))
                      : Old);
      }
  }
}

TEST_P(MIPSBitfield, ShuffleLanesAndSignExtensionAreIndependentOfMemoryEndian) {
  struct Form {
    unsigned Function, SA, Bytes, Unit;
  };
  constexpr Form Forms[] = {
#define MIPS_SHUFFLE(Name, Function, SA, Bytes, Unit)                          \
  {Function, SA, Bytes, Unit},
#include "MIPSBitfieldFixtures.def"
  };
  for (const auto Form : Forms) {
    if (Form.Bytes == 8 && !isMIPS64(GetParam()))
      continue;
    for (uint64_t Input :
         std::array<uint64_t, 5>{0, UINT64_MAX, 0x00800000, 0x0123456789abcdef,
                                 0x807fff0180fe0180}) {
      if (Form.Bytes != 8)
        Input = wordValue(Input);
      uint64_t Expected = 0;
      if (!Form.Unit) {
        Expected = Form.Bytes == 1 ? uint64_t(int64_t(int8_t(Input)))
                                   : uint64_t(int64_t(int16_t(Input)));
      } else {
        for (unsigned Byte = 0; Byte != Form.Bytes; ++Byte) {
          const unsigned From =
              Form.Unit == 1 ? Byte ^ 1 : (3 - Byte / 2) * 2 + Byte % 2;
          Expected |= ((Input >> (From * 8)) & 255) << (Byte * 8);
        }
        if (Form.Bytes == 4)
          Expected = wordValue(Expected);
      }
      for (unsigned Destination : {0u, 2u, 4u}) {
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(mipsreg::r(4), targetValue(Input));
        const auto Ops = lift((31u << 26) | (4u << 16) | (Destination << 11) |
                              (Form.SA << 6) | Form.Function);
        for (const auto &Op : Ops) {
          EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == mipsreg::Zero);
          ASSERT_TRUE(Emulator.step(Op));
        }
        if (Destination)
          EXPECT_EQ(Emulator.getRegister(mipsreg::r(Destination)),
                    targetValue(Expected));
      }
    }
  }
}

TEST_P(MIPSBitfield, InvalidFieldsAndUnsupportedWidthNeverAppendEffects) {
  std::vector<uint32_t> Words = {
      (31u << 26) | (4u << 21) | (2u << 16) | (31u << 11) |
          (1u << 6),                                          // EXT overflow
      (31u << 26) | (4u << 21) | (2u << 16) | (1u << 6) | 4u, // INS reversed
      (31u << 26) | (1u << 21) | (4u << 16) | (2u << 11) | (2u << 6) | 0x20,
      (31u << 26) | (4u << 16) | (2u << 11) | (3u << 6) | 0x20,
      (31u << 26) | (4u << 21) | (2u << 16) | (31u << 11) | (1u << 6) | 1u,
      (31u << 26) | (4u << 21) | (2u << 16) | (1u << 6) | 6u,
      (31u << 26) | (4u << 21) | (2u << 16) | (1u << 6) | 7u,
      (31u << 26) | (1u << 21) | (4u << 16) | (2u << 11) | (2u << 6) | 0x24};
  if (!isMIPS64(GetParam())) {
    for (unsigned Function : {1u, 2u, 3u, 5u, 6u, 7u})
      Words.push_back((31u << 26) | (4u << 21) | (2u << 16) | Function);
    for (unsigned SA : {2u, 5u})
      Words.push_back((31u << 26) | (4u << 16) | (2u << 11) | (SA << 6) | 0x24);
  }
  MIPSLifter Lifter(GetParam());
  for (uint32_t Word : Words) {
    cs_insn Raw{};
    Raw.size = 4;
    Raw.address = 0x10000;
    const auto Bytes = encode(Word, GetParam());
    std::copy(Bytes.begin(), Bytes.end(), Raw.bytes);
    std::vector<LowOp> Ops(1);
    EXPECT_THROW(Lifter.lift(Raw, Ops), UnliftedInstruction)
        << std::hex << Word;
    EXPECT_EQ(Ops.size(), 1u);
    EXPECT_EQ(Ops[0].Opcode, NdOp::NOP);
  }
}

TEST_P(MIPSBitfield, EveryRawFieldEncodingIsAdmittedOrRefusedAtomically) {
  MIPSLifter Lifter(GetParam());
  for (unsigned Function = 0; Function != 8; ++Function)
    for (unsigned RD = 0; RD != 32; ++RD)
      for (unsigned SA = 0; SA != 32; ++SA) {
        // Independent raw encoding constraints from the ISA: extracting from
        // either end must fit; insertions cannot have their endpoints reversed.
        // DEXT permits a field crossing bit 31, and DINSM always ends above it.
        const bool Wide = Function != 0 && Function != 4;
        bool Valid = !Wide || isMIPS64(GetParam());
        switch (Function) {
        case 0:
        case 1:
        case 2:
          Valid &= RD + SA < 32;
          break;
        case 4:
        case 6:
        case 7:
          Valid &= RD >= SA;
          break;
        }
        const uint32_t Word = (31u << 26) | (4u << 21) | (2u << 16) |
                              (RD << 11) | (SA << 6) | Function;
        cs_insn Raw{};
        Raw.size = 4;
        Raw.address = 0x10000;
        const auto Bytes = encode(Word, GetParam());
        std::copy(Bytes.begin(), Bytes.end(), Raw.bytes);
        std::vector<LowOp> Ops(1);
        SCOPED_TRACE(::testing::Message() << std::hex << Word);
        if (Valid) {
          EXPECT_NO_THROW(Lifter.lift(Raw, Ops));
          EXPECT_GT(Ops.size(), 1u);
        } else {
          EXPECT_THROW(Lifter.lift(Raw, Ops), UnliftedInstruction);
          EXPECT_EQ(Ops.size(), 1u);
        }
        EXPECT_EQ(Ops[0].Opcode, NdOp::NOP);
      }
}

TEST_P(MIPSBitfield, DefinedBoundaryInstructionsMatchPinnedUnicorn) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  const bool Wide = isMIPS64(GetParam());
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    static_cast<uc_mode>(
                        (Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                        (archLittleEndian(GetParam()) ? UC_MODE_LITTLE_ENDIAN
                                                      : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, Wide ? int(UC_CPU_MIPS64_MIPS64R2_GENERIC)
                                           : int(UC_CPU_MIPS32_24KC)),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  std::vector<uint32_t> Words;
  for (unsigned Bits : {32u, 64u}) {
    if (Bits == 64 && !Wide)
      continue;
    for (bool Insert : {false, true})
      for (unsigned Pos : {0u, 1u, 15u, 31u, Bits - 1})
        for (unsigned Size : {1u, Bits - Pos})
          Words.push_back(fieldWord(Insert, Bits, Pos, Size));
  }
#define MIPS_SHUFFLE(Name, Function, SA, Bytes, Unit)                          \
  if (Bytes != 8 || Wide)                                                      \
    Words.push_back((31u << 26) | (4u << 16) | (2u << 11) | (SA << 6) |        \
                    Function);
#include "MIPSBitfieldFixtures.def"
  // Distinct addresses avoid a stale translated block after code replacement.
  for (unsigned I = 0; I != Words.size(); ++I) {
    const auto Bytes = encode(Words[I], GetParam());
    const uint64_t Address = 0x10000 + I * 4;
    ASSERT_EQ(uc_mem_write(Raw, Address, Bytes.data(), Bytes.size()),
              UC_ERR_OK);
    for (uint64_t Value :
         std::array<uint64_t, 3>{UINT64_MAX, 0x00800000, 0xffffffff89abcdef}) {
      Value = targetValue(Value);
      uint64_t Old = targetValue(wordValue(0x87654321));
      uint32_t WordValue = Value, WordOld = Old;
      ASSERT_EQ(
          uc_reg_write(Raw, UC_MIPS_REG_4,
                       Wide ? static_cast<const void *>(&Value) : &WordValue),
          UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_2,
                             Wide ? static_cast<const void *>(&Old) : &WordOld),
                UC_ERR_OK);
      ASSERT_EQ(uc_emu_start(Raw, Address, Address + 4, 0, 1), UC_ERR_OK);
      uint64_t Expected = 0;
      uint32_t WordExpected = 0;
      ASSERT_EQ(
          uc_reg_read(Raw, UC_MIPS_REG_2,
                      Wide ? static_cast<void *>(&Expected) : &WordExpected),
          UC_ERR_OK);
      if (!Wide)
        Expected = WordExpected;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(mipsreg::r(4), Value);
      Emulator.setRegister(mipsreg::V0, Old);
      for (const auto &Op : lift(Words[I]))
        ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected)
          << std::hex << Words[I];
    }
  }
#endif
}

TEST(MIPS64WordInputs, InvalidWordInputsAreRefusedBeforeObservableWrites) {
  struct Form {
    const char *Name;
    uint32_t Word;
    unsigned First, Second;
  };
  constexpr Form Forms[] = {
#define MIPS_WORD_INPUT(Name, Word, First, Second) {#Name, Word, First, Second},
#include "MIPSWordInputFixtures.def"
  };
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Bitness::Bits64;
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (const auto Form : Forms) {
      SCOPED_TRACE(Form.Name);
      const auto Bytes = encode(Form.Word, Target);
      DecodedInsn Instruction{};
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000,
                                        Instruction),
                4);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      for (unsigned Register : {Form.First, Form.Second}) {
        if (!Register)
          continue;
        for (uint64_t Bad :
             {0x100000001ULL, 0x80000000ULL, 0xffffffff00000001ULL}) {
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegister(mipsreg::r(4), 1);
          Emulator.setRegister(mipsreg::r(5), 1);
          Emulator.setRegister(mipsreg::V0, 0x55);
          Emulator.setRegister(mipsreg::HI, 0x66);
          Emulator.setRegister(mipsreg::LO, 0x77);
          Emulator.setRegister(mipsreg::r(Register), Bad);
          bool Accepted = true;
          for (const auto &Op : Ops)
            if (!Emulator.step(Op)) {
              Accepted = false;
              break;
            }
          EXPECT_FALSE(Accepted);
          EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
                    Register == 2 ? Bad : 0x55);
          EXPECT_EQ(Emulator.getRegister(mipsreg::HI), 0x66u);
          EXPECT_EQ(Emulator.getRegister(mipsreg::LO), 0x77u);
        }
      }
    }
  }
}

TEST(MIPS64WordInputs, LeftShiftAndVariableShiftCountsAcceptArbitraryHighBits) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Bitness::Bits64;
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (const auto Case : {std::pair{0x00041000u, wordValue(0x89abcdef)},
                            std::pair{0x00a41004u, wordValue(0x13579bde)},
                            std::pair{0x00a41006u, uint64_t(0x44d5e6f7)},
                            std::pair{0x00a41007u, wordValue(0xc4d5e6f7)},
                            std::pair{0x00a41046u, wordValue(0xc4d5e6f7)}}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      const bool Left = (Case.first & 3) == 0;
      Emulator.setRegister(mipsreg::r(4), Left ? 0x0123456789abcdefULL
                                               : wordValue(0x89abcdef));
      Emulator.setRegister(mipsreg::r(5), 0x100000001ULL);
      const auto Bytes = encode(Case.first, Target);
      DecodedInsn Instruction{};
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000,
                                        Instruction),
                4);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Case.second);
    }
  }
}

TEST(MIPS64WordInputs, ZeroDestinationDoesNotDiscardStrictWordChecks) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Bitness::Bits64;
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (uint32_t Word :
         {0x24800001u, 0x70850002u, 0x00040042u, 0x7c803800u, 0x7c0400a0u}) {
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(mipsreg::r(4), 0x100000001ULL);
      Emulator.setRegister(mipsreg::r(5), 1);
      const auto Bytes = encode(Word, Target);
      DecodedInsn Instruction{};
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000,
                                        Instruction),
                4);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      bool Accepted = true;
      for (const auto &Op : Ops) {
        EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == mipsreg::Zero);
        if (!Emulator.step(Op)) {
          Accepted = false;
          break;
        }
      }
      EXPECT_FALSE(Accepted);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSBitfield,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
