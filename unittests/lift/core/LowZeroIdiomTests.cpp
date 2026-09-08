#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedTypePass.h"

using namespace neverd;

namespace {
LowOp operation(NdOp Opcode, NdVar Output,
                std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = 0x1000;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}
SourceFunctionTypeHint declaration(Arch Architecture) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeInt(8);
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error)) << Error;
  return Hint;
}
LowFunc zero(Arch Architecture, unsigned Width) {
  const auto &TRI = getTargetRegInfo(Architecture);
  const auto Register = TRI.FPParamRegs[0];
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "sum_carrier_zero";
  Low.Blocks.emplace_back();
  auto &B = Low.Blocks[0];
  B.Id = 0;
  B.StartAddr = 0x1000;
  B.EndAddr = 0x1004;
  B.Ops.push_back(
      operation(NdOp::INT_XOR, NdVar::reg(Register, Width),
                {NdVar::reg(Register, Width), NdVar::reg(Register, Width)}));
  B.Ops.push_back(operation(Width == 8 ? NdOp::COPY : NdOp::SUBBYTES,
                            NdVar::reg(TRI.IntReturnReg, 8),
                            {NdVar::reg(Register, Width)}));
  if (Width != 8)
    B.Ops.back().addInput(NdVar::cst(0, 4));
  B.Ops.push_back(
      operation(NdOp::RETURN, {}, {NdVar::reg(TRI.IntReturnReg, 8)}));
  return Low;
}

TEST(LowZeroIdioms, WideRegisterSelfXorHasNoIncomingParameterBeforeSSA) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (unsigned Width : {8U, 16U, 32U, 64U}) {
      auto Low = zero(Architecture, Width);
      auto Med =
          LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops) {
          EXPECT_NE(Op.Opcode, NdOp::INT_XOR);
          if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
            EXPECT_FALSE(Op.Output == Op.Inputs[0] &&
                         Op.Output.Kind == MedVar::Reg &&
                         Op.Output.RegOff ==
                             getTargetRegInfo(Architecture).FPParamRegs[0]);
        }
      Med.SourceTypeHint = declaration(Architecture);
      inferMedTypes(Med, Architecture);
      ASSERT_TRUE(Med.SourceTypeHint) << int(Architecture) << ":" << Width;
      EXPECT_TRUE(Med.SourceParametersBound);
      EXPECT_TRUE(Med.Params.empty());
      EXPECT_EQ(Med.ReturnValueEvidence, MedReturnValueEvidence::Unknown);
      auto High = MedToHighConverter().convert(Med, Architecture);
      EXPECT_TRUE(High.SourceTypeHint);
      EXPECT_TRUE(High.Params.empty());
    }
  }
}

TEST(LowZeroIdioms, DistinctRegistersDoNotLoseTheirActualIncomingValues) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Low = zero(Architecture, 16);
    Low.Blocks[0].Ops[0].Inputs[1].Offset =
        getTargetRegInfo(Architecture).FPParamRegs[1];
    auto Med =
        LowToMedConverter().convert(Low, Architecture, BinaryFormat::MachO);
    bool Found = false;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops)
        Found |= Op.Opcode == NdOp::INT_XOR;
    EXPECT_TRUE(Found);
    Med.SourceTypeHint = declaration(Architecture);
    inferMedTypes(Med, Architecture);
    EXPECT_FALSE(Med.SourceTypeHint);
  }
}

TEST(LowZeroIdioms, OrderedSelfXorIsNeverSilentlyReplacedByAPureConstant) {
  auto Low = zero(Arch::X64, 16);
  Low.Blocks[0].Ops[0].MemoryOrdering = NdMemoryOrdering::Acquire;
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::MachO);
  bool Found = false;
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.Opcode == NdOp::INT_XOR) {
        Found = true;
        EXPECT_EQ(Op.MemoryOrdering, NdMemoryOrdering::Acquire);
      }
  EXPECT_TRUE(Found);
}

MedFunc seeded(bool Used) {
  MedFunc F;
  F.SourceTypeHint = declaration(Arch::X64);
  F.Blocks.emplace_back();
  F.Blocks[0].Id = 0;
  MedVar Seed;
  Seed.Kind = MedVar::Reg;
  Seed.Id = 10;
  Seed.SSAVer = 0;
  Seed.RegOff = getTargetRegInfo(Arch::X64).FPParamRegs[0];
  Seed.Size = 16;
  Seed.TheArch = Arch::X64;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = Seed;
  Copy.addInput(Seed);
  F.Blocks[0].Ops.push_back(Copy);
  if (Used) {
    Copy.Output.Id = 11;
    Copy.Output.Kind = MedVar::Temp;
    F.Blocks[0].Ops.push_back(Copy);
  }
  return F;
}

TEST(LowZeroIdioms,
     DeadEntrySeedDoesNotRejectAnOtherwiseCompleteSourceSignature) {
  auto F = seeded(false);
  inferMedTypes(F, Arch::X64);
  ASSERT_TRUE(F.SourceTypeHint);
  EXPECT_TRUE(F.SourceParametersBound);
  EXPECT_TRUE(F.Params.empty());
  auto Used = seeded(true);
  inferMedTypes(Used, Arch::X64);
  EXPECT_FALSE(Used.SourceTypeHint);
}

TEST(LowZeroIdioms,
     PhiAndCallArgumentUsesStillRequireIncomingParameterEvidence) {
  for (bool PhiUse : {false, true}) {
    auto F = seeded(false);
    const auto Seed = F.Blocks[0].Ops[0].Output;
    if (PhiUse) {
      PhiNode Phi;
      Phi.Output = Seed;
      Phi.Output.SSAVer = 1;
      Phi.Args.emplace_back(0, Seed);
      F.Blocks[0].Phis.push_back(Phi);
    } else {
      MedCallInfo Call;
      Call.Args.push_back(Seed);
      F.CallInfos.push_back(Call);
    }
    inferMedTypes(F, Arch::X64);
    EXPECT_FALSE(F.SourceTypeHint);
  }
}
} // namespace
