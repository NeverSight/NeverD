//===- HighCallParameterForwardingTests.cpp - Value-preserving parameters
//---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file A parameter's transformed value is not an unchanged argument.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/high/lower/CallArgCollectionDetail.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include <vector>

namespace {
using namespace neverd;

MedVar forwarded(NdOp Opcode, uint16_t Width = 8, bool ThroughPhi = false) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Blocks.resize(ThroughPhi ? 4 : 1);
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = F.Entry + 16 * I;
  }
  auto &B = F.Blocks.front();
  B.Id = 0;
  B.StartAddr = F.Entry;
  MedVar P;
  P.Kind = MedVar::Reg;
  P.Id = 1;
  P.Size = 8;
  P.RegOff = x86reg::RCX;
  P.TheArch = Arch::X64;
  F.Params = {P};
  MedVar T = P;
  T.Kind = MedVar::Temp;
  T.Id = 2;
  T.SSAVer = 1;
  T.Size = Width;
  T.RegOff = 0;
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = T;
  Op.addInput(P);
  if (Opcode != NdOp::COPY)
    Op.addInput(MedVar::makeConst(Opcode == NdOp::INT_AND ? 255 : 1, 8));
  B.Ops.push_back(Op);
  MedVar Argument = T;
  if (ThroughPhi) {
    Argument.Id = 3;
    B.Succs = {1, 2};
    for (size_t I : {1u, 2u}) {
      F.Blocks[I].Preds = {0};
      F.Blocks[I].Succs = {3};
    }
    F.Blocks.back().Preds = {1, 2};
    F.Blocks.back().Phis.push_back({Argument, {{1, P}, {2, T}}});
  }
  std::vector<ExprPtr> Found(4);
  Found[0] = HighExpr::makeVar(Argument);
  auto Slot = [](uint64_t Offset) { return Offset == x86reg::RCX ? 0 : -1; };
  auto Param = [](const MedVar &V) {
    return V.Kind == MedVar::Param || V.RegOff == x86reg::RCX ? 0 : -1;
  };
  const auto &TRI = getTargetRegInfo(Arch::X64);
  call_args_detail::CallArgContext Context{
      .Found = Found,
      .MaxArgs = 4,
      .ParamRegs = TRI.integerParamRegs(BinaryFormat::COFF),
      .CurBlock = F.Blocks.back(),
      .CurMed = &F,
      .RegToArgIdx = Slot,
      .AbiParamIndex = Param};
  const auto *Policy =
      call_args_detail::callArgPolicy(Arch::X64, BinaryFormat::COFF);
  Policy->ResolvePassThroughParams(Context);
  return Found.front()->Var;
}

TEST(HighCallParameterForwarding, CopiesMayNameTheirIncomingParameter) {
  EXPECT_EQ(forwarded(NdOp::COPY).Kind, MedVar::Param);
  EXPECT_EQ(forwarded(NdOp::COPY, 8, true).Kind, MedVar::Param);
}

TEST(HighCallParameterForwarding, TransformedArgumentsKeepTheirActualValue) {
  // For input 0x1234, AND 255 is 0x34, not 0x1234. Dependency on a parameter
  // is insufficient evidence for substituting the parameter itself.
  for (NdOp Op :
       {NdOp::INT_AND, NdOp::INT_OR, NdOp::INT_LEFT, NdOp::INT_RIGHT}) {
    EXPECT_EQ(forwarded(Op).Kind, MedVar::Temp);
    EXPECT_EQ(forwarded(Op, 8, true).Kind, MedVar::Temp);
  }
  EXPECT_EQ(forwarded(NdOp::COPY, 4).Kind, MedVar::Temp);
}
} // namespace
