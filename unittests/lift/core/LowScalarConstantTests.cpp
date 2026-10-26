//===- LowScalarConstantTests.cpp - Exact LowIR arithmetic facts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare bounded scalar facts with concrete fixed-width execution.

#include "gtest/gtest.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <utility>
#include <vector>

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

LowOp unary(NdOp Opcode, uint16_t Width, uint64_t Value, uint16_t OutputWidth) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::reg(0, OutputWidth);
  Op.NumInputs = 1;
  Op.Inputs[0] = NdVar::cst(Value, Width);
  return Op;
}

TEST(LowScalarConstant, UnaryResultsMatchConcreteFixedWidthExecution) {
  for (uint16_t Width : {1, 2, 4, 8}) {
    const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
    for (NdOp Opcode : {NdOp::INT_NOT, NdOp::INT_NEGATE, NdOp::INT_NEG2})
      for (uint64_t Value :
           {uint64_t{0}, uint64_t{1}, Mask >> 1, uint64_t{1} << (Width * 8 - 1),
            Mask, UINT64_MAX}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << unsigned(Opcode) << ':' << Value);
        const auto Op = unary(Opcode, Width, Value, Width);
        NdOpEmulator Emulator(BinaryImage{});
        ASSERT_TRUE(Emulator.step(Op));
        const auto Expected = Emulator.getRegister(0);
        ASSERT_TRUE(Expected);
        EXPECT_EQ(lowScalarConstantResult(Op, constant), *Expected & Mask);
      }
  }
}

TEST(LowScalarConstant, SignExtensionMatchesConcreteExecution) {
  for (uint16_t Width : {1, 2, 4, 8})
    for (uint16_t OutputWidth : {1, 2, 4, 8}) {
      if (OutputWidth < Width)
        continue;
      const uint64_t Mask = UINT64_MAX >> ((8 - Width) * 8);
      const uint64_t OutputMask = UINT64_MAX >> ((8 - OutputWidth) * 8);
      for (uint64_t Value :
           {uint64_t{0}, uint64_t{1}, Mask >> 1, uint64_t{1} << (Width * 8 - 1),
            Mask, UINT64_MAX}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << ':' << OutputWidth << ':' << Value);
        const auto Op = unary(NdOp::INT_SEXT, Width, Value, OutputWidth);
        NdOpEmulator Emulator(BinaryImage{});
        ASSERT_TRUE(Emulator.step(Op));
        const auto Expected = Emulator.getRegister(0);
        ASSERT_TRUE(Expected);
        EXPECT_EQ(lowScalarConstantResult(Op, constant),
                  *Expected & OutputMask);
      }
    }
}

TEST(LowScalarConstant, InvalidUnaryOperationsNeverCreateFacts) {
  for (NdOp Opcode :
       {NdOp::INT_NOT, NdOp::INT_NEGATE, NdOp::INT_NEG2, NdOp::INT_SEXT})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Op = unary(Opcode, 4, UINT32_MAX, 4);
      switch (Mutation) {
      case 0:
        Op.Inputs[0] = NdVar::reg(8, 4);
        break;
      case 1:
        Op.Inputs[0].Size = 0;
        break;
      case 2:
        Op.Inputs[0].Size = 8;
        break;
      case 3:
        Op.Output.Size = 0;
        break;
      case 4:
        Op.Output.Size = 16;
        break;
      case 5:
        Op.NumInputs = 2;
        break;
      case 6:
        Op.Output = NdVar::cst(0, 4);
        break;
      }
      EXPECT_FALSE(lowScalarConstantResult(Op, constant));
    }
  for (NdOp Opcode : {NdOp::INT_NOT, NdOp::INT_NEGATE, NdOp::INT_NEG2})
    EXPECT_FALSE(lowScalarConstantResult(unary(Opcode, 4, 1, 8), constant));
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
LowFunc registerCall(std::vector<LowOp> Setup, NdVar Target) {
  LowFunc Function;
  Function.Blocks.emplace_back();
  auto &Operations = Function.Blocks.back().Ops;
  Operations = std::move(Setup);
  for (size_t I = 0; I < Operations.size(); ++I) {
    Operations[I].Addr = 0x1000 + I;
    Operations[I].Seq = 0;
  }
  LowOp Call;
  Call.Opcode = NdOp::INDIR_CALL;
  Call.Addr = 0x2000;
  Call.Seq = 1;
  Call.addInput(Target);
  Operations.push_back(Call);
  return Function;
}

TEST(ConstantRegisterCall, ExactArithmeticRetainsTheOriginalOccurrence) {
  auto Seed = unary(NdOp::COPY, 8, 0x140001000, 8);
  auto Add = binary(NdOp::INT_ADD, 8, 0, 0x40);
  Add.Inputs[0] = Seed.Output;
  const auto Function = registerCall({Seed, Add}, Add.Output);
  const auto Targets = constantRegisterCallTargets(Function);
  ASSERT_EQ(Targets.size(), 1u);
  EXPECT_EQ(Targets.begin()->second, 0x140001040u);
  EXPECT_EQ(Targets.begin()->first,
            sourceCallOccurrenceKey(Function.Blocks.front().Ops.back()));
  EXPECT_FALSE(Targets.begin()->first.StaticTarget);
  EXPECT_EQ(Function.Blocks.front().Ops.back().Opcode, NdOp::INDIR_CALL);
}

TEST(ConstantRegisterCall, UnknownAndPartialWritesNeverRetainAStaleTarget) {
  const auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  for (const auto Opcode : {NdOp::COPY, NdOp::LOAD})
    for (const uint16_t Width : {1, 2, 4, 8, 16}) {
      SCOPED_TRACE(::testing::Message() << unsigned(Opcode) << ':' << Width);
      LowOp Overwrite;
      Overwrite.Opcode = Opcode;
      Overwrite.Output = NdVar::reg(0, Width);
      Overwrite.addInput(NdVar::reg(32, Width));
      EXPECT_TRUE(constantRegisterCallTargets(
                      registerCall({Seed, Overwrite}, Seed.Output))
                      .empty());
    }
  // Knowing only the low four bytes cannot identify an eight-byte target.
  auto Narrow = Seed;
  Narrow.Output.Size = Narrow.Inputs[0].Size = 4;
  EXPECT_TRUE(
      constantRegisterCallTargets(registerCall({Narrow}, Seed.Output)).empty());
}

TEST(ConstantRegisterCall, CallsAndOpaqueEffectsKillEarlierRegisterFacts) {
  const auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  for (const auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INTRINSIC}) {
    LowOp Effect;
    Effect.Opcode = Opcode;
    Effect.addInput(NdVar::cst(0x3000, 8));
    EXPECT_TRUE(
        constantRegisterCallTargets(registerCall({Seed, Effect}, Seed.Output))
            .empty());
  }
  auto Function = registerCall({Seed}, Seed.Output);
  const auto Call = Function.Blocks.front().Ops.back();
  Function.Blocks.front().Ops.pop_back();
  Function.Blocks.emplace_back();
  Function.Blocks.back().Ops.push_back(Call);
  EXPECT_TRUE(constantRegisterCallTargets(Function).empty());
}

TEST(ConstantRegisterCall, MemorySlotsAndReusedTemporariesAreNotCodeValues) {
  const auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  EXPECT_TRUE(constantRegisterCallTargets(
                  registerCall({Seed}, NdVar::cst(0x140001040, 8)))
                  .empty());
  auto Temp = Seed;
  Temp.Output = NdVar::tmp(0x10000, 8);
  EXPECT_TRUE(
      constantRegisterCallTargets(registerCall({Temp}, Temp.Output)).empty());
  auto SameInstruction = registerCall({Temp}, Temp.Output);
  SameInstruction.Blocks.front().Ops.front().Addr = 0x2000;
  EXPECT_EQ(constantRegisterCallTargets(SameInstruction).size(), 1u);
}

TEST(ConstantRegisterCall, ExhaustedWorkCannotPublishAPrefix) {
  const auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  auto Function = registerCall({Seed}, Seed.Output);
  ASSERT_EQ(constantRegisterCallTargets(Function).size(), 1u);
  Function.Blocks.emplace_back();
  Function.Blocks.back().Ops.resize(
      limits::ir::kConstantRegisterCallOperations);
  EXPECT_TRUE(constantRegisterCallTargets(Function).empty());
}
} // namespace
