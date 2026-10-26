//===- LowScalarConstantTests.cpp - Exact LowIR arithmetic facts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare bounded scalar facts with concrete fixed-width execution.

#include "gtest/gtest.h"

#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/NdOpEmulator.h"

namespace {
using namespace neverd;

std::optional<uint64_t> constant(const NdVar &Value) {
  return Value.isConst() ? std::optional<uint64_t>{Value.Offset} : std::nullopt;
}

LowOp binary(NdOp Opcode, uint16_t Width, uint64_t Left, uint64_t Right) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::reg(0, Width);
  Op.NumInputs = 2;
  Op.Inputs[0] = NdVar::cst(Left, Width);
  Op.Inputs[1] = NdVar::cst(Right, Width);
  return Op;
}

TEST(LowScalarConstant, MultiplicationMatchesConcreteFixedWidthExecution) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (uint64_t Left : {uint64_t{0}, uint64_t{1}, uint64_t{4},
                          uint64_t{0x80000000}, UINT64_MAX})
      for (uint64_t Right : {uint64_t{0}, uint64_t{1}, uint64_t{3},
                             uint64_t{0x80000000}, UINT64_MAX}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << Left << ':' << Right);
        const auto Op = binary(NdOp::INT_MULT, Width, Left, Right);
        NdOpEmulator Emulator(BinaryImage{});
        ASSERT_TRUE(Emulator.step(Op));
        const auto Expected = Emulator.getRegister(0);
        ASSERT_TRUE(Expected);
        const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
        EXPECT_EQ(lowScalarConstantResult(Op, constant), *Expected & Mask);
      }
}

TEST(LowScalarConstant, ShiftsMatchDeclaredWidthAndSaturatingCounts) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    const uint64_t Bits = Width * 8;
    const uint64_t Mask = UINT64_MAX >> (64 - Bits);
    for (NdOp Opcode : {NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR})
      for (uint64_t Value : {uint64_t{0}, uint64_t{1}, uint64_t{4},
                             uint64_t{1} << (Bits - 1), Mask, UINT64_MAX})
        for (uint64_t Count :
             {uint64_t{0}, uint64_t{1}, Bits - 1, Bits, Bits + 1, UINT64_MAX}) {
          SCOPED_TRACE(::testing::Message() << Width << ':' << unsigned(Opcode)
                                            << ':' << Value << ':' << Count);
          const auto Op = binary(Opcode, Width, Value, Count);
          NdOpEmulator Emulator(BinaryImage{});
          ASSERT_TRUE(Emulator.step(Op));
          const auto Expected = Emulator.getRegister(0);
          ASSERT_TRUE(Expected);
          EXPECT_EQ(lowScalarConstantResult(Op, constant), *Expected & Mask);
        }
  }
  EXPECT_EQ(lowScalarConstantResult(binary(NdOp::INT_LEFT, 4, 4, 0), constant),
            4u);
  EXPECT_EQ(
      lowScalarConstantResult(binary(NdOp::INT_RIGHT, 4, 4, 32), constant), 0u);
  EXPECT_EQ(lowScalarConstantResult(binary(NdOp::INT_ASHR, 4, 0x80000000, 32),
                                    constant),
            UINT32_MAX);
}

TEST(LowScalarConstant, UnknownOperandsAndMalformedWidthsStayUnknown) {
  for (NdOp Opcode :
       {NdOp::INT_MULT, NdOp::INT_LEFT, NdOp::INT_RIGHT, NdOp::INT_ASHR})
    for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << unsigned(Opcode) << ':' << Mutation);
      auto Op = binary(Opcode, 4, 4, 0);
      switch (Mutation) {
      case 0:
        Op.Inputs[0] = NdVar::reg(8, 4);
        break;
      case 1:
        Op.Inputs[1] = NdVar::reg(8, 4);
        break;
      case 2:
        Op.Inputs[0].Size = 2;
        break;
      case 3:
        Op.Inputs[1].Size = 8;
        break;
      case 4:
        Op.Output.Size = 0;
        break;
      case 5:
        Op.Output.Size = 16;
        break;
      case 6:
        Op.NumInputs = 1;
        break;
      case 7:
        Op.Output = NdVar::cst(0, 4);
        break;
      }
      EXPECT_FALSE(lowScalarConstantResult(Op, constant));
    }
}

TEST(LowScalarConstant, BitwiseAbsorbersMatchEveryConcreteByte) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (NdOp Opcode : {NdOp::INT_AND, NdOp::INT_OR})
      for (bool Reverse : {false, true}) {
        const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
        const uint64_t Absorber = Opcode == NdOp::INT_AND ? 0 : Mask;
        auto Op = binary(Opcode, Width, Absorber, Absorber);
        Op.Inputs[Reverse ? 0 : 1] = NdVar::reg(8, Width);
        EXPECT_EQ(lowScalarConstantResult(Op, constant), Absorber);
        // The query knows nothing about the register. Independently execute
        // all byte patterns replicated across the declared operand width.
        for (uint64_t Byte = 0; Byte != 256; ++Byte) {
          NdOpEmulator Emulator(BinaryImage{});
          Emulator.setRegister(8, Byte * UINT64_C(0x0101010101010101));
          ASSERT_TRUE(Emulator.step(Op));
          const auto Value = Emulator.getRegister(0);
          ASSERT_TRUE(Value);
          EXPECT_EQ(*Value & Mask, Absorber);
        }
      }
}

TEST(LowScalarConstant, BitwisePartialMasksAndMalformedWidthsStayUnknown) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (NdOp Opcode : {NdOp::INT_AND, NdOp::INT_OR})
      for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
        const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
        auto Op = binary(Opcode, Width, 0, Opcode == NdOp::INT_AND ? 0 : Mask);
        Op.Inputs[0] = NdVar::reg(8, Width);
        if (Mutation == 0)
          Op.Inputs[1].Offset = Opcode == NdOp::INT_AND ? Mask : 0;
        if (Mutation == 1)
          Op.Inputs[1].Offset = 1;
        if (Mutation == 2)
          Op.Inputs[0].Size = Width == 8 ? 4 : 8;
        if (Mutation == 3)
          Op.Inputs[1].Size = Width == 8 ? 4 : 8;
        if (Mutation == 4)
          Op.Output.Size = 0;
        EXPECT_FALSE(lowScalarConstantResult(Op, constant));
      }
}

TEST(LowScalarConstant, BitwiseNotMatchesConcreteFixedWidthExecution) {
  for (uint16_t Width = 1; Width <= 8; ++Width)
    for (uint64_t Byte = 0; Byte != 256; ++Byte) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Byte);
      LowOp Op;
      Op.Opcode = NdOp::INT_NOT;
      Op.Output = NdVar::reg(0, Width);
      // High carrier bits must not affect the declared result width.
      Op.addInput(NdVar::cst(Byte * UINT64_C(0x0101010101010101), Width));
      NdOpEmulator Emulator(BinaryImage{});
      ASSERT_TRUE(Emulator.step(Op));
      const auto Expected = Emulator.getRegister(0);
      ASSERT_TRUE(Expected);
      const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
      EXPECT_EQ(lowScalarConstantResult(Op, constant), *Expected & Mask);
    }
}

TEST(LowScalarConstant, BitwiseNotRejectsUnknownAndMalformedWidths) {
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowOp Op;
    Op.Opcode = NdOp::INT_NOT;
    Op.Output = NdVar::reg(0, 4);
    Op.addInput(NdVar::cst(1, 4));
    switch (Mutation) {
    case 0:
      Op.Inputs[0] = NdVar::reg(8, 4);
      break;
    case 1:
      Op.Inputs[0].Size = 0;
      break;
    case 2:
      Op.Inputs[0].Size = 2;
      break;
    case 3:
      Op.Inputs[0].Size = 8;
      break;
    case 4:
      Op.Output.Size = 0;
      break;
    case 5:
      Op.Output.Size = 16;
      break;
    case 6:
      Op.NumInputs = 0;
      break;
    case 7:
      Op.addInput(NdVar::cst(1, 4));
      break;
    case 8:
      Op.Output = NdVar::cst(0, 4);
      break;
    }
    EXPECT_FALSE(lowScalarConstantResult(Op, constant));
  }
}
} // namespace
