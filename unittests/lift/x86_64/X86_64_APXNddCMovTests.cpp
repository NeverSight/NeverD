//===- X86_64_APXNddCMovTests.cpp - APX NDD CMOV semantics ----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

constexpr std::array<unsigned, 16> kCMovIds = {
    X86_INS_CMOVO, X86_INS_CMOVNO, X86_INS_CMOVB,  X86_INS_CMOVAE,
    X86_INS_CMOVE, X86_INS_CMOVNE, X86_INS_CMOVBE, X86_INS_CMOVA,
    X86_INS_CMOVS, X86_INS_CMOVNS, X86_INS_CMOVP,  X86_INS_CMOVNP,
    X86_INS_CMOVL, X86_INS_CMOVGE, X86_INS_CMOVLE, X86_INS_CMOVG,
};

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

std::vector<uint8_t> encodeRegister(unsigned Condition, bool W, unsigned PP) {
  constexpr unsigned NDD = 30;
  constexpr unsigned Reg = 17;
  constexpr unsigned RM = 29;
  return {
      0x62,
      static_cast<uint8_t>(0x44 | ((Reg & 8) ? 0 : 0x80) |
                           ((Reg & 16) ? 0 : 0x10) | ((RM & 8) ? 0 : 0x20) |
                           ((RM & 16) ? 0x08 : 0)),
      static_cast<uint8_t>((W ? 0x80 : 0) | (((~NDD) & 15) << 3) | 0x04 |
                           (PP & 1)),
      0x10,
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      static_cast<uint8_t>(0xc0 | ((Reg & 7) << 3) | (RM & 7)),
  };
}

std::vector<uint8_t> encodeMemory(unsigned Condition, bool W, unsigned PP) {
  // CMOVcc r30, r17, [r20 + r29*4 - 16] (ND=1, NF=0).
  return {
      0x62,
      0xac,
      static_cast<uint8_t>((W ? 0x80 : 0) | 0x08 | (PP & 1)),
      0x10,
      static_cast<uint8_t>(0x40 | (Condition & 15)),
      0x4c,
      0xac,
      0xf0,
  };
}

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes,
                          Arch Target = Arch::X64) {
  Decoder Dec;
  if (!Dec.init(Target)) {
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

TEST(X86APXNddCMov, RegisterSourceSelectsExactOperandAndZeroesUpperBits) {
  constexpr uint64_t Initial = UINT64_C(0x0123456789abcdef);
  constexpr uint64_t FallbackValue = UINT64_C(0x8877665544332211);
  constexpr uint64_t ConditionalValue = UINT64_C(0xfedcba9876543210);
  const RegInfo Destination = mapCapstoneReg(X86_REG_R30);
  const RegInfo Fallback = mapCapstoneReg(X86_REG_R17);
  const RegInfo Conditional = mapCapstoneReg(X86_REG_R29);

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
      const LiftedInstruction Move =
          liftX64(encodeRegister(Condition, Width.W, Width.PP));
      SCOPED_TRACE(::testing::Message()
                   << "condition=" << Condition << " bytes=" << Width.Bytes);
      ASSERT_EQ(Move.Id, kCMovIds[Condition]);
      ASSERT_FALSE(Move.Ops.empty());

      for (unsigned Result = 0; Result < 2; ++Result) {
        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(Destination.Offset, Initial);
        Emulator.setRegister(Fallback.Offset, FallbackValue);
        Emulator.setRegister(Conditional.Offset, ConditionalValue);
        setFlags(Emulator, FlagCases[Result]);

        ASSERT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
        const uint64_t Mask = Width.Bytes == 8
                                  ? UINT64_MAX
                                  : (UINT64_C(1) << (Width.Bytes * 8)) - 1;
        const uint64_t Selected = Result ? ConditionalValue : FallbackValue;
        EXPECT_EQ(Emulator.getRegister(Destination.Offset), Selected & Mask);
        EXPECT_EQ(Emulator.getRegister(Fallback.Offset), FallbackValue);
        EXPECT_EQ(Emulator.getRegister(Conditional.Offset), ConditionalValue);
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
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86APXNddCMov, MemorySourceAlwaysReadsBeforeSelecting) {
  constexpr uint64_t Target = 0x9000;
  constexpr uint64_t Initial = UINT64_C(0x0123456789abcdef);
  constexpr uint64_t FallbackValue = UINT64_C(0xa8a7a6a5a4a3a2a1);
  constexpr uint64_t MemoryValue = UINT64_C(0x8877665544332211);
  const RegInfo Destination = mapCapstoneReg(X86_REG_R30);
  const RegInfo Fallback = mapCapstoneReg(X86_REG_R17);

  struct WidthCase {
    bool W;
    unsigned PP;
    unsigned Bytes;
  };
  constexpr std::array<WidthCase, 3> Widths = {
      WidthCase{false, 1, 2}, WidthCase{false, 0, 4}, WidthCase{true, 0, 8}};

  auto Initialize = [&](NdOpEmulator &Emulator, bool Condition) {
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(Destination.Offset, Initial);
    Emulator.setRegister(Fallback.Offset, FallbackValue);
    Emulator.setRegister(x86reg::R20, Target);
    Emulator.setRegister(x86reg::R29, 4);
    Emulator.setRegister(x86reg::CF, 1);
    Emulator.setRegister(x86reg::PF, 1);
    Emulator.setRegister(x86reg::ZF, Condition ? 1 : 0);
    Emulator.setRegister(x86reg::SF, 1);
    Emulator.setRegister(x86reg::OF, 0);
  };

  for (const WidthCase &Width : Widths) {
    SCOPED_TRACE(::testing::Message() << "bytes=" << Width.Bytes);
    const LiftedInstruction Move = liftX64(encodeMemory(4, Width.W, Width.PP));
    ASSERT_EQ(Move.Id, X86_INS_CMOVE);
    ASSERT_FALSE(Move.Ops.empty());
    const uint64_t Mask =
        Width.Bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (Width.Bytes * 8)) - 1;

    for (const bool Condition : {false, true}) {
      BinaryImage Mapped;
      Mapped.Arch = Arch::X64;
      Mapped.Bits = Bitness::Bits64;
      Segment Data;
      Data.VA = Target;
      Data.Size = 8;
      Data.Flags = SegmentFlags::Readable;
      Data.Data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
      Mapped.Segments.push_back(std::move(Data));
      NdOpEmulator Emulator(Mapped);
      Initialize(Emulator, Condition);

      ASSERT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
      EXPECT_EQ(Emulator.getRegister(Destination.Offset),
                (Condition ? MemoryValue : FallbackValue) & Mask);
      ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Target);
      EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Width.Bytes);
      EXPECT_EQ(Emulator.getRegister(Fallback.Offset), FallbackValue);
      EXPECT_EQ(Emulator.getRegister(x86reg::R20), Target);
      EXPECT_EQ(Emulator.getRegister(x86reg::R29), 4u);
      EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1u);
      EXPECT_EQ(Emulator.getRegister(x86reg::PF), 1u);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF), Condition ? 1u : 0u);
      EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1u);
      EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
      EXPECT_FALSE(Emulator.skips().any());
    }

    BinaryImage Unmapped;
    Unmapped.Arch = Arch::X64;
    Unmapped.Bits = Bitness::Bits64;
    NdOpEmulator FaultPath(Unmapped);
    Initialize(FaultPath, false);
    EXPECT_LT(FaultPath.run(Move.Ops), Move.Ops.size());
    EXPECT_EQ(FaultPath.getRegister(Destination.Offset), Initial);
    // The failed access proves that a false condition did not suppress the
    // source read. Load records contain only accesses that completed.
    EXPECT_TRUE(FaultPath.getLoadRecords().empty());
    EXPECT_EQ(FaultPath.getRegister(Fallback.Offset), FallbackValue);
    EXPECT_EQ(FaultPath.getRegister(x86reg::ZF), 0u);
    EXPECT_FALSE(FaultPath.skips().any());
  }
}

} // namespace

TEST(X86CMov, OrdinaryRegisterFormKeepsWidthAndAllFlagConditions) {
  for (const Arch Target : {Arch::X86, Arch::X64})
    for (const unsigned Bytes : {2u, 4u, 8u}) {
      if (Target == Arch::X86 && Bytes == 8)
        continue;
      const uint64_t Initial = Target == Arch::X64
                                   ? UINT64_C(0x8877665544332211)
                                   : UINT64_C(0x44332211);
      const uint64_t Source = Target == Arch::X64 ? UINT64_C(0xfedcba9876543210)
                                                  : UINT64_C(0x76543210);
      const uint64_t Mask =
          Bytes == 8 ? UINT64_MAX : (UINT64_C(1) << (Bytes * 8)) - 1;
      for (unsigned Condition = 0; Condition < 16; ++Condition) {
        std::vector<uint8_t> Code;
        if (Bytes == 2)
          Code.push_back(0x66);
        if (Bytes == 8)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x0f, uint8_t(0x40 + Condition), 0xc2});
        const auto Move = liftX64(Code, Target);
        ASSERT_EQ(Move.Id, kCMovIds[Condition]);
        EXPECT_EQ(std::count_if(Move.Ops.begin(), Move.Ops.end(),
                                [](const LowOp &Op) {
                                  return Op.Opcode == NdOp::SELECT;
                                }),
                  1);

        for (unsigned Flags = 0; Flags < 32; ++Flags) {
          SCOPED_TRACE(::testing::Message()
                       << "arch=" << unsigned(Target) << " bytes=" << Bytes
                       << " condition=" << Condition << " flags=" << Flags);
          BinaryImage Image;
          Image.Arch = Target;
          Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegister(x86reg::RAX, Initial);
          Emulator.setRegister(x86reg::RDX, Source);
          setFlags(Emulator, Flags);
          Emulator.setRegister(x86reg::AF, 1);
          Emulator.setRegister(x86reg::DF, 1);
          ASSERT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
          const auto Selected =
              conditionResult(Condition, Flags) ? Source : Initial;
          EXPECT_EQ(Emulator.getRegister(x86reg::RAX),
                    (Selected & Mask) | (Bytes == 2 ? Initial & ~Mask : 0));
          EXPECT_EQ(Emulator.getRegister(x86reg::RDX), Source);
          for (const auto &[Register, Bit] :
               {std::pair{x86reg::CF, 0u}, std::pair{x86reg::PF, 1u},
                std::pair{x86reg::ZF, 2u}, std::pair{x86reg::SF, 3u},
                std::pair{x86reg::OF, 4u}})
            EXPECT_EQ(Emulator.getRegister(Register), (Flags >> Bit) & 1);
          EXPECT_EQ(Emulator.getRegister(x86reg::AF), 1u);
          EXPECT_EQ(Emulator.getRegister(x86reg::DF), 1u);
          EXPECT_FALSE(Emulator.skips().any());
        }
      }
    }
}

TEST(X86CMov, OrdinaryFalseConditionStillReadsAndCanFault) {
  constexpr va_t Address = 0x9000;
  constexpr uint64_t Initial = UINT64_C(0x8877665544332211);
  for (const unsigned Bytes : {2u, 4u, 8u}) {
    std::vector<uint8_t> Code;
    if (Bytes == 2)
      Code.push_back(0x66);
    if (Bytes == 8)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x0f, 0x44, 0x02}); // cmove ax/eax/rax,[rdx]
    const auto Move = liftX64(Code);
    ASSERT_EQ(Move.Id, X86_INS_CMOVE);
    for (const bool Mapped : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "bytes=" << Bytes << " mapped=" << Mapped);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      if (Mapped) {
        Segment Data;
        Data.VA = Address;
        Data.Size = 8;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(8, 0xa5);
        Image.Segments.push_back(std::move(Data));
      }
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setLoadCollect(true);
      Emulator.setRegister(x86reg::RAX, Initial);
      Emulator.setRegister(x86reg::RDX, Address);
      setFlags(Emulator, 0);
      if (Mapped) {
        ASSERT_EQ(Emulator.run(Move.Ops), Move.Ops.size());
        ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Address);
        EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Bytes);
        EXPECT_EQ(Emulator.getRegister(x86reg::RAX),
                  Bytes == 4 ? Initial & UINT32_MAX : Initial);
      } else {
        EXPECT_LT(Emulator.run(Move.Ops), Move.Ops.size());
        EXPECT_TRUE(Emulator.getLoadRecords().empty());
        EXPECT_EQ(Emulator.getRegister(x86reg::RAX), Initial);
      }
      EXPECT_EQ(Emulator.getRegister(x86reg::RDX), Address);
      EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
      EXPECT_FALSE(Emulator.skips().any());
    }
  }
}
