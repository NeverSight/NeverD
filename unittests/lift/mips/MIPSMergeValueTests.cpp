//===- MIPSMergeValueTests.cpp - Complete pair proof boundaries
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent SSA fixtures distinguish complete pairs from observable partial
/// results, wrong address relations and missing physical occurrence evidence.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MIPS/MIPSMergeValues.h"
#include "neverd/ir/med/MedIR.h"

#include <utility>

using namespace neverd;

namespace {

MedVar variable(int Id, unsigned Bytes, Arch Target,
                MedVar::VarKind Kind = MedVar::Temp) {
  MedVar V;
  V.Kind = Kind;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  V.TheArch = Target;
  return V;
}

MedVar scalar(uint64_t V, unsigned Bytes) {
  return MedVar::makeConst(V, Bytes, ConstantAddressProvenance::Scalar);
}

struct Pair {
  MedFunc Function;
  LowFunc Physical;
  Arch Target;
  unsigned Width;
  unsigned Bytes;

  Pair(Arch A, unsigned N, bool Reverse)
      : Target(A), Width(isMIPS64(A) ? 8 : 4), Bytes(N) {
    Function.Blocks.resize(1);
    Physical.Blocks.resize(1);
    auto &Ops = Function.Blocks[0].Ops;
    const MedVar Base = variable(10, Width, A, MedVar::Reg);
    const MedVar Old = variable(11, Bytes, A, MedVar::Reg);
    for (unsigned I = 0; I < 2; ++I) {
      const bool Left = bool(I) == Reverse;
      MedOp Address;
      Address.Opcode = NdOp::INT_ADD;
      Address.Output = variable(20 + I, Width, A);
      Address.addInput(Base);
      Address.addInput(scalar(Left == archLittleEndian(A) ? N - 1 : 0, Width));
      Ops.push_back(Address);
      MedOp Load;
      Load.Opcode = NdOp::INTRINSIC;
      Load.Output = variable(30 + I, N, A);
      Load.Addr = 0x1000 + 4 * I;
      Load.OriginSeq = 10 + I;
      const Intrinsic Id =
          N == 4 ? (Left ? Intrinsic::MIPSLwl : Intrinsic::MIPSLwr)
                 : (Left ? Intrinsic::MIPSLdl : Intrinsic::MIPSLdr);
      Load.addInput(MedVar::makeConst(uint64_t(Id), sizeof(Intrinsic)));
      Load.addInput(Address.Output);
      Load.addInput(I ? Ops[1].Output : Old);
      Ops.push_back(Load);

      LowOp Low;
      Low.Opcode = NdOp::INTRINSIC;
      Low.Addr = Load.Addr;
      Low.Seq = Load.OriginSeq;
      Low.Output = NdVar::tmp(30 + I, N);
      Low.Inputs[0] = NdVar::cst(uint64_t(Id), sizeof(Intrinsic));
      Low.Inputs[1] = NdVar::reg(32, Width);
      Low.Inputs[2] = NdVar::reg(16, N);
      Low.NumInputs = 3;
      Physical.Blocks[0].Ops.push_back(Low);
      LowInstructionBoundary Boundary;
      Boundary.Address = Load.Addr;
      Boundary.Size = 4;
      Boundary.FirstOp = I;
      Boundary.OpCount = 1;
      Physical.Blocks[0].InstructionBoundaries.push_back(Boundary);
    }
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(Ops[3].Output);
    Ops.push_back(Return);
  }

  MedOp &first() { return Function.Blocks[0].Ops[1]; }
  MedOp &second() { return Function.Blocks[0].Ops[3]; }
  unsigned run() {
    return discardUnobservedMIPSMergeInputs(Function, Physical, Target);
  }
};

class MIPSMergeValues : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSMergeValues, BothOrdersDiscardOnlyTheFirstOldValue) {
  for (unsigned N : {4u, 8u}) {
    if (N == 8 && !isMIPS64(GetParam()))
      continue;
    for (bool Reverse : {false, true}) {
      Pair P(GetParam(), N, Reverse);
      const auto Partial = P.first().Output;
      ASSERT_EQ(P.run(), 1u);
      EXPECT_TRUE(P.first().Inputs[2].isConst());
      EXPECT_EQ(P.first().Inputs[2].ConstVal, 0u);
      EXPECT_EQ(P.second().Inputs[2], Partial);
      EXPECT_EQ(P.run(), 0u);
    }
  }
}

TEST_P(MIPSMergeValues, MIPS16PairsRequireFourBytePhysicalInstructions) {
  for (bool Reverse : {false, true}) {
    Pair P(GetParam(), 4, Reverse);
    for (auto &B : P.Physical.Blocks[0].InstructionBoundaries)
      B.Mode = InstructionMode::MIPS16;
    EXPECT_EQ(P.run(), isMIPS64(GetParam()) ? 0u : 1u);
    for (unsigned Case = 0; Case < 3; ++Case) {
      Pair Bad(GetParam(), 4, Reverse);
      auto &Boundaries = Bad.Physical.Blocks[0].InstructionBoundaries;
      for (auto &B : Boundaries)
        B.Mode = InstructionMode::MIPS16;
      if (Case == 0)
        Boundaries[1].Size = 2;
      if (Case == 1)
        Boundaries[1].DelaySlot = LowDelaySlot{};
      if (Case == 2)
        Boundaries[1].Mode = InstructionMode::MicroMIPS;
      EXPECT_EQ(Bad.run(), 0u);
      EXPECT_FALSE(Bad.first().Inputs[2].isConst());
    }
  }
}

TEST_P(MIPSMergeValues, MicroPairsRequireTwoFullInstructionsInTheSameMode) {
  const unsigned Bytes = isMIPS64(GetParam()) ? 8 : 4;
  for (bool Reverse : {false, true}) {
    for (unsigned Bad = 0; Bad < 4; ++Bad) {
      Pair P(GetParam(), Bytes, Reverse);
      auto &B = P.Physical.Blocks[0].InstructionBoundaries;
      B[0].Mode = B[1].Mode = InstructionMode::MicroMIPS;
      if (Bad == 1)
        B[0].Size = 2;
      if (Bad == 2)
        B[1].Mode = InstructionMode::MIPSClassic;
      if (Bad == 3)
        B[1].DelaySlot = LowDelaySlot{};
      EXPECT_EQ(P.run(), Bad == 0 ? 1u : 0u);
    }
  }
}

TEST_P(MIPSMergeValues, RefusesUnprovedAddressAndOccurrenceRelations) {
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    Pair P(GetParam(), 4, false);
    auto &Ops = P.Function.Blocks[0].Ops;
    auto &Boundaries = P.Physical.Blocks[0].InstructionBoundaries;
    switch (Case) {
    case 0:
      Ops[2].Inputs[1].ConstVal += 4;
      break;
    case 1:
      Ops[2].Inputs[0].SSAVer += 1;
      break;
    case 2:
      Ops[2].Inputs[0].Kind = MedVar::Temp;
      break;
    case 3:
      Ops[2].Inputs[1].Provenance = ConstantAddressProvenance::AddressFragment;
      break;
    case 4:
      P.second().OriginSeq += 1;
      break;
    case 5:
      P.second().Addr += 4;
      break;
    case 6:
      Boundaries[1].Address += 4;
      break;
    case 7:
      Boundaries.clear();
      break;
    case 8:
      Boundaries[0].Control = LowInstructionControl::Branch;
      break;
    case 9:
      std::swap(Ops[0].Inputs[1], Ops[2].Inputs[1]);
      break;
    case 10:
    case 11:
      P.first().Inputs[1] = MedVar::makeConst(
          archLittleEndian(GetParam()) ? 0x1003 : 0x1000, P.Width,
          ConstantAddressProvenance::DataAddress, 0x1000);
      P.second().Inputs[1] = MedVar::makeConst(
          archLittleEndian(GetParam()) ? 0x1000 : 0x1003, P.Width,
          ConstantAddressProvenance::DataAddress, Case == 10 ? 0x1000 : 0x2000);
      break;
    }
    EXPECT_EQ(P.run(), 0u);
    EXPECT_FALSE(P.first().Inputs[2].isConst());
  }
}

TEST_P(MIPSMergeValues, IntermediateObservationsAndControlKeepTheOldValue) {
  for (unsigned Case = 0; Case < 8; ++Case) {
    SCOPED_TRACE(Case);
    Pair P(GetParam(), 4, false);
    auto &Block = P.Function.Blocks[0];
    const auto Partial = P.first().Output;
    switch (Case) {
    case 0:
      Block.Ops.back().Inputs[0] = Partial;
      break;
    case 1: {
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(variable(40, P.Width, GetParam()));
      Store.addInput(Partial);
      Block.Ops.push_back(Store);
      break;
    }
    case 2:
      Block.Ops[2].Inputs[0] = Partial;
      break;
    case 3:
      Block.Phis.resize(1);
      break;
    case 4:
      Block.ExceptionalSuccs.resize(1);
      break;
    case 5:
      P.Function.CallInfos.resize(1);
      break;
    case 6: {
      MedOp Call;
      Call.Opcode = NdOp::INDIR_CALL;
      Call.addInput(variable(40, P.Width, GetParam()));
      Block.Ops.push_back(Call);
      break;
    }
    case 7: {
      // Even an implicit final physical-register observation is not allowed
      // to become a fabricated value when the pair completes elsewhere.
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = variable(40, 4, GetParam(), MedVar::Reg);
      Copy.Output.RegOff = 32;
      Copy.addInput(Partial);
      Block.Ops.push_back(Copy);
      break;
    }
    }
    EXPECT_EQ(P.run(), 0u);
    EXPECT_FALSE(P.first().Inputs[2].isConst());
  }
}

TEST_P(MIPSMergeValues, TransportMustKeepEveryMergeByte) {
  for (bool Narrow : {false, true}) {
    Pair P(GetParam(), 4, false);
    const auto Partial = P.first().Output;
    MedOp Copy;
    Copy.Opcode = NdOp::SUBBYTES;
    Copy.Output = variable(50, Narrow ? 2 : 4, GetParam());
    Copy.addInput(Partial);
    Copy.addInput(scalar(0, 4));
    P.second().Inputs[2] = Copy.Output;
    P.Function.Blocks[0].Ops.insert(P.Function.Blocks[0].Ops.begin() + 2, Copy);
    EXPECT_EQ(P.run(), Narrow ? 0u : 1u);
  }
}

TEST_P(MIPSMergeValues, AddressDisplacementsWrapAtTheGuestWidth) {
  Pair P(GetParam(), 4, false);
  const uint64_t Base = isMIPS64(GetParam()) ? UINT64_MAX - 1 : UINT32_MAX - 1;
  const uint64_t Mask = isMIPS64(GetParam()) ? UINT64_MAX : UINT32_MAX;
  auto &Ops = P.Function.Blocks[0].Ops;
  Ops[0].Inputs[1] =
      scalar(archLittleEndian(GetParam()) ? (Base + 3) & Mask : Base, P.Width);
  Ops[2].Inputs[1] =
      scalar(archLittleEndian(GetParam()) ? Base : (Base + 3) & Mask, P.Width);
  EXPECT_EQ(P.run(), 1u);
}

TEST_P(MIPSMergeValues, WordSignExtensionAndProjectionKeepTheSameLowBytes) {
  Pair P(GetParam(), 4, false);
  MedOp Extend;
  Extend.Opcode = NdOp::INT_SEXT;
  Extend.Output = variable(50, P.Width, GetParam());
  Extend.addInput(P.first().Output);
  MedOp Slice;
  Slice.Opcode = NdOp::SUBBYTES;
  Slice.Output = variable(51, 4, GetParam());
  Slice.addInput(Extend.Output);
  Slice.addInput(scalar(0, 4));
  P.second().Inputs[2] = Slice.Output;
  auto &Ops = P.Function.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 2, {Extend, Slice});
  EXPECT_EQ(P.run(), 1u);
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSMergeValues,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
