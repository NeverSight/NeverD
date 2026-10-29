//===- LowScalarConstantTests.cpp - Exact LowIR arithmetic facts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare bounded scalar facts with concrete fixed-width execution.

#include "gtest/gtest.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CallRegisterBody.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/NdOpEmulator.h"

#include <array>
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

TEST(LowScalarConstant, SelfDifferenceDoesNotNeedAnIncomingConstant) {
  for (uint16_t Width = 1; Width <= sizeof(uint64_t); ++Width)
    for (bool Temporary : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Width << ':' << Temporary);
      auto Op = binary(NdOp::INT_SUB, Width, 0, 0);
      Op.Inputs[0] = Op.Inputs[1] =
          Temporary ? NdVar::tmp(8, Width) : NdVar::reg(8, Width);
      EXPECT_EQ(lowScalarConstantResult(Op, constant), 0u);
      for (uint64_t Byte = 0; Byte != 256; ++Byte) {
        NdOpEmulator Emulator(BinaryImage{});
        auto Seed = unary(NdOp::COPY, Width,
                          Byte * UINT64_C(0x0101010101010101), Width);
        Seed.Output = Op.Inputs[0];
        ASSERT_TRUE(Emulator.step(Seed));
        ASSERT_TRUE(Emulator.step(Op));
        EXPECT_EQ(Emulator.getRegister(0), 0u);
      }
    }
}

TEST(LowScalarConstant, SelfDifferenceRequiresTheSameScalarOccurrence) {
  for (bool Temporary : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Temporary << ':' << Mutation);
      auto Op = binary(NdOp::INT_SUB, 4, 0, 0);
      Op.Inputs[0] = Op.Inputs[1] =
          Temporary ? NdVar::tmp(8, 4) : NdVar::reg(8, 4);
      switch (Mutation) {
      case 0:
        ++Op.Inputs[1].Offset;
        break;
      case 1:
        Op.Inputs[1] = Temporary ? NdVar::reg(8, 4) : NdVar::tmp(8, 4);
        break;
      case 2:
        Op.Inputs[1].Size = 2;
        break;
      case 3:
        Op.Output.Size = 8;
        break;
      case 4:
        Op.Output.Size = Op.Inputs[0].Size = Op.Inputs[1].Size = 16;
        break;
      case 5:
        Op.NumInputs = 1;
        break;
      case 6:
        Op.Inputs[0] = Op.Inputs[1] = NdVar::ram(8, 4);
        break;
      }
      EXPECT_FALSE(lowScalarConstantResult(Op, constant));
    }
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
        Op.Inputs[0].Size = 0;
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

TEST(LowScalarConstant, NarrowImmediatesAreZeroExtendedBeforeArithmetic) {
  for (NdOp Code : {NdOp::INT_ADD, NdOp::INT_SUB, NdOp::INT_MULT, NdOp::INT_AND,
                    NdOp::INT_OR, NdOp::INT_XOR, NdOp::INT_LEFT,
                    NdOp::INT_RIGHT, NdOp::INT_ASHR})
    for (uint16_t Width : {1, 2, 4}) {
      auto Op = binary(Code, 8, 0x123456789abc, UINT64_MAX);
      Op.Inputs[1].Size = Width;
      NdOpEmulator Emulator(BinaryImage{});
      ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(lowScalarConstantResult(Op, constant), Emulator.getRegister(0));
    }
  auto Op = binary(NdOp::INT_ADD, 8, 1, UINT64_MAX);
  Op.Inputs[1].Size = 1;
  EXPECT_EQ(lowScalarConstantResult(Op, constant), 256u);
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
          Op.Inputs[1].Size = 0;
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

LowFunc directEdgeCall() {
  auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  auto F = registerCall({Seed}, Seed.Output);
  auto &Pred = F.Blocks.front();
  auto Call = Pred.Ops.back();
  Pred.Ops.pop_back();
  Pred.Id = 0;
  Pred.StartAddr = F.Entry = 0x1000;
  Pred.EndAddr = 0x1003;
  Pred.Succs = {1};
  LowOp Jump;
  Jump.Opcode = NdOp::BRANCH;
  Jump.Addr = 0x1001;
  Jump.addInput(NdVar::cst(0x2000, 8));
  Pred.Ops.push_back(Jump);
  LowInstructionBoundary Edge;
  Edge.Address = Jump.Addr;
  Edge.Size = 2;
  Edge.FirstOp = 1;
  Edge.OpCount = 1;
  Edge.Control = LowInstructionControl::Branch;
  Edge.ControlFlags = LowInstructionControlFlag::Branch;
  Edge.Immediate = 0x2000;
  Pred.InstructionBoundaries.push_back(Edge);
  F.Blocks.emplace_back();
  auto &Current = F.Blocks.back();
  Current.Id = 1;
  Current.StartAddr = Call.Addr;
  Current.EndAddr = Call.Addr + 2;
  Current.Preds = {0};
  Current.Ops = {Call};
  F.DecodedInstructionCount = F.LiftedInstructionCount = 3;
  return F;
}

TEST(ConstantRegisterCall, DirectPredecessorsAndCachedBodiesShareTheProof) {
  auto F = directEdgeCall();
  const auto Expected = constantRegisterCallTargets(F);
  ASSERT_EQ(Expected.size(), 1u);
  EXPECT_EQ(Expected.begin()->second, 0x140001040u);
  EXPECT_EQ(Expected.begin()->first,
            sourceCallOccurrenceKey(F.Blocks.back().Ops.front()));
  CallRegisterBody Cached;
  Cached.Blocks = F.Blocks;
  Cached.Complete = true;
  EXPECT_EQ(constantRegisterCallTargets(CallRegisterBodyView(Cached)),
            Expected);
  // AL and other physical register queries still use only their call block.
  const auto Value =
      constantRegisterValueAtCall(F, Expected.begin()->first, NdVar::reg(0, 8));
  EXPECT_TRUE(Value.OccurrenceFound);
  EXPECT_FALSE(Value.Value);
  auto &Edge = F.Blocks.front().InstructionBoundaries.back();
  Edge.Control = LowInstructionControl::None;
  Edge.ControlFlags = LowInstructionControlFlag::None;
  Edge.Immediate.reset();
  F.Blocks.front().EndAddr = F.Blocks.back().StartAddr;
  Edge.Address = F.Blocks.front().EndAddr - Edge.Size;
  F.Blocks.front().Ops.back().Addr = Edge.Address;
  EXPECT_EQ(constantRegisterCallTargets(F), Expected);
}

TEST(ConstantRegisterCall,
     PredecessorProofRejectsAlternateStateAndStaleValues) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = directEdgeCall();
    auto &Pred = F.Blocks.front();
    auto &Call = F.Blocks.back();
    if (Mutation == 0)
      F.ModuleAnalysisRoots.insert(Call.StartAddr);
    if (Mutation == 1)
      F.TruncatedPathAddresses.push_back(Call.StartAddr);
    if (Mutation == 2)
      Call.Preds.push_back(7);
    if (Mutation == 3)
      Pred.Succs.push_back(7);
    if (Mutation == 4)
      Pred.ExceptionalSuccs.emplace_back();
    if (Mutation == 5)
      Call.ExceptionalPreds.emplace_back();
    if (Mutation == 6)
      Pred.InstructionBoundaries.back().Immediate = 0x3000;
    if (Mutation == 7)
      Pred.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Indirect;
    if (Mutation == 8)
      Pred.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    if (Mutation == 9) {
      auto Unknown = unary(NdOp::COPY, 4, 0, 4);
      Unknown.Inputs[0] = NdVar::reg(32, 4);
      Unknown.Addr = 0x1001;
      Pred.Ops.insert(Pred.Ops.begin() + 1, Unknown);
      Pred.InstructionBoundaries.back().FirstOp = 2;
    }
    if (Mutation == 10) {
      auto Effect = Pred.Ops.back();
      Effect.Opcode = NdOp::CALL;
      Pred.Ops.insert(Pred.Ops.begin() + 1, Effect);
      Pred.InstructionBoundaries.back().FirstOp = 2;
    }
    if (Mutation == 11)
      Pred.InstructionBoundaries.back().Size = 0;
    if (Mutation == 12)
      Pred.InstructionBoundaries.back().Address = Pred.EndAddr;
    if (Mutation == 13)
      Pred.InstructionBoundaries.back().FirstOp = Pred.Ops.size() + 1;
    if (Mutation == 14)
      Pred.InstructionBoundaries.back().OpCount = 0;
    EXPECT_TRUE(constantRegisterCallTargets(F).empty());
  }
  auto F = directEdgeCall();
  F.Blocks.front().Preds = {1};
  F.Blocks.back().Succs = {0};
  auto Backedge = F.Blocks.front().InstructionBoundaries.back();
  Backedge.Address = F.Blocks.back().StartAddr;
  Backedge.FirstOp = 0;
  Backedge.OpCount = F.Blocks.back().Ops.size();
  Backedge.Immediate = F.Blocks.front().StartAddr;
  F.Blocks.back().InstructionBoundaries.push_back(Backedge);
  LowBlock Entry;
  Entry.Id = 2;
  Entry.StartAddr = F.Entry = 0x900;
  F.Blocks.insert(F.Blocks.begin(), Entry);
  EXPECT_TRUE(constantRegisterCallTargets(F).empty());
}

TEST(CallRegisterConstant, ExactOccurrenceAndRegisterByteWidths) {
  const auto Seed = unary(NdOp::COPY, 8, 0x1234567800000302, 8);
  auto Function = registerCall({Seed}, Seed.Output);
  for (const auto Opcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
    auto &Call = Function.Blocks.front().Ops.back();
    Call.Opcode = Opcode;
    Call.Inputs[0] = Opcode == NdOp::CALL ? NdVar::cst(0x2000, 8) : Seed.Output;
    const auto Site = *sourceCallOccurrenceKey(Call);
    for (const uint16_t Width : {1, 2, 4, 8}) {
      const auto Result =
          constantRegisterValueAtCall(Function, Site, NdVar::reg(0, Width));
      EXPECT_TRUE(Result.OccurrenceFound);
      EXPECT_FALSE(Result.BudgetExceeded);
      EXPECT_EQ(Result.Value,
                Seed.Inputs[0].Offset & (UINT64_MAX >> ((8 - Width) * 8)));
    }
    auto Stale = Site;
    ++Stale.Sequence;
    EXPECT_FALSE(constantRegisterValueAtCall(Function, Stale, Seed.Output)
                     .OccurrenceFound);
  }
}

TEST(CallRegisterConstant, UpperByteWritesCannotInventOrEraseLowByteFacts) {
  const auto Seed = unary(NdOp::COPY, 8, 2, 8);
  auto Unknown = Seed;
  Unknown.Output = NdVar::reg(1, 1);
  Unknown.Inputs[0] = NdVar::reg(32, 1);
  auto Function = registerCall({Seed, Unknown}, Seed.Output);
  const auto Site =
      *sourceCallOccurrenceKey(Function.Blocks.front().Ops.back());
  EXPECT_EQ(constantRegisterValueAtCall(Function, Site, NdVar::reg(0, 1)).Value,
            2u);
  EXPECT_FALSE(constantRegisterValueAtCall(Function, Site, Seed.Output).Value);
  Function.Blocks.front().Ops[1].Output.Offset = 0;
  EXPECT_FALSE(
      constantRegisterValueAtCall(Function, Site, NdVar::reg(0, 1)).Value);
}

TEST(CallRegisterConstant,
     AmbiguousOccurrencesAndBudgetExhaustionDiscardFacts) {
  const auto Seed = unary(NdOp::COPY, 8, 0x140001040, 8);
  auto Function = registerCall({Seed}, Seed.Output);
  const auto Site =
      *sourceCallOccurrenceKey(Function.Blocks.front().Ops.back());
  ASSERT_TRUE(constantRegisterValueAtCall(Function, Site, Seed.Output).Value);
  auto Other = Function.Blocks.front();
  Other.Id = 1;
  Other.Ops.front().Inputs[0].Offset += 16;
  Function.Blocks.push_back(Other);
  EXPECT_TRUE(constantRegisterCallTargets(Function).empty());
  EXPECT_FALSE(constantRegisterValueAtCall(Function, Site, Seed.Output).Value);
  Function.Blocks.resize(1);
  Function.Blocks.emplace_back();
  Function.Blocks.back().Ops.resize(
      limits::ir::kConstantRegisterCallOperations -
      Function.Blocks.front().Ops.size());
  EXPECT_TRUE(constantRegisterValueAtCall(Function, Site, Seed.Output).Value);
  Function.Blocks.back().Ops.emplace_back();
  const auto Result = constantRegisterValueAtCall(Function, Site, Seed.Output);
  EXPECT_TRUE(Result.BudgetExceeded);
  EXPECT_FALSE(Result.Value);
}

TEST(CallRegisterConstant, PartialWritesMatchIndependentByteStorage) {
  for (bool KnownWrite : {false, true})
    for (unsigned Offset = 0; Offset != 24; ++Offset)
      for (unsigned Width : {1U, 2U, 4U, 8U, 16U}) {
        if (KnownWrite && Width > sizeof(uint64_t))
          continue;
        SCOPED_TRACE(KnownWrite);
        SCOPED_TRACE(Offset);
        SCOPED_TRACE(Width);
        std::array<uint8_t, 40> Bytes{};
        std::array<bool, 40> Known{};
        std::vector<LowOp> Setup;
        for (unsigned Base = 0; Base != 24; Base += 8) {
          uint64_t Bits = 0;
          for (unsigned I = 0; I != 8; ++I) {
            Bytes[Base + I] = 0x10 + Base + I;
            Known[Base + I] = true;
            Bits |= uint64_t{Bytes[Base + I]} << (I * 8);
          }
          auto Seed = unary(NdOp::COPY, 8, Bits, 8);
          Seed.Output.Offset = Base;
          Setup.push_back(Seed);
        }
        const uint64_t Bits = 0xfedcba9876543210;
        auto Write = unary(NdOp::COPY, Width, Bits, Width);
        Write.Output.Offset = Offset;
        if (!KnownWrite)
          Write.Inputs[0] = NdVar::reg(128, Width);
        Setup.push_back(Write);
        for (unsigned I = 0; I < Width; ++I) {
          Known[Offset + I] = KnownWrite;
          if (KnownWrite)
            Bytes[Offset + I] = static_cast<uint8_t>(Bits >> (I * 8));
        }
        const auto Function = registerCall(std::move(Setup), NdVar::reg(0, 8));
        const auto Site =
            *sourceCallOccurrenceKey(Function.Blocks.front().Ops.back());
        for (unsigned Begin = 0; Begin != 32; ++Begin)
          for (unsigned Size : {1U, 2U, 4U, 8U}) {
            std::optional<uint64_t> Expected = 0;
            for (unsigned I = 0; I < Size; ++I) {
              if (!Known[Begin + I]) {
                Expected.reset();
                break;
              }
              *Expected |= uint64_t{Bytes[Begin + I]} << (I * 8);
            }
            EXPECT_EQ(constantRegisterValueAtCall(Function, Site,
                                                  NdVar::reg(Begin, Size))
                          .Value,
                      Expected)
                << "read byte " << Begin << " width " << Size;
          }
      }
}

TEST(LowScalarConstant, AbsorbersRequireCurrentNumericBytes) {
  for (unsigned Mode = 0; Mode < 13; ++Mode) {
    SCOPED_TRACE(Mode);
    LowBlock Block;
    auto Set = unary(NdOp::COPY, 8, Mode == 1 ? 1 : 0, 8);
    Set.Addr = 0x1000;
    auto Mask = binary(NdOp::INT_AND, 8, 0, 0);
    Mask.Addr = 0x1004;
    Mask.Output = NdVar::tmp(TmpBase + 16, 8);
    Mask.Inputs[0] = NdVar::tmp(TmpBase + 8, 8);
    Mask.Inputs[1] = Set.Output;
    Block.Ops = {Set, Mask};
    if (Mode >= 2 && Mode <= 4) {
      LowOp Barrier;
      Barrier.Addr = 0x1002;
      Barrier.Opcode = Mode == 2   ? NdOp::CALL
                       : Mode == 3 ? NdOp::INDIR_CALL
                                   : NdOp::INTRINSIC;
      Block.Ops.insert(Block.Ops.begin() + 1, Barrier);
    }
    if (Mode == 5)
      Block.Ops.front().Inputs[0].Provenance =
          ConstantAddressProvenance::Address;
    if (Mode == 6) {
      auto Partial = unary(NdOp::COPY, 1, 1, 1);
      Partial.Output.Offset = 3;
      Block.Ops.insert(Block.Ops.begin() + 1, Partial);
    }
    if (Mode == 7 || Mode == 8) {
      Block.Ops.front().Output = NdVar::tmp(TmpBase, 8);
      Block.Ops.back().Inputs[1] = Block.Ops.front().Output;
      Block.Ops.back().Addr = Set.Addr;
      if (Mode == 8) {
        auto Partial = unary(NdOp::COPY, 1, 1, 1);
        Partial.Addr = Set.Addr;
        Partial.Output = NdVar::tmp(TmpBase + 3, 1);
        Block.Ops.insert(Block.Ops.begin() + 1, Partial);
      }
    }
    if (Mode == 9)
      Block.Ops.back().Inputs[1].Size = 4;
    if (Mode == 10)
      Block.Ops.front().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    std::vector<LowBlock> Blocks{Block};
    if (Mode == 11) {
      Blocks.emplace_back();
      Blocks.back().Ops = {Blocks.front().Ops.back()};
      Blocks.front().Ops.pop_back();
    }
    size_t Remaining = Mode == 12 ? 0 : 128;
    const auto Absorbed = lowBlockAbsorbingConstants(Blocks, Remaining);
    EXPECT_EQ(Absorbed.size(), Mode == 0 || Mode == 7 ? 1u : 0u);
  }
}

} // namespace
